// tape_sort.cpp - THE TAPE READER (TAPE_READER.md): the pull machine.
//
// Four components, nothing else:
//
//   THE TAPE     one sequential corpus reader + a request queue. A request is
//                {prefix bytes, key_len, sink}; a pass serves EVERY outstanding
//                request simultaneously (a flat trie over the request prefixes;
//                a position matching nothing costs one lookup). The queue empty
//                -> the reader idles: poll + sleep, ZERO I/O, zero signals.
//   THE PLAN     the survey (subdiv_survey.h, carried over) + the count pass
//                give the whole radix partition up front into UNITS of
//                --unit-target-rows. Journaled once, before any extraction.
//                Units ordered by prefix = the output order.
//   THE SORTERS  the wavekey leaf sort (carried over): 8-bit MSD, 257 buckets,
//                bucket 0 = virtual END, the run-length digit, tie groups issue
//                ONE big-window follow-up request (XSA_TIE_WINDOW_BYTES), the
//                LCE fallback only for the residual. Sorters never read the
//                corpus; only the tape does.
//   THE EMITTER  a completed unit emits pieces in the EXISTING piece format,
//                appended in unit order to the single output tape; the finish,
//                the trie tap, the container and the docstarts are UNCHANGED.
//
// Deleted vs the band route: the band-part/consumed-marker protocol, the piece
// spool between sorter and concat, the concat stage, the position-window
// machinery, and the runtime budget model that sized waves from knobs. The
// governor (term table, plan-time bound, statvfs, no signals) is unchanged.
//
// No text is materialized to disc; every corpus read is a windowed pread, and
// the chunk plane (position, key, run digit) lives in RAM bounded by
//     outstanding_chunk_bytes <= envelope - working_reserve
// with a plan-time refusal (numbers, no I/O) for a unit that cannot fit.

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <set>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dirent.h>
#include <csignal>
#include <execinfo.h>
#include <sys/syscall.h>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <fstream>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <malloc.h>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

static void die(const std::string& m) {
    std::fprintf(stderr, "tape_sort: %s\n", m.c_str());
    std::exit(2);
}
static void require(bool b, const char* s) { if (!b) die(s); }

static double now_sec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now()
                                            .time_since_epoch())
        .count();
}
static inline u64 bswap64(u64 x) { return __builtin_bswap64(x); }

static const u8* T = nullptr;
static u64 N = 0, FILE_N = 0;
static u64 KEY_N = 0; // positions with >= 24 real bytes enter the tape; the rest are tails
static constexpr u8 SENTINEL_SYMBOL = 0;
static const u8* REMAP = nullptr;
static uint16_t PAIRMAP[65536];
static bool PAIRMAP_READY = false;
static inline void build_pairmap() {
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned b = 0; b < 256; ++b) {
            const unsigned ma = REMAP ? REMAP[a] : a;
            const unsigned mb = REMAP ? REMAP[b] : b;
            PAIRMAP[(a << 8) | b] = uint16_t((ma << 8) | mb);
        }
    PAIRMAP_READY = true;
}
static inline u64 pack_sym(u64 p, u8 sym) { return p | (u64(sym) << 48); }
static inline u64 unpack_pos(u64 v) { return v & ((1ull << 48) - 1); }

// ---------------------------------------------------------------------------
// THE COMPARISON CORE (carried over from the band route verbatim: this is the
// dollar-order suffix comparison the tails, the seams and the LCE residual
// need; the leaf sort itself never calls it for a row the window resolved).
// ---------------------------------------------------------------------------
static inline int suf_cmp_lcp(u64 a, u64 b, u64 from, u64& lcp) {
    const u64 cap = std::min(FILE_N - a, FILE_N - b);
    lcp = from;
    u64 i = from;
    if (PAIRMAP_READY) {
        while (i + 2 <= cap) {
            const uint16_t x = PAIRMAP[(unsigned(T[a + i]) << 8) | T[a + i + 1]];
            const uint16_t y = PAIRMAP[(unsigned(T[b + i]) << 8) | T[b + i + 1]];
            if (x != y) {
                lcp = i + ((x >> 8) == (y >> 8) ? 1 : 0);
                return ((x >> 8) == (y >> 8)) ? (((x & 0xff) < (y & 0xff)) ? -1 : 1)
                                              : ((x < y) ? -1 : 1);
            }
            i += 2;
        }
        for (; i < cap; ++i) {
            const u8 xb = REMAP ? REMAP[T[a + i]] : T[a + i];
            const u8 yb = REMAP ? REMAP[T[b + i]] : T[b + i];
            if (xb != yb) { lcp = i; return xb < yb ? -1 : 1; }
        }
        lcp = cap;
        if (a == b) return 0;
        return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
    }
    for (; i < cap; ++i) {
        const u8 xb = REMAP ? REMAP[T[a + i]] : T[a + i];
        const u8 yb = REMAP ? REMAP[T[b + i]] : T[b + i];
        if (xb != yb) { lcp = i; return xb < yb ? -1 : 1; }
    }
    lcp = cap;
    if (a == b) return 0;
    return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
}

static u64 g_wave_fallback_walk_max = 4ull << 30;
static bool g_wave_fallback_walk_set = false;
static inline int suf_cmp_from_counted(u64 a, u64 b, u64 from, u64& walked) {
    const u64 cap = std::min(FILE_N - a, FILE_N - b);
    if (from >= cap) {
        if (a == b) return 0;
        return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
    }
    u64 i = from;
    while (i + 8 <= cap) {
        u64 x, y;
        std::memcpy(&x, T + a + i, 8);
        std::memcpy(&y, T + b + i, 8);
        if (x != y) {
            u8 xs[8], ys[8];
            std::memcpy(xs, &x, 8);
            std::memcpy(ys, &y, 8);
            for (int j = 0; j < 8; ++j) {
                walked += 1;
                if (xs[j] != ys[j]) return xs[j] < ys[j] ? -1 : 1;
            }
        }
        walked += 8;
        i += 8;
    }
    for (; i < cap; ++i) {
        ++walked;
        const u8 xb = REMAP ? REMAP[T[a + i]] : T[a + i];
        const u8 yb = REMAP ? REMAP[T[b + i]] : T[b + i];
        if (xb != yb) return xb < yb ? -1 : 1;
    }
    if (a == b) return 0;
    return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
}

static inline int suf_cmp_from(u64 a, u64 b, u64 from) {
    const u64 cap = std::min(FILE_N - a, FILE_N - b);
    if (from >= cap) {
        if (a == b) return 0;
        return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
    }
    if (PAIRMAP_READY) {
        u64 i = from;
        while (i + 2 <= cap) {
            const uint16_t x = PAIRMAP[(unsigned(T[a + i]) << 8) | T[a + i + 1]];
            const uint16_t y = PAIRMAP[(unsigned(T[b + i]) << 8) | T[b + i + 1]];
            if (x != y) return x < y ? -1 : 1;
            i += 2;
        }
        for (; i < cap; ++i) {
            const u8 xb = REMAP ? REMAP[T[a + i]] : T[a + i];
            const u8 yb = REMAP ? REMAP[T[b + i]] : T[b + i];
            if (xb != yb) return xb < yb ? -1 : 1;
        }
        if (a == b) return 0;
        return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
    }
    if (!REMAP) {
        u64 i = from;
        while (i + 8 <= cap) {
            u64 x, y;
            std::memcpy(&x, T + a + i, 8);
            std::memcpy(&y, T + b + i, 8);
            if (x != y) {
                u8 xs[8], ys[8];
                std::memcpy(xs, &x, 8);
                std::memcpy(ys, &y, 8);
                for (int j = 0; j < 8; ++j)
                    if (xs[j] != ys[j]) return xs[j] < ys[j] ? -1 : 1;
            }
            i += 8;
        }
        for (; i < cap; ++i)
            if (T[a + i] != T[b + i]) return T[a + i] < T[b + i] ? -1 : 1;
    } else {
        for (u64 i = from; i < cap; ++i) {
            const u8 xb = REMAP[T[a + i]], yb = REMAP[T[b + i]];
            if (xb != yb) return xb < yb ? -1 : 1;
        }
    }
    if (a == b) return 0;
    return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
}

// A finite prefix comparison, used by the point-mass proof.
static inline int prefix_cmp(u64 a, u64 b, u64 depth) {
    const u64 cap = std::min({depth, FILE_N - a, FILE_N - b});
    for (u64 i = 0; i < cap; ++i) {
        unsigned x = REMAP ? REMAP[T[a + i]] : T[a + i];
        unsigned y = REMAP ? REMAP[T[b + i]] : T[b + i];
        if (x != y) return x < y ? -1 : 1;
    }
    if (cap == depth || a == b) return 0;
    return FILE_N - a < FILE_N - b ? -1 : 1;
}

// ---------------------------------------------------------------------------
// THE RSS RECEIPT (carried over): the budget assertion is on OWNED (anon) RSS.
// ---------------------------------------------------------------------------
// THE ANON-MEMORY GUARD knob (checked by the RSS monitor below): our
// refusal fires BEFORE the OS OOM killer's, with numbers.
static u64 g_mem_guard_mb = 196 * 1024;    // XSA_MEM_GUARD_MB (0 = off)
struct RssMonitor {
    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<u64> peak_anon{0}, peak_file{0}, peak_total{0};
    static bool sample(u64& anon, u64& file, u64& total) {
        FILE* f = std::fopen("/proc/self/status", "r");
        if (!f) return false;
        char line[256];
        anon = file = total = 0;
        bool any = false;
        while (std::fgets(line, sizeof line, f)) {
            unsigned long long kb = 0;
            if (std::sscanf(line, "VmRSS: %llu kB", &kb) == 1) { total = kb; any = true; }
            else if (std::sscanf(line, "RssAnon: %llu kB", &kb) == 1) anon = kb;
            else if (std::sscanf(line, "RssFile: %llu kB", &kb) == 1) file = kb;
        }
        std::fclose(f);
        return any;
    }
    void start() {
        th = std::thread([this] {
            while (!stop.load(std::memory_order_relaxed)) {
                u64 a = 0, f = 0, t = 0;
                if (sample(a, f, t)) {
                    if (a > peak_anon.load()) peak_anon.store(a);
                    if (f > peak_file.load()) peak_file.store(f);
                    if (t > peak_total.load()) peak_total.store(t);
                    // THE ANON-MEMORY GUARD: our refusal fires BEFORE the
                    // OS OOM killer's - same doctrine as the disk guards.
                    // Numbers, not a journal line: the process dies with
                    // its peaks and the guard value.
                    if (g_mem_guard_mb && a / 1024 > g_mem_guard_mb) {
                        std::fprintf(stderr,
                            "MEM_GUARD: anon RSS %" PRIu64 " MiB exceeds the"
                            " guard %" PRIu64 " MiB (peaks: anon=%" PRIu64
                            " file=%" PRIu64 " total=%" PRIu64 " MiB) - a"
                            " plane grew past its pricing; refusing before"
                            " the OS OOM does damage\n",
                            a / 1024, g_mem_guard_mb, peak_anon.load() / 1024,
                            peak_file.load() / 1024, peak_total.load() / 1024);
                        ::_exit(2);
                    }
                }
                for (int i = 0; i < 5 && !stop.load(std::memory_order_relaxed); ++i)
                    ::usleep(100000);
            }
        });
    }
    void finish() {
        if (!th.joinable()) return;
        stop.store(true);
        th.join();
        u64 a = 0, f = 0, t = 0;
        if (sample(a, f, t)) {
            if (a > peak_anon.load()) peak_anon.store(a);
            if (f > peak_file.load()) peak_file.store(f);
            if (t > peak_total.load()) peak_total.store(t);
        }
    }
};

static void madv_release(const u8* base, u64 bytes) {
    if (!base || !bytes) return;
    const u64 page = 4096;
    if (bytes < page) return;
    ::madvise((void*)base, size_t(bytes / page * page), MADV_DONTNEED);
}

// Read text bytes through a WINDOW (carried over): buffered sequential pread
// into a reused buffer, remapped once in place.
static void read_window(int fd, u64 from, u64 want, std::vector<u8>& buf) {
    if (buf.size() < want) buf.resize(size_t(want));
    size_t got = 0;
    while (got < want) {
        ssize_t r = ::pread(fd, buf.data() + got, size_t(want - got),
                            off_t(from + got));
        if (r <= 0) die("pread text window");
        got += size_t(r);
    }
    if (REMAP) for (u64 i = 0; i < want; ++i) buf[i] = REMAP[buf[i]];
}

template <class V>
static void bswap_release(V& v) { V().swap(v); }

static std::atomic<u64> FD_PEAK{0};
static u64 liveFdCount() {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return 0;
    u64 n = 0;
    while (struct dirent* e = ::readdir(d))
        if (e->d_name[0] != '.') ++n;
    ::closedir(d);
    return n;
}
static u64 noteFdPeak() {
    const u64 n = liveFdCount();
    if (n > FD_PEAK.load(std::memory_order_relaxed)) FD_PEAK.store(n);
    return n;
}
static std::atomic<u64> PLAN_CORPUS_BYTES{0};
static std::atomic<u64> PLAN_CORPUS_CALLS{0};

// The process's own I/O receipt (P1's denominators).
static void read_proc_io(u64& rchar, u64& rb, u64& syscr) {
    rchar = rb = syscr = 0;
    if (FILE* f = std::fopen("/proc/self/io", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            unsigned long long v = 0;
            if (std::sscanf(line, "rchar: %llu", &v) == 1) rchar = v;
            else if (std::sscanf(line, "read_bytes: %llu", &v) == 1) rb = v;
            else if (std::sscanf(line, "syscr: %llu", &v) == 1) syscr = v;
        }
        std::fclose(f);
    }
}

// ---------------------------------------------------------------------------
// THE RUN DIGIT + the leaf-sort knobs (carried over).
// ---------------------------------------------------------------------------
static u64 g_wave_key_bytes = 8;        // K
static u64 g_wave_run_cap = 65536;      // L's cap
static u64 g_tie_window_bytes = 4096;   // the ONE big follow-up window (level 2)
// ---- LEVEL 3: the parse (the named fallback). A tie class whose big-window
// plane (rows x tie window) exceeds g_parse_plane_max is a DEEP REPEAT class
// (kilobytes+ of mutual match, announced by its own size): instead of paying
// the window plane, the tape extracts its continuations CUT AT MINIMIZER
// TRIGGERS (the PFP discipline, as carried in bit6/pfp_ds_vendor: a k-mer is a
// trigger iff it is strictly smaller than the w k-mers preceding it; phrases
// end with their trigger). Identical chunks hash to one dictionary entry, so
// the shared bytes are touched once and never re-read; a row's key is its
// PHRASE-ID SEQUENCE (integers), with the mid-phrase tail as bytes (the
// PFP-BWT mid-phrase rule). Phrase-level ties widen the budget (rounds,
// journaled); what survives the round cap goes to the LCE residual.
static u64 g_parse_plane_max = 256ull << 20;   // XSA_PARSE_PLANE_MAX
static u64 g_parse_dict_allowance = 1ull << 20;  // XSA_PARSE_DICT_BYTES (per request)
// THE SPIN GUARD: a silent spin is impossible by contract. Every phase
// bumps the heartbeat; the watchdog thread aborts LOUDLY (with the phase
// and counters) when it stops advancing for XSA_SPIN_GUARD_S seconds.
static std::atomic<u64> g_spin_beat{0};
static std::atomic<int> g_spin_where{0};   // 1 tape 2 plan 3 radix 4 parse 5 fallback 6 emit
static void beat(int where) {
    g_spin_where.store(where, std::memory_order_relaxed);
    g_spin_beat.fetch_add(1, std::memory_order_relaxed);
}
static u64 g_parse_phrases = 64;               // XSA_PARSE_PHRASES (round 1 budget)
static u64 g_parse_rounds = 4;                 // XSA_PARSE_ROUNDS (widening rounds)
static u64 g_pfp_k = 8;                        // XSA_PFP_K (trigger k-mer)
static u64 g_pfp_w = 8;                        // XSA_PFP_W (trigger window)
static std::atomic<u64> g_ac_reports{0};
static std::atomic<u64> PARSE_CLASSES{0}, PARSE_ROWS{0}, PARSE_PHRASES{0},
    PARSE_PREFIX_REL{0},
    PARSE_DICT_BYTES{0}, PARSE_ROUNDS_TOTAL{0};
static std::atomic<u64> RUNKEY_CAPPED{0}, RUNKEY_ROWS{0};
static std::atomic<u64> RUNKEY_FAM[3] = {{0}, {0}, {0}};
static constexpr u64 SS_WAVE_RUN_BYTES = 4 + 2;
static constexpr u64 SS_WAVE_SMALL_ROWS = 64;
static constexpr unsigned SS_WAVE_BUCKETS = 257;
static constexpr u64 SS_SCAN_CHUNK = 4u << 20;

static inline u64 unit_rec_bytes(u64 key_len) { return 8 + key_len + SS_WAVE_RUN_BYTES; }

// Writes L (run of sigma at [d, d+L)) and t (0 = END, else symbol+1, 257 =
// capped/unknown), out of bytes the caller already holds (carried over).
static inline void wavekey_run_digit(const u8* q, u64 avail, u64 cap, u32& L, u16& t) {
    const u8 sigma = q[0];
    u64 l = 1;
    const u64 lim = std::min<u64>(avail, cap);
    while (l < lim && q[l] == sigma) ++l;
    L = u32(l);
    if (l >= avail) t = 0;
    else if (l >= cap) t = 257;
    else t = u16(unsigned(q[l]) + 1u);
}

// THE TIE LEDGER (per unit; summed and journaled as WAVE_KEY / TIE_GROUPS /
// TIE_TOTALS - the level-3 decision gate).
struct WaveKeyStats {
    u64 rows = 0, key_bytes = 0, tie_rows = 0, rounds = 0, extra_passes = 0;
    u64 bytes_read = 0, syscalls = 0, fallback_rows = 0, no_shrink = 0;
    u64 tie_groups = 0, tie_window_bytes = 0;
    std::vector<u64> tie_group_sizes;
    double t_radix = 0, t_extract = 0, t_fallback = 0;
};
struct WaveKeyTotals {
    std::atomic<u64> rows{0}, key_bytes{0}, tie_rows{0}, rounds{0}, rounds_max{0},
        extra_passes{0}, bytes_read{0}, syscalls{0}, fallback_rows{0}, no_shrink{0},
        bands{0}, tie_groups{0}, tie_window_bytes{0}, tie_big_groups{0},
        tie_residual_groups{0}, tie_residual_rows{0};
    void add(const WaveKeyStats& s) {
        rows += s.rows; key_bytes += s.key_bytes; tie_rows += s.tie_rows;
        rounds += s.rounds; extra_passes += s.extra_passes; bytes_read += s.bytes_read;
        syscalls += s.syscalls; fallback_rows += s.fallback_rows; no_shrink += s.no_shrink;
        bands += 1;
        tie_groups += s.tie_groups;
        tie_window_bytes += s.tie_window_bytes;
        u64 cur = rounds_max.load();
        while (s.rounds > cur && !rounds_max.compare_exchange_weak(cur, s.rounds)) {}
    }
};
static WaveKeyTotals WAVE_KEY;

// One MSD level over [lo,hi) (carried over): STABLE counting sort on dig(i)
// into the ping-pong planes; the run digit's payload rides with the row.
static void wk_msd_level(const std::vector<u64>& cur, std::vector<u64>& alt,
                         const std::vector<u8>& curb, std::vector<u8>& altb,
                         const std::vector<u32>* curL, std::vector<u32>* altL,
                         const std::vector<u16>* curT, std::vector<u16>* altT,
                         u64 k, u64 lo, u64 hi,
                         const std::function<unsigned(u64)>& dig,
                         std::vector<u64>& starts) {
    starts.assign(SS_WAVE_BUCKETS + 1, 0);
    for (u64 i = lo; i < hi; ++i) ++starts[size_t(dig(i)) + 1];
    for (unsigned b = 0; b < SS_WAVE_BUCKETS; ++b) starts[b + 1] += starts[b];
    std::vector<u64> cursor(starts.begin(), starts.end() - 1);
    for (u64 i = lo; i < hi; ++i) {
        const unsigned b = dig(i);
        const u64 at = lo + cursor[b]++;
        alt[size_t(at)] = cur[size_t(i)];
        std::memcpy(&altb[size_t(at) * k], &curb[size_t(i) * k], size_t(k));
        if (curL) {
            (*altL)[size_t(at)] = (*curL)[size_t(i)];
            (*altT)[size_t(at)] = (*curT)[size_t(i)];
        }
    }
    for (unsigned b = 0; b <= SS_WAVE_BUCKETS; ++b) starts[b] += lo;
}

// ===========================================================================
// THE CHUNK: a request's sink. RAM planes; the tape is the only writer; the
// owning sorter is the only reader. Nothing is materialized to disc.
// ===========================================================================
struct Chunk {
    std::vector<u64> P;    // packed position|symbol (initial) / position (follow-up)
    std::vector<u8>  Kb;   // key_len bytes per row (padded past the text)
    std::vector<u32> Lp;   // run digit L   (initial requests only)
    std::vector<u16> Tp;   // run digit t   (initial requests only)
    // ---- parse requests (level 3): phrase-id streams + the class dictionary.
    // THE KEY IS (phrase id, intra-phrase offset) - NO per-row bytes: each
    // distinct phrase is held ONCE in the dictionary and every row references
    // it. ids: row-major [rows x phraseBudget]; ids[r*B+0] = the phrase
    // CONTAINING the row (a proper dict id, or PARTIAL_FLAG|pdict index for
    // the edge phrases); ids[r*B+i>0] = the following full phrases.
    // MARKERS: ~0u = TRUNCATED (the window or the budget ran out - unknown
    // beyond, compares equal-so-far so the tie machinery widens or defers to
    // the residual); 0xFFFFFFFEu = the TRUE text end (shorter-first, the
    // dollar rule). off0[r] = the row's offset within its containing phrase.
    bool parse = false;
    u64 phraseBudget = 0;
    std::vector<u32> ids;
    std::vector<u32> off0;
    std::vector<std::vector<u8>> dict;   // proper id -> bytes (ends with its trigger k-mer)
    std::vector<std::vector<u8>> pdict;  // partial id -> bytes (the text edges)
    u64 key_len = 0;
    u64 reserved_rows = 0;
    void reserve_rows(u64 rows, u64 klen) {
        key_len = klen;
        reserved_rows = rows;
        P.reserve(size_t(rows));
        Kb.reserve(size_t(rows) * size_t(klen));
        Lp.reserve(size_t(rows));
        Tp.reserve(size_t(rows));
    }
    u64 rows() const { return P.size(); }
    u64 plane_bytes() const {
        return P.size() * 8 + Kb.size() + Lp.size() * 4 + Tp.size() * 2;
    }
    u64 reserved_bytes() const {
        return reserved_rows * (8 + key_len + (Lp.empty() && Tp.empty() ? 0 : SS_WAVE_RUN_BYTES));
    }
};

// A follow-up request handle: enqueued first (all spans of a unit enqueue
// before any waits, so ONE tape pass serves them all), then waited.
struct FollowUp {
    std::vector<u8> prefix;   // the group's common prefix: pre + the key window
    u64 posLo = 0, posHi = ~0ull;   // point-mass slices only
    u64 key_len = 0;          // the big window's width
    u64 rowsExpected = 0;     // the group's row count (the chunk is exactly this)
    bool runFilter = false;   // (sigma, L, t) at filterBase: the group among the class
    u64 filterBase = 0;
    u8 runSigma = 0;
    u32 runL = 0;
    u16 runT = 0;
    bool parse = false;       // level 3: extract trigger-cut phrases, not a window
    u64 phraseBudget = 0;     // round r's phrase count
    u64 round = 0;            // 1-based widening round
    std::vector<u64> positions;   // the group's exact rows (positional serving)
    Chunk* chunk = nullptr;
    void* rq = nullptr;       // the TapeRequest the sorter issued (owned per unit)
    bool served = false;
};

// ===========================================================================
// THE LEAF SORT (carried over from the wavekey lane). Round 0 - the captured
// window + the run digit, zero corpus access - is VERBATIM. The tie ladder is
// the operator's amendment: a group that survives round 0 issues ONE
// follow-up request for a big context window (XSA_TIE_WINDOW_BYTES, default
// 4096); the doubling rounds 16/32/64 are in-memory radix levels over that
// one window. What survives the whole window goes to the counted-walk LCE
// fallback and is journaled as the level-3 decision gate (TIE_GROUPS).
// ===========================================================================
static void wavekey_sort_unit(std::vector<u64>& P, std::vector<u8>& Kb,
                              std::vector<u32>& Lp, std::vector<u16>& Tp,
                              u64 kbase, u64 unit_id, const u8* pre, u64 preLen,
                              u64 posLo, u64 posHi,
                              const std::function<void(FollowUp&)>& enqueue_followup,
                              const std::function<void(FollowUp&)>& wait_followup,
                              const std::function<void(FollowUp&)>& free_followup,
                              std::vector<u64>& lcpv, WaveKeyStats& st) {
    const u64 n = P.size();
    const u64 K = g_wave_key_bytes;
    std::vector<u64> origP;
    const bool wk_verify = ::getenv("XSA_WAVEKEY_VERIFY") != nullptr;
    if (wk_verify) origP = P;
    u64 perm_s = 0, perm_x = 0;
    if (wk_verify) for (const u64 pk : origP) { perm_s += pk; perm_x ^= pk; }
    auto check_perm = [&](const char* where, u64 a, u64 b) {
        if (!wk_verify) return;
        u64 s1 = 0, x1 = 0;
        for (const u64 pk : P) { s1 += pk; x1 ^= pk; }
        if (s1 != perm_s || x1 != perm_x)
            die(std::string("WAVEKEY_PERM_FAIL ") + where + " a=" + std::to_string(a) +
                " b=" + std::to_string(b) + " (rows lost or duplicated)");
    };
    lcpv.assign(size_t(n), 0);
    st.rows += n;
    st.key_bytes += n * K;
    if (n < 2) return;
    std::vector<u64> P2(n, 0);
    std::vector<u8> Kb2(n * size_t(K), 0);
    std::vector<u32> Lp2(n, 0);
    std::vector<u16> Tp2(n, 0);
    std::vector<u64> starts;
    std::vector<std::pair<u64, u64>> spans, nxt, small;
    std::vector<std::pair<std::pair<u64, u64>, u64>> residual;   // (span, proven depth)
    auto rem_of = [](u64 pk) { return FILE_N - unpack_pos(pk); };
    auto dig0 = [&](u64 lvl, u64 g, u64 i) -> unsigned {
        if (rem_of(P[size_t(i)]) <= g) return 0u;
        return unsigned(Kb[size_t(i) * size_t(K) + size_t(lvl)]) + 1u;
    };
    (void)dig0;

    // ---- ROUND 0: the captured window + the run digit. ZERO corpus access. ----
    const u64 t0 = now_sec();
    spans.assign(1, std::make_pair(u64(0), n));
    st.rounds = 1;
    auto fam_of = [&](u64 i) -> unsigned {
        const u16 t = Tp[size_t(i)];
        if (t == 257) return 1u;
        const unsigned sigma = Kb[size_t(i) * size_t(K)];
        if (t == 0) return 0u;
        return (unsigned(t) - 1u) < sigma ? 0u : 2u;
    };
    auto dig_order = [&](u64 lvl, u64 i) -> unsigned {
        const u64 rem = rem_of(P[size_t(i)]);
        if (rem <= kbase) return 0u;
        if (lvl == 0) return unsigned(Kb[size_t(i) * size_t(K)]) + 1u;
        const unsigned fam = fam_of(i);
        if (lvl == 1) return fam;
        if (lvl <= 5) {
            const unsigned shift = 8u * unsigned(5 - lvl);
            const unsigned b = (Lp[size_t(i)] >> shift) & 0xffu;
            return fam == 2u ? (255u - b) : b;
        }
        if (lvl == 6) return fam == 1u ? 0u : unsigned(Tp[size_t(i)]);
        const u64 j = lvl - 6;
        if (rem <= kbase + j) return 0u;
        return unsigned(Kb[size_t(i) * size_t(K) + size_t(j)]) + 1u;
    };
    const u64 DLEV = K + 6;
    std::vector<std::pair<u64, u64>> prev_parent(1, std::make_pair(u64(0), n));
    for (u64 lvl = 0; lvl < DLEV && !spans.empty(); ++lvl) {
        std::vector<std::pair<u64, u64>> big, parent(spans);
        for (const auto& sp : spans) {
            if (sp.second - sp.first > SS_WAVE_SMALL_ROWS) big.push_back(sp);
            else small.push_back(sp);
        }
        if (big.empty()) { spans.clear(); break; }
        for (const auto& sp : prev_parent) {
            const size_t len = size_t(sp.second - sp.first);
            std::memcpy(&P2[size_t(sp.first)], &P[size_t(sp.first)], len * sizeof(u64));
            std::memcpy(&Kb2[size_t(sp.first) * size_t(K)], &Kb[size_t(sp.first) * size_t(K)],
                        len * size_t(K));
            std::memcpy(&Lp2[size_t(sp.first)], &Lp[size_t(sp.first)], len * sizeof(u32));
            std::memcpy(&Tp2[size_t(sp.first)], &Tp[size_t(sp.first)], len * sizeof(u16));
        }
        nxt.clear();
        for (const auto& sp : big) {
            const u64 lvlc = lvl;
            wk_msd_level(P, P2, Kb, Kb2, &Lp, &Lp2, &Tp, &Tp2, K, sp.first, sp.second,
                         [&](u64 i) { return dig_order(lvlc, i); }, starts);
            for (unsigned b = 0; b < SS_WAVE_BUCKETS; ++b) {
                const u64 s = starts[b], e = starts[b + 1];
                if (e - s > 1) nxt.emplace_back(s, e);
            }
        }
        P.swap(P2); Kb.swap(Kb2); Lp.swap(Lp2); Tp.swap(Tp2);
        check_perm("round0-level", lvl, big.size());
        prev_parent = parent;
        spans.swap(nxt);
    }
    for (const auto& sp : spans) small.push_back(sp);
    spans.clear();
    st.t_radix += now_sec() - t0;

    // THE CURRENT WINDOW base, as the direct-comparison helpers see it (round 0:
    // kbase; the big window: woff). Carried over as a mutable because both modes
    // share diverge/less_at.
    u64 kbase_off = kbase;
    std::vector<u8>* wk_keys = nullptr;
    std::vector<u32>* wk_slot = nullptr;
    u64 wk_kk = K;
    auto byte_at = [&](u64 i, u64 j) -> u8 {
        if (!wk_keys) return Kb[size_t(i) * size_t(K) + size_t(j)];
        return (*wk_keys)[size_t((*wk_slot)[size_t(i)]) * size_t(wk_kk) + size_t(j)];
    };
    auto diverge = [&](u64 ia, u64 ib) -> u64 {
        const u64 la = rem_of(P[size_t(ia)]), lb = rem_of(P[size_t(ib)]);
        for (u64 j = 0; j < wk_kk; ++j) {
            const u64 g = kbase_off + j;
            const bool ea = la <= g, eb = lb <= g;
            if (ea || eb) {
                if (ea && eb) return la == lb ? wk_kk : j;
                return j;
            }
            if (byte_at(ia, j) != byte_at(ib, j)) return j;
        }
        return wk_kk;
    };
    auto less_at = [&](u64 ia, u64 ib) -> bool {
        const u64 la = rem_of(P[size_t(ia)]), lb = rem_of(P[size_t(ib)]);
        for (u64 j = 0; j < wk_kk; ++j) {
            const u64 g = kbase_off + j;
            const bool ea = la <= g, eb = lb <= g;
            if (ea || eb) return ea && eb ? la < lb : ea;
            const u8 xa = byte_at(ia, j), xb = byte_at(ib, j);
            if (xa != xb) return xa < xb;
        }
        return false;
    };
    auto order_less = [&](u64 ia, u64 ib) -> bool {
        const u64 ra = rem_of(P[size_t(ia)]), rb = rem_of(P[size_t(ib)]);
        if (ra <= kbase || rb <= kbase) return ra < rb;
        const size_t ka = size_t(ia) * size_t(K), kb = size_t(ib) * size_t(K);
        if (Kb[ka] != Kb[kb]) return Kb[ka] < Kb[kb];
        const unsigned fa = fam_of(ia), fb = fam_of(ib);
        if (fa != fb) return fa < fb;
        const u32 La = Lp[size_t(ia)], Lb = Lp[size_t(ib)];
        if (La != Lb) return fa == 2u ? (La > Lb) : (La < Lb);
        if (Tp[size_t(ia)] != Tp[size_t(ib)]) return Tp[size_t(ia)] < Tp[size_t(ib)];
        const u64 lo = std::min(ra, rb);
        for (u64 j = 1; j < K; ++j) {
            if (kbase + j >= lo) return ra < rb;
            if (Kb[ka + size_t(j)] != Kb[kb + size_t(j)]) return Kb[ka + size_t(j)] < Kb[kb + size_t(j)];
        }
        return false;
    };
    auto lcp_pair = [&](u64 ia, u64 ib) -> u64 {
        const u64 ra = rem_of(P[size_t(ia)]), rb = rem_of(P[size_t(ib)]);
        const u64 lo = std::min(ra, rb);
        if (lo <= kbase) return lo;
        const size_t ka = size_t(ia) * size_t(K), kb = size_t(ib) * size_t(K);
        if (Kb[ka] != Kb[kb]) return kbase;
        const u32 La = Lp[size_t(ia)], Lb = Lp[size_t(ib)];
        if (La != Lb) return kbase + std::min<u64>(La, Lb);
        if (Tp[size_t(ia)] != Tp[size_t(ib)]) return kbase + La;
        for (u64 j = La + 1; j < K; ++j) {
            if (kbase + j >= lo) return lo;
            if (Kb[ka + size_t(j)] != Kb[kb + size_t(j)]) return kbase + j;
        }
        return ~0ull;
    };
    auto settle_small = [&](std::vector<std::pair<u64, u64>>& todo, int mode,
                            std::vector<std::pair<u64, u64>>& carry) {
        std::vector<u64> idx, prow;
        std::vector<u8> kv;
        std::vector<u32> ks;
        for (const auto& sp : todo) {
            const u64 m = sp.second - sp.first;
            if (m < 2) continue;
            prow.assign(P.begin() + ptrdiff_t(sp.first), P.begin() + ptrdiff_t(sp.second));
            idx.resize(size_t(m));
            for (u64 i = 0; i < m; ++i) idx[size_t(i)] = i;
            if (mode == 0) {
                std::sort(idx.begin(), idx.end(), [&](u64 x, u64 y) {
                    return order_less(sp.first + x, sp.first + y);
                });
            } else {
                std::sort(idx.begin(), idx.end(), [&](u64 x, u64 y) {
                    return less_at(sp.first + x, sp.first + y);
                });
            }
            std::vector<u64> sorted(m, 0), slcp(m, 0);
            std::vector<u8> tie(m, 0);
            for (u64 i = 0; i < m; ++i) sorted[size_t(i)] = prow[size_t(idx[size_t(i)])];
            for (u64 i = 1; i < m; ++i) {
                const u64 ia = sp.first + idx[size_t(i - 1)], ib = sp.first + idx[size_t(i)];
                if (mode == 0) {
                    const u64 v = lcp_pair(ia, ib);
                    if (v == ~0ull) tie[size_t(i)] = 1;
                    else slcp[size_t(i)] = v;
                } else {
                    slcp[size_t(i)] = kbase_off + diverge(ia, ib);
                    if (slcp[size_t(i)] >= kbase_off + wk_kk) tie[size_t(i)] = 1;
                }
            }
            std::vector<u32> kl(m, 0);
            std::vector<u16> kt(m, 0);
            if (mode == 0) {
                kv.resize(size_t(m) * size_t(K));
                for (u64 i = 0; i < m; ++i) {
                    const u64 src = sp.first + idx[size_t(i)];
                    std::memcpy(&kv[size_t(i) * size_t(K)], &Kb[size_t(src) * size_t(K)], size_t(K));
                    kl[size_t(i)] = Lp[size_t(src)];
                    kt[size_t(i)] = Tp[size_t(src)];
                }
            } else {
                ks.resize(size_t(m));
                for (u64 i = 0; i < m; ++i)
                    ks[size_t(i)] = (*wk_slot)[size_t(sp.first + idx[size_t(i)])];
            }
            for (u64 i = 0; i < m; ++i) {
                P[size_t(sp.first + i)] = sorted[size_t(i)];
                if (mode == 0) {
                    std::memcpy(&Kb[size_t(sp.first + i) * size_t(K)],
                                &kv[size_t(i) * size_t(K)], size_t(K));
                    Lp[size_t(sp.first + i)] = kl[size_t(i)];
                    Tp[size_t(sp.first + i)] = kt[size_t(i)];
                } else {
                    (*wk_slot)[size_t(sp.first + i)] = ks[size_t(i)];
                }
                if (i) lcpv[size_t(sp.first + i)] = slcp[size_t(i)];
            }
            u64 i = 0;
            while (i < m) {
                u64 j = i + 1;
                while (j < m && tie[size_t(j)]) ++j;
                if (j - i > 1) carry.emplace_back(sp.first + i, sp.first + j);
                i = j;
            }
        }
    };
    std::vector<std::pair<u64, u64>> carry;
    if (!small.empty()) {
        settle_small(small, 0, carry);
        check_perm("round0-settle", 0, small.size());
        small.clear();
    }
    spans.swap(carry);
    carry.clear();
    for (u64 i = 1; i < n; ++i) {
        const u64 v = lcp_pair(i - 1, i);
        if (v != ~0ull) lcpv[size_t(i)] = v;
    }

    // ---- THE RUN-KEY RECEIPT (P3): rows in, rows out of the run digit. ----
    u64 round0_tie = 0;
    for (const auto& sp : spans) round0_tie += sp.second - sp.first;
    {
        u64 families[3] = {0, 0, 0};
        u64 lhist[4] = {0, 0, 0, 0};
        const u64 lv = g_wave_run_cap;
        for (u64 i = 0; i < n; ++i) {
            ++families[fam_of(i)];
            if (Lp[size_t(i)] <= 1) ++lhist[0];
            else if (Lp[size_t(i)] <= 8) ++lhist[1];
            else if (Lp[size_t(i)] < lv) ++lhist[2];
            else ++lhist[3];
        }
        RUNKEY_ROWS += n;
        RUNKEY_FAM[0] += families[0]; RUNKEY_FAM[1] += families[1]; RUNKEY_FAM[2] += families[2];
        RUNKEY_CAPPED += lhist[3];
        std::fprintf(stderr,
            "SORT_RUNKEY unit=%" PRIu64 " rows_in=%" PRIu64 " rows_out=%" PRIu64
            " tie_rows=%" PRIu64 " families=%" PRIu64 "/%" PRIu64 "/%" PRIu64
            " l_hist=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 " run_cap=%" PRIu64
            " rule=order=(sigma,family,L,t,key[1..K))\n",
            unit_id, n, n - round0_tie, round0_tie, families[0], families[1],
            families[2], lhist[0], lhist[1], lhist[2], lhist[3], lv);
    }

    // ---- THE TIE LADDER (the operator's level 2): ONE follow-up request per
    // surviving group, for a big context window; the radix levels below walk
    // that window in memory. One pass replaces the doubling ladder's six.
    const u64 woff = kbase + K;      // the big window's absolute offset
    const u64 kk = g_tie_window_bytes;
    u64 rows_tied = 0;
    for (const auto& sp : spans) rows_tied += sp.second - sp.first;
    st.tie_rows += rows_tied;
    st.tie_groups = spans.size();
    st.tie_window_bytes = rows_tied * kk;
    st.rounds = spans.empty() ? 1 : 2;
    std::vector<FollowUp> fus;
    if (!spans.empty()) {
        if (rows_tied * kk > (1ull << 40))
            die("TAPE_TIE_WINDOW_TOO_BIG: unit " + std::to_string(unit_id) + " tie rows " +
                std::to_string(rows_tied) + " x window " + std::to_string(kk) +
                " B exceeds 1 TiB of chunk plane - this is the nested-repeat shape the "
                "level-3 parse exists for; see TIE_GROUPS and raise XSA_TIE_WINDOW_BYTES "
                "only with the operator's decision");
        // enqueue every group's request FIRST: one pass serves them all.
        // THE GROUP IS POSITIONAL: its rows are the tied positions in this
        // unit's chunk (in memory). The tape serves them by direct position
        // lookup during its sweep - no prefix is matched against the corpus
        // for a follow-up (prefix matching is only for the initial radix
        // partition), so no matcher memory grows with repeat depth.
        for (const auto& sp : spans) {
            FollowUp fu;
            fu.prefix.assign(pre, pre + size_t(preLen));
            fu.prefix.insert(fu.prefix.end(),
                             Kb.begin() + ptrdiff_t(sp.first) * ptrdiff_t(K),
                             Kb.begin() + ptrdiff_t(sp.first + 1) * ptrdiff_t(K));
            fu.posLo = posLo; fu.posHi = posHi;
            fu.key_len = kk;
            fu.rowsExpected = sp.second - sp.first;
            fu.positions.reserve(size_t(sp.second - sp.first));
            for (u64 i = sp.first; i < sp.second; ++i)
                fu.positions.push_back(unpack_pos(P[size_t(i)]));
            (void)kbase;   // the window offset is fu.prefix.size() (pre + key)
            // THE DEEP-REPEAT ANNOUNCEMENT: a group whose big-window plane
            // (rows x tie window) exceeds the knob is a kilobytes-plus mutual
            // match class - the parse's trigger population. Everything else
            // takes the ordinary big window (level 2).
            if (fu.rowsExpected * (kk + 8) > g_parse_plane_max) {
                fu.parse = true;
                fu.phraseBudget = g_parse_phrases;
                fu.round = 1;
                fu.key_len = g_parse_phrases * 4 + 2048;   // the plane's per-row price
            }
            fus.push_back(std::move(fu));
        }
        for (auto& fu : fus) enqueue_followup(fu);
        st.extra_passes = 1;
    }
    // the group-size distribution, journaled per unit (the level-3 gate)
    if (!spans.empty()) {
        std::vector<u64> sizes;
        for (const auto& sp : spans) sizes.push_back(sp.second - sp.first);
        std::vector<u64> ss(sizes);
        std::sort(ss.begin(), ss.end());
        const auto pick = [&](double q) -> u64 {
            if (ss.empty()) return 0;
            const size_t at = size_t(double(ss.size() - 1) * q);
            return ss[at];
        };
        std::fprintf(stderr,
            "TIE_GROUPS unit=%" PRIu64 " groups=%zu rows_tied=%" PRIu64
            " window_bytes_total=%" PRIu64 " window=%" PRIu64
            " p50=%" PRIu64 " p99=%" PRIu64 " max=%" PRIu64 "\n",
            unit_id, ss.size(), rows_tied, rows_tied * kk, kk,
            pick(0.5), pick(0.99), ss.empty() ? 0 : ss.back());
        for (u64 s : sizes) st.tie_group_sizes.push_back(s);
    }

    // THE SLOT PLANE (hoisted once per unit): chunk-row indices. The radix
    // and the parse share it; 0xffffffff = no chunk row yet.
    std::vector<u32> slot(size_t(n), 0xffffffffu);
    // THE SCATTER SCRATCH (hoisted once per unit: churn of chunk-sized
    // vectors was the RSS leak the mallopt fix and this hoist removed)
    std::vector<unsigned> sDig;
    std::vector<u64> sCur, sP;
    std::vector<u32> sS;
    std::vector<u8> sSrc, sK2;
    // serve one group's follow-up: wait, verify the chunk is EXACTLY the
    // group (prefix + run filter are its definition), set the slot plane
    auto serveGroup = [&](FollowUp& fu, const std::pair<u64, u64>& sp) {
        wait_followup(fu);
        Chunk* ck = fu.chunk;
        if (!ck) die("TAPE_FOLLOWUP_FAIL: no chunk for the tie window of unit " +
                     std::to_string(unit_id));
        std::vector<std::pair<u64, u64>> items;
        items.reserve(size_t(sp.second - sp.first));
        for (u64 i = sp.first; i < sp.second; ++i)
            items.emplace_back(unpack_pos(P[size_t(i)]), i);
        std::sort(items.begin(), items.end());
        if (ck->rows() != items.size()) {
            std::string hx;
            for (u64 i = 0; i < fu.prefix.size() && i < 80; ++i) {
                char t[8]; std::snprintf(t, sizeof t, "%02x ", fu.prefix[size_t(i)]);
                hx += t;
            }
            std::string grp, got;
            for (size_t i = 0; i < items.size() && i < 8; ++i)
                grp += " " + std::to_string(items[i].first);
            for (size_t i = 0; i < ck->rows() && i < 24; ++i)
                got += " " + std::to_string(unpack_pos(ck->P[i]));
            std::fprintf(stderr,
                "TAPE_FOLLOWUP_DEBUG unit=%" PRIu64 " kbase=%" PRIu64 " K=%" PRIu64
                " L=%u t=%u prefix_len=%zu prefix=%s\n  group_pos[%zu]=%s\n  served[%" PRIu64 "]=%s\n",
                unit_id, kbase, K, Lp[size_t(sp.first)], Tp[size_t(sp.first)],
                fu.prefix.size(), hx.c_str(), items.size(), grp.c_str(), ck->rows(), got.c_str());
            die("TAPE_FOLLOWUP_COUNT: unit " + std::to_string(unit_id) + " group of " +
                std::to_string(items.size()) + " rows was served " +
                std::to_string(ck->rows()) + " - prefix routing is not exact");
        }
        for (size_t t = 0; t < items.size(); ++t) {
            if (unpack_pos(ck->P[t]) != items[t].first)
                die("TAPE_FOLLOWUP_ROW: unit " + std::to_string(unit_id) + " expected pos " +
                    std::to_string(items[t].first) + " got " +
                    std::to_string(unpack_pos(ck->P[t])));
            slot[size_t(items[t].second)] = u32(t);
        }
    };
    // one parse ROUND for one group: sort over the phrase-id streams (the
    // mid-phrase rule), journal, and return the surviving tie spans with
    // their proven depth. The WAVE DRIVER owns widening and the residual.
    struct ParseTies {
        std::vector<std::pair<u64, u64>> ties;
        u64 provMin = ~0ull;
    };
    auto parseSortOnce = [&](FollowUp& fu, const std::pair<u64, u64>& sp,
                             u64 rounds) -> ParseTies {
        Chunk* ck = fu.chunk;
        {
        ParseTies out;
            if (ck->off0.size() != ck->rows() ||
                ck->ids.size() != ck->rows() * fu.phraseBudget)
                die("PARSE_SHAPE: unit " + std::to_string(unit_id) + " rows=" +
                    std::to_string(ck->rows()) + " off0=" +
                    std::to_string(ck->off0.size()) + " ids=" +
                    std::to_string(ck->ids.size()) + " budget=" +
                    std::to_string(fu.phraseBudget));
            // ================= LEVEL 3: THE PARSE =================
            // The deep-repeat class arrived keyed (phrase id, intra-phrase
            // offset): no per-row bytes - each distinct phrase is held once
            // in the class dictionary, identical chunks hashed to one entry.
            const u64 sp0 = sp.first, sp1 = sp.second;
            {
                beat(4);
                Chunk* pc = fu.chunk;
                const u64 B = pc->phraseBudget;
                // ---- the dictionary's exact periods (the run interiors) ----
                // A phrase with exact period per compares equal-from any two
                // offsets congruent modulo per: run interiors - the level-3
                // population - then cost O(1) per comparison, not O(phrase).
                const u64 D = pc->dict.size();
                std::vector<u32> per(size_t(D), 0);
                for (u64 i = 0; i < D; ++i) {
                    const auto& ph = pc->dict[size_t(i)];
                    const u32 nn = u32(ph.size());
                    // failure function (O(len)): smallest exact period
                    std::vector<u32> pi(size_t(nn), 0);
                    for (u32 q2 = 1; q2 < nn; ++q2) {
                        u32 k2 = pi[q2 - 1];
                        while (k2 && ph[size_t(q2)] != ph[size_t(k2)]) k2 = pi[k2 - 1];
                        if (ph[size_t(q2)] == ph[size_t(k2)]) ++k2;
                        pi[size_t(q2)] = k2;
                    }
                    const u32 cand = nn ? nn - pi[size_t(nn - 1)] : 0;
                    if (cand && cand <= nn / 2) per[size_t(i)] = cand;
                }
                // ---- THE PREFIX-FREE GATE (asserted, not assumed): every
                // dictionary phrase ends with its trigger k-mer, and that
                // k-mer occurs at no other position in any dictionary
                // phrase. The trigger discipline is what makes phrase order
                // equal byte order; the comparator below ALSO byte-verifies
                // every resolving step (belt and braces), but a violation
                // means the trigger discipline is broken and DIES here.
                {
                    std::map<std::pair<u64, u64>, u64> nonEnd, endCnt;
                    auto khash2 = [&](const u8* q) {
                        u64 h1 = 1469598103934665603ull, h2 = 1469598103934665603ull;
                        for (u64 i = 0; i < g_pfp_k; ++i) {
                            h1 = (h1 ^ q[i]) * 1099511628211ull;
                            h2 = (h2 + q[i] * (i + 1)) * 1099511628211ull;
                        }
                        return std::make_pair(h1, h2);
                    };
                    u64 relThis = 0;
                    for (const auto& ph : pc->dict) {
                        // STRUCTURAL INVARIANT (the two-sided rule's spacing
                        // lemma): every proper phrase is > k long and ends
                        // with its own trigger k-mer. A violation here is an
                        // implementation bug, not data.
                        if (ph.size() < g_pfp_k)
                            die("PARSE_GATE: a proper phrase shorter than the trigger "
                                "k-mer - a structural invariant of the trigger rule "
                                "is broken; refusing");
                        for (size_t i = 0; i + g_pfp_k <= ph.size(); ++i) {
                            const auto h = khash2(ph.data() + i);
                            if (i + g_pfp_k == ph.size()) ++endCnt[h];
                            else ++nonEnd[h];
                        }
                    }
                    for (const auto& ph : pc->dict) {
                        const auto h = khash2(ph.data() + ph.size() - g_pfp_k);
                        if (nonEnd.count(h) && nonEnd.at(h) > 0) ++relThis;
                    }
                    // THE PREFIX-FREE HEALTH METRIC. The operator's original
                    // mandate asserted this as a hard gate on the theorem
                    // that the trigger discipline yields a prefix-free
                    // dictionary; the two-sided rule (needed for phrases
                    // >= k) does NOT guarantee it - a trigger k-mer's
                    // occurrence inside another phrase need not be a
                    // trigger there (its forward window bytes differ), and
                    // 'mixed' constructs exactly that. Order is exact by
                    // CONSTRUCTION instead (see the comparator): the count
                    // is journaled so a corpus where it explodes is
                    // visible, and the SA ORACLE is the enforcement.
                    PARSE_PREFIX_REL.fetch_add(relThis);
                }
                // ---- the stream comparator (the mid-phrase rule) ----
                // Walks both rows' byte streams through the dictionary: bytes
                // of the current phrase from the intra-phrase offset, then
                // the following phrases. TRUNCATED (either side) = unknown
                // beyond: EQUAL-SO-FAR (0) - never a fabricated order; the
                // tie machinery widens or defers to the byte-exact residual.
                // The TRUE end is the dollar rule (shorter first). The exact
                // periods make run interiors O(1) per step.
                //
                // WHY ORDER == BYTE ORDER HERE, WHATEVER THE DICTIONARY
                // HOLDS: every resolving step compares dictionary BYTES
                // (never phrase ids or ranks as order); equal-id skips are
                // equality-only (identical bytes, by intern); the
                // period-congruent skip is byte-exactness of a periodic
                // phrase; and when one phrase is a byte-prefix of another
                // (a dictionary prefix-relation - possible with the
                // two-sided trigger rule, journaled above), the walk simply
                // CONTINUES into the shorter stream's next phrase against
                // the longer one's remainder. So phrase order equals byte
                // order by construction, not by a prefix-free theorem; the
                // SA ORACLE is the belt, this construction the braces.
                const std::vector<u32>& ids = pc->ids;
                const std::vector<u32>& off0 = pc->off0;
                // returns -1/0/+1 and sets lcp (bytes matched) on resolution
                auto streamCmp = [&](u32 a, u32 b, u64& lcp) -> int {
                    u32 ia = 0, ib = 0;            // phrase indices
                    u32 pa = off0[size_t(a)], pb = off0[size_t(b)];
                    u64 consumed = 0;
                    auto phraseOf = [&](u32 id) -> const std::vector<u8>& {
                        return (id & 0x80000000u)
                                   ? pc->pdict[size_t(id & 0x7fffffffu)]
                                   : pc->dict[size_t(id)];
                    };
                    // THE STEP COUNTER: every iteration advances ia or ib
                    // (or returns), so a legal walk is bounded by ~2B steps.
                    // Past that is the walk-past-end failure mode (an id
                    // budget with no terminator): die with the numbers.
                    u64 steps = 0;
                    const u64 stepCap = 4 * B + 16;
                    for (;;) {
                        if (++steps > stepCap)
                            die("PARSE_WALK: a stream comparison exceeded " +
                                std::to_string(stepCap) + " steps (rows " +
                                std::to_string(a) + "/" + std::to_string(b) +
                                ", ia=" + std::to_string(ia) + ", ib=" +
                                std::to_string(ib) + ", pa=" + std::to_string(pa) +
                                ", pb=" + std::to_string(pb) + ", consumed=" +
                                std::to_string(consumed) + ") - the id walk ran "
                                "past its budget; refusing");
                        // the id budget exhausted with real phrases still
                        // remaining: UNKNOWN beyond (equal-so-far), never a
                        // fabricated order and NEVER a read past the row's
                        // id list - the widen/residual machinery owns it.
                        if (ia >= B || ib >= B) { lcp = consumed; return 0; }
                        const u32 ida = ids[size_t(u64(a) * B + ia)];
                        const u32 idb = ids[size_t(u64(b) * B + ib)];
                        if (ida == ~0u || idb == ~0u) { lcp = consumed; return 0; }
                        if (ida == 0xFFFFFFFEu && idb == 0xFFFFFFFEu) {
                            lcp = consumed; return 0;
                        }
                        if (ida == 0xFFFFFFFEu) { lcp = consumed; return -1; }
                        if (idb == 0xFFFFFFFEu) { lcp = consumed; return 1; }
                        const auto& da = phraseOf(ida);
                        const auto& db = phraseOf(idb);
                        const u32 la = u32(da.size()) - pa;
                        const u32 lb = u32(db.size()) - pb;
                        const u32 nn = std::min(la, lb);
                        if (ida == idb) {
                            if (pa == pb) {
                                // identical remainder: skip the whole phrase
                                consumed += la;
                                if (la < lb) { ++ia; pa = 0; pb += nn; }
                                else if (lb < la) { ++ib; pb = 0; pa += nn; }
                                else { ++ia; ++ib; pa = pb = 0; }
                                continue;
                            }
                            const u32 pp = per[size_t(ida)];
                            if (pp && ((pa > pb ? pa - pb : pb - pa) % pp) == 0) {
                                // run interior: congruent suffixes of a
                                // periodic phrase - equal for nn bytes
                                consumed += nn;
                            } else {
                                const u8* xa = da.data() + pa;
                                const u8* xb2 = db.data() + pb;
                                for (u32 d = 0; d < nn; ++d)
                                    if (xa[d] != xb2[d]) {
                                        lcp = consumed + d;
                                        return xa[d] < xb2[d] ? -1 : 1;
                                    }
                                consumed += nn;
                            }
                        } else {
                            const u8* xa = da.data() + pa;
                            const u8* xb2 = db.data() + pb;
                            for (u32 d = 0; d < nn; ++d)
                                if (xa[d] != xb2[d]) {
                                    lcp = consumed + d;
                                    return xa[d] < xb2[d] ? -1 : 1;
                                }
                            consumed += nn;
                        }
                        if (la < lb) { ++ia; pa = 0; pb += nn; }
                        else if (lb < la) { ++ib; pb = 0; pa += nn; }
                        else { ++ia; ++ib; pa = pb = 0; }
                    }
                };
                // ---- sort the span over the streams ----
                {
                    std::vector<u64> idx(size_t(sp1 - sp0));
                    for (u64 i = 0; i < sp1 - sp0; ++i) idx[size_t(i)] = sp0 + i;
                    std::sort(idx.begin(), idx.end(), [&](u64 x, u64 y) {
                        u64 lcp = 0;
                        if (slot[size_t(x)] == 0xffffffffu || slot[size_t(y)] == 0xffffffffu)
                            die("PARSE_SLOT: unit " + std::to_string(unit_id) +
                                " row " + std::to_string(x) + "/" + std::to_string(y) +
                                " has no chunk row");
                        return streamCmp(slot[size_t(x)], slot[size_t(y)], lcp) < 0;
                    });
                    std::vector<u64> sortedP(size_t(sp1 - sp0));
                    std::vector<u32> sortedSlot(size_t(sp1 - sp0));
                    for (u64 i = 0; i < sp1 - sp0; ++i) {
                        sortedP[size_t(i)] = P[size_t(idx[size_t(i)])];
                        sortedSlot[size_t(i)] = slot[size_t(idx[size_t(i)])];
                    }
                    for (u64 i = 0; i < sp1 - sp0; ++i) {
                        P[size_t(sp0 + i)] = sortedP[size_t(i)];
                        slot[size_t(sp0 + i)] = sortedSlot[size_t(i)];
                    }
                    check_perm("parse-sort", rounds, 1);
                    // adjacent LCPs (the column falls out of the sort)
                    for (u64 i = 1; i < sp1 - sp0; ++i) {
                        u64 lcp = 0;
                        const int c = streamCmp(sortedSlot[size_t(i - 1)],
                                                 sortedSlot[size_t(i)], lcp);
                        if (c != 0) lcpv[size_t(sp0 + i)] = lcp;
                        else lcpv[size_t(sp0 + i)] = lcp;   // the tie boundary's proven depth
                    }
                    if (sp0 > 0) {
                        // the span's first row vs its round-0 predecessor
                        const u64 v = lcp_pair(sp0 - 1, sp0);
                        if (v != ~0ull) lcpv[size_t(sp0)] = v;
                    }
                }
                // ---- tie spans: widen (rounds) or fall to the LCE residual ----
                std::vector<std::pair<u64, u64>> ties;
                {
                    u64 i = sp0;
                    while (i < sp1) {
                        u64 j = i + 1;
                        while (j < sp1) {
                            u64 lcp = 0;
                            const int c = streamCmp(slot[size_t(i)], slot[size_t(j)], lcp);
                            if (c != 0) break;
                            ++j;
                        }
                        if (j - i > 1) ties.emplace_back(i, j);
                        i = j;
                    }
                }
                // the proven depth of a truncated row (for the residual)
                auto phraseLenOf = [&](u32 id) -> u64 {
                    return ((id & 0x80000000u) ? pc->pdict[size_t(id & 0x7fffffffu)]
                                               : pc->dict[size_t(id)]).size();
                };
                auto provOf = [&](u32 row) -> u64 {
                    u64 prov = 0;
                    {
                        const u32 id = ids[size_t(u64(row) * B)];
                        if (id == ~0u) return 0;   // nothing proven: a TRUNC-only row
                        prov += phraseLenOf(id) -
                                std::min<u64>(phraseLenOf(id), off0[size_t(row)]);
                    }
                    for (u64 q2 = 1; q2 < B; ++q2) {
                        const u32 id = ids[size_t(u64(row) * B + q2)];
                        if (id == ~0u || id == 0xFFFFFFFEu) break;
                        prov += phraseLenOf(id);
                    }
                    return prov;
                };
                PARSE_CLASSES.fetch_add(1);
                PARSE_ROWS.fetch_add(sp1 - sp0);
                PARSE_PHRASES.fetch_add(B * (sp1 - sp0));
                PARSE_DICT_BYTES.fetch_add([&] {
                    u64 b = 0;
                    for (const auto& ph : pc->dict) b += ph.size();
                    for (const auto& ph : pc->pdict) b += ph.size();
                    return b;
                }());
                PARSE_ROUNDS_TOTAL.fetch_add(1);
                std::fprintf(stderr,
                    "PARSE_CLASS unit=%" PRIu64 " rows=%" PRIu64 " phrases=%" PRIu64
                    " dict_entries=%" PRIu64 " partials=%" PRIu64 " dict_bytes=%" PRIu64
                    " rounds=%" PRIu64 " tie_spans=%zu"
                    " prefix_relations=%" PRIu64 " budget=%" PRIu64 "\n",
                    unit_id, sp1 - sp0, B * (sp1 - sp0), pc->dict.size(),
                    pc->pdict.size(), PARSE_DICT_BYTES.load(), rounds, ties.size(),
                    PARSE_PREFIX_REL.load(), B);
                out.ties = ties;
                for (const auto& t : ties)
                    for (u64 i = t.first; i < t.second; ++i)
                        out.provMin = std::min(out.provMin, provOf(slot[size_t(i)]));
                if (!ties.empty() && rounds >= g_parse_rounds) {
                    // DIAGNOSTIC at the round cap: the first tied pair's keying
                    const u32 ra = slot[size_t(ties[0].first)];
                    const u32 rb = slot[size_t(ties[0].first + 1)];
                    std::string da, db;
                    for (u64 q2 = 0; q2 < 8 && q2 < B; ++q2) {
                        char t[16];
                        std::snprintf(t, sizeof t, "%08x ", ids[size_t(u64(ra) * B + q2)]);
                        da += t;
                        std::snprintf(t, sizeof t, "%08x ", ids[size_t(u64(rb) * B + q2)]);
                        db += t;
                    }
                    std::fprintf(stderr,
                        "PARSE_TIE_DEBUG unit=%" PRIu64 " rows=%u/%u off0=%u/%u"
                        " idsA=%s idsB=%s provA=%" PRIu64 " provB=%" PRIu64 "\n",
                        unit_id, ra, rb, off0[size_t(ra)], off0[size_t(rb)],
                        da.c_str(), db.c_str(), provOf(ra), provOf(rb));
                }
                return out;   // the WAVE DRIVER owns widen/residual
        }
        }
        die("PARSE_SORT: called for a non-parse group");
    };
    // ---- THE PARSE WAVE DRIVER: one tape pass per widening round. All of a
    // round's requests enqueue BEFORE any wait (the ladder's parse groups and
    // the radix's reroutes batch into the same pass; widened requests batch
    // per round), so a pass amortizes the whole wave - one request at a time
    // was a pass per group and the storm the adversarial corpus exposed.
    struct ParseJob {
        std::pair<u64, u64> span;
        FollowUp fu;
        ParseJob(std::pair<u64, u64> s, FollowUp f) : span(s), fu(std::move(f)) {}
    };
    auto runParseWave = [&](std::vector<ParseJob>& wave0) {
        std::vector<ParseJob> wave = std::move(wave0);
        u64 rounds = 0;
        while (!wave.empty()) {
            ++rounds;
            beat(4);
            std::vector<ParseJob> next;
            for (auto& job : wave) {
                serveGroup(job.fu, job.span);
                const ParseTies pt = parseSortOnce(job.fu, job.span, rounds);
                const u64 B = job.fu.phraseBudget;
                if (pt.ties.empty()) { free_followup(job.fu); continue; }
                const u64 widen = std::min<u64>(B * 8, g_parse_phrases * 512);
                if (rounds >= g_parse_rounds || widen <= B) {
                    // the round cap or a non-widenable budget: the byte-exact
                    // LCE residual, from the round's PROVEN depth
                    for (const auto& t : pt.ties) {
                        residual.emplace_back(t, pt.provMin);
                        WAVE_KEY.tie_residual_groups.fetch_add(1);
                        WAVE_KEY.tie_residual_rows.fetch_add(t.second - t.first);
                    }
                    free_followup(job.fu);
                    continue;
                }
                // WIDEN x 8, DEFERRED: enqueue now, wait in the next round's
                // pass (all of the round's widenings share it)
                FollowUp fu2;
                fu2.prefix = job.fu.prefix;      // only its size is used (kbase)
                fu2.posLo = posLo; fu2.posHi = posHi;
                fu2.key_len = job.fu.key_len;
                fu2.rowsExpected = job.fu.rowsExpected;
                fu2.positions = job.fu.positions;   // the group's exact rows
                fu2.parse = true;
                fu2.phraseBudget = widen;
                fu2.round = rounds + 1;
                enqueue_followup(fu2);
                next.emplace_back(job.span, std::move(fu2));
                free_followup(job.fu);
            }
            wave = std::move(next);
        }
    };
    std::vector<std::pair<u64, u64>> deep;   // radix spans that tied over the window
    // whole-window survivors of a group's radix: the parse's population
    std::vector<ParseJob> parseJobs;   // the parse wave (enqueued, un-waited)
    for (size_t gi = 0; gi < spans.size(); ++gi) {
        const auto sp = spans[gi];
        FollowUp& fu = fus[gi];
        if (fu.parse) {
            // collect: the ladder already ENQUEUED every parse group, and
            // the radix's reroutes join them below - one pass serves all
            parseJobs.emplace_back(sp, std::move(fus[gi]));
            continue;
        }
        serveGroup(fu, sp);
        Chunk* ck = fu.chunk;
        {
        const u64 woffG = fu.prefix.size();   // the window starts past the known prefix
        wk_keys = &ck->Kb; wk_slot = &slot; wk_kk = kk; kbase_off = woffG;
        // ---- THE WINDOW RADIX, SPAN-LOCAL AND DIVERGENCE-JUMPING. One scatter
        // per ACTUAL split: a level whose byte every row of the span shares is
        // skipped by a direct scan (adjacent-slot digit comparison), which
        // finds the next level that can split. A byte-at-a-time walk over the
        // whole 4096-byte window re-copied the span per level and was the
        // wall on the adversarial fixtures; this does S x (jump) byte
        // compares and one scatter per split.
        const auto digkAt = [&](u64 i, u64 lvl) -> unsigned {
            if (rem_of(P[size_t(i)]) <= woffG + lvl) return 0u;
            return unsigned((*wk_keys)[size_t((*wk_slot)[size_t(i)]) * size_t(kk) +
                                         size_t(lvl)]) + 1u;
        };
        std::vector<std::pair<std::pair<u64, u64>, u64>> work;   // (span, level)
        work.emplace_back(sp, 0);
        while (!work.empty()) {
            beat(3);
            const auto s2l = work.back();
            work.pop_back();
            const u64 lo = s2l.first.first, hi = s2l.first.second;
            u64 lvl = s2l.second;
            if (hi - lo <= 1) continue;
            if (hi - lo <= SS_WAVE_SMALL_ROWS) { small.push_back(s2l.first); continue; }
            // find the next level that can split this span
            u64 splitLvl = ~0ull;
            for (u64 L = lvl; L < kk; ++L) {
                const unsigned d0 = digkAt(lo, L);
                bool same = true;
                for (u64 i = lo + 1; i < hi; ++i) {
                    if (digkAt(i, L) != d0) { same = false; break; }
                }
                if (!same) { splitLvl = L; break; }
            }
            if (splitLvl == ~0ull) {
                // every row is equal over the whole window: the rows
                // SURVIVED level 2 - the parse's population. (Rows whose
                // window crosses the text end are partitioned at the
                // collection point below - a prefix cannot express the
                // sentinel padding they compared as equal.)
                deep.emplace_back(s2l.first);
                continue;
            }
            // ONE STABLE SCATTER at the splitting level (span-local). The
            // round-0 key plane travels with the row (payload, as before).
            const u64 m = hi - lo;
            sDig.resize(size_t(m));
            starts.assign(SS_WAVE_BUCKETS + 1, 0);
            for (u64 i = 0; i < m; ++i) {
                sDig[size_t(i)] = digkAt(lo + i, splitLvl);
                ++starts[size_t(sDig[size_t(i)]) + 1];
            }
            for (unsigned b = 0; b < SS_WAVE_BUCKETS; ++b) starts[b + 1] += starts[b];
            sCur.assign(starts.begin(), starts.end() - 1);
            if (K) {
                sSrc.resize(size_t(m) * size_t(K));
                std::memcpy(sSrc.data(), &Kb[size_t(lo) * size_t(K)], size_t(m) * size_t(K));
            }
            sP.resize(size_t(m));
            sS.resize(size_t(m));
            sK2.resize(size_t(m) * size_t(K));
            for (u64 i = 0; i < m; ++i) {
                const u64 at = sCur[size_t(sDig[size_t(i)])]++;
                sP[size_t(at)] = P[size_t(lo + i)];
                sS[size_t(at)] = (*wk_slot)[size_t(lo + i)];
                if (K) std::memcpy(&sK2[size_t(at) * size_t(K)],
                                   &sSrc[size_t(i) * size_t(K)], size_t(K));
            }
            for (u64 i = 0; i < m; ++i) {
                P[size_t(lo + i)] = sP[size_t(i)];
                if (K) std::memcpy(&Kb[size_t(lo + i) * size_t(K)],
                                   &sK2[size_t(i) * size_t(K)], size_t(K));
                (*wk_slot)[size_t(lo + i)] = sS[size_t(i)];
            }
            check_perm("window-scatter", splitLvl, 1);
            for (unsigned b = 0; b < SS_WAVE_BUCKETS; ++b) {
                const u64 s = starts[b], e = starts[b + 1];
                if (e > s + 1) work.emplace_back(std::make_pair(lo + s, lo + e),
                                                  splitLvl + 1);
                if (s > 0 && e > s) lcpv[size_t(lo + s)] = woffG + splitLvl;
            }
        }
        if (!small.empty()) { settle_small(small, 1, carry); small.clear(); }
        // what still ties over the whole window SURVIVED level 2: the parse
        for (const auto& s2 : carry) deep.emplace_back(s2);
        carry.clear();
        std::fprintf(stderr,
            "SORT_ROUND unit=%" PRIu64 " round=1 off=%" PRIu64 " k=%" PRIu64
            " rows_in=%" PRIu64 " rows_tied=%" PRIu64
            " rule=one_big_window_replaces_the_doubling_passes\n",
            unit_id, woffG, kk, sp.second - sp.first,
            [&] { u64 r = 0; for (const auto& s2 : residual) if (s2.first.first >= sp.first && s2.first.second <= sp.second) r += s2.first.second - s2.first.first; return r; }());
        // THE LEVEL-3 REROUTE: whole-window survivors announced deep
        // repeats; the GROUP reroutes through the parse - THE PARSE IS THE
        // SLICE for a surviving class: the tape materializes the group's
        // continuation as (phrase id, intra-phrase offset, following phrase
        // sequence), so the deep shared bytes exist once in the class
        // dictionary and the ordering work happens in phrase space. The
        // request is POSITIONAL (the group's rows are known here); no
        // prefix is matched against the corpus.
        if (!deep.empty()) {
            FollowUp fu2;
            fu2.prefix = fu.prefix;          // only its size is used (kbase)
            fu2.posLo = posLo; fu2.posHi = posHi;
            fu2.rowsExpected = fu.rowsExpected;
            fu2.positions = fu.positions;   // the group's exact rows
            fu2.parse = true;
            fu2.phraseBudget = g_parse_phrases;
            fu2.round = 1;
            fu2.key_len = g_parse_phrases * 4 + 2048;
            enqueue_followup(fu2);
            parseJobs.emplace_back(sp, std::move(fu2));
        }
        deep.clear();
        }
        free_followup(fu);          // the window's plane is done: return it now
    }
    // THE PARSE WAVE: sort every parse job (the ladder's groups + the radix's
    // reroutes), widening round by round - one tape pass per round.
    if (!parseJobs.empty()) runParseWave(parseJobs);
    // (the unit's slot plane lives until the leaf returns; it is stack-scoped)

    // ---- THE FALLBACK: the big window's leftovers. Walks from the PROVED
    // depth, under the counted WALK BUDGET (carried over). ----
    if (!residual.empty()) {
        const double tf0 = now_sec();
        u64 walked = 0;
        u64 nextReport = g_wave_fallback_walk_max / 8 + 1;
        for (const auto& entry : residual) {
            beat(5);
            const auto sp = entry.first;
            const u64 walk_from = entry.second;   // this span's proven depth
            st.fallback_rows += sp.second - sp.first;
            if (sp.second - sp.first > 1 && walked > nextReport) {
                nextReport += g_wave_fallback_walk_max / 8 + 1;
                std::fprintf(stderr,
                    "WAVEKEY_FALLBACK_PROGRESS unit=%" PRIu64 " rows_so_far=%" PRIu64
                    " walked_bytes=%" PRIu64 " span=[%" PRIu64 ",%" PRIu64 ")"
                    " walk_from=%" PRIu64 " budget=%" PRIu64 "\n",
                    unit_id, st.fallback_rows, walked, sp.first, sp.second, walk_from,
                    g_wave_fallback_walk_max);
            }
            std::sort(P.begin() + ptrdiff_t(sp.first), P.begin() + ptrdiff_t(sp.second),
                      [walk_from, &walked](u64 a, u64 b) {
                          int c = suf_cmp_from_counted(unpack_pos(a), unpack_pos(b), walk_from, walked);
                          if (walked > g_wave_fallback_walk_max)
                              die("WAVEKEY_FALLBACK_BUDGET: a unit's LCE fallback compared " +
                                  std::to_string(walked) + " corpus bytes (budget " +
                                  std::to_string(g_wave_fallback_walk_max) +
                                  ") - the tie set the big window left is periodic (nested "
                                  "repeats): this is the LEVEL-3 shape. Report TIE_GROUPS / "
                                  "TIE_TOTALS and get the operator's decision before the parse "
                                  "is built");
                          return c < 0;
                      });
            for (u64 i = sp.first + 1; i < sp.second; ++i) {
                u64 lcp = 0;
                suf_cmp_lcp(unpack_pos(P[size_t(i - 1)]), unpack_pos(P[size_t(i)]), walk_from, lcp);
                lcpv[size_t(i)] = lcp;
            }
            if (sp.first) {
                u64 lcp = 0;
                suf_cmp_lcp(unpack_pos(P[size_t(sp.first - 1)]), unpack_pos(P[size_t(sp.first)]),
                            std::min<u64>(lcpv[size_t(sp.first)], walk_from), lcp);
                lcpv[size_t(sp.first)] = lcp;
            }
        }
        st.t_fallback += now_sec() - tf0;
    }

    // ---- THE ORACLE GUARD (XSA_WAVEKEY_VERIFY=1): brute force, per unit. ----
    if (wk_verify) {
        std::vector<u64> ref = P;
        std::sort(ref.begin(), ref.end(), [](u64 a, u64 b) {
            return suf_cmp_from(unpack_pos(a), unpack_pos(b), 0) < 0;
        });
        for (u64 i = 0; i < n; ++i) {
            if (ref[size_t(i)] != P[size_t(i)]) {
                const u64 a = unpack_pos(P[size_t(i)]), b = unpack_pos(ref[size_t(i)]);
                u64 lcp = 0;
                suf_cmp_lcp(a, b, 0, lcp);
                std::fprintf(stderr,
                    "WAVEKEY_VERIFY_FAIL unit=%" PRIu64 " row=%" PRIu64 " kbase=%" PRIu64
                    " k=%" PRIu64 " got_pos=%" PRIu64 " want_pos=%" PRIu64 " lcp=%" PRIu64
                    " got_suffix=%.40s want_suffix=%.40s\n",
                    unit_id, i, kbase, K, a, b, lcp,
                    (const char*)(T + a), (const char*)(T + b));
                die("WAVEKEY_VERIFY_FAIL: the unit order disagrees with the brute-force suffix order");
            }
        }
        for (u64 i = 1; i < n; ++i) {
            u64 lcp = 0;
            suf_cmp_lcp(unpack_pos(P[size_t(i - 1)]), unpack_pos(P[size_t(i)]), 0, lcp);
            if (lcpv[size_t(i)] != lcp) {
                std::fprintf(stderr,
                    "WAVEKEY_VERIFY_LCP_FAIL unit=%" PRIu64 " row=%" PRIu64
                    " got=%" PRIu64 " want=%" PRIu64 " pos=%" PRIu64 " prev=%" PRIu64 "\n",
                    unit_id, i, lcpv[size_t(i)], lcp,
                    unpack_pos(P[size_t(i)]), unpack_pos(P[size_t(i - 1)]));
                die("WAVEKEY_VERIFY_LCP_FAIL: the LCP plane disagrees with a direct walk");
            }
        }
        std::fprintf(stderr, "WAVEKEY_VERIFY_OK unit=%" PRIu64 " rows=%" PRIu64
                     " kbase=%" PRIu64 " k=%" PRIu64 "\n", unit_id, n, kbase, K);
    }
}

// ===========================================================================
// THE SURVEY (carried over verbatim: subdiv_survey.h - one pass, exact chunk
// counts, prefix cache, run column, page column).
// ===========================================================================
#include "subdiv_survey.h"

// THE CLASS LABEL the journals carry (carried over from subdiv_descent.h).
static std::string prefix_label(const u8* pre, u64 n) {
    std::string s;
    for (u64 i = 0; i < n && i < 24; ++i) {
        const unsigned b = pre[i];
        if (b == 32) s += "\\x20";
        else if (b > 32 && b < 127) s += char(b);
        else { char t[8]; std::snprintf(t, sizeof t, "\\x%02x", b); s += t; }
    }
    return s.empty() ? std::string("<root>") : s;
}

// ===========================================================================
// THE BOUND: `outstanding_chunk_bytes <= envelope - working_reserve`, as one
// accountant with per-unit receipts. A violation DIES with the per-request
// accounting (the discipline P1 demands: report the accounting, never tune).
// ===========================================================================
struct Unit;
struct Bound {
    u64 cap = 0;
    std::mutex mu;
    std::condition_variable cv;
    u64 live = 0, peak = 0;
    // per-unit receipts for the die message
    struct Receipt { u64 unit; const char* phase; u64 rows; u64 bytes; };
    std::vector<Receipt> holding;
    void add(u64 bytes, u64 unit, const char* phase, u64 rows) {
        std::unique_lock<std::mutex> lk(mu);
        if (live + bytes > cap) {
            std::string acc;
            for (const auto& r : holding)
                acc += " unit=" + std::to_string(r.unit) + " phase=" + r.phase +
                       " rows=" + std::to_string(r.rows) + " bytes=" + std::to_string(r.bytes);
            std::fprintf(stderr,
                "TAPE_BOUND_VIOLATION live=%" PRIu64 " want=%" PRIu64 " cap=%" PRIu64
                " unit=%" PRIu64 " phase=%s rows=%" PRIu64 "%s\n",
                live, bytes, cap, unit, phase, rows, acc.c_str());
            die("TAPE_BOUND: outstanding chunk bytes exceed the envelope - this is the "
                "per-request accounting; the plan's estimate for the named unit is wrong "
                "or the envelope is a knob error");
        }
        live += bytes;
        if (live > peak) peak = live;
        if (bytes) holding.push_back({unit, phase, rows, bytes});
    }
    void sub(u64 bytes) {
        {
            std::lock_guard<std::mutex> lk(mu);
            live = live >= bytes ? live - bytes : 0;
            for (size_t i = holding.size(); i-- > 0;) {
                if (holding[i].bytes >= bytes && holding[i].bytes == bytes) {
                    holding.erase(holding.begin() + ptrdiff_t(i));
                    if (!bytes) break;
                }
            }
        }
        cv.notify_all();
    }
    void wait_for(u64 bytes, u64 unit, const char* phase, u64 rows) {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return live + bytes <= cap; });
        live += bytes;
        if (live > peak) peak = live;
        if (bytes) holding.push_back({unit, phase, rows, bytes});
        (void)unit; (void)phase; (void)rows;
    }
};

// ===========================================================================
// THE TAPE: one sequential reader, a request queue, one matcher per pass.
// ===========================================================================
struct TapeRequest {
    const u8* pre = nullptr;     // prefix bytes (symbol space) ...
    u32 preLen = 0;              // ... of length preLen when bHi == 0 (EXACT:
    unsigned bLo = 0, bHi = 0;  // follow-ups), else preLen bytes + byte in [bLo,bHi)
    u64 kbase = 0;               // key capture offset
    u64 keyLen = 0;              // key bytes to capture
    bool withRun = false;        // capture the run digit (initial requests)
    u64 posLo = 0, posHi = ~0ull;
    bool sliced = false;
    // POSITIONAL follow-ups: the rows are KNOWN (the unit's chunk holds the
    // tied positions in memory). Membership is never re-derived by matching
    // a prefix against the corpus - that costs matcher memory proportional
    // to repeat depth (measured: 55053 follow-ups x deep shared prefixes =
    // 11.28M automaton nodes = 11.55 GB = two box OOMs). The sweep serves
    // these rows by direct position lookup; the matcher carries ONLY the
    // initial radix partition (1-4 byte prefixes).
    bool positional = false;
    std::vector<u64> positions;   // the exact rows, ascending
    Chunk* chunk = nullptr;
    u64 capRows = 0;             // reserved rows; exceeding it is a plan failure
    std::atomic<bool> served{false};
    u64 unitId = ~0ull;
    const char* what = "unit";
    std::atomic<bool>* servedTarget = nullptr;   // the unit's own served flag
    std::vector<u8> exactPre;    // storage for exact prefixes
    // THE LIVE BILL: the bound is charged by the bytes actually captured, in
    // slabs, BY THE TAPE (a class chunk is a superset of the groups that asked
    // for it, so the asker cannot price it exactly up front).
    struct Bound* bill = nullptr;
    u64 billedRows = 0;
    u64 billedBytes = 0;
    u64 slabRows = 0;   // the charge granularity (the asker's row estimate)
    bool runFilter = false;
    u64 filterBase = 0;   // the run's absolute offset (the unit's kbase)
    u8 runSigma = 0;
    u32 runL = 0;
    u16 runT = 0;
    bool parse = false;        // level 3
    u64 phraseBudget = 0;
    u64 round = 0;
    std::unordered_map<u64, u32> dictOf;   // proper phrase hash -> local dict id
    std::unordered_map<u64, u32> pdictOf;  // partial phrase hash -> pdict index
    u64 dictBilled = 0;   // dictionary bytes billed to the bound (freed at follow-up end)
    u64 curEnt = 0;                        // the request's frontier cursor (live ents)
};

struct Bound;
struct Tape {
    int fd = -1;
    std::mutex mu;
    std::condition_variable cvReq;      // a request arrived
    std::vector<TapeRequest*> queue;
    std::atomic<u64>* unparked = nullptr;   // sorters currently able to issue more
    std::atomic<bool>* admitting = nullptr; // the main thread mid-batch of admissions
    std::atomic<bool> stopping{false};
    // receipts
    u64 passes = 0, rowsRouted = 0, bytesRead = 0, idlePeriods = 0;
    double idleTotal = 0;

    void enqueue(TapeRequest* r) {
        {
            std::lock_guard<std::mutex> lk(mu);
            queue.push_back(r);
        }
        cvReq.notify_all();
    }

    // ---- the matcher: a flat trie over the outstanding prefixes. They are
    // pairwise disjoint (a follow-up extends one unit's range and that unit's
    // initial request is never outstanding twice), so a position matches at
    // most one request and a leaf ends the walk.
    struct Matcher {
        std::vector<std::array<u32, 256>> nodes;
        std::vector<std::vector<u32>> reqs;   // per node: request indices + 1
        std::vector<u32> fail;    // the failure link (0 = root)
        std::vector<u32> outLink; // nearest fail-chain node WITH requests
        std::vector<u32> depth;   // the node's pattern length (trie depth)
        size_t insert(const u8* p, u64 len) {
            if (nodes.empty()) { nodes.resize(1); reqs.emplace_back(); }
            u32 at = 0;
            for (u64 i = 0; i < len; ++i) {
                const u32 nxt = nodes[at][p[i]];
                if (!nxt) {
                    if (nodes.size() >= (1ull << 20))
                        die("MATCHER_NODES: refusing allocation at nodes=" +
                            std::to_string(nodes.size()) + " limit=1048576");
                    nodes.emplace_back();
                    nodes[at][p[i]] = u32(nodes.size());   // 1-BASED: 0 = no child
                    reqs.emplace_back();
                    at = u32(nodes.size() - 1);
                } else at = nxt - 1;
            }
            return at;
        }
        // THE AUTOMATON (Aho-Corasick, the DFA form): failure links make the
        // scan O(1) amortized per TEXT BYTE, independent of prefix depth.
        // The per-position trie walk was O(depth): with the level-3 reroutes
        // carrying multi-kilobyte prefixes (pre + the whole 4096-byte window),
        // every corpus position walked thousands of table probes - minutes
        // per pass on a 262 KiB corpus. The automaton consumes each byte
        // once and reports matches at their prefix ENDS.
        void build() {
            const size_t n = nodes.size();
            if (!n) return;   // no matcher requests this pass (all positional)
            u64 withReq = 0, withOut = 0;
            for (size_t i = 0; i < n; ++i) if (!reqs[i].empty()) ++withReq;
            fail.assign(n, 0);
            outLink.assign(n, 0);
            depth.assign(n, 0);
            std::vector<u32> bfs;
            for (unsigned c = 0; c < 256; ++c)
                if (nodes[0][c]) bfs.push_back(nodes[0][c] - 1);
            for (size_t h = 0; h < bfs.size(); ++h) {
                const u32 v = bfs[h];
                outLink[v] = !reqs[v].empty() ? v : outLink[fail[v]];
                if (outLink[v]) ++withOut;
                for (unsigned c = 0; c < 256; ++c) {
                    const u32 nx = nodes[v][c];
                    if (nx) {
                        fail[nx - 1] = nodes[fail[v]][c] ? nodes[fail[v]][c] - 1 : 0;
                        depth[nx - 1] = depth[v] + 1;
                        bfs.push_back(nx - 1);
                    } else {
                        // goto compression: the DFA transition
                        nodes[v][c] = nodes[fail[v]][c];
                    }
                }
            }
            std::fprintf(stderr, "AC_BUILD nodes=%zu with_reqs=%" PRIu64
                         " with_out=%" PRIu64 " bfs=%zu\n", n, withReq, withOut,
                         bfs.size());
        }
    };

    void threadLoop() {
        u64 idleSince = 0;
        u64 idleReadBytes0 = 0;
        bool idling = false;
        std::vector<u8> buf, pad;
        for (;;) {
            // ---- THE PAUSE: queue empty -> idle; poll + sleep, ZERO I/O ----
            std::vector<TapeRequest*> snap;
            {
                std::unique_lock<std::mutex> lk(mu);
                cvReq.wait_for(lk, std::chrono::milliseconds(10), [&] {
                    return stopping.load() || !queue.empty();
                });
                if (stopping.load() && queue.empty()) break;
                if (queue.empty()) {
                    if (!idling) {
                        idling = true;
                        idleSince = size_t(now_sec() * 1000.0);
                        u64 rchar = 0, rb = 0, syscr = 0;
                        read_proc_io(rchar, rb, syscr);
                        idleReadBytes0 = rb;
                    }
                    continue;
                }
                // THE FIXPOINT: serve every request that can exist yet - a pass
                // starts only when every live sorter is parked or done, so one
                // pass amortizes all follow-ups issued so far.
                const bool busy = (unparked && unparked->load(std::memory_order_acquire) > 0) ||
                                 (admitting && admitting->load(std::memory_order_acquire));
                if (busy) {
                    lk.unlock();
                    ::usleep(1000);
                    beat(1);   // waiting for a live sorter to park: visible
                    continue;
                }
                snap.swap(queue);
            }
            if (idling) {
                idling = false;
                const double idleS = now_sec() - double(idleSince) / 1000.0;
                u64 rchar = 0, rb = 0, syscr = 0;
                read_proc_io(rchar, rb, syscr);
                ++idlePeriods;
                idleTotal += idleS;
                if (idleS >= 0.2)
                    std::fprintf(stderr,
                        "TAPE_IDLE idle_s=%.3f read_bytes_delta=%" PRIu64
                        " requests_on_wake=%zu rule=poll_and_sleep_zero_io_zero_signals\n",
                        idleS, rb - idleReadBytes0, snap.size());
            }
            if (snap.empty()) continue;
            runPass(snap, buf, pad);
        }
    }

    // ---- THE TRIGGER FRONTIER (level 3, shared by every parse request of a
    // pass). The pass walks the corpus ASCENDING, so the minimizer stream is
    // computed once and extended monotonically; a row never re-reads bytes a
    // previous row of the pass already touched. THE DISCIPLINE IS PFP's (as
    // carried in bit6/pfp_ds_vendor): a k-mer start s is a TRIGGER iff it is
    // the leftmost minimum of the window of w k-mers starting at s; phrases end
    // with their trigger k-mer, and consecutive triggers are <= w apart.
    struct Frontier {
        struct Ent { u64 end; u64 start; u64 off; u32 len; u64 h; };  // phrase [start, end]; h = its FNV (computed once)
        std::vector<Ent> ents;      // ascending end; off into arena
        std::vector<u8> arena;      // phrase bytes, concatenated
        u64 frontierStart = 0;      // k-mer starts below this are classified
        bool clamped0 = false;     // ents[0] starts mid-text: a PARTIAL, not a phrase
        // the text is exhausted when no k-mer start with a full forward
        // window remains unclassified (the final w are withheld by the
        // upTo margin while a window is still open; at the corpus end the
        // classification covers everything the rule can see)
        bool exhaustedText() const {
            return frontierStart + g_pfp_k + g_pfp_w > FILE_N;
        }
        // drop phrases that ended below `lo` (no future row can land in
        // them); compacts the arena and re-bases every live cursor.
        u64 trimBelow(u64 lo, std::vector<TapeRequest*>& reqs) {
            u64 k2 = 0;
            while (k2 < ents.size() && ents[size_t(k2)].end < lo) ++k2;
            if (!k2) return 0;
            const u64 dropBytes = ents[size_t(k2 - 1)].off + ents[size_t(k2 - 1)].len;
            ents.erase(ents.begin(), ents.begin() + ptrdiff_t(k2));
            for (auto& e : ents) e.off -= dropBytes;
            arena.erase(arena.begin(), arena.begin() + ptrdiff_t(dropBytes));
            for (TapeRequest* r : reqs)
                if (r->parse && r->curEnt >= k2)
                    r->curEnt -= k2;   // never below 0: it pointed at a live ent
                else if (r->parse && r->curEnt < k2)
                    r->curEnt = 0;
            return k2;
        }
    };
    void runPass(std::vector<TapeRequest*>& snap, std::vector<u8>& buf, std::vector<u8>& pad) {
        const double t0 = now_sec();
        auto kmHash = [](const u8* q) -> u64 {   // FNV-1a over the k-mer at q
            u64 h = 1469598103934665603ull;
            for (u64 i = 0; i < g_pfp_k; ++i) { h ^= q[i]; h *= 1099511628211ull; }
            return h;
        };
        // classify trigger starts up to `upTo` using bytes valid from winBase.
        // THE TRIGGER RULE (two-sided strict window minimum): start s is a
        // trigger iff km(s) is STRICTLY smaller than the w k-mers preceding
        // it AND the w k-mers following it. Consequence (the spacing lemma):
        // two triggers are at least w+1 apart - suppose s < s' with
        // s' - s <= w: then s' lies in s's forward window (km(s) < km(s'))
        // and s lies in s's backward window (km(s') < km(s)) - contradiction.
        // So every phrase [prev_end+1, s+k-1] is >= w+1 > k bytes long and
        // ends with its own trigger k-mer. The prefix-free gate below
        // asserts the dictionary consequence on the class's actual data.
        auto extendFrontier = [&](Frontier& F, u64 upTo, const u8* win, u64 winBase) {
            for (u64 s = std::max(F.frontierStart, winBase);
                 s + g_pfp_k + g_pfp_w <= upTo; ++s) {
                const u64 h = kmHash(win + (s - winBase));
                bool isTrig = true;
                const u64 back = std::min<u64>(g_pfp_w, s - winBase);
                for (u64 j = 1; j <= back; ++j) {
                    if (!(h < kmHash(win + (s - j - winBase)))) { isTrig = false; break; }
                }
                if (isTrig)
                    for (u64 j = 1; j <= g_pfp_w; ++j) {
                        if (!(h < kmHash(win + (s + j - winBase)))) { isTrig = false; break; }
                    }
                if (isTrig) {
                    const u64 end = s + g_pfp_k - 1;
                    // ent[0]'s content may begin past the true phrase start
                    // (classification starts mid-text): it is interned as a
                    // PARTIAL (pdict), never as a proper phrase.
                    const u64 start = F.ents.empty()
                        ? std::max(F.frontierStart, winBase)
                        : F.ents.back().end + 1;
                    if (F.ents.empty() && start > 0) F.clamped0 = true;
                    if (start > end) die("trigger frontier: phrase start past end");
                    const u32 len = u32(end - start + 1);
                    u64 hh = 1469598103934665603ull;
                    for (u64 b = 0; b < len; ++b) {
                        hh ^= win[size_t(start + b - winBase)];
                        hh *= 1099511628211ull;
                    }
                    Frontier::Ent e{end, start, F.arena.size(), len, hh};
                    F.arena.insert(F.arena.end(), win + (start - winBase),
                                   win + (end - winBase) + 1);
                    F.ents.push_back(e);
                }
                F.frontierStart = s + 1;
            }
        };
        // ---- build the matcher: ONLY the initial radix partition ----
        // Follow-ups are POSITIONAL (their rows are known) and never enter
        // the matcher: matching a follow-up's deep shared prefix against
        // the corpus costs matcher memory proportional to repeat depth
        // (measured: 55053 follow-ups = 11.28M nodes = 11.55 GB = two box
        // OOMs). The initial prefixes are 1-4 bytes; the node count is
        // GUARDED, not journaled - a breach dies with numbers.
        Matcher M;
        u64 posRows = 0;
        std::vector<std::pair<u64, TapeRequest*>> posIdx;   // (position, request)
        for (const TapeRequest* r : snap)
            if (r->positional) posRows += r->positions.size();
        posIdx.reserve(size_t(posRows));
        for (size_t ri = 0; ri < snap.size(); ++ri) {
            TapeRequest* r = snap[ri];
            if (r->positional) {
                for (u64 p : r->positions) posIdx.emplace_back(p, r);
                continue;
            }
            // preLen bytes + one byte in [bLo,bHi): the radix partition
            std::vector<u8> pfx(r->pre, r->pre + r->preLen);
            for (unsigned c = r->bLo; c < r->bHi; ++c) {
                pfx.resize(size_t(r->preLen));
                pfx.push_back(u8(c));
                const size_t at = M.insert(pfx.data(), pfx.size());
                M.reqs[at].push_back(u32(ri + 1));
            }
        }
        std::sort(posIdx.begin(), posIdx.end());
        for (size_t i2 = 1; i2 < posIdx.size(); ++i2) {
            if (posIdx[i2].first == posIdx[i2 - 1].first)
                die("TAPE_POSITIONS: two follow-up requests claim position " +
                    std::to_string(posIdx[i2].first) +
                    " - the groups are not disjoint");
            if (posIdx[i2].first >= KEY_N)
                die("TAPE_POSITIONS: follow-up row " +
                    std::to_string(posIdx[i2].first) + " is at or past KEY_N=" +
                    std::to_string(KEY_N) + " - the plan owns the tails," +
                    " not the tape");
        }
        size_t posCursor = 0;
        std::fprintf(stderr, "TRIE_BUILD pass=%" PRIu64 " matcher_reqs_matcher_only"
                     " nodes=%zu node_bytes=%zu positional_rows=%" PRIu64
                     " positional_index_bytes=%" PRIu64 "\n",
                     passes + 1, M.nodes.size(), M.nodes.size() * 1024,
                     posRows, u64(posIdx.size() * 16));
        if (M.nodes.size() > (1ull << 20))
            die("MATCHER_NODES: " + std::to_string(M.nodes.size()) +
                " automaton nodes (guard 1M) - a request with a deep prefix"
                " reached the matcher; only the initial 1-4 byte radix"
                " partition may match. Refusing with numbers before the"
                " memory does damage");
        M.build();   // failure + output links: O(1) amortized per text byte
        // ---- the look-ahead is data: predicate depth + key window + run cap ----
        u64 need = 1, maxPre = 1;
        for (TapeRequest* r : snap) {
            const u64 plen = u64(r->preLen) + (r->bHi ? 1 : 0);
            need = std::max(need, plen);
            maxPre = std::max(maxPre, plen);
            need = std::max(need, r->kbase + r->keyLen);
            if (r->withRun) need = std::max(need, r->kbase + g_wave_run_cap + 1);
            if (r->parse)
                need = std::max(need, u64(g_pfp_k) + 64);   // the lookback is the run-up
        }
        // the automaton state, carried ACROSS windows: it encodes the last
        // maxPrefixLen consumed bytes, so a prefix starting below `base`
        // still matches when its end arrives; reset per pass (new matcher).
        u32 at = 0;
        // the served-ring: one entry per start position window (see the
        // report handler); maxPre is the longest pattern this pass
        u64 servedMaskVal = 1;
        while (servedMaskVal < maxPre) servedMaskVal <<= 1;
        const size_t servedMask = size_t(servedMaskVal - 1);
        std::vector<u64> servedRing(size_t(servedMaskVal), ~0ull);
        Frontier F;
        u64 rowsThis = 0, unmatched = 0, bytesThis = 0;
        // Matchers report at prefix END, while KEY_N bounds row STARTS.
        // Consume the lookahead for the last owned rows; tails remain owned
        // exclusively by the sorter.
        const u64 scanHi = std::min(FILE_N, KEY_N + maxPre - 1);
        for (u64 base = 0; base < scanHi;) {
            const u64 chunk = std::min<u64>(SS_SCAN_CHUNK, scanHi - base);
            // the run-up covers BOTH the trigger rule's lookback AND the
            // longest request prefix: a prefix may START below `base` and
            // end inside the window (the automaton carries the state), and
            // its row's bytes [p, p+need) must be readable from `from`.
            const u64 runup = std::max<u64>(g_pfp_w + g_pfp_k - 1, maxPre);
            const u64 from = base > runup ? base - runup : 0;
            const u64 off = base - from;   // the run-up (symbol + trigger lookback)
            const u64 want = std::min<u64>(chunk + off + need, FILE_N > from ? FILE_N - from : 0);
            if (want) read_window(fd, from, want, buf);
            bytesThis += want;
            beat(1);   // one tape window scanned
            for (u64 j = 0; j < chunk; ++j) {
            const u64 pEnd = base + j;      // this byte's absolute position
            // POSITIONAL follow-ups: the rows are KNOWN; the sweep serves
            // them by direct position lookup - no matcher entry, no
            // corpus-wide membership re-derivation, no memory that grows
            // with repeat depth. (Rows are ascending across requests; the
            // cursor advances once per row BEFORE the capture so the
            // capture's 'continue' ends the row, never the loop.)
            while (posCursor < posIdx.size() && posIdx[posCursor].first == pEnd) {
                TapeRequest* r = posIdx[posCursor].second;
                const u64 p = pEnd;   // the row's position (known, exact)
                ++posCursor;
                const u64 jRow = off + (pEnd - base);
                const u64 avail = FILE_N - pEnd;
                const u8* qraw = buf.data() + size_t(jRow);
                const u8* q = qraw;
                if (jRow + need > want) {
                    if (pEnd + need <= FILE_N) continue;   // a later window
                    if (pad.size() < size_t(need)) pad.assign(size_t(need), SENTINEL_SYMBOL);
                    if (avail > need) die("tape pad overflow");
                    std::memcpy(pad.data(), q, size_t(avail));
                    q = pad.data();
                }
                beat(1);   // a positional row served
            Chunk* ck = r->chunk;
            if (r->parse) {
                // ---- LEVEL 3 CAPTURE: the continuation cut at triggers,
                // keyed (phrase id, intra-phrase offset). NO per-row bytes:
                // each distinct phrase is interned once into the class
                // dictionary (identical chunks hash to one entry - the
                // compression happens at extraction); rows only carry ids.
                if (ck->rows() >= r->capRows)
                    die("TAPE_CHUNK_OVER_TARGET: parse request unit=" +
                        std::to_string(r->unitId) + " capped at " +
                        std::to_string(r->capRows) + " rows, got more");
                if (F.ents.empty() && F.frontierStart == 0 && p > 2 * (g_pfp_k + g_pfp_w))
                    F.frontierStart = p - 2 * (g_pfp_k + g_pfp_w);
                const u64 validHi = from + want;      // bytes valid in buf
                // mid-windows withhold the last w+k-1 starts (the
                // forward window is not readable yet); the LAST window
                // classifies to the text end - those final starts have
                // no forward window EVER, so without this the frontier
                // never reports exhaustion, the final partial phrase is
                // never captured, no stream ever reaches TEND, and
                // every class ties through all widening rounds.
                const u64 upTo2 = (from + want >= FILE_N)
                    ? FILE_N
                    : (validHi > (g_pfp_w + g_pfp_k - 1)
                           ? validHi - (g_pfp_w + g_pfp_k - 1) : 0);
                extendFrontier(F, upTo2, buf.data(), from);
                ck->P.push_back(pack_sym(p, p == 0 ? SENTINEL_SYMBOL : qraw[-1]));
                const u64 row = ck->P.size() - 1;
                const u64 B = r->phraseBudget;
                std::vector<u32> rowIds(size_t(B), ~0u);  // append-only:
                // ids is NEVER resized behind the writer (resize would
                // value-initialize and wipe the just-written ids)
                // hash a byte range; interns into dict (proper) or pdict
                // the hash is COMPUTED ONCE per phrase at frontier
                // creation (Ent.h); re-hashing up-to-12 KiB phrases per
                // row x phrase was tens of GB of hashing per pass
                auto intern = [&](const u8* q2, u32 len, u64 hh,
                                  bool partial) -> u32 {
                    auto& tab = partial ? ck->pdict : ck->dict;
                    auto& mapOf = partial ? r->pdictOf : r->dictOf;
                    auto it = mapOf.find(hh);
                    if (it != mapOf.end()) return it->second |
                        (partial ? 0x80000000u : 0u);
                    const u32 id = u32(tab.size());
                    tab.emplace_back(q2, q2 + len);
                    mapOf.emplace(hh, id);
                    // the dictionary's bytes are bound-billed HERE (the
                    // asker cannot price a dictionary it has not seen)
                    r->dictBilled += u64(len) + 16;
                    if (r->bill)
                        r->bill->add(u64(len) + 16, r->unitId,
                                     "parse-dict", r->billedRows);
                    return id | (partial ? 0x80000000u : 0u);
                };
                u32* idOut = rowIds.data();
                u64 prev = 0;
                // the phrase containing p: the first live ent with end >= p
                while (r->curEnt < F.ents.size() && F.ents[size_t(r->curEnt)].end < p)
                    ++r->curEnt;
                if (r->curEnt >= F.ents.size()) {
                    // no trigger end >= p classified yet: the row's stream
                    // is UNKNOWN beyond -> TRUNCATED (widen / residual)
                    ck->off0.push_back(0);
                    ck->ids.insert(ck->ids.end(), rowIds.begin(), rowIds.end());
                    ++rowsThis;
                    beat(1);   // a captured row (parse, truncated)
                    if (r->bill && r->slabRows && ck->rows() >= r->billedRows) {
                        r->bill->add(r->slabRows * (r->keyLen + 8), r->unitId,
                                     r->what, r->billedRows);
                        r->billedRows += r->slabRows;
                        r->billedBytes += r->slabRows * (r->keyLen + 8);
                    }
                    continue;
                }
                const Frontier::Ent& ce = F.ents[size_t(r->curEnt)];
                if (ce.start <= p) {
                    // p is inside a classified phrase: (id, offset). The
                    // FIRST classified phrase starts mid-text (its true
                    // start is unknown): it is a PARTIAL, never proper.
                    const bool part0 = (r->curEnt == 0 && F.clamped0);
                    idOut[0] = intern(F.arena.data() + ce.off, ce.len, ce.h, part0);
                    ck->off0.push_back(u32(p - ce.start));
                    prev = ce.end;
                } else {
                    // p lies in the gap before the first classified
                    // phrase: a partial from the bytes in hand
                    idOut[0] = intern(q, u32(ce.end - p + 1),
                                      [&] {   // the partial: hash its bytes
                                          u64 h2 = 1469598103934665603ull;
                                          for (u64 b2 = 0; b2 < u32(ce.end - p + 1); ++b2) {
                                              h2 ^= q[b2]; h2 *= 1099511628211ull;
                                          }
                                          return h2;
                                      }(), true);
                    ck->off0.push_back(0);
                    prev = ce.end;
                }
                // the following phrases, until the budget or the text end
                u64 i = 1;
                for (; i < B; ++i) {
                    u64 entIdx = r->curEnt + 1;
                    while (entIdx < F.ents.size() &&
                           F.ents[size_t(entIdx)].end <= prev)
                        ++entIdx;
                    if (entIdx >= F.ents.size()) {
                        if (!F.exhaustedText()) {
                            idOut[i] = ~0u;   // not yet classified: TRUNCATED
                            break;
                        }
                        // the TRUE end: the final partial phrase, then TEND
                        if (prev >= FILE_N - 1) { idOut[i] = 0xFFFFFFFEu; break; }
                        const u64 fl = FILE_N - 1 - prev;
                        if (prev + 1 < base || fl > (1u << 20)) {
                            // the partial's bytes are not in this window
                            // (a long un-triggered tail): UNKNOWN beyond
                            idOut[i] = ~0u; break;
                        }
                        idOut[i] = intern(q + (prev + 1 - p), u32(fl),
                                          [&] {
                                              u64 h2 = 1469598103934665603ull;
                                              for (u64 b2 = 0; b2 < fl; ++b2) {
                                                  h2 ^= q[size_t(prev + 1 - p + b2)];
                                                  h2 *= 1099511628211ull;
                                              }
                                              return h2;
                                          }(), true);
                        if (i + 1 < B) { idOut[i + 1] = 0xFFFFFFFEu; ++i; }
                        break;
                    }
                    const Frontier::Ent& e = F.ents[size_t(entIdx)];
                    idOut[i] = intern(F.arena.data() + e.off, e.len, e.h, false);
                    prev = e.end;
                }
                ck->ids.insert(ck->ids.end(), rowIds.begin(), rowIds.end());
                ++rowsThis;
                if (r->bill && r->slabRows && ck->rows() >= r->billedRows) {
                    r->bill->add(r->slabRows * (r->keyLen + 8), r->unitId,
                                 r->what, r->billedRows);
                    r->billedRows += r->slabRows;
                    r->billedBytes += r->slabRows * (r->keyLen + 8);
                }
                continue;
            }
            if (ck->rows() >= r->capRows)
                die("TAPE_CHUNK_OVER_TARGET: request unit=" + std::to_string(r->unitId) +
                    " capped at " + std::to_string(r->capRows) + " rows, got more "
                    "(the plan's estimate is wrong by more than the headroom; this is "
                    "the per-request accounting, not a knob to tune)");
            if (r->bill && r->slabRows && ck->rows() >= r->billedRows) {
                r->bill->add(r->slabRows * (r->keyLen + 8), r->unitId,
                             r->what, r->billedRows);
                r->billedRows += r->slabRows;
                r->billedBytes += r->slabRows * (r->keyLen + 8);
            }
            const u8* kq = q + r->kbase;
            const size_t at2 = ck->Kb.size();
            ck->P.push_back(pack_sym(p, p == 0 ? SENTINEL_SYMBOL : qraw[-1]));
            ck->Kb.resize(at2 + size_t(r->keyLen));
            std::memcpy(ck->Kb.data() + at2, kq, size_t(r->keyLen));
            if (r->withRun) {
                u32 runL = 0; u16 runT = 0;
                wavekey_run_digit(kq, avail - std::min<u64>(avail, r->kbase),
                                  g_wave_run_cap, runL, runT);
                ck->Lp.push_back(runL);
                ck->Tp.push_back(runT);
            }
            ++rowsThis;
            }

            // consume ONE byte: a single table probe (DFA form)
            if (!M.nodes.empty()) {
                const u32 nx = M.nodes[at][buf[size_t(off + j)]];
                at = nx ? nx - 1 : 0;
            }
            if (M.nodes.empty() || !M.outLink[at]) continue;
            ++g_ac_reports;
            // report every request whose prefix ENDS at pEnd: its row starts
            // at pEnd - len + 1 (each position belongs to at most one group,
            // so at most one request matches per report)
            // EVERY pattern in the fail chain owns a DIFFERENT row (each
            // has its own start): serve them all, one per start. The FIRST
            // report owns a row: for a row p, the shortest pattern matching
            // at p ends earliest, so the first report with start p IS the
            // shortest - exactly the per-position walk's choice; longer
            // patterns for the same p arrive later and are skipped. The
            // ring remembers the last maxPre starts (a pattern's start is
            // within maxPre of its end); a start is marked whether or not
            // its candidates passed (the walk never tried longer patterns
            // after a rejected candidate either).
            bool servedAny = false;
            for (u32 on = M.outLink[at]; on; on = M.outLink[M.fail[on]]) {
            const u64 pFirst = pEnd - M.depth[on] + 1;
            if (pFirst >= KEY_N) continue;
            const size_t slot = size_t(pFirst) & servedMask;
            if (servedRing[slot] == pFirst) continue;
            servedRing[slot] = pFirst;
            bool servedOne = false;
            int fState = 0;
            u64 fBase = 0;
            u8 fSigma = 0; u32 fL = 0; u16 fT = 0;
            for (u32 cand : M.reqs[on]) {
                TapeRequest* c = snap[size_t(cand - 1)];
                const u64 L = c->preLen + (c->bHi ? 1 : 0);
                const u64 p = pEnd - L + 1;          // the ROW position
                if (p + L - 1 != pEnd) continue;      // cannot happen; clarity
                const u64 jRow = off + (p - base);   // >= 0: runup covers L
                if (c->sliced && (p < c->posLo || p >= c->posHi)) continue;
                // THE ROW: q points at its position, pad past the text end -
                // computed BEFORE the run filter so a run crossing the text
                // end walks SENTINELS (the padded view), not raw buffer bytes
                const u64 avail = FILE_N - p;
                const u8* qraw = buf.data() + size_t(jRow);
                const u8* q = qraw;
                if (jRow + need > want) {
                    if (p + need <= FILE_N) continue;   // a later window owns it
                    if (pad.size() < size_t(need)) pad.assign(size_t(need), SENTINEL_SYMBOL);
                    if (avail > need) die("tape pad overflow");
                    std::memcpy(pad.data(), q, size_t(avail));
                    q = pad.data();
                }
                if (c->runFilter) {
                    const u8* fq = q + c->filterBase;
                    const u64 favail = avail > c->filterBase
                                           ? avail - c->filterBase : 0;
                    if (!favail) continue;   // no run: not a member
                    if (!fState || fBase != c->filterBase) {
                        fSigma = fq[0];
                        wavekey_run_digit(fq, favail, g_wave_run_cap, fL, fT);
                        fBase = c->filterBase;
                        fState = 1;
                    }
                    if (fSigma != c->runSigma || fL != c->runL || fT != c->runT)
                        continue;
                }
                TapeRequest* r = c;
                Chunk* ck = r->chunk;
                if (r->parse) {
                    // ---- LEVEL 3 CAPTURE: the continuation cut at triggers,
                    // keyed (phrase id, intra-phrase offset). NO per-row bytes:
                    // each distinct phrase is interned once into the class
                    // dictionary (identical chunks hash to one entry - the
                    // compression happens at extraction); rows only carry ids.
                    if (ck->rows() >= r->capRows)
                        die("TAPE_CHUNK_OVER_TARGET: parse request unit=" +
                            std::to_string(r->unitId) + " capped at " +
                            std::to_string(r->capRows) + " rows, got more");
                    if (F.ents.empty() && F.frontierStart == 0 && p > 2 * (g_pfp_k + g_pfp_w))
                        F.frontierStart = p - 2 * (g_pfp_k + g_pfp_w);
                    const u64 validHi = from + want;      // bytes valid in buf
                    // mid-windows withhold the last w+k-1 starts (the
                    // forward window is not readable yet); the LAST window
                    // classifies to the text end - those final starts have
                    // no forward window EVER, so without this the frontier
                    // never reports exhaustion, the final partial phrase is
                    // never captured, no stream ever reaches TEND, and
                    // every class ties through all widening rounds.
                    const u64 upTo2 = (from + want >= FILE_N)
                        ? FILE_N
                        : (validHi > (g_pfp_w + g_pfp_k - 1)
                               ? validHi - (g_pfp_w + g_pfp_k - 1) : 0);
                    extendFrontier(F, upTo2, buf.data(), from);
                    ck->P.push_back(pack_sym(p, p == 0 ? SENTINEL_SYMBOL : qraw[-1]));
                    const u64 row = ck->P.size() - 1;
                    const u64 B = r->phraseBudget;
                    std::vector<u32> rowIds(size_t(B), ~0u);  // append-only:
                    // ids is NEVER resized behind the writer (resize would
                    // value-initialize and wipe the just-written ids)
                    // hash a byte range; interns into dict (proper) or pdict
                    // the hash is COMPUTED ONCE per phrase at frontier
                    // creation (Ent.h); re-hashing up-to-12 KiB phrases per
                    // row x phrase was tens of GB of hashing per pass
                    auto intern = [&](const u8* q2, u32 len, u64 hh,
                                      bool partial) -> u32 {
                        auto& tab = partial ? ck->pdict : ck->dict;
                        auto& mapOf = partial ? r->pdictOf : r->dictOf;
                        auto it = mapOf.find(hh);
                        if (it != mapOf.end()) return it->second |
                            (partial ? 0x80000000u : 0u);
                        const u32 id = u32(tab.size());
                        tab.emplace_back(q2, q2 + len);
                        mapOf.emplace(hh, id);
                        // the dictionary's bytes are bound-billed HERE (the
                        // asker cannot price a dictionary it has not seen)
                        r->dictBilled += u64(len) + 16;
                        if (r->bill)
                            r->bill->add(u64(len) + 16, r->unitId,
                                         "parse-dict", r->billedRows);
                        return id | (partial ? 0x80000000u : 0u);
                    };
                    u32* idOut = rowIds.data();
                    u64 prev = 0;
                    // the phrase containing p: the first live ent with end >= p
                    while (r->curEnt < F.ents.size() && F.ents[size_t(r->curEnt)].end < p)
                        ++r->curEnt;
                    if (r->curEnt >= F.ents.size()) {
                        // no trigger end >= p classified yet: the row's stream
                        // is UNKNOWN beyond -> TRUNCATED (widen / residual)
                        ck->off0.push_back(0);
                        ck->ids.insert(ck->ids.end(), rowIds.begin(), rowIds.end());
                        ++rowsThis;
                        beat(1);   // a captured row (window)
                        beat(1);   // a captured row (parse)
                        beat(1);   // a captured row (parse trunc)
                        if (r->bill && r->slabRows && ck->rows() >= r->billedRows) {
                            r->bill->add(r->slabRows * (r->keyLen + 8), r->unitId,
                                         r->what, r->billedRows);
                            r->billedRows += r->slabRows;
                            r->billedBytes += r->slabRows * (r->keyLen + 8);
                        }
                        continue;
                    }
                    const Frontier::Ent& ce = F.ents[size_t(r->curEnt)];
                    if (ce.start <= p) {
                        // p is inside a classified phrase: (id, offset). The
                        // FIRST classified phrase starts mid-text (its true
                        // start is unknown): it is a PARTIAL, never proper.
                        const bool part0 = (r->curEnt == 0 && F.clamped0);
                        idOut[0] = intern(F.arena.data() + ce.off, ce.len, ce.h, part0);
                        ck->off0.push_back(u32(p - ce.start));
                        prev = ce.end;
                    } else {
                        // p lies in the gap before the first classified
                        // phrase: a partial from the bytes in hand
                        idOut[0] = intern(q, u32(ce.end - p + 1),
                                          [&] {   // the partial: hash its bytes
                                              u64 h2 = 1469598103934665603ull;
                                              for (u64 b2 = 0; b2 < u32(ce.end - p + 1); ++b2) {
                                                  h2 ^= q[b2]; h2 *= 1099511628211ull;
                                              }
                                              return h2;
                                          }(), true);
                        ck->off0.push_back(0);
                        prev = ce.end;
                    }
                    // the following phrases, until the budget or the text end
                    u64 i = 1;
                    for (; i < B; ++i) {
                        u64 entIdx = r->curEnt + 1;
                        while (entIdx < F.ents.size() &&
                               F.ents[size_t(entIdx)].end <= prev)
                            ++entIdx;
                        if (entIdx >= F.ents.size()) {
                            if (!F.exhaustedText()) {
                                idOut[i] = ~0u;   // not yet classified: TRUNCATED
                                break;
                            }
                            // the TRUE end: the final partial phrase, then TEND
                            if (prev >= FILE_N - 1) { idOut[i] = 0xFFFFFFFEu; break; }
                            const u64 fl = FILE_N - 1 - prev;
                            if (prev + 1 < base || fl > (1u << 20)) {
                                // the partial's bytes are not in this window
                                // (a long un-triggered tail): UNKNOWN beyond
                                idOut[i] = ~0u; break;
                            }
                            idOut[i] = intern(q + (prev + 1 - p), u32(fl),
                                              [&] {
                                                  u64 h2 = 1469598103934665603ull;
                                                  for (u64 b2 = 0; b2 < fl; ++b2) {
                                                      h2 ^= q[size_t(prev + 1 - p + b2)];
                                                      h2 *= 1099511628211ull;
                                                  }
                                                  return h2;
                                              }(), true);
                            if (i + 1 < B) { idOut[i + 1] = 0xFFFFFFFEu; ++i; }
                            break;
                        }
                        const Frontier::Ent& e = F.ents[size_t(entIdx)];
                        idOut[i] = intern(F.arena.data() + e.off, e.len, e.h, false);
                        prev = e.end;
                    }
                    ck->ids.insert(ck->ids.end(), rowIds.begin(), rowIds.end());
                    ++rowsThis;
                    if (r->bill && r->slabRows && ck->rows() >= r->billedRows) {
                        r->bill->add(r->slabRows * (r->keyLen + 8), r->unitId,
                                     r->what, r->billedRows);
                        r->billedRows += r->slabRows;
                        r->billedBytes += r->slabRows * (r->keyLen + 8);
                    }
                    continue;
                }
                if (ck->rows() >= r->capRows)
                    die("TAPE_CHUNK_OVER_TARGET: request unit=" + std::to_string(r->unitId) +
                        " capped at " + std::to_string(r->capRows) + " rows, got more "
                        "(the plan's estimate is wrong by more than the headroom; this is "
                        "the per-request accounting, not a knob to tune)");
                if (r->bill && r->slabRows && ck->rows() >= r->billedRows) {
                    r->bill->add(r->slabRows * (r->keyLen + 8), r->unitId,
                                 r->what, r->billedRows);
                    r->billedRows += r->slabRows;
                    r->billedBytes += r->slabRows * (r->keyLen + 8);
                }
                const u8* kq = q + r->kbase;
                const size_t at2 = ck->Kb.size();
                ck->P.push_back(pack_sym(p, p == 0 ? SENTINEL_SYMBOL : qraw[-1]));
                ck->Kb.resize(at2 + size_t(r->keyLen));
                std::memcpy(ck->Kb.data() + at2, kq, size_t(r->keyLen));
                if (r->withRun) {
                    u32 runL = 0; u16 runT = 0;
                    wavekey_run_digit(kq, avail - std::min<u64>(avail, r->kbase),
                                      g_wave_run_cap, runL, runT);
                    ck->Lp.push_back(runL);
                    ck->Tp.push_back(runT);
                }
                ++rowsThis;
                beat(1);

                servedOne = true;
                break;
            }
            servedAny = servedAny || servedOne;
            }
            if (!servedAny) ++unmatched;
        }
            // the frontier holds phrase bytes for the whole pass otherwise -
            // text materialization. Phrases that ended below the NEXT window
            // can never contain a future row: drop them, compact the arena.
            {
                const u64 nextBase = base + chunk;
                if (!F.ents.empty() && F.ents.front().end < nextBase)
                    F.trimBelow(nextBase, snap);
            }
            base += chunk;
        }
        ++passes;
        rowsRouted += rowsThis;
        bytesRead += bytesThis;
        std::fprintf(stderr,
            "PASS n=%" PRIu64 " requests=%zu rows=%" PRIu64 " unmatched=%" PRIu64
            " bytes=%" PRIu64 " wall=%.2f"
            " rule=one_sweep_serves_every_outstanding_request\n",
            passes, snap.size(), rowsThis, unmatched, bytesThis, now_sec() - t0);
        for (TapeRequest* r : snap) {
            r->served.store(true, std::memory_order_release);
            if (r->servedTarget)
                r->servedTarget->store(true, std::memory_order_release);
        }
    }
};

// ===========================================================================
// THE PLAN: the whole radix partition, up front, into UNITS of
// --unit-target-rows. The descent is carried over from the wave lane (exact
// root counts from the survey, sample-scaled census deeper from the prefix
// cache, no depth cap, the single-child / 99.9%-shaving rule to the
// point-mass branch, the tail-run proof, slices in DESCENDING position).
// The local/global strategy, coverage and the position-window machinery are
// DELETED (nothing in this design reads positions from disc).
// ===========================================================================
struct Unit {
    u64 depth = 1;            // key-prefix length D (>= 1)
    u32 preOff = 0, preLen = 0;
    unsigned bLo = 0, bHi = 256;
    u64 posLo = 0, posHi = ~0ull;
    bool sliced = false;
    // POSITIONAL follow-ups: the rows are KNOWN (the unit's chunk holds the
    // tied positions in memory). Membership is never re-derived by matching
    // a prefix against the corpus - that costs matcher memory proportional
    // to repeat depth (measured: 55053 follow-ups x deep shared prefixes =
    // 11.28M automaton nodes = 11.55 GB = two box OOMs). The sweep serves
    // these rows by direct position lookup; the matcher carries ONLY the
    // initial radix partition (1-4 byte prefixes).
    bool positional = false;
    std::vector<u64> positions;   // the exact rows, ascending
    u64 rowsEst = 0;
    bool borrow = false;
    // ---- runtime ----
    Chunk* chunk = nullptr;
    std::atomic<bool> served{false};
    std::atomic<bool> sorted{false};
    std::atomic<bool> emitted{false};
    std::atomic<u64> pub{0};        // 0 = not published, 1 = empty, else lastPos+2
    std::vector<u64> P;             // the sorted rows (handed to the emitter)
    std::vector<u64> lcpv;
    u64 boundWaitBytes = 0;         // what admission committed (recBytes x capRows)
    u64 boundSortBytes = 0;         // what the sorter committed (sortDelta x rows)
    std::vector<std::pair<u64, u64>> esc;   // (global rank, value) for LCP >= 255
};

struct UnitPlan {
    std::deque<Unit> units;          // never relocated: Unit carries atomics
    std::vector<u8> preArena;
    u64 maxDepth = 1;
    u64 pointMassSplits = 0, borrowUnits = 0, maxClassRows = 0;
    std::map<u64, u64> depthHist;
};

static UnitPlan plan_units(const Survey& S, const std::vector<u64>& hist,
                           const std::vector<u64>& sk, u64 stride, u64 trailing,
                           u64 unitTarget, u64 threads, u64 capBytes,
                           u64 recBytes, u64 sortExtraBytes, u64 emitBytes) {
    UnitPlan UP;
    std::deque<Unit>& wl = UP.units;
    std::vector<u8>& preArena = UP.preArena;
    const u64 waveGoal = std::max<u64>(1, unitTarget);
    std::function<void(size_t, size_t, std::vector<u8>&, bool)> descend;
    u64 planCacheMisses = 0;

    auto intern = [&](const std::vector<u8>& pre, unsigned b0, unsigned b1, u64 rows,
                      bool sliced, u64 plo, u64 phi) -> size_t {
        const u32 preOff = u32(preArena.size());
        preArena.insert(preArena.end(), pre.begin(), pre.end());
        wl.emplace_back();
        beat(2);
        Unit& W = wl.back();
        W.preOff = preOff;
        W.preLen = u32(pre.size());
        W.depth = u64(pre.size()) + 1;
        W.bLo = b0; W.bHi = b1;
        W.posLo = plo; W.posHi = phi; W.sliced = sliced;
        W.rowsEst = rows;
        UP.maxDepth = std::max(UP.maxDepth, W.depth);
        ++UP.depthHist[W.depth];
        UP.maxClassRows = std::max(UP.maxClassRows, rows);
        std::fprintf(stderr,
            "UNIT id=%zu depth=%" PRIu64 " bytes=[%u,%u) rows_est=%" PRIu64
            " prefix=%s sliced=%d arena_bytes=%" PRIu64
            " arena_price=rows*(8+K_max+6+work) rule=units_ordered_by_prefix_is_the_output_order\n",
            wl.size() - 1, W.depth, b0, b1, rows,
            prefix_label(preArena.data() + W.preOff, W.preLen).c_str(), int(sliced),
            rows * (recBytes + sortExtraBytes));
        return wl.size() - 1;
    };
    auto emitWave = [&](const std::vector<u8>& pre, unsigned b0, unsigned b1, u64 rows) {
        intern(pre, b0, b1, rows, false, 0, ~0ull);
    };
    // ---- THE POINT-MASS BRANCH (carried over: prove the order or refuse) ----
    auto emitPointMass = [&](const std::vector<u8>& pre, unsigned c, u64 depth, u64 rows) {
        auto sym_at = [](u64 p) -> unsigned {
            if (p >= FILE_N) return SENTINEL_SYMBOL;
            return REMAP ? REMAP[T[p]] : T[p];
        };
        auto in_class = [&](u64 p) -> bool {
            for (size_t i = 0; i < pre.size(); ++i) if (sym_at(p + i) != pre[i]) return false;
            return sym_at(p + depth - 1) == c;
        };
        const u64 runLo0 = trailing;
        const u64 runHi0 = FILE_N > depth - 1 ? FILE_N - (depth - 1) : 0;
        const u64 runRows = runHi0 > runLo0 ? runHi0 - runLo0 : 0;
        const double tpm = now_sec();
        const u64 nt = std::max<u64>(1, threads);
        std::atomic<u64> bad{0};
        std::vector<std::thread> ts3;
        for (u64 t = 0; t < nt; ++t)
            ts3.emplace_back([&, t]() {
                const u64 lo2 = N * t / nt, hi2 = N * (t + 1) / nt;
                for (u64 p = lo2; p < hi2; ++p)
                    if (in_class(p) != (p >= runLo0 && p < runHi0)) { bad.fetch_add(1); return; }
            });
        for (auto& th : ts3) th.join();
        const bool exactlyRun = bad.load() == 0;
        std::fprintf(stderr,
            "UNIT_POINT_MASS_PROOF depth=%" PRIu64 " class_rows_sampled=%" PRIu64
            " tail_slice_rows=%" PRIu64 " mismatches=%" PRIu64 " wall=%.2f"
            " (one corpus pass, the only one this branch ever costs)\n",
            depth, rows, runRows, bad.load(), now_sec() - tpm);
        if (!exactlyRun) {
            // ---- BORROW ONE UNIT FOR THIS CLASS, or refuse with numbers ----
            const u64 needBytes = rows * (recBytes + sortExtraBytes);
            if (needBytes <= capBytes) {
                ++UP.borrowUnits;
                std::fprintf(stderr,
                    "UNIT_BORROW class_rows=%" PRIu64 " need_bytes=%" PRIu64
                    " cap_bytes=%" PRIu64 " path=one_unit"
                    " reason=point_mass_no_key_byte_splits_it_no_position_slice_is_sound"
                    " note=a point mass may exceed the unit target by up to one class\n",
                    rows, needBytes, capBytes);
                intern(pre, c, c + 1, rows, false, 0, ~0ull);
                wl.back().borrow = true;
                return;
            }
            std::fprintf(stderr,
                "UNIT_BORROW class_rows=%" PRIu64 " need_bytes=%" PRIu64
                " cap_bytes=%" PRIu64 " path=refuse\n",
                rows, needBytes, capBytes);
            die("UNIT_POINT_MASS: a key-prefix class of " + std::to_string(rows) +
                " rows at depth " + std::to_string(depth) + " exceeds the unit goal " +
                std::to_string(waveGoal) + ", is NOT the single-symbol tail run, and does "
                "not fit one borrowed arena (" + std::to_string(needBytes) + " B) inside the "
                "envelope (" + std::to_string(capBytes) + " B). THE LEVERS: raise --budget-bytes"
                " so the one unit fits, or a deeper survey prefix cache. NO MERGE is a path here");
        }
        const u64 nslices = (runRows + waveGoal - 1) / waveGoal;
        std::fprintf(stderr,
            "UNIT_POINT_MASS depth=%" PRIu64 " class_rows=%" PRIu64 " unit_goal=%" PRIu64
            " trailing_run_rows=%" PRIu64 " provable=1 slices=%" PRIu64
            " rule=tail-run: ascending key order is DESCENDING position\n",
            depth, rows, waveGoal, runRows, nslices);
        for (u64 i = 0; i < nslices; ++i) {            // i = 0 is the TOP slice
            const u64 hi2 = runLo0 + runRows - i * waveGoal;
            const u64 lo2 = hi2 - std::min<u64>(waveGoal, hi2 - runLo0);
            intern(pre, c, c + 1, hi2 - lo2, true, lo2, hi2);
            ++UP.pointMassSplits;
        }
    };
    // ---- the descent (carried over; children in ASCENDING byte order) ----
    Plan plan;
    plan.s = &S;
    plan.perm.resize(S.samples);
    for (u64 i = 0; i < S.samples; ++i) plan.perm[i] = u32(i);
    descend = [&](size_t lo, size_t hi, std::vector<u8>& pre, bool exact) {
        const u64 off = u64(pre.size());
        const bool cached = off < S.P;
        if (!cached) {
            ++planCacheMisses;
            std::fprintf(stderr,
                "PLAN_CACHE_MISS depth=%" PRIu64 " class_rows=%" PRIu64 " samples=%zu"
                " cache_depth=%u rule=fall_through_to_corpus_windows\n",
                off, u64(hi - lo) * stride, hi - lo, S.P);
        }
        auto sym_at = [](u64 p) -> unsigned {
            if (p >= FILE_N) return SENTINEL_SYMBOL;
            return REMAP ? REMAP[T[p]] : T[p];
        };
        auto byte_at = [&](u32 i) -> unsigned {
            if (cached) return S.sym(i, off);
            PLAN_CORPUS_CALLS.fetch_add(1, std::memory_order_relaxed);
            PLAN_CORPUS_BYTES.fetch_add(1, std::memory_order_relaxed);
            return sym_at(S.pos(i) + off);
        };
        std::sort(plan.perm.begin() + lo, plan.perm.begin() + hi, [&](u32 a, u32 b) {
            const unsigned x = byte_at(a), y = byte_at(b);
            return x != y ? x < y : a < b;
        });
        u64 nodeRows = 0;
        for (size_t i = lo; i < hi; ++i) nodeRows += stride;
        if (exact) {
            nodeRows = 0;
            for (size_t i = lo; i < hi; ++i) nodeRows += hist[byte_at(plan.perm[i])];
        }
        bool runOpen = true;
        unsigned runLo = 0;
        u64 runRows = 0;
        size_t runIdx = lo;
        size_t a = lo;
        while (a < hi) {
            const unsigned c = byte_at(plan.perm[a]);
            size_t b = a + 1;
            while (b < hi && byte_at(plan.perm[b]) == c) ++b;
            const u64 rows = exact ? hist[c] : u64(b - a) * stride;
            if (rows > waveGoal) {
                if (runOpen && runLo < c) emitWave(pre, runLo, c, runRows);
                runOpen = false; runRows = 0;
                if (a == lo && b == hi || rows * 1000 > nodeRows * 999) {
                    emitPointMass(pre, c, off + 1, rows);
                } else {
                    pre.push_back(u8(c));
                    descend(a, b, pre, false);
                    pre.pop_back();
                }
                runOpen = true; runLo = c + 1; runIdx = b;
            } else {
                if (runOpen && runRows + rows > waveGoal) {
                    if (runLo < c) emitWave(pre, runLo, c, runRows);
                    runLo = c; runRows = 0; runIdx = a;
                }
                runOpen = true;
                runRows += rows;
            }
            a = b;
        }
        if (runOpen && runLo < 256) emitWave(pre, runLo, 256, runRows);
    };
    (void)emitBytes;
    std::vector<u8> rootPre;
    if (!plan.perm.empty()) descend(0, plan.perm.size(), rootPre, true);
    else emitWave(rootPre, 0, 256, 0);
    std::fprintf(stderr,
        "SS_PLAN units=%zu cache_depth=%u cache_misses=%" PRIu64
        " point_mass_splits=%" PRIu64 " borrow_units=%" PRIu64 " max_class_rows=%" PRIu64
        " max_depth=%" PRIu64 " corpus_reads=%" PRIu64 " corpus_read_calls=%" PRIu64
        " rule=whole_partition_up_front_answer_from_prefix_cache\n",
        wl.size(), S.P, planCacheMisses, UP.pointMassSplits, UP.borrowUnits,
        UP.maxClassRows, UP.maxDepth,
        PLAN_CORPUS_BYTES.load(), PLAN_CORPUS_CALLS.load());
    return UP;
}

// ===========================================================================
// THE EMITTER. A completed unit is emitted IN UNIT ORDER: the LCP slice for
// the finish (unchanged 'SXLS' contract), the band stream for the trie tap
// when TRIE_TAP is armed (unchanged band_<i>.sa + .opened + end contract),
// and the rows feed THE PIECE CUTTER, which writes the EXISTING piece format
// byte-identically: BPS1 start, PRC1 pieces at 65536-run bands flow-controlled
// against the finish's runcols.consumed, BPG1 progress records (refs=0
// classes=0 on this route), BPT1 terminal, SXRE runexc.
// ===========================================================================
#include "../../bit6/durable_piece.h"

static void xput(int fd, const void* p, size_t n, u64 at, const char* what) {
    const char* q = (const char*)p;
    size_t gone = 0;
    while (gone < n) {
        ssize_t w = ::pwrite(fd, q + gone, n - gone, off_t(at + gone));
        if (w <= 0) die(std::string("write ") + what);
        gone += size_t(w);
    }
}

struct OutFile {
    std::ofstream f; std::vector<char> buf;
    explicit OutFile(const std::string& path, size_t cap = 1 << 20) {
        require(!std::filesystem::exists(path), "output exists; refusing to clobber");
        f.open(path, std::ios::binary);
        require(bool(f), "cannot open output");
        buf.reserve(cap);
    }
    void raw(const void* p, size_t k) {
        const char* q = static_cast<const char*>(p);
        if (buf.size() + k > buf.capacity()) { f.write(buf.data(), buf.size()); buf.clear(); }
        if (k >= buf.capacity()) { f.write(q, k); return; }
        buf.insert(buf.end(), q, q + k);
    }
    void w64(u64 v) {
        char b[8];
        for (int i = 0; i < 8; ++i) b[i] = char(v >> (8 * i));
        raw(b, 8);
    }
    ~OutFile() { if (f.is_open()) { f.write(buf.data(), buf.size()); f.close(); } }
};

// The finish's piece-window handshake (carried over verbatim).
static u64 v2_piece_window() {
    const char* e = getenv("CROSS_V2_PIECE_WINDOW");
    u64 window = e ? u64(strtoull(e, nullptr, 10)) : 1100;
    require(window > 0, "v2 piece window must be positive");
    return window;
}
static int64_t v2_consumed_band(const std::string& prefix) {
    int fd = ::open((prefix + ".runcols.consumed").c_str(), O_RDONLY);
    if (fd < 0) return -1;
    char b[24]{};
    ssize_t z = ::read(fd, b, sizeof b - 1);
    ::close(fd);
    require(z > 0, "empty consumed marker");
    char* end = nullptr;
    long long value = strtoll(b, &end, 10);
    require(end != b && value >= -1, "malformed consumed marker");
    return value;
}
static void v2_flow_wait(const std::string& prefix, u64 band, u64 window) {
    for (;;) {
        int64_t consumed = v2_consumed_band(prefix);
        u64 limit = consumed < 0 ? window - 1 : u64(consumed) + window;
        if (band <= limit) return;
        ::usleep(50000);
    }
}

// One band piece file body (carried over byte-for-byte from the emission sink).
static u64 v2_write_piece(const std::string& dir, u64 band, u64 shard, u64 firstRun,
                          const uint8_t* chars, const uint32_t* lens,
                          const u64* heads, const u64* tails, u64 rc, unsigned w) {
    if (!rc) return 0;
    std::string tmp = dir + "/p-" + std::to_string(band) + "-" + std::to_string(shard) + ".partial";
    std::string fin = dir + "/p-" + std::to_string(band) + "-" + std::to_string(shard);
    require(!std::filesystem::exists(fin), "piece exists; refusing to clobber");
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    require(fd >= 0, "create piece tmp");
    auto wfull = [&](const void* d, u64 len) {
        const char* q = (const char*)d;
        while (len) { ssize_t z = ::write(fd, q, len); require(z > 0, "piece write"); q += z; len -= u64(z); }
    };
    char hdr[44];
    uint32_t magic = 0x31435250;  // "PRC1"
    std::memcpy(hdr, &magic, 4);
    std::memcpy(hdr + 4, &band, 8); std::memcpy(hdr + 12, &shard, 4);
    std::memcpy(hdr + 16, &firstRun, 8); std::memcpy(hdr + 24, &rc, 8);
    std::memcpy(hdr + 32, &w, 4); u64 zero = 0; std::memcpy(hdr + 36, &zero, 8);
    wfull(hdr, 44);
    wfull(chars, rc);
    wfull(lens, 4 * rc);
    u64 bits = u64(w) * rc, words = (bits + 63) / 64;
    std::vector<u64> out(words ? words : 1, 0);
    auto pack = [&](const u64* v) {
        __uint128_t res = 0; unsigned avail = 0; u64 wi = 0;
        for (u64 i = 0; i < rc; ++i) {
            res |= __uint128_t(v[i]) << avail; avail += w;
            if (avail >= 64) { out[wi++] = u64(res); res >>= 64; avail -= 64; }
        }
        if (avail) out[wi] = u64(res);
    };
    pack(heads);
    wfull(out.data(), 8 * words);
    pack(tails);
    wfull(out.data(), 8 * words);
    u64 rowCount = 0;
    for (u64 i = 0; i < rc; ++i) rowCount += lens[i];
    durable_piece::publish(fd, tmp, fin, dir, band, shard, firstRun, rc, rowCount);
    return words;
}

// ---- THE PIECE CUTTER: rows in rank order -> pieces in the existing format ----
struct PieceCutter {
    struct BandRun { uint8_t c; uint32_t len; u64 head, tail; };
    struct BandJob { u64 band, firstRun; std::vector<BandRun> runs; std::vector<char> gmark; };
    const std::string prefix, dir;
    const u64 n, window;
    const unsigned width;
    const u64 bandSize = 65536;
    // ordered pass state (mirrors the emission sink exactly)
    u64 next = 0, rows = 0, index = 0, bandStart = 0;
    struct Pending { uint8_t c = 0; u64 len = 0, head = 0, tail = 0; } pending;
    std::vector<BandRun> band;
    u64 pendHeadRank = 0, bandBaseRank = 0, bandCumLen = 0;
    std::vector<u64> runExc;
    u64 excRuns = 0;
    std::array<u64, 256> counts{}, frequencies{};
    // writer pool
    std::mutex mu;
    std::condition_variable cvJob, cvSlot;
    std::deque<BandJob> jobs;
    std::vector<std::thread> writers;
    u64 slotsFree = 0, writersUsed = 0;
    bool stop = false;
    double tFlow = 0, tWrite = 0;

    PieceCutter(const std::string& p, u64 nRows, u64 threadCount)
        : prefix(p), dir(p + ".pieces"), n(nRows), window(v2_piece_window()),
          width(unsigned(64 - __builtin_clzll(nRows))) {
        require(::mkdir(dir.c_str(), 0777) == 0, "create pieces directory (it must be fresh)");
        {
            OutFile f(dir + "/start.tmp");
            f.raw("BPS1", 4);
            f.w64(n); f.w64(width); f.w64(1); f.w64(window);
        }
        require(::rename((dir + "/start.tmp").c_str(), (dir + "/start").c_str()) == 0,
                "publish band stream start");
        writersUsed = std::min<u64>(32, std::max<u64>(1, threadCount / 3));
        slotsFree = std::max<u64>(2, 2 * writersUsed);
        if (slotsFree > window) slotsFree = window;
        for (u64 i = 0; i < writersUsed; ++i) writers.emplace_back([this] { writerLoop(); });
    }
    ~PieceCutter() {
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cvJob.notify_all();
        for (auto& t : writers) if (t.joinable()) t.join();
    }

    void feedRow(u64 p, uint8_t c) {
        const u64 row = next;
        if (pending.len && pending.c != c) flushRun();
        if (!pending.len) {
            pending = Pending{c, 1, p, p};
            pendHeadRank = row;
        } else { pending.tail = p; ++pending.len; }
        ++next; ++rows;
    }
    void flushRun() {
        if (!pending.len) return;
        {
            const u64 predicted = bandBaseRank + bandCumLen;
            if (predicted != pendHeadRank) {
                runExc.push_back(pending.head);
                runExc.push_back(pendHeadRank);
                runExc.push_back(pendHeadRank + pending.len - 1);
                ++excRuns;
            }
            bandCumLen += pending.len;
        }
        counts[pending.c] += pending.len;
        ++frequencies[pending.c];
        require(pending.len == u64(uint32_t(pending.len)), "run length exceeds the piece field");
        band.push_back(BandRun{pending.c, uint32_t(pending.len), pending.head, pending.tail});
        ++index;
        pending = Pending{};
        if (index % bandSize == 0) closeBand();
    }
    static void push64(std::vector<char>& b, u64 v) {
        for (int i = 0; i < 8; ++i) b.push_back(char(v >> (8 * i)));
    }
    void closeBand() {
        const u64 count = band.size();
        require(count && count <= bandSize, "piece cutter band count");
        const u64 bandNo = bandStart / bandSize;
        require(bandStart == bandNo * bandSize, "band alignment");
        require(bandStart + count == index, "band run coverage");
        std::vector<char> gmark;
        gmark.insert(gmark.end(), {'B', 'P', 'G', '1'});
        push64(gmark, bandNo); push64(gmark, count);
        push64(gmark, 0); push64(gmark, 0);   // refs = 0, classes = 0 (no seam classes)
        BandJob job;
        job.band = bandNo; job.firstRun = bandStart;
        job.runs.swap(band);
        job.gmark = std::move(gmark);
        bandBaseRank = next;
        bandCumLen = 0;
        {
            std::unique_lock<std::mutex> lk(mu);
            cvSlot.wait(lk, [&] { return slotsFree > 0; });
            --slotsFree;
        }
        const double t0 = now_sec();
        v2_flow_wait(prefix, bandNo, window);
        tFlow += now_sec() - t0;
        {
            std::lock_guard<std::mutex> lk(mu);
            jobs.push_back(std::move(job));
        }
        cvJob.notify_one();
        bandStart = index;
    }
    void writerLoop() {
        for (;;) {
            BandJob job;
            {
                std::unique_lock<std::mutex> lk(mu);
                cvJob.wait(lk, [&] { return stop || !jobs.empty(); });
                if (jobs.empty()) { require(stop, "cutter writer wake without work"); return; }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            const double t0 = now_sec();
            const u64 rc = job.runs.size();
            std::vector<uint8_t> jchars(rc);
            std::vector<uint32_t> jlens(rc);
            std::vector<u64> jheads(rc), jtails(rc);
            for (u64 i = 0; i < rc; ++i) {
                jchars[i] = job.runs[i].c;
                jlens[i] = job.runs[i].len;
                jheads[i] = job.runs[i].head;
                jtails[i] = job.runs[i].tail;
            }
            v2_write_piece(dir, job.band, 0, job.firstRun, jchars.data(), jlens.data(),
                           jheads.data(), jtails.data(), rc, width);
            const std::string tmp = dir + "/g-tmp-" + std::to_string(job.band);
            const std::string fin = dir + "/g-" + std::to_string(job.band);
            require(!std::filesystem::exists(fin), "band progress exists; refusing to clobber");
            int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            require(fd >= 0, "create band progress");
            xput(fd, job.gmark.data(), job.gmark.size(), 0, "band progress write");
            require(::close(fd) == 0, "close band progress");
            require(::rename(tmp.c_str(), fin.c_str()) == 0, "publish band progress");
            {
                std::lock_guard<std::mutex> lk(mu);
                tWrite += now_sec() - t0;
                ++slotsFree;
            }
            cvSlot.notify_one();
        }
    }
    void finish() {
        if (next != n)
            die("piece cutter did not see every row: rows_completed=" +
                std::to_string(next) + " expected=" + std::to_string(n));
        flushRun();
        if (band.size()) closeBand();
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
        }
        cvJob.notify_all();
        for (auto& t : writers) if (t.joinable()) t.join();
        writers.clear();
        {
            OutFile f(dir + "/terminal.tmp");
            f.raw("BPT1", 4);
            f.w64((index - 1) / bandSize); f.w64(index); f.w64(0); f.w64(0);
        }
        require(::rename((dir + "/terminal.tmp").c_str(), (dir + "/terminal").c_str()) == 0,
                "publish validated terminal marker");
    }
    u64 writeRunExc(const std::string& path) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        require(fd >= 0, "create run-rank exception list");
        u64 h[2] = {0x45525853ull, u64(runExc.size() / 3)};
        xput(fd, h, 16, 0, "runexc header");
        if (!runExc.empty()) xput(fd, runExc.data(), runExc.size() * sizeof(u64), 16, "runexc body");
        require(::fsync(fd) == 0, "sync runexc");
        ::close(fd);
        return u64(runExc.size() / 3);
    }
};

// ===========================================================================
// MAIN: the four components wired together.
// ===========================================================================
int main(int argc, char** argv) {
    // THE CHUNK ALLOCATOR POLICY: follow-up group windows are ~40 KB vectors -
    // below glibc's 128 KB mmap threshold they are heap allocations, and freed
    // into per-thread arenas that NEVER return the bytes (measured: +5.5 GB of
    // retained anon RSS over the bound's live bytes on mixed, anon 9.6 GB
    // against bound.live 4.1 GB). A low mmap threshold makes every chunk-sized
    // allocation a mapping, so a freed chunk is a munmap and the RSS receipt
    // tracks the bound again.
    ::mallopt(M_MMAP_THRESHOLD, 32768);
    ::mallopt(M_TRIM_THRESHOLD, 131072);
    std::string corpus, remapPath, outPrefix, bandsDir;
    u64 budget_bytes = 0, scratch_budget_bytes = 0;
    double memory_gb = 0.0;
    u64 threads = 32, unit_target_rows = 100000000ull, sample_n = 4000000;
    u64 wave_key_bytes = 0;
    std::fprintf(stderr, "tape_sort: THE TAPE READER (TAPE_READER.md) - the pull machine\n");
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() {
            if (i + 1 >= argc) die("missing value for " + a);
            return std::string(argv[++i]);
        };
        if (a == "--corpus") corpus = nxt();
        else if (a == "--remap") remapPath = nxt();
        else if (a == "--out-prefix") outPrefix = nxt();
        else if (a == "--bands") bandsDir = nxt();
        else if (a == "--budget-bytes") budget_bytes = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--memory-gb") memory_gb = std::atof(nxt().c_str());
        else if (a == "--scratch-budget-bytes")
            scratch_budget_bytes = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--threads") threads = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--unit-target-rows")
            unit_target_rows = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--sample") sample_n = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--wave-key-bytes") wave_key_bytes = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--wave-run-cap") g_wave_run_cap = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--wave-fallback-walk-max") {
            g_wave_fallback_walk_max = std::strtoull(nxt().c_str(), nullptr, 10);
            g_wave_fallback_walk_set = true;
        }
        else die("unknown arg " + a);
    }
    if (corpus.empty() || outPrefix.empty() || bandsDir.empty())
        die("--corpus, --out-prefix and --bands are required");
    // ---- the knobs (env = the route `xsa build` travels; flags win) ----
    {
        std::string src = "default";
        if (!wave_key_bytes) {
            if (const char* e = ::getenv("XSA_WAVE_KEY_BYTES")) {
                wave_key_bytes = std::strtoull(e, nullptr, 10);
                src = "env";
            }
        }
        if (const char* e = ::getenv("XSA_WAVE_KEY_RUN_CAP"))
            g_wave_run_cap = std::strtoull(e, nullptr, 10);
        if (const char* e = ::getenv("XSA_TIE_WINDOW_BYTES"))
            g_tie_window_bytes = std::strtoull(e, nullptr, 10);
        if (const char* e = ::getenv("XSA_PARSE_PLANE_MAX"))
            g_parse_plane_max = std::strtoull(e, nullptr, 10);
        if (const char* e4 = ::getenv("XSA_MEM_GUARD_MB")) {
            g_mem_guard_mb = std::strtoull(e4, nullptr, 10);
            if (g_mem_guard_mb && g_mem_guard_mb < 1024)
                die("XSA_MEM_GUARD_MB must be 0 (off) or >= 1024");
        }
        if (const char* e2 = ::getenv("XSA_PARSE_DICT_BYTES")) {
            g_parse_dict_allowance = std::strtoull(e2, nullptr, 10);
            if (g_parse_dict_allowance < 4096 || g_parse_dict_allowance > (8ull << 30))
                die("XSA_PARSE_DICT_BYTES must be in [4KiB, 8GiB]");
        }
        if (const char* e = ::getenv("XSA_PARSE_PHRASES"))
            g_parse_phrases = std::strtoull(e, nullptr, 10);
        if (const char* e = ::getenv("XSA_PARSE_ROUNDS"))
            g_parse_rounds = std::strtoull(e, nullptr, 10);
        if (const char* e = ::getenv("XSA_PFP_K"))
            g_pfp_k = std::strtoull(e, nullptr, 10);
        if (const char* e = ::getenv("XSA_PFP_W"))
            g_pfp_w = std::strtoull(e, nullptr, 10);
        if (g_parse_plane_max < (1u << 20) || g_parse_phrases < 4 ||
            g_parse_phrases > (1u << 20) || g_parse_rounds < 1 || g_parse_rounds > 16 ||
            g_pfp_k < 2 || g_pfp_k > 32 || g_pfp_w < 2 || g_pfp_w > 64)
            die("level-3 knobs out of range (XSA_PARSE_PLANE_MAX/PHRASES/ROUNDS, "
                "XSA_PFP_K/W)");
        if (const char* e = ::getenv("XSA_WAVE_FALLBACK_WALK_MAX")) {
            g_wave_fallback_walk_max = std::strtoull(e, nullptr, 10);
            g_wave_fallback_walk_set = true;
        }
        if (!wave_key_bytes) wave_key_bytes = 8;
        if (wave_key_bytes < 1 || wave_key_bytes > 1024)
            die("--wave-key-bytes must be in [1,1024]");
        if (g_wave_run_cap < 2 || g_wave_run_cap > (1u << 22))
            die("--wave-run-cap must be in [2,4194304]");
        if (g_tie_window_bytes < 64 || g_tie_window_bytes > (1u << 22))
            die("XSA_TIE_WINDOW_BYTES must be in [64,4194304]");
        g_wave_key_bytes = wave_key_bytes;
        std::fprintf(stderr,
            "SS_WAVE_KEY_KNOB key_bytes=%" PRIu64 " run_cap=%" PRIu64
            " tie_window=%" PRIu64 " fallback_walk_max=%" PRIu64 " source=%s"
            " parse_plane_max=%" PRIu64 " parse_phrases=%" PRIu64
            " parse_rounds=%" PRIu64 " pfp_k=%" PRIu64 " pfp_w=%" PRIu64
            " rule=run_digit;big_window;parse_for_deep_classes;LCE_residual\n",
            g_wave_key_bytes, g_wave_run_cap, g_tie_window_bytes,
            g_wave_fallback_walk_max, src.c_str(), g_parse_plane_max,
            g_parse_phrases, g_parse_rounds, g_pfp_k, g_pfp_w);
    }
    const bool trieTap = ::getenv("TRIE_TAP") != nullptr;
    const u64 lcpSliceWindow = [] {
        const char* e = ::getenv("SS_LCP_SLICE_WINDOW");
        const u64 v = (e && *e) ? std::strtoull(e, nullptr, 10) : 8;
        if (v < 1 || v > 4096) die("SS_LCP_SLICE_WINDOW must be in [1,4096]");
        return v;
    }();
    const bool lcpOn = true;   // the product arm: per-unit LCP slices for the finish

    // ---- the corpus ----
    int fd = ::open(corpus.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("cannot open/stat corpus");
    FILE_N = u64(st.st_size);
    if (!FILE_N) die("empty corpus");
    N = FILE_N + 1;
    KEY_N = FILE_N >= 24 ? FILE_N - 23 : 0;
    if (!g_wave_fallback_walk_set)
        g_wave_fallback_walk_max = std::max<u64>(4ull << 30, 64ull * FILE_N);
    T = (const u8*)::mmap(nullptr, size_t(FILE_N), PROT_READ, MAP_PRIVATE, fd, 0);
    if (T == MAP_FAILED) die("mmap corpus");
    std::vector<u8> remapBuf;
    if (!remapPath.empty()) {
        int rf = ::open(remapPath.c_str(), O_RDONLY);
        if (rf < 0) die("cannot open remap");
        remapBuf.resize(256);
        if (::read(rf, remapBuf.data(), 256) != 256) die("remap short read");
        ::close(rf);
        REMAP = remapBuf.data();
    }
    bool used[256]{};
    for (u64 p = 0; p < FILE_N; ++p) used[T[p]] = true;
    unsigned present = 0;
    for (unsigned c = 0; c < 256; ++c) present += used[c];
    if (REMAP) {
        bool seen[256]{};
        int previous = -1;
        for (unsigned c = 0; c < 256; ++c) {
            if (seen[REMAP[c]]) die("remap must be a permutation");
            seen[REMAP[c]] = true;
            if (used[c]) {
                if (int(REMAP[c]) <= previous) die("remap does not preserve byte order");
                previous = REMAP[c];
            }
        }
    }
    // The finish reserves cell zero for the virtual END (carried over).
    for (unsigned c = 0; c < 256; ++c)
        if (used[c] && (REMAP ? REMAP[c] : c) == 0)
            die("tape consumer reserves cell zero; virtual END consumer required");
    build_pairmap();
    std::fprintf(stderr, "END_SYMBOL mode=virtual position=%" PRIu64 " multiplicity=1 rank=0\n", FILE_N);
    std::fprintf(stderr, "END_FORMAT k=%u ends_in_sentinel=1 end_code=none\nTAIL_GROUP rows=%" PRIu64 "\n",
                 present, N - KEY_N);
    std::fprintf(stderr, "SS_LCP_ARM lcp_source=%s slices=1\n",
                 lcpOn ? "sorter-units" : "none");

    // ---- THE ENVELOPE and the bound's shape ----
    const u64 envelope_bytes = budget_bytes ? budget_bytes : u64(memory_gb * 1e9);
    if (!envelope_bytes) die("--budget-bytes (or --memory-gb) must be positive");
    const u64 recBytes = unit_rec_bytes(g_wave_key_bytes);          // 8 + K + 6
    const u64 sortDelta = recBytes + 24;   // ping-pong + lcpv + slot planes
    const u64 emitHold = 17;               // P + lcpv + the slice's u8 share
    const u64 workReserve =
        threads * (SS_SCAN_CHUNK + (1u << 22)) + (256u << 20) + g_tie_window_bytes * 64;
    constexpr u64 SS_SAFETY_PCT = 85;
    const u64 capBytes = (envelope_bytes > workReserve)
        ? (envelope_bytes - workReserve) / 100 * SS_SAFETY_PCT : 0;
    if (!capBytes) die("TAPE_ENVELOPE_TOO_SMALL: envelope=" + std::to_string(envelope_bytes) +
                       " work_reserve=" + std::to_string(workReserve));
    if (N >= (1ull << 48)) die("positions must fit the packed row's 48 bits");

    // ---- THE SURVEY (one pass) ----
    RssMonitor rss;
    rss.start();
    Survey S;
    survey_run(S, fd, FILE_N, sample_n, std::max<u64>(1, threads));
    std::vector<u64> hist(256, 0);
    for (u64 c = 0; c < S.chunks; ++c)
        for (unsigned b = 0; b < 256; ++b) hist[b] += S.cbin[size_t(c) * 256 + b];
    ++hist[SENTINEL_SYMBOL];
    {
        u64 total = 0;
        for (unsigned b = 0; b < 256; ++b) total += hist[b];
        if (total != N)
            die("SS_SURVEY_COUNT: the survey's exact chunk counts sum to " +
                std::to_string(total) + " against N=" + std::to_string(N));
        std::fprintf(stderr, "SS_SURVEY_COUNTS sum=%" PRIu64 " n=%" PRIu64 " chunks=%" PRIu64
                     " rule=exact_per_chunk_per_top_level_bin\n", total, N, S.chunks);
    }
    madv_release(T, FILE_N);

    // ---- the sample + the tail run ----
    const u64 stride = S.stride;
    std::vector<u64> sk;
    sk.reserve(KEY_N / stride + 2);
    for (u64 p = 0; p < KEY_N; p += stride) sk.push_back(p);
    if (sk.size() > S.samples) die("SS_SURVEY_SAMPLE: the stride sample is longer than the survey's");
    u64 trailing = FILE_N;
    if (FILE_N) {
        const unsigned last = REMAP ? REMAP[T[FILE_N - 1]] : T[FILE_N - 1];
        while (trailing && unsigned(REMAP ? REMAP[T[trailing - 1]] : T[trailing - 1]) == last)
            --trailing;
    }

    // ---- THE PLAN: the whole partition, up front ----
    const double tPlan = now_sec();
    UnitPlan UP = plan_units(S, hist, sk, stride, trailing, unit_target_rows,
                             std::max<u64>(1, threads), capBytes, recBytes, sortDelta,
                             emitHold);
    std::deque<Unit>& units = UP.units;
    const u64 nUnits = units.size();
    // THE PLAN-TIME REFUSAL (numbers, no I/O): a unit whose chunk cannot fit
    // at the maximum key length is a knob error.
    for (u64 u = 0; u < nUnits; ++u) {
        const u64 need = units[size_t(u)].rowsEst * (recBytes + sortDelta);
        if (need > capBytes) {
            std::fprintf(stderr,
                "SS_TAPE_CONFIG unit=%" PRIu64 " rows_est=%" PRIu64 " need_bytes=%" PRIu64
                " cap_bytes=%" PRIu64 " rec_bytes=%" PRIu64 " sort_delta=%" PRIu64 "\n",
                u, units[size_t(u)].rowsEst, need, capBytes, recBytes, sortDelta);
            die("TAPE_UNIT_TOO_BIG: unit " + std::to_string(u) + " cannot fit the envelope at "
                "the maximum key length (" + std::to_string(need) + " B of " +
                std::to_string(capBytes) + "). THE LEVERS: raise --budget-bytes, or lower "
                "--unit-target-rows so the plan cuts smaller units");
        }
    }
    std::fprintf(stderr,
        "SS_TAPE units=%" PRIu64 " unit_target_rows=%" PRIu64 " n=%" PRIu64
        " chunk_plane_bytes_total=%" PRIu64 " rec_bytes=%" PRIu64 " sort_delta=%" PRIu64
        " emit_hold=%" PRIu64 "\n", nUnits, unit_target_rows, N, N * recBytes,
        recBytes, sortDelta, emitHold);
    std::fprintf(stderr,
        "SS_TAPE_BUDGET budget_bytes=%" PRIu64 " scratch_budget_bytes=%" PRIu64
        " memory_gb=%.2f cap_bytes=%" PRIu64 " work_reserve_bytes=%" PRIu64
        " safety_pct=%" PRIu64 " unit_target_rows=%" PRIu64 " key_bytes=%" PRIu64
        " rec_bytes=%" PRIu64 " bytes_per_row=%" PRIu64 " units=%" PRIu64
        " piece_window=%" PRIu64 " lcp_slice_window=%" PRIu64 " threads=%" PRIu64
        " rule=outstanding_chunk_bytes<=envelope-working_reserve;plan_refused_before_io\n",
        envelope_bytes, scratch_budget_bytes, memory_gb, capBytes, workReserve,
        u64(SS_SAFETY_PCT), unit_target_rows, g_wave_key_bytes, recBytes,
        recBytes + sortDelta, nUnits, v2_piece_window(), lcpSliceWindow, threads);
    // the survey's blob is dead weight from here: the plan answered everything
    bswap_release(S.pre);
    bswap_release(S.cbin);
    bswap_release(sk);

    // ---- the pieces output tape + the pieces dir must be fresh ----
    {
        struct stat ts{};
        if (::stat(bandsDir.c_str(), &ts) != 0 || !S_ISDIR(ts.st_mode))
            die("the bands directory must exist and be fresh");
        if (::access((bandsDir + "/producer.done").c_str(), F_OK) == 0)
            die("stale producer.done in the bands directory");
    }
    PieceCutter cutter(outPrefix, N, threads);

    // ---- THE TAPE, THE BOUND, THE SORTERS, THE EMITTER ----
    Tape tape;
    tape.fd = fd;
    std::atomic<u64> unparked{0};
    std::atomic<bool> admitting{false};
    tape.unparked = &unparked;
    tape.admitting = &admitting;
    Bound bound;
    bound.cap = capBytes;

    // the follow-up requests of one in-flight unit are tracked globally (fuAll)
    // and freed by the sorter when the unit's sort completes.

    const u64 nSorters = std::max<u64>(1, threads);
    std::atomic<u64> nextSort{0};
    std::vector<std::thread> sorterThreads;
    // every follow-up request a sorter issues is owned per UNIT and freed once
    // the unit's sort completes (the chunk is the radix's window plane).
    std::mutex fuMu;
    std::vector<TapeRequest*> fuAll;

    auto sorterLoop = [&](u64 tid) {
        std::vector<u8> qpad;
        for (;;) {
            const u64 u = nextSort.fetch_add(1);
            if (u >= nUnits) break;
            Unit& U = units[size_t(u)];
            // wait for the unit's initial request to be served
            while (!U.served.load(std::memory_order_acquire)) ::usleep(10000);
            const u64 rows = U.chunk->rows();
            // THE SORTERS REQUEST MORE DATA ONLY WHEN THEY HAVE CAPACITY
            U.boundSortBytes = sortDelta * std::max<u64>(1, rows);
            bound.wait_for(U.boundSortBytes, u, "sort", rows);
            unparked.fetch_add(1);
            WaveKeyStats st;
            std::vector<u64> lcpv;
            auto enqueue_followup = [&](FollowUp& fu) {
                TapeRequest* rq = new TapeRequest();
                rq->positional = true;             // the rows are KNOWN
                rq->positions = fu.positions;       // exact: no matcher entry
                rq->kbase = fu.prefix.size();       // the window's offset
                rq->keyLen = fu.key_len;
                rq->withRun = false;
                rq->sliced = U.sliced;
                rq->posLo = U.posLo; rq->posHi = U.posHi;
                rq->unitId = u;
                rq->parse = fu.parse;
                rq->phraseBudget = fu.phraseBudget;
                rq->round = fu.round;
                rq->chunk = new Chunk();
                rq->chunk->parse = fu.parse;
                rq->chunk->phraseBudget = fu.phraseBudget;
                rq->capRows = fu.rowsExpected;   // exact: prefix + run filter
                rq->chunk->key_len = fu.key_len;
                rq->chunk->P.reserve(size_t(fu.rowsExpected));
                if (fu.parse) {
                    rq->chunk->ids.reserve(size_t(fu.rowsExpected) *
                                           size_t(fu.phraseBudget));
                    rq->chunk->off0.reserve(size_t(fu.rowsExpected));
                } else {
                    rq->chunk->Kb.reserve(size_t(fu.rowsExpected) *
                                          size_t(fu.key_len));
                }
                // THE CHUNK IS EXACT (prefix + run filter), so the asker
                // prices it exactly and waits for capacity here - PARKED while
                // blocked, so the tape's pass fixpoint still fires.
                const u64 plane = fu.parse
                    ? fu.rowsExpected * (fu.phraseBudget * 4 + 8) +
                      64 + g_parse_dict_allowance
                    : fu.rowsExpected * (fu.key_len + 8) + 64;
                // (parse) the dict allowance is a small HEADROOM GATE: the
                // actual dictionary bytes are BILLED BY THE CAPTURE as they
                // intern (r->dictBilled), so the bound tracks the real plane
                // and free_followup returns exactly what was taken.
                unparked.fetch_sub(1);
                bound.wait_for(plane, u, "tie-window", fu.rowsExpected);
                unparked.fetch_add(1);
                rq->slabRows = std::max<u64>(1, fu.rowsExpected);
                rq->runFilter = fu.runFilter;
                rq->filterBase = fu.filterBase;
                rq->runSigma = fu.runSigma;
                rq->runL = fu.runL;
                rq->runT = fu.runT;
                fu.chunk = rq->chunk;
                fu.rq = rq;
                {
                    std::lock_guard<std::mutex> lk(fuMu);
                    fuAll.push_back(rq);
                }
                tape.enqueue(rq);
            };
            auto wait_followup = [&](FollowUp& fu) {
                TapeRequest* rq = (TapeRequest*)fu.rq;
                unparked.fetch_sub(1);
                u64 sp0 = 0;
                while (!rq->served.load(std::memory_order_acquire)) {
                    ::usleep(10000);
                    if (++sp0 % 500 == 0) beat(1);   // a parked wait, visible
                }
                unparked.fetch_add(1);
            };
            auto free_followup = [&](FollowUp& fu) {
                TapeRequest* rq = (TapeRequest*)fu.rq;
                bound.sub(fu.rowsExpected * (fu.key_len + 8) + 64 +
                          rq->dictBilled);
                delete rq->chunk;
                delete rq;
                rq = nullptr;
            };
            {
                const u8* pre = U.preLen ? UP.preArena.data() + U.preOff : nullptr;
                const double ts0 = now_sec();
                wavekey_sort_unit(U.chunk->P, U.chunk->Kb, U.chunk->Lp, U.chunk->Tp,
                                  U.preLen, u, pre, U.preLen, U.posLo, U.posHi,
                                  enqueue_followup, wait_followup, free_followup,
                                  lcpv, st);
                // ---- THE TAILS (positions [KEY_N, N)): sorted directly and
                // merged into the ordered rows (carried over).
                if (KEY_N < N) {
                    std::vector<u64> tailP;
                    qpad.resize(UP.maxDepth + 8);
                    for (u64 p = KEY_N; p < N; ++p) {
                        const u64 avail = p < FILE_N ? FILE_N - p : 0;
                        for (u64 i = 0; i < U.depth; ++i)
                            qpad[size_t(i)] = i < avail
                                ? u8(REMAP ? REMAP[T[p + i]] : T[p + i])
                                : SENTINEL_SYMBOL;
                        bool hit = true;
                        for (u64 i = 0; i + 1 < U.depth; ++i)
                            if (qpad[size_t(i)] != pre[i]) { hit = false; break; }
                        if (!hit) continue;
                        const unsigned cE = qpad[size_t(U.depth - 1)];
                        if (cE < U.bLo || cE >= U.bHi) continue;
                        if (U.sliced && (p < U.posLo || p >= U.posHi)) continue;
                        tailP.push_back(pack_sym(p, p == 0 ? SENTINEL_SYMBOL
                                                  : u8(REMAP ? REMAP[T[p - 1]] : T[p - 1])));
                    }
                    if (!tailP.empty()) {
                        std::sort(tailP.begin(), tailP.end(), [](u64 a, u64 b) {
                            return suf_cmp_from(unpack_pos(a), unpack_pos(b), 0) < 0;
                        });
                        for (const u64 tp : tailP) {
                            const u64 pos = unpack_pos(tp);
                            const size_t i = size_t(std::lower_bound(
                                U.chunk->P.begin(), U.chunk->P.end(), pos,
                                [](u64 a, u64 t) {
                                    return suf_cmp_from(unpack_pos(a), t, 0) < 0;
                                }) - U.chunk->P.begin());
                            U.chunk->P.insert(U.chunk->P.begin() + ptrdiff_t(i), tp);
                            lcpv.insert(lcpv.begin() + ptrdiff_t(i), 0);
                            u64 lcp = 0;
                            if (i) suf_cmp_lcp(unpack_pos(U.chunk->P[i - 1]), pos, 0, lcp);
                            lcpv[i] = lcp;
                            if (i + 1 < U.chunk->P.size()) {
                                suf_cmp_lcp(pos, unpack_pos(U.chunk->P[i + 1]), 0, lcp);
                                lcpv[i + 1] = lcp;
                            }
                            if (i == 0) lcpv[0] = 0;
                        }
                    }
                }
                {
                    u64 anon=0,file=0,tot=0;
                    RssMonitor::sample(anon,file,tot);
                    std::fprintf(stderr,
                        "SS_UNIT unit=%" PRIu64 " rows=%" PRIu64 " sort_s=%.2f rounds=%" PRIu64
                        " tie_rows=%" PRIu64 " tie_groups=%" PRIu64 " fallback_rows=%" PRIu64
                        " anon_mb=%" PRIu64 " bound_live_mb=%" PRIu64 "\n",
                        u, U.chunk->P.size(), now_sec() - ts0, st.rounds, st.tie_rows,
                        st.tie_groups, st.fallback_rows, anon/1024, bound.live/1048576);
                }
            }
            WAVE_KEY.add(st);
            // hand off to the emitter and release the sort's extra planes
            U.P = std::move(U.chunk->P);
            U.lcpv = std::move(lcpv);
            bswap_release(U.chunk->Kb);
            bswap_release(U.chunk->Lp);
            bswap_release(U.chunk->Tp);
            const u64 hold = emitHold * std::max<u64>(1, U.P.size()) + 16 * 1024;
            bound.sub(U.boundWaitBytes + U.boundSortBytes);
            bound.add(hold, u, "emit-hold", U.P.size());
            U.pub.store(U.P.empty() ? 1ull : (unpack_pos(U.P.back()) + 2),
                        std::memory_order_release);
            U.sorted.store(true, std::memory_order_release);
            unparked.fetch_sub(1);
        }
    };

    // ---- THE EMITTER THREAD: one thread, in unit order ----
    std::atomic<u64> emittedRows{0};
    u64 lcpSlices = 0, lcpSliceBytes = 0, lcpEscapes = 0, lcpMaxSliceBytes = 0;
    u64 lcpLiveBytes = 0, lcpPeakLiveBytes = 0, lcpPublished = 0;
    std::deque<u64> lcpLive;
    u64 endRows = 0;
    std::thread emitterThread([&] {
        const std::string stopFlag = bandsDir + "/../stop.requested";
        u64 prevLast = ~0ull;
        for (u64 u = 0; u < nUnits; ++u) {
            Unit& U = units[size_t(u)];
            while (!U.sorted.load(std::memory_order_acquire)) ::usleep(10000);
            if (::access(stopFlag.c_str(), F_OK) == 0) {
                std::fprintf(stderr, "STOP_OBSERVED phase=order-tape at_unit=%" PRIu64 "\n", u);
                int f = ::open((bandsDir + "/producer.interrupted").c_str(),
                               O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (f >= 0) ::close(f);
                ::_exit(75);
            }
            const u64 rows = U.P.size();
            const u64 rankBase = emittedRows.load();
            // ---- THE SEAM: LCP with the previous unit's last row ----
            if (rows && prevLast != ~0ull) {
                u64 cross = 0;
                suf_cmp_lcp(prevLast, unpack_pos(U.P.front()), 0, cross);
                U.lcpv[0] = cross;
            }
            std::vector<std::pair<u64, u64>> esc;
            std::vector<u8> bandLcp(size_t(rows), 0);
            for (u64 i = 1; i < rows; ++i) {
                const u64 val = U.lcpv[size_t(i)];
                if (val >= 255) esc.emplace_back(rankBase + i, val);
                else bandLcp[size_t(i)] = uint8_t(val);
            }
            if (rows && prevLast != ~0ull) {
                bandLcp[0] = uint8_t(std::min<u64>(U.lcpv[0], 255));
                if (U.lcpv[0] >= 255) esc.emplace_back(rankBase, U.lcpv[0]);
            }
            std::sort(esc.begin(), esc.end());
            // ---- THE LCP SLICE (the finish's unchanged contract) ----
            if (u >= lcpSliceWindow) {
                const std::string want = bandsDir + "/band_" + std::to_string(u - lcpSliceWindow) +
                                         ".lcp.consumed";
                while (::access(want.c_str(), F_OK) != 0) ::usleep(10000);
                lcpLiveBytes -= lcpLive.front();
                lcpLive.pop_front();
            }
            {
                u64 maxL = 0;
                for (const auto& e : esc) maxL = std::max(maxL, e.second);
                const std::string tmp = bandsDir + "/band_" + std::to_string(u) + ".lcp.tmp";
                const std::string dst = bandsDir + "/band_" + std::to_string(u) + ".lcp";
                int lf = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (lf < 0) die("create lcp slice " + tmp);
                u8 h[48];
                std::memset(h, 0, sizeof h);
                const uint32_t magic = 0x534C5853u, ver = 1;
                std::memcpy(h + 0, &magic, 4);
                std::memcpy(h + 4, &ver, 4);
                const u64 rowsN = rows, escN = esc.size(), flags = 1;
                std::memcpy(h + 8, &rowsN, 8);
                std::memcpy(h + 16, &escN, 8);
                std::memcpy(h + 24, &maxL, 8);
                std::memcpy(h + 32, &rankBase, 8);
                std::memcpy(h + 40, &flags, 8);
                xput(lf, h, 48, 0, "lcp slice header");
                if (rows) xput(lf, bandLcp.data(), rows, 48, "lcp slice body");
                for (size_t ei = 0; ei < esc.size(); ++ei) {
                    const u64 pair[2] = {esc[ei].first, esc[ei].second};
                    xput(lf, pair, 16, 48 + rows + 16 * ei, "lcp slice escapes");
                }
                if (::close(lf)) die("close lcp slice");
                if (::rename(tmp.c_str(), dst.c_str()) != 0) die("publish lcp slice");
                int df = ::open((dst + ".done").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (df < 0) die("publish lcp done marker");
                ::close(df);
            }
            const u64 sliceBytes = rows + esc.size() * 16;
            ++lcpSlices;
            lcpSliceBytes += sliceBytes;
            lcpEscapes += esc.size();
            if (sliceBytes > lcpMaxSliceBytes) lcpMaxSliceBytes = sliceBytes;
            ++lcpPublished;
            lcpLive.push_back(sliceBytes);
            lcpLiveBytes += sliceBytes;
            lcpPeakLiveBytes = std::max(lcpPeakLiveBytes, lcpLiveBytes);
            // ---- THE TRIE TAP'S BAND STREAM (only when armed) ----
            if (trieTap) {
                const std::string saPath = bandsDir + "/band_" + std::to_string(u) + ".sa";
                int bf = ::open(saPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (bf < 0) die("create band " + saPath);
                size_t off = 0;
                const size_t bytes = U.P.size() * 8;
                while (off < bytes) {
                    ssize_t wr = ::write(bf, (const char*)U.P.data() + off, bytes - off);
                    if (wr <= 0) die("write band");
                    off += size_t(wr);
                }
                if (::close(bf)) die("close band");
                int of = ::open((saPath + ".opened").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (of < 0) die("publish opened marker");
                ::close(of);
            }
            // ---- THE PIECES (the existing format, appended in unit order) ----
            for (const u64 pk : U.P) {
                const uint8_t c = uint8_t(pk >> 48);
                if (c == 0) ++endRows;
                cutter.feedRow(unpack_pos(pk), c);
            }
            emittedRows.fetch_add(rows);
            if (rows) prevLast = unpack_pos(U.P.back());
            bound.sub(emitHold * std::max<u64>(1, rows) + 16 * 1024);
            U.emitted.store(true, std::memory_order_release);
            bswap_release(U.P);
            bswap_release(U.lcpv);
        }
    });

    // ---- spawn everything; admission runs on the main thread ----
    std::thread tapeThread([&] { tape.threadLoop(); });
    for (u64 t = 0; t < nSorters; ++t)
        sorterThreads.emplace_back([&, t] { sorterLoop(t); });
    const double tRun = now_sec();
    {
        u64 admitted = 0;
        admitting.store(true, std::memory_order_release);
        while (admitted < nUnits) {
            Unit& U = units[size_t(admitted)];
            const u64 rowsCap = U.rowsEst + U.rowsEst / 4 + 65536;
            const u64 wait = recBytes * rowsCap;
            {
                std::lock_guard<std::mutex> lk(bound.mu);
                if (bound.live + wait > bound.cap) {
                    // blocked on bound space: the batch that fits is the batch
                    // that may be served; a later batch gets a later pass
                    admitting.store(false, std::memory_order_release);
                    ::usleep(10000);
                    admitting.store(true, std::memory_order_release);
                    continue;
                }
                bound.live += wait;
                if (bound.live > bound.peak) bound.peak = bound.live;
                bound.holding.push_back({admitted, "chunk", rowsCap, wait});
                U.boundWaitBytes = wait;
            }
            bound.cv.notify_all();
            U.chunk = new Chunk();
            U.chunk->reserve_rows(0, g_wave_key_bytes);
            U.chunk->P.reserve(size_t(rowsCap));
            U.chunk->Kb.reserve(size_t(rowsCap) * size_t(g_wave_key_bytes));
            U.chunk->Lp.reserve(size_t(rowsCap));
            U.chunk->Tp.reserve(size_t(rowsCap));
            TapeRequest* rq = new TapeRequest();
            rq->pre = U.preLen ? UP.preArena.data() + U.preOff : nullptr;
            rq->preLen = U.preLen;
            rq->bLo = U.bLo; rq->bHi = U.bHi;
            rq->kbase = U.preLen;
            rq->keyLen = g_wave_key_bytes;
            rq->withRun = true;
            rq->sliced = U.sliced;
            rq->posLo = U.posLo; rq->posHi = U.posHi;
            rq->chunk = U.chunk;
            rq->capRows = rowsCap;
            rq->unitId = admitted;
            rq->what = "unit";
            rq->servedTarget = &U.served;
            rq->billedRows = rowsCap;   // admission already committed the whole unit
            rq->billedBytes = 0;
            tape.enqueue(rq);
            ++admitted;
        }
        admitting.store(false, std::memory_order_release);
        tape.cvReq.notify_all();
    }
    for (auto& t : sorterThreads) t.join();
    emitterThread.join();
    tape.stopping.store(true);
    tape.cvReq.notify_all();
    tapeThread.join();

    // ---- the emitter's terminal receipts (the unchanged contracts) ----
    if (endRows != 1)
        die("TAPE_EMIT_END_ROWS: the BWT must carry EXACTLY one symbol-0 (END) row, got " +
            std::to_string(endRows));
    std::fprintf(stderr, "TAPE_EMIT_END_ROWS rows=%" PRIu64 " symbol=0 expected=1\n", endRows);
    cutter.finish();
    {
        const u64 nExc = cutter.writeRunExc(outPrefix + ".runexc");
        std::fprintf(stderr, "TAPE_EMIT_RUNEXC path=%s count=%" PRIu64 "\n",
                     (outPrefix + ".runexc").c_str(), nExc);
    }
    if (trieTap) {
        const std::string endPath = bandsDir + "/end";
        int ef = ::open(endPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (ef < 0) die("create tape end");
        std::string count = std::to_string(nUnits) + "\n";
        if (::write(ef, count.data(), count.size()) != (ssize_t)count.size())
            die("write tape end");
        ::close(ef);
        int rf = ::open((bandsDir + "/end.ready").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (rf < 0) die("publish end.ready");
        ::close(rf);
    }
    {
        int f = ::open((bandsDir + "/producer.done").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (f < 0) die("publish producer.done");
        ::close(f);
    }
    if (emittedRows.load() != N)
        die("TAPE_ROW_COVERAGE: emitted " + std::to_string(emittedRows.load()) +
            " rows against N=" + std::to_string(N));
    std::fprintf(stderr,
        "TAPE_EMIT_DONE rows=%" PRIu64 " runs=%" PRIu64 " seam_pass=none\n",
        N, cutter.index);

    // ---- the receipts (P1's denominators live here) ----
    rss.finish();
    const u64 peak_anon_kb = rss.peak_anon.load();
    const u64 peak_file_kb = rss.peak_file.load();
    const double anon_ratio = double(peak_anon_kb) * 1024.0 / double(envelope_bytes);
    noteFdPeak();
    std::fprintf(stderr,
        "SS_TAPE_PEAK peak_rss_kb=%" PRIu64 " budget_ratio=%.4f budget_bytes=%" PRIu64
        " peak_anon_kb=%" PRIu64 " peak_file_kb=%" PRIu64
        " bound_peak_bytes=%" PRIu64 " cap_bytes=%" PRIu64
        " rule=budget_ratio=owned_peak_rss/envelope,asserted<=0.90\n",
        u64(peak_anon_kb + peak_file_kb), anon_ratio, envelope_bytes, peak_anon_kb,
        peak_file_kb, bound.peak, capBytes);
    if (anon_ratio > 0.90)
        die("TAPE_BUDGET_VIOLATION: owned peak RSS is " + std::to_string(anon_ratio) +
            " of the envelope - the bound priced a different process than the one that ran");
    if (lcpSlices != nUnits) die("lcp slices do not cover the units");
    std::fprintf(stderr,
        "SS_LCP_STREAM mode=sorter-units slices=%" PRIu64 " rows=%" PRIu64
        " escapes=%" PRIu64 " bytes=%" PRIu64 " max_live_lcp_slice_bytes=%" PRIu64
        " slice_bytes_per_row=%.6f window=%" PRIu64 "\n",
        lcpSlices, emittedRows.load(), lcpEscapes, lcpSliceBytes, lcpPeakLiveBytes,
        double(lcpSliceBytes) / double(std::max<u64>(1, N)), lcpSliceWindow);
    std::fprintf(stderr,
        "PASS_TOTAL passes=%" PRIu64 " rows_routed=%" PRIu64 " bytes_read=%" PRIu64
        " idle_periods=%" PRIu64 " idle_s=%.2f corpus_bytes=%" PRIu64
        " rule=passes==1+tie_rounds_when_the_envelope_holds_every_unit\n",
        tape.passes, tape.rowsRouted, tape.bytesRead, tape.idlePeriods, tape.idleTotal,
        FILE_N);
    {
        u64 tieGroups = WAVE_KEY.tie_groups.load();
        std::fprintf(stderr,
            "TIE_TOTALS k=%" PRIu64 " tie_window=%" PRIu64 " groups=%" PRIu64
            " rows_tied=%" PRIu64 " window_bytes_total=%" PRIu64
            " corpus_x=%.6f residual_groups=%" PRIu64 " residual_rows=%" PRIu64
            " rule=level3_required_iff_extrapolated_window_bytes_total_exceeds_200GB\n",
            g_wave_key_bytes, g_tie_window_bytes, tieGroups, WAVE_KEY.tie_rows.load(),
            WAVE_KEY.tie_window_bytes.load(),
            FILE_N ? double(WAVE_KEY.tie_window_bytes.load()) / double(FILE_N) : 0.0,
            WAVE_KEY.tie_residual_groups.load(), WAVE_KEY.tie_residual_rows.load());
    }
    std::fprintf(stderr,
        "WAVE_KEY totals rows=%" PRIu64 " key_bytes=%" PRIu64 " tie_rows=%" PRIu64
        " rounds=%" PRIu64 " rounds_max=%" PRIu64 " extra_passes=%" PRIu64
        " fallback_rows=%" PRIu64 " no_shrink=%" PRIu64 " units=%" PRIu64
        " run_family0=%" PRIu64 " run_family_unknown=%" PRIu64 " run_family1=%" PRIu64
        " run_capped=%" PRIu64 " run_cap=%" PRIu64 "\n",
        WAVE_KEY.rows.load(), WAVE_KEY.key_bytes.load(), WAVE_KEY.tie_rows.load(),
        WAVE_KEY.rounds.load(), WAVE_KEY.rounds_max.load(), WAVE_KEY.extra_passes.load(),
        WAVE_KEY.fallback_rows.load(), WAVE_KEY.no_shrink.load(), WAVE_KEY.bands.load(),
        RUNKEY_FAM[0].load(), RUNKEY_FAM[1].load(), RUNKEY_FAM[2].load(),
        RUNKEY_CAPPED.load(), g_wave_run_cap);
    {
        unsigned long long rchar = 0, rb = 0, syscr = 0;
        { u64 r2 = 0, b2 = 0, s2 = 0; read_proc_io(r2, b2, s2); rchar = r2; rb = b2; syscr = s2; }
        std::fprintf(stderr,
            "SS_IO rchar=%llu read_bytes=%llu syscr=%llu corpus_bytes=%" PRIu64
            " rows=%" PRIu64 " rchar_per_corpus=%.4f read_bytes_per_corpus=%.4f"
            " passes=%" PRIu64
            " rule=rchar_asked;read_bytes_from_the_device\n",
            rchar, rb, syscr, FILE_N, N,
            FILE_N ? double(rchar) / double(FILE_N) : 0.0,
            FILE_N ? double(rb) / double(FILE_N) : 0.0, tape.passes);
    }
    std::fprintf(stderr,
        "ac_reports=%" PRIu64 " parse_classes=%" PRIu64 " rows=%" PRIu64 " phrases=%" PRIu64
        " dict_bytes=%" PRIu64 " rounds=%" PRIu64 " prefix_relations=%" PRIu64
        " rule=level3_parse_only_classes_that_announce_deep_repeats\n",
        g_ac_reports.load(), PARSE_CLASSES.load(), PARSE_ROWS.load(),
        PARSE_PHRASES.load(), PARSE_DICT_BYTES.load(), PARSE_ROUNDS_TOTAL.load(),
        PARSE_PREFIX_REL.load());
    std::fprintf(stderr,
        "SS_DONE n=%" PRIu64 " units=%" PRIu64 " passes=%" PRIu64
        " tie_rounds_max=%" PRIu64 " plan_s=%.2f run_s=%.2f total_s=%.2f\n",
        N, nUnits, tape.passes, WAVE_KEY.rounds_max.load(),
        tRun - tPlan, now_sec() - tRun, now_sec() - tPlan);
    return 0;
}
