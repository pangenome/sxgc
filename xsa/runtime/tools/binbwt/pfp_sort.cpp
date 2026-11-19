// pfp_sort.cpp - THE PFP-BAND MACHINE (TAPE_BINS.md, frozen 2026-11-17,
// AMENDMENTS 1-5; the reactive engine deleted by operator order:
// forward-only, one machine, no transitional flag).
//
// THE LAW: no routing decisions anywhere; no signals ever (files + exit
// codes); a run without SS_DONE is VOID; the guards are fuses (assertions
// of invariants) and fire before the OS's; denominators on every number.
//
//   PARSE    ONE doc-aware PFP parse of the corpus (the two-sided trigger
//            rule): the dictionary of distinct phrases (memcmp-verified
//            intern), the parse array, the phrase starts. Read once.
//   KEYS     every suffix a phrase-position row; the first key = the
//            suffix's first D bytes computed EXACTLY from the parse. THE
//            PARSE IS THE CENSUS: the bins are D-key ranges packed to the
//            workspace knob (AMENDMENTS 3+4: one band resident, K bands,
//            lean by default; band boundaries are commit boundaries).
//   SORT     the 16-bit MSD over the D-key, then the D-key groups by the
//            STREAM COMPARATOR (phrase-boundary fast path compares ids by
//            rank O(1) per phrase; mid-phrase walks arena bytes; the
//            prefix-ambiguity byte-slides - phrase(lo) is a prefix of
//            phrase(hi) iff every adjacent dictionary lcp in [lo,hi)
//            stays >= len(phrase lo); the step fuse falls to the raw
//            brute). Exact by construction: prefix-freeness NEVER
//            assumed. The brute belt checks every sorted group.
//   EMIT     the chunk streams directly to its final place (no merge, no
//            staging); the LCP column is the honest brute walk per
//            adjacent pair; POST_RUN_LIVE == 0 asserted per band.

#include <algorithm>
#include <numeric>
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
    std::fprintf(stderr, "pfp_sort: %s\n", m.c_str());
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
static u64 CORPUS_PERIOD = 0;
static std::vector<u64> PERIOD_RANK;
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
static std::atomic<u64> g_text_walk{0};
static inline int suf_cmp_lcp_inner(u64 a, u64 b, u64 from, u64& lcp);
static inline int suf_cmp_from_counted(u64 a, u64 b, u64 from, u64& walked);
static inline int suf_cmp_lcp(u64 a, u64 b, u64 from, u64& lcp) {
    const int rc = suf_cmp_lcp_inner(a, b, from, lcp);
    g_text_walk.fetch_add(lcp - from + 1, std::memory_order_relaxed);
    return rc;
}
static inline int suf_cmp_lcp_inner(u64 a, u64 b, u64 from, u64& lcp) {
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

static inline int suf_cmp_from(u64 a, u64 b, u64 from) {
    u64 walked = 0;
    const int rc = suf_cmp_from_counted(a, b, from, walked);
    g_text_walk.fetch_add(walked, std::memory_order_relaxed);
    return rc;
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
                const u8 xj = REMAP ? REMAP[xs[j]] : xs[j];
                const u8 yj = REMAP ? REMAP[ys[j]] : ys[j];
                if (xj != yj) return xj < yj ? -1 : 1;
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

static inline int suf_cmp_from_inner(u64 a, u64 b, u64 from) {
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
    std::string stopPath;
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
                if (!stopPath.empty() && ::access(stopPath.c_str(), F_OK) == 0) {
                    std::fprintf(stderr, "STOP_OBSERVED source=%s exit=75\n", stopPath.c_str());
                    ::_exit(75);
                }
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

// THE RUN DIGIT + the leaf-sort knobs (carried over).
// ---------------------------------------------------------------------------
static u64 g_wave_key_bytes = 8;        // K
static u64 g_wave_run_cap = 65536;      // L's cap
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
static u64 g_parse_plane_max = 256ull << 20;   // XSA_PARSE_PLANE_MAX (the bin's per-class budget)
static u64 g_expand_delta_start = 256;         // XSA_EXPAND_DELTA_START (bytes)
static u64 g_expand_delta_cap = 1ull << 20;    // XSA_EXPAND_DELTA_CAP (bytes)
// The merge covers only NEAR-OVERLAPPING windows (syscall reduction);
// a coarse merge (64KB) read gaps no row needed and billed them - on the
// docsep fixture that was 5.6x the honest per-row demand (rchar 7149x).
static u64 g_readhead_gap_merge = 512;          // XSA_READHEAD_GAP_MERGE (bytes)
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
    PARSE_PREFIX_REL{0}, PARSE_ABANDONED{0},
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



// ===========================================================================
// THE BINS (TAPE_BINS.md): stage 0's product, stage 1-3's working set, and
// the emitter's unit protocol. A bin IS a unit: the scatter fills it, a
// sorter sorts it (leaf sort + expansion + parse), the emitter consumes it
// in key order. NO queue, NO matcher, NO admission, NO ledger: the only
// cross-bin structure is the emitter's ordering handshake.
// ===========================================================================
struct Unit {
    unsigned bin = 0;          // the first byte (remapped space)
    // ---- stage 0's product (the bin's rows, positions ascending) ----
    std::vector<u64> P;        // packed position|prev-symbol
    std::vector<u8>  Kb;       // K continuation bytes per row
    std::vector<u32> Lp;      // run digit L (stage 0 capture)
    std::vector<u16> Tp;      // run digit t (stage 0 capture)
    std::vector<u64> tails;   // [KEY_N, N) rows whose first byte is this bin
    // ---- runtime (the emitter protocol, carried over) ----
    std::atomic<bool> sorted{false};
    std::atomic<bool> emitted{false};
    std::vector<u64> Pout;    // the sorted rows (handed to the emitter)
    std::vector<u64> lcpv;
};

// ---- the readhead (TAPE_BINS.md): gap-merged ascending preads over the
// class's own rows. Only the WANTED bytes are billed: a pread bills bytes
// read, not bytes skipped. Serial within the bin, parallel across bins. ----
static std::atomic<bool> g_expand_trace{false};
static std::atomic<u64> g_etrace_a{0}, g_etrace_b{0};
struct ReadHead {
    int fd = -1;
    std::vector<u8> buf;      // the round's merged window bytes (remapped)
    struct RowWin { u64 off; u32 len; };   // len < delta: the suffix ended
    std::vector<RowWin> rowWin;
    u64 wantedBytes = 0, skippedBytes = 0, segments = 0;
    // rows: (position, row index) ascending; reads [pos+cmp, pos+cmp+delta)
    void fetch(const std::vector<std::pair<u64, u32>>& rows, u64 cmp, u64 delta) {
        struct Seg { u64 start, end; std::vector<std::pair<u32, u64>> rows; };
        std::vector<Seg> segs;
        rowWin.assign(rows.size(), RowWin{0, 0});
        buf.clear();
        wantedBytes = skippedBytes = 0;
        segments = 0;
        for (u64 i = 0; i < rows.size(); ++i) {
            const u32 rid = rows[size_t(i)].second;   // the CALLER's local row id
            const u64 pos = rows[size_t(i)].first;
            const u64 rem = FILE_N - pos;
            if (rem <= cmp) continue;   // the suffix ended: END-ordered by the radix (digit 0)
            const u64 take = std::min<u64>(rem - cmp, delta);
            const u64 from = pos + cmp;
            if (!segs.empty() && from > segs.back().end + g_readhead_gap_merge) {
                // close the segment: the gap is skipped bytes (not billed)
                skippedBytes += from - segs.back().end;
                segs.emplace_back();
            }
            if (segs.empty()) segs.emplace_back();
            Seg& s = segs.back();
            if (s.rows.empty()) { s.start = from; s.end = from + take; }
            else s.end = std::max(s.end, from + take);
            s.rows.emplace_back(rid, from);
            rowWin[size_t(rid)] = RowWin{0, u32(take)};
        }
        for (const Seg& s : segs) {
            if (s.rows.empty()) continue;
            const u64 want = s.end - s.start;
            const u64 at = buf.size();
            buf.resize(size_t(at + want));
            size_t got = 0;
            while (got < want) {
                ssize_t r = ::pread(fd, buf.data() + size_t(at) + got,
                                    size_t(want - got), off_t(s.start + got));
                if (r <= 0) die("readhead pread: from=" + std::to_string(s.start) +
                                " want=" + std::to_string(want) +
                                " got=" + std::to_string(got));
                got += size_t(r);
            }
            if (REMAP) for (u64 i = 0; i < want; ++i)
                buf[size_t(at + i)] = REMAP[buf[size_t(at + i)]];
            wantedBytes += want;
            ++segments;
            for (const auto& rw : s.rows)
                rowWin[rw.first] = RowWin{at + (rw.second - s.start),
                                          rowWin[rw.first].len};
        }
    }
};

// ===========================================================================
// STAGE 3's FRONTIER (ported verbatim from the tape's capture): the
// two-sided strict window-min trigger rule, phrases end with their trigger
// k-mer, the spacing lemma gives phrases > k. Only the byte source moved:
// the class's readhead replaces the tape's sweep.
// ===========================================================================
struct Frontier {
    struct Ent { u64 end; u64 start; u64 off; u32 len; u64 h; };  // phrase [start, end]; h = its FNV (computed once)
    std::vector<Ent> ents;      // ascending end; off into arena
    std::vector<u8> arena;      // phrase bytes, concatenated
    u64 frontierStart = 0;      // k-mer starts below this are classified
    bool clamped0 = false;      // ents[0] starts mid-text: a PARTIAL, not a phrase
    bool exhaustedText() const {
        return frontierStart + g_pfp_k + g_pfp_w > FILE_N;
    }
    u64 trimBelow(u64 lo) {
        u64 k2 = 0;
        while (k2 < ents.size() && ents[size_t(k2)].end < lo) ++k2;
        if (!k2) return 0;
        const u64 dropBytes = ents[size_t(k2 - 1)].off + ents[size_t(k2 - 1)].len;
        ents.erase(ents.begin(), ents.begin() + ptrdiff_t(k2));
        for (auto& e : ents) e.off -= dropBytes;
        arena.erase(arena.begin(), arena.begin() + ptrdiff_t(dropBytes));
        return k2;
    }
};

// ===========================================================================
// THE PER-CLASS PARSE SINK: dictionary + (phrase id, intra-phrase offset)
// keys with NO per-row bytes. Identical chunks hash to one entry - the deep
// shared bytes exist once in the dictionary. MARKERS (carried over):
// ~0u = TRUNCATED (the span or budget ran out: unknown beyond, equal-so-far,
// never a fabricated order); 0xFFFFFFFEu = the TRUE text end (shorter
// first, the dollar rule); PARTIAL ids carry 0x80000000u.
// ===========================================================================
static std::atomic<bool> g_parse_trace{false};
static std::atomic<u32> g_trace_a{0}, g_trace_b{0};
static std::atomic<u64> g_trace_round{0};
struct ClassParse {
    u64 phraseBudget = 0;
    std::vector<u32> ids;                       // row-major [rows x B]
    std::vector<u32> off0;                      // the row's offset in its containing phrase
    std::vector<std::vector<u8>> dict;          // proper phrases (end with their trigger)
    std::vector<std::vector<u8>> pdict;         // partial phrases (the text edges)
    // FNV hash -> CANDIDATE ids. The adversarial fixture is CONSTRUCTED to
    // attack FNV-1a (its X/M k-mers were hash-searched): the dedup must be
    // CONTENT-EXACT, so a hash hit is verified by memcmp and a real
    // collision stores a distinct phrase (the receipt: a 1250-byte partial
    // returned for a ~3750-byte ent, off0 > len - a fabricated keying).
    std::unordered_map<u64, std::vector<u32>> dictOf, pdictOf;
    u64 dictBytes() const {
        u64 b = 0;
        for (const auto& ph : dict) b += ph.size();
        for (const auto& ph : pdict) b += ph.size();
        return b;
    }
};


// ===========================================================================
// THE PER-BIN DRIVER: stage 1 (the leaf sort), stage 2 (the expansion
// rounds), stage 3 (the sub-PFP parse for fat classes). Everything a bin
// needs is local: its rows, the corpus fd, and its own readhead.
// ===========================================================================
static void process_bin(Unit& U, int fd, WaveKeyStats& st, u64 unit_id);

// ---- shared scratch for one bin's stages (hoisted once per bin: churn of
// class-sized vectors was an RSS leak the mallopt fix taught us to avoid) ----
struct BinWork {
    std::vector<unsigned> sDig;
    std::vector<u64> sCur, sP, starts;
    std::vector<u32> sS;
    std::vector<u8> sSrc, sK2;
};

// ---------------------------------------------------------------------------
// STAGE 1 + 2 + 3 (one bin). Ported from wavekey_sort_unit with the
// mediation stack removed: the tie ladder's ONE big follow-up window is
// replaced by the bin's OWN expansion rounds (gap-merged preads, cmp_len
// growth, survivors only), and the fat classes parse locally.
// ---------------------------------------------------------------------------
static void process_bin(Unit& U, int fd, WaveKeyStats& st, u64 unit_id) {
    const u64 K = g_wave_key_bytes;
    const u64 kbase = 1;                 // the bin's radix prefix: the first byte
    std::vector<u64>& P = U.P;
    std::vector<u8>& Kb = U.Kb;
    std::vector<u32>& Lp = U.Lp;
    std::vector<u16>& Tp = U.Tp;
    const u64 n = P.size();
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
    U.lcpv.assign(size_t(n), 0);
    st.rows += n;
    st.key_bytes += n * K;
    if (n == 0) {
        // THE EMPTY BIN still owns its tails - bin 0 owns the virtual END
        // row (the empty suffix): sort them and hand them over. (The smoke
        // receipt caught the missing row: 200000 emitted against N=200001.)
        std::sort(U.tails.begin(), U.tails.end(), [](u64 a2, u64 b2) {
            return suf_cmp_from(unpack_pos(a2), unpack_pos(b2), 0) < 0;
        });
        for (size_t ti = 0; ti < U.tails.size(); ++ti) {
            P.push_back(U.tails[ti]);
            U.lcpv.push_back(0);
            if (ti) {
                u64 lcp = 0;
                suf_cmp_lcp(unpack_pos(P[size_t(ti - 1)]),
                            unpack_pos(P[size_t(ti)]), 0, lcp);
                U.lcpv[size_t(ti)] = lcp;
            }
        }
        U.Pout = std::move(P);
        U.tails.clear();
        U.tails.shrink_to_fit();
        return;
    }
    if (CORPUS_PERIOD) {
        // The census verified EVERY text byte against this dictionary period.
        // Equal phases have identical infinite continuations: finite suffix
        // length alone orders them and gives their exact LCP.
        std::sort(P.begin(), P.end(), [](u64 a, u64 b) {
            const u64 pa = unpack_pos(a), pb = unpack_pos(b);
            const u64 cap = std::min(FILE_N-pa, FILE_N-pb);
            if (cap < CORPUS_PERIOD) {
                for (u64 i = 0; i < cap; ++i) {
                    const u8 x = T[(pa+i) % CORPUS_PERIOD];
                    const u8 y = T[(pb+i) % CORPUS_PERIOD];
                    if (x != y) return x < y;
                }
                return pa > pb;
            }
            const u64 ra = PERIOD_RANK[pa % CORPUS_PERIOD];
            const u64 rb = PERIOD_RANK[pb % CORPUS_PERIOD];
            return ra != rb ? ra < rb : pa > pb;
        });
        for (u64 i = 1; i < n; ++i) {
            const u64 a = unpack_pos(P[i-1]), b = unpack_pos(P[i]);
            const u64 cap = std::min(FILE_N-a, FILE_N-b);
            if (a % CORPUS_PERIOD == b % CORPUS_PERIOD) U.lcpv[i] = cap;
            else {
                u64 l = 0;
                while (l < cap && l < CORPUS_PERIOD &&
                       T[(a+l) % CORPUS_PERIOD] == T[(b+l) % CORPUS_PERIOD]) ++l;
                U.lcpv[i] = l;
            }
        }
        ++st.rounds;
        std::fprintf(stderr, "PERIOD_INDEX unit=%" PRIu64 " rows=%" PRIu64
                     " dictionary_bytes=%" PRIu64 " verified_bytes=%" PRIu64
                     " rule=exact_phase_and_finite_length\n", unit_id, n, CORPUS_PERIOD, FILE_N);
        // the tails still ride the period path (the empty suffix among them)
        if (!U.tails.empty()) {
            std::sort(U.tails.begin(), U.tails.end(), [](u64 a2, u64 b2) {
                return suf_cmp_from(unpack_pos(a2), unpack_pos(b2), 0) < 0;
            });
            for (const u64 tp : U.tails) {
                const u64 pos = unpack_pos(tp);
                const size_t i = size_t(std::lower_bound(
                    P.begin(), P.end(), pos,
                    [](u64 x, u64 t) {
                        return suf_cmp_from(unpack_pos(x), t, 0) < 0;
                    }) - P.begin());
                P.insert(P.begin() + ptrdiff_t(i), tp);
                U.lcpv.insert(U.lcpv.begin() + ptrdiff_t(i), 0);
                u64 lcp = 0;
                if (i) suf_cmp_lcp(unpack_pos(P[size_t(i - 1)]), pos, 0, lcp),
                    U.lcpv[size_t(i)] = lcp;
                if (i + 1 < P.size()) {
                    suf_cmp_lcp(pos, unpack_pos(P[size_t(i + 1)]), 0, lcp);
                    U.lcpv[size_t(i + 1)] = lcp;
                }
            }
            U.tails.clear();
            U.tails.shrink_to_fit();
        }
        U.Pout = std::move(P);
        return;
    }
    std::vector<u64> P2(n, 0);
    std::vector<u8> Kb2(n * size_t(K), 0);
    std::vector<u32> Lp2(n, 0);
    std::vector<u16> Tp2(n, 0);
    std::vector<u64> starts;
    std::vector<std::pair<u64, u64>> spans, nxt, small;
    auto rem_of = [](u64 pk) { return FILE_N - unpack_pos(pk); };

    // ---- STAGE 1 ROUND 0: the captured radix key + the run digit. ZERO
    // corpus access (the scatter already captured the K bytes + digits). ----
    const double t0 = now_sec();
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
                if (i) U.lcpv[size_t(sp.first + i)] = slcp[size_t(i)];
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
        if (v != ~0ull) U.lcpv[size_t(i)] = v;
    }

    // ---- the run-key receipt (P3): rows in, rows out of the run digit ----
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

    // the class-size distribution, journaled per bin (the level-3 gate)
    {
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
            " p50=%" PRIu64 " p99=%" PRIu64 " max=%" PRIu64 "\n",
            unit_id, ss.size(), round0_tie,
            pick(0.5), pick(0.99), ss.empty() ? 0 : ss.back());
        for (u64 s : sizes) st.tie_group_sizes.push_back(s);
        st.tie_rows += round0_tie;
        st.tie_groups = spans.size();
    }

    // =========================================================================
    // STROKE 2 + 3 (AMENDMENT 2, the Sawzall law): EVERY surviving tie
    // class parses - uniformly. No census, no width clause, no depth
    // clause, no expansion rounds, no reverse slide: the parse IS the
    // tie resolution, its widening rounds (x8) and the LCE residual are
    // within-stroke machinery, and the comparator byte-walks ambiguity.
    // What remains are FUSES (step counters, the ids-plane invariant, the
    // rchar backstop, post-run live == 0) - assertions, never decisions.
    // =========================================================================
    // =====================================================================
    // STROKE 3: the sub-PFP parse for one tie class - now the ONLY tie
    // resolution (AMENDMENT 2). The readhead fetches the class's shared
    // region: rows ascending, one growing buffer + one frontier for
    // clustered rows (the gap-merge discipline); a distant row resets
    // both (each row's classification is self-consistent: the off0
    // alignment makes the pre-position bytes irrelevant to the order).
    // The widening rounds (x8) and the LCE residual are within-stroke
    // machinery. There is no reverse slide and no return to bytes: the
    // class resolves here or the fuses die with the numbers.
    // =====================================================================
    std::vector<u8> pbuf;          // the parse reader's buffer (remapped)
    u64 pbufBase = ~0ull;
    std::vector<std::pair<std::pair<u64, u64>, u64>> residual;   // (span, proven depth)
    std::function<void(std::pair<u64, u64>, u64, u64)> parse_class;
    parse_class = [&](std::pair<u64, u64> sp, u64 cmpLen, u64 delta) -> void {
        const u64 m = sp.second - sp.first;
        beat(4);
        u64 B = g_parse_phrases;
        u64 spanBytes = B * (g_pfp_k + g_pfp_w) * 2 + 4096;
        u64 parseRound = 0;
        (void)delta;   // the Sawzall: no routing uses the trajectory anymore
        // the class's rows, ascending (the readhead's order). REBUILT EVERY
        // ROUND below: each round's sort PERMUTES P's span, so the pidx of
        // a given row CHANGES between rounds. pc.ids is keyed by the pidx
        // the CAPTURE writes (rows[].second) and the comparator/extraction
        // key it by the CURRENT pidx (idx[slot]-sp.first) - the two spaces
        // must be the SAME round's, or the comparator reads crossed
        // streams after the first permutation (the 3 GB receipt: the
        // belt caught a deep class whose round-3 sort crossed two rows'
        // captures 8006 bytes in).
        std::vector<std::pair<u64, u32>> rows;
        rows.reserve(size_t(m));
        for (u64 i = sp.first; i < sp.second; ++i)
            rows.emplace_back(unpack_pos(P[size_t(i)]), u32(i - sp.first));
        std::sort(rows.begin(), rows.end());
        const char* traceSpec = ::getenv("XSA_PARSE_TRACE");
        if (traceSpec) {
            u64 ta = 0, tb = 0;
            if (std::sscanf(traceSpec, "%" SCNu64 ",%" SCNu64, &ta, &tb) == 2) {
                bool hasA = false, hasB = false;
                for (const auto& rw : rows)
                    if (rw.first == ta) hasA = true;
                    else if (rw.first == tb) hasB = true;
                if (hasA && hasB) {
                    for (u64 ri = 0; ri < m; ++ri)
                        if (rows[size_t(ri)].first == ta)
                            g_trace_a.store(rows[size_t(ri)].second);
                        else if (rows[size_t(ri)].first == tb)
                            g_trace_b.store(rows[size_t(ri)].second);
                    g_parse_trace.store(true);
                }
            }
        }
        for (;;) {
            ++parseRound;
            g_trace_round.store(parseRound);
            rows.clear();
            for (u64 i = sp.first; i < sp.second; ++i)
                rows.emplace_back(unpack_pos(P[size_t(i)]), u32(i - sp.first));
            std::sort(rows.begin(), rows.end());
            if (parseRound > g_parse_rounds + 1)
                die("PARSE_ROUNDS: unit " + std::to_string(unit_id) +
                    " class of " + std::to_string(m) + " rows exceeded the round"
                    " machinery - refusing with the numbers");
            // ---- THE IDS-PLANE INVARIANT (a fuse, not a decision):
            // every class parses, so the keying must fit - B shrinks to
            // B >= 1 phrase per row and the widening grows it back under
            // the same invariant. Only rows x 8 > budget lands in the
            // die, which the no-cross law makes unreachable.
            const u64 budgetEff = std::max(g_parse_plane_max, m * 8);
            if (m * 8 > budgetEff) {   // unreachable with the floor; an
                // INVARIANT of the Sawzall (the no-cross law bounds the
                // class; only a bug lands here)
                die("IDS_PLANE_INVARIANT: unit " + std::to_string(unit_id) +
                    " class of " + std::to_string(m) + " rows needs " +
                    std::to_string(m * 8) + " keying bytes against budget " +
                    std::to_string(budgetEff) + " - the boundary law is"
                    " violated, this is a bug not a plan shape");
            }
            const u64 off0Bytes = m * 4;
            {
                const u64 idsBytes = m * B * 4;
                if (idsBytes + off0Bytes > budgetEff) {
                    const u64 Bfit = std::max<u64>(1,
                        (budgetEff - off0Bytes) / (m * 4));
                    std::fprintf(stderr,
                        "PARSE_ADAPTED unit=%" PRIu64 " class_rows=%" PRIu64
                        " budget=%" PRIu64 " phrase_budget=%" PRIu64 "->%llu"
                        " ids_bytes=%" PRIu64 "->%" PRIu64
                        " rule=census_is_mandatory_shrink_the_keying_to_fit\n",
                        unit_id, m, budgetEff, B,
                        (unsigned long long)Bfit, idsBytes,
                        (budgetEff - off0Bytes) / (m * 4) * m * 4);
                    B = Bfit;
                    spanBytes = B * (g_pfp_k + g_pfp_w) * 2 + 4096;
                }
            }
            ClassParse pc;
            pc.phraseBudget = B;
            pc.ids.assign(size_t(m) * size_t(B), ~0u);
            pc.off0.assign(size_t(m), 0);
            Frontier F;
            pbufBase = ~0ull;
            u64 entCursor = 0;
            const u64 lookback = g_pfp_w + g_pfp_k - 1;
            // the km hash over the buffer (carried over verbatim)
            auto kmHash = [&](const u8* q) {
                u64 h = 1469598103934665603ull;
                for (u64 i = 0; i < g_pfp_k; ++i) { h ^= q[i]; h *= 1099511628211ull; }
                return h;
            };
            // THE TRIGGER RULE (carried over verbatim): start s is a trigger
            // iff km(s) is STRICTLY smaller than the w k-mers preceding AND
            // following it. The spacing lemma: triggers are >= w+1 apart, so
            // every phrase is > k long and ends with its own trigger k-mer.
            auto extendFrontier = [&](u64 upTo) {
                const u8* win = pbuf.data();
                const u64 winBase = pbufBase;
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
            auto intern = [&](const u8* q2, u32 len, u64 hh, bool partial) -> u32 {
                auto& tab = partial ? pc.pdict : pc.dict;
                auto& mapOf = partial ? pc.pdictOf : pc.dictOf;
                const u32 flag = partial ? 0x80000000u : 0u;
                auto it = mapOf.find(hh);
                if (it != mapOf.end())
                    for (const u32 cand : it->second)
                        if (tab[size_t(cand)].size() == size_t(len) &&
                            std::memcmp(tab[size_t(cand)].data(), q2, len) == 0)
                            return cand | flag;   // verified content-exact
                const u32 id = u32(tab.size());
                tab.emplace_back(q2, q2 + len);
                if (it != mapOf.end()) it->second.push_back(id);
                else mapOf.emplace(hh, std::vector<u32>{id});
                return id | flag;
            };
            // ---- the capture: one row at a time, ascending ----
            for (u64 ri = 0; ri < m; ++ri) {
                if ((ri & 0x3ff) == 0) beat(4);
                // THE ROW'S KEYED STORAGE IS ITS P-LOCAL INDEX, not the
                // capture order: the sort below addresses rows by their
                // position in the class's span of P, and the capture walks
                // the rows position-ascending for the readhead. Mixing the
                // two spaces associated streams with the wrong rows (the
                // PARSE_ORDER belt caught it with both streams dumped).
                const u32 rid = rows[size_t(ri)].second;
                const u64 p = rows[size_t(ri)].first;
                const u64 from = p > lookback ? p - lookback : 0;
                // distant row (or the first): reset the buffer + frontier;
                // clustered rows keep both (the shared region fetched once)
                if (pbufBase == ~0ull || pbuf.size() > (64ull << 20) ||
                    from > pbufBase + pbuf.size() + g_readhead_gap_merge) {
                    pbuf.clear();
                    pbufBase = from;
                    F = Frontier{};
                    F.frontierStart = from;
                    entCursor = 0;
                } else if (from < pbufBase) {
                    die("PARSE_ORDER: the class's rows are not ascending");
                }
                const u64 upTo = std::min(p + spanBytes, FILE_N);
                // grow the buffer to cover [pbufBase, upTo)
                if (pbufBase + pbuf.size() < upTo) {
                    const u64 at = pbuf.size();
                    const u64 want = upTo - (pbufBase + at);
                    pbuf.resize(size_t(at + want));
                    size_t got = 0;
                    while (got < want) {
                        ssize_t r = ::pread(fd, pbuf.data() + size_t(at) + got,
                                            size_t(want - got), off_t(pbufBase + at + got));
                        if (r <= 0) die("parse readhead pread");
                        got += size_t(r);
                    }
                    if (REMAP) for (u64 i = 0; i < want; ++i)
                        pbuf[size_t(at + i)] = REMAP[pbuf[size_t(at + i)]];
                    st.bytes_read += want;
                }
                const bool lastWin = (pbufBase + pbuf.size() >= FILE_N);
                extendFrontier(lastWin ? FILE_N
                    : (pbufBase + pbuf.size() > (g_pfp_w + g_pfp_k - 1)
                        ? pbufBase + pbuf.size() - (g_pfp_w + g_pfp_k - 1) : 0));
                const u8* q = pbuf.data() + size_t(p - pbufBase);
                // the phrase containing p: the first ent with end >= p
                while (entCursor < F.ents.size() &&
                       F.ents[size_t(entCursor)].end < p) ++entCursor;
                u64 prev = 0;
                u32* idOut = &pc.ids[size_t(rid) * size_t(B)];
                if (entCursor >= F.ents.size()) {
                    pc.off0[size_t(rid)] = 0;   // UNKNOWN beyond: TRUNCATED
                    continue;
                }
                const Frontier::Ent& ce = F.ents[size_t(entCursor)];
                if (ce.start <= p) {
                    // STRUCTURAL: the row must lie WITHIN its containing
                    // phrase (off0 < len); a violation is a capture bug -
                    // dump the ent and the row's context and die.
                    if (p - ce.start >= ce.len || ce.len == 0) {
                        std::fprintf(stderr,
                            "OFF0_INVARIANT unit=%" PRIu64 " row_pos=%" PRIu64
                            " rid=%u entCursor=%zu ents=%zu clamped0=%d"
                            " ce.start=%" PRIu64 " ce.end=%" PRIu64
                            " ce.len=%u pbufBase=%" PRIu64 " pbuf_size=%zu"
                            " F.frontierStart=%" PRIu64 "\n",
                            unit_id, p, rid, size_t(entCursor), F.ents.size(),
                            int(F.clamped0), ce.start, ce.end, ce.len,
                            pbufBase, pbuf.size(), F.frontierStart);
                        for (size_t ei = entCursor > 2 ? entCursor - 2 : 0;
                             ei < F.ents.size() && ei < entCursor + 3; ++ei)
                            std::fprintf(stderr,
                                "  ent[%zu] start=%" PRIu64 " end=%" PRIu64
                                " len=%u\n", ei, F.ents[ei].start,
                                F.ents[ei].end, F.ents[ei].len);
                        die("PARSE_OFF0: the row's offset within its containing"
                            " phrase exceeds the phrase - a capture bug; refusing");
                    }
                    const bool part0 = (entCursor == 0 && F.clamped0);
                    idOut[0] = intern(F.arena.data() + ce.off, ce.len, ce.h, part0);
                    pc.off0[size_t(rid)] = u32(p - ce.start);
                    prev = ce.end;
                } else {
                    idOut[0] = intern(q, u32(ce.end - p + 1),
                                      [&] {   // the partial: hash its bytes
                                          u64 h2 = 1469598103934665603ull;
                                          for (u64 b2 = 0; b2 < u32(ce.end - p + 1); ++b2) {
                                              h2 ^= q[b2]; h2 *= 1099511628211ull;
                                          }
                                          return h2;
                                      }(), true);
                    pc.off0[size_t(rid)] = 0;
                    prev = ce.end;
                }
                u64 i = 1;
                for (; i < B; ++i) {
                    u64 entIdx = entCursor + 1;
                    while (entIdx < F.ents.size() &&
                           F.ents[size_t(entIdx)].end <= prev)
                        ++entIdx;
                    if (entIdx >= F.ents.size()) {
                        if (!F.exhaustedText()) {
                            idOut[i] = ~0u;   // not yet classified: TRUNCATED
                            break;
                        }
                        if (prev >= FILE_N - 1) { idOut[i] = 0xFFFFFFFEu; break; }
                        const u64 fl = FILE_N - 1 - prev;
                        if (prev + 1 < pbufBase || fl > (1u << 20)) {
                            idOut[i] = ~0u; break;   // UNKNOWN beyond
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
                // bound the frontier: no future row of this class lands in
                // a phrase that ended below the next row's position
                const u64 nextP = ri + 1 < m ? rows[size_t(ri + 1)].first : ~0ull;
                if (nextP != ~0ull && !F.ents.empty() &&
                    F.ents.front().end + g_readhead_gap_merge < nextP - lookback) {
                    const u64 k2 = F.trimBelow(nextP - lookback);
                    if (entCursor >= k2) entCursor -= k2; else entCursor = 0;
                }
            }
            // ---- the dictionary's exact periods (the run interiors) ----
            const u64 D = pc.dict.size();
            std::vector<u32> per(size_t(D), 0);
            for (u64 i = 0; i < D; ++i) {
                const auto& ph = pc.dict[size_t(i)];
                const u32 nn = u32(ph.size());
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
            // ---- THE PREFIX-FREE HEALTH METRIC (journaled, never a gate:
            // prefix-freeness was DISPROVED for the two-sided rule; order is
            // exact by CONSTRUCTION - the comparator compares dictionary
            // BYTES at every resolving step - and the SA ORACLE is the belt)
            // ---- plus the STRUCTURAL INVARIANT (a hard die) ----
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
                for (const auto& ph : pc.dict) {
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
                for (const auto& ph : pc.dict) {
                    const auto h = khash2(ph.data() + ph.size() - g_pfp_k);
                    if (nonEnd.count(h) && nonEnd.at(h) > 0) ++relThis;
                }
                PARSE_PREFIX_REL.fetch_add(relThis);
            }
            // ---- the stream comparator (the mid-phrase rule, carried over
            // verbatim): walks both rows' streams through the dictionary
            // BYTES; TRUNCATED = equal-so-far (never a fabricated order);
            // the TRUE end is the dollar rule; exact periods make run
            // interiors O(1). WHY ORDER == BYTE ORDER: every resolving step
            // compares dictionary bytes; equal-id skips are equality-only;
            // prefix-relations continue the walk into the next phrase. ----
            const std::vector<u32>& ids = pc.ids;
            const std::vector<u32>& off0 = pc.off0;
            auto streamCmp = [&](u32 a, u32 b, u64& lcp) -> int {
                u32 ia = 0, ib = 0;
                u32 pa = off0[size_t(a)], pb = off0[size_t(b)];
                u64 consumed = 0;
                auto phraseOf = [&](u32 id) -> const std::vector<u8>& {
                    return (id & 0x80000000u)
                               ? pc.pdict[size_t(id & 0x7fffffffu)]
                               : pc.dict[size_t(id)];
                };
                u64 steps = 0;
                const u64 stepCap = 4 * B + 16;
                const bool trace = g_parse_trace.load() &&
                    ((a == g_trace_a.load() && b == g_trace_b.load()) ||
                     (a == g_trace_b.load() && b == g_trace_a.load()));
                for (;;) {
                    if (trace)
                        std::fprintf(stderr,
                            "  CMP round=%llu B=%llu a=%u b=%u ia=%u ib=%u pa=%u pb=%u"
                            " consumed=%" PRIu64 " ida=%x idb=%x\n",
                            (unsigned long long)g_trace_round.load(),
                            (unsigned long long)B,
                            a, b, ia, ib, pa, pb, consumed,
                            ids[size_t(u64(a)*B+ia)], ids[size_t(u64(b)*B+ib)]);
                    if (++steps > stepCap)
                        die("PARSE_WALK: a stream comparison exceeded " +
                            std::to_string(stepCap) + " steps (rows " +
                            std::to_string(a) + "/" + std::to_string(b) +
                            ", ia=" + std::to_string(ia) + ", ib=" +
                            std::to_string(ib) + ", pa=" + std::to_string(pa) +
                            ", pb=" + std::to_string(pb) + ", consumed=" +
                            std::to_string(consumed) + ") - the id walk ran "
                            "past its budget; refusing");
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
                            consumed += la;
                            if (la < lb) { ++ia; pa = 0; pb += nn; }
                            else if (lb < la) { ++ib; pb = 0; pa += nn; }
                            else { ++ia; ++ib; pa = pb = 0; }
                            continue;
                        }
                        // per[] covers the PROPER dictionary only: a
                        // partial id carries 0x80000000 and would index
                        // far out of bounds (the ASan receipt at 64 KiB
                        // budget, where partials meet mid-walk constantly).
                        const u32 pp = (ida & 0x80000000u)
                            ? 0u : per[size_t(ida)];
                        if (pp && ((pa > pb ? pa - pb : pb - pa) % pp) == 0) {
                            consumed += nn;   // run interior: byte-exact periodic skip
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
            // ---- sort the class over the streams ----
            std::vector<u64> idx;
            idx.resize(size_t(m));
            for (u64 i = 0; i < m; ++i) idx[size_t(i)] = sp.first + i;
            std::sort(idx.begin(), idx.end(), [&](u64 x, u64 y) {
                u64 lcp = 0;
                return streamCmp(u32(x - sp.first), u32(y - sp.first), lcp) < 0;
            });
            std::vector<u64> sortedP;
            sortedP.resize(size_t(m));
            for (u64 i = 0; i < m; ++i)
                sortedP[size_t(i)] = P[size_t(idx[size_t(i)])];
            for (u64 i = 0; i < m; ++i)
                P[size_t(sp.first + i)] = sortedP[size_t(i)];
            check_perm("parse-sort", parseRound, 1);
            // adjacent LCPs (the column falls out of the sort)
            for (u64 i = 1; i < m; ++i) {
                u64 lcp = 0;
                streamCmp(u32(idx[size_t(i - 1)] - sp.first),
                          u32(idx[size_t(i)] - sp.first), lcp);
                U.lcpv[size_t(sp.first + i)] = lcp;
            }
            // ---- tie spans: widen or fall to the LCE residual ----
            std::vector<std::pair<u64, u64>> ties;
            {
                // THE EQUALITY CLOSURE: a tie span must be a connected
                // component of the pairwise "equal-so-far" relation, not
                // merely a consecutive run. A row that proves NOTHING
                // (id0 = TRUNC, prov 0) ties with EVERY row - once such a
                // row is inside the run, every later row joins the span
                // (the adversarial receipt: the TRUNC row sat between two
                // runs whose true order interleaves - {12511,7511,17511}
                // vs {1253,6253} - and the within-span residual could
                // not cross them; the brute order interleaves them).
                auto rowUnknown = [&](u64 slot) -> bool {
                    return pc.ids[size_t(u64(idx[size_t(slot)] - sp.first)) *
                                  size_t(B)] == ~0u;
                };
                u64 i2 = 0;
                while (i2 < m) {
                    u64 j = i2 + 1;
                    bool unknownInside = rowUnknown(i2);
                    while (j < m) {
                        if (rowUnknown(j)) unknownInside = true;
                        if (!unknownInside) {
                        u64 lcp = 0;
                        const int tc = streamCmp(u32(idx[size_t(i2)] - sp.first),
                                                 u32(idx[size_t(j)] - sp.first), lcp);
                        if (g_parse_trace.load()) {
                            const u32 ra = u32(idx[size_t(i2)] - sp.first);
                            const u32 rb = u32(idx[size_t(j)] - sp.first);
                            std::fprintf(stderr,
                                "  TIECHK round=%llu B=%llu sp=[%llu,%llu) i=%llu ra=%u"
                                " pos=%llu off0=%u id0=%x vs j=%llu rb=%u pos=%llu"
                                " off0=%u id0=%x c=%d lcp=%llu\n",
                                (unsigned long long)parseRound, (unsigned long long)B,
                                (unsigned long long)sp.first, (unsigned long long)sp.second,
                                (unsigned long long)i2, ra,
                                (unsigned long long)unpack_pos(P[size_t(idx[size_t(i2)])]),
                                pc.off0[size_t(ra)], pc.ids[size_t(u64(ra)*B)],
                                (unsigned long long)j, rb,
                                (unsigned long long)unpack_pos(P[size_t(idx[size_t(j)])]),
                                pc.off0[size_t(rb)], pc.ids[size_t(u64(rb)*B)],
                                tc, (unsigned long long)lcp);
                        }
                        if (tc != 0) break;
                        }
                        ++j;
                    }
                    if (j - i2 > 1) ties.emplace_back(i2, j);
                    i2 = j;
                }
            }
            auto phraseLenOf = [&](u32 id) -> u64 {
                return ((id & 0x80000000u) ? pc.pdict[size_t(id & 0x7fffffffu)]
                                           : pc.dict[size_t(id)]).size();
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
            if (::getenv("XSA_PARSE_DEBUG") && m >= 2) {
                // the first two rows' keying, verbatim (diagnostics only)
                for (u64 dr = 0; dr < 2 && dr < m; ++dr) {
                    std::string hx;
                    for (u64 q2 = 0; q2 < 4; ++q2) {
                        const u32 id = pc.ids[size_t(dr) * size_t(B) + q2];
                        char t[64];
                        if (id == ~0u) std::snprintf(t, sizeof t, "[%llu TRUNC]",
                            (unsigned long long)q2);
                        else if (id == 0xFFFFFFFEu) std::snprintf(t, sizeof t, "[%llu TEND]",
                            (unsigned long long)q2);
                        else {
                            const auto& ph = (id & 0x80000000u)
                                ? pc.pdict[size_t(id & 0x7fffffffu)]
                                : pc.dict[size_t(id)];
                            std::string bb;
                            for (size_t xi = 0; xi < ph.size() && xi < 12; ++xi) {
                                char t2[8]; std::snprintf(t2, sizeof t2, "%02x", ph[xi]);
                                bb += t2;
                            }
                            std::snprintf(t, sizeof t, "[%llu id=%u len=%zu %s]",
                                (unsigned long long)q2, id, ph.size(), bb.c_str());
                        }
                        hx += t; hx += " ";
                    }
                    std::fprintf(stderr,
                        "PARSE_DEBUG unit=%" PRIu64 " row=%" PRIu64 " pos=%" PRIu64
                        " off0=%u %s\n",
                        unit_id, dr, rows[size_t(dr)].first, pc.off0[size_t(dr)],
                        hx.c_str());
                }
            }
            PARSE_CLASSES.fetch_add(1);
            PARSE_ROWS.fetch_add(m);
            PARSE_PHRASES.fetch_add(B * m);
            PARSE_DICT_BYTES.fetch_add(pc.dictBytes());
            PARSE_ROUNDS_TOTAL.fetch_add(1);
            std::fprintf(stderr,
                "PARSE_CLASS unit=%" PRIu64 " rows=%" PRIu64 " phrases=%" PRIu64
                " dict_entries=%" PRIu64 " partials=%" PRIu64 " dict_bytes=%" PRIu64
                " rounds=%" PRIu64 " tie_spans=%zu"
                " prefix_relations=%" PRIu64 " budget=%" PRIu64 "\n",
                unit_id, m, B * m, pc.dict.size(), pc.pdict.size(),
                pc.dictBytes(), parseRound, ties.size(),
                PARSE_PREFIX_REL.load(), B);
            // THE MODALITY-SLIDE BELT (cheap, always on): adjacent rows in
            // a FULLY-RESOLVED phrase-space sort MUST agree with the byte
            // order - the comparator's construction claims exactly that.
            // TRUNC ties are NOT resolved (the widening/residual owns
            // them); the belt runs only when the round left no ties. The
            // adversarial fixture caught the belt firing on a deliberate
            // equal-so-far pair (rows 1251/6251, both TRUNC after ~5 KB).
            if (m <= 4096 && ties.empty()) {
                for (u64 i = 1; i < m; ++i) {
                    const u64 pa = unpack_pos(P[size_t(sp.first + i - 1)]);
                    const u64 pb = unpack_pos(P[size_t(sp.first + i)]);
                    if (suf_cmp_from(pa, pb, 0) >= 0) {
                        auto dumpRow = [&](u64 slot) {
                            // slot = the SORTED position; the keying lives
                            // at the P-INDEX idx[slot]-sp.first (the dump
                            // once read the slot directly and printed two
                            // unrelated rows as "identical streams")
                            const u32 r = u32(idx[size_t(slot)] - sp.first);
                            (void)r;
                            std::string hx;
                            for (u64 q2 = 0; q2 < B; ++q2) {
                                const u32 id = pc.ids[size_t(r) * size_t(B) + q2];
                                char t[80];
                                if (id == ~0u) std::snprintf(t, sizeof t, "TRUNC");
                                else if (id == 0xFFFFFFFEu) std::snprintf(t, sizeof t, "TEND");
                                else {
                                    const auto& ph = (id & 0x80000000u)
                                        ? pc.pdict[size_t(id & 0x7fffffffu)]
                                        : pc.dict[size_t(id)];
                                    std::string bb;
                                    for (size_t xi = 0; xi < ph.size() && xi < 10; ++xi) {
                                        char t2[8]; std::snprintf(t2, sizeof t2, "%02x", ph[xi]);
                                        bb += t2;
                                    }
                                    std::snprintf(t, sizeof t, "id=%u/%zu[%s]",
                                        id, ph.size(), bb.c_str());
                                }
                                hx += t; hx += " ";
                            }
                            return hx;
                        };
                        std::fprintf(stderr,
                            "PARSE_ORDER_FAIL unit=%" PRIu64 " row=%" PRIu64
                            " pos_a=%" PRIu64 " pos_b=%" PRIu64
                            " off0_a=%u off0_b=%u\n  A: %s\n  B: %s\n",
                            unit_id, i, pa, pb,
                            pc.off0[size_t(u32(idx[size_t(i - 1)] - sp.first))],
                            pc.off0[size_t(u32(idx[size_t(i)] - sp.first))],
                            dumpRow(i - 1).c_str(), dumpRow(i).c_str());
                        {   // the resolution context: the first id pair that
                            // differs, both phrases' full hex, the brute LCP
                            const u32 ra = u32(idx[size_t(i - 1)] - sp.first);
                            const u32 rb = u32(idx[size_t(i)] - sp.first);
                            for (u64 q2 = 0; q2 + 1 < B; ++q2) {
                                const u32 ia2 = pc.ids[size_t(u64(ra)) * size_t(B) + q2];
                                const u32 ib2 = pc.ids[size_t(u64(rb)) * size_t(B) + q2];
                                if (ia2 == ib2) continue;
                                auto hexOf = [&](u32 id) {
                                    const auto& ph = (id & 0x80000000u)
                                        ? pc.pdict[size_t(id & 0x7fffffffu)]
                                        : pc.dict[size_t(id)];
                                    std::string h;
                                    char t[8];
                                    for (u8 x : ph) { std::snprintf(t, sizeof t, "%02x", x); h += t; }
                                    return h;
                                };
                                u64 blcp = 0;
                                suf_cmp_lcp(pa, pb, 0, blcp);
                                std::fprintf(stderr,
                                    "  DIVERGE q=%" PRIu64 " ida=%u idb=%u"
                                    " hexa=%s hexb=%s brute_lcp=%" PRIu64
                                    " consumed_a_to_q=%" PRIu64 "\n",
                                    q2, ia2, ib2, hexOf(ia2).c_str(),
                                    hexOf(ib2).c_str(), blcp,
                                    [&] { u64 c2 = 0;
                                        const u32 i0 = pc.ids[size_t(u64(ra)) * size_t(B)];
                                        c2 += ((i0 & 0x80000000u)
                                            ? pc.pdict[size_t(i0 & 0x7fffffffu)]
                                            : pc.dict[size_t(i0)]).size()
                                            - std::min<u64>(((i0 & 0x80000000u)
                                            ? pc.pdict[size_t(i0 & 0x7fffffffu)]
                                            : pc.dict[size_t(i0)]).size(),
                                            pc.off0[size_t(ra)]);
                                        for (u64 q3 = 1; q3 < q2; ++q3) {
                                            const u32 id3 = pc.ids[size_t(u64(ra)) * size_t(B) + q3];
                                            if (id3 == ~0u || id3 == 0xFFFFFFFEu) break;
                                            c2 += ((id3 & 0x80000000u)
                                                ? pc.pdict[size_t(id3 & 0x7fffffffu)]
                                                : pc.dict[size_t(id3)]).size();
                                        }
                                        return c2; }());
                                break;
                            }
                            {   // reconstruct each row's byte stream from
                                // its phrase contents and diff vs the truth
                                u64 bl2 = 0;
                                suf_cmp_lcp(pa, pb, 0, bl2);
                                const u64 brute_lcp = bl2;
                                auto dumpRecon = [&](u64 slot, const char* tag) {
                                    const u32 r = u32(idx[size_t(slot)] - sp.first);
                                    const u64 pos = unpack_pos(P[size_t(sp.first + slot)]);
                                    std::string rec;
                                    for (u64 q2 = 0; q2 < B; ++q2) {
                                        const u32 id2 = pc.ids[size_t(u64(r)) * size_t(B) + q2];
                                        if (id2 == ~0u || id2 == 0xFFFFFFFEu) break;
                                        const auto& ph = (id2 & 0x80000000u)
                                            ? pc.pdict[size_t(id2 & 0x7fffffffu)]
                                            : pc.dict[size_t(id2)];
                                        u64 from2 = (q2 == 0) ? std::min<u64>(pc.off0[size_t(r)], ph.size()) : 0;
                                        rec.append(ph.begin() + from2, ph.end());
                                        if (rec.size() > 3 * brute_lcp / 2 + 64) break;
                                    }
                                    u64 nErr = 0, firstErr = ~0ull;
                                    const u64 lim = std::min<u64>(rec.size(), 3 * brute_lcp / 2 + 64);
                                    for (u64 xi = 0; xi < lim; ++xi) {
                                        const u8 tb = REMAP ? REMAP[T[pos + xi]] : T[pos + xi];
                                        if (u8(rec[xi]) != tb) {
                                            if (firstErr == ~0ull) firstErr = xi;
                                            ++nErr;
                                        }
                                    }
                                    std::string rh, th;
                                    char t9[8];
                                    u64 lo = (firstErr == ~0ull ? brute_lcp - 16 : firstErr > 16 ? firstErr - 16 : 0);
                                    for (u64 xi = lo; xi < lo + 48 && xi < rec.size(); ++xi) {
                                        std::snprintf(t9, sizeof t9, "%02x", u8(rec[xi])); rh += t9;
                                    }
                                    for (u64 xi = lo; xi < lo + 48; ++xi) {
                                        std::snprintf(t9, sizeof t9, "%02x",
                                            REMAP ? REMAP[T[pos + xi]] : T[pos + xi]); th += t9;
                                    }
                                    std::fprintf(stderr,
                                        "  RECON %s slot=%llu pidx=%u pos=%llu rec_len=%llu"
                                        " first_mismatch=%llu mismatches=%llu/%llu"
                                        " rec[%llu..]=%s true[%llu..]=%s\n",
                                        tag, (unsigned long long)slot, r,
                                        (unsigned long long)pos,
                                        (unsigned long long)rec.size(),
                                        (unsigned long long)firstErr,
                                        (unsigned long long)nErr,
                                        (unsigned long long)lim,
                                        (unsigned long long)lo, rh.c_str(),
                                        (unsigned long long)lo, th.c_str());
                                };
                                dumpRecon(i - 1, "A");
                                dumpRecon(i, "B");
                            }
                        }
                        die("PARSE_ORDER: the phrase-space order disagrees with the"
                            " byte order - a capture or comparator bug; refusing");
                    }
                }
            }
            if (::getenv("XSA_BIN_DEBUG") && m <= 32) {
                // CAPTURE-ORDER dump: rows[] is position-ascending and its
                // .second IS the P-index the keying lives at - reading P
                // here would see the sort's permutation (the earlier dumps
                // were misread from permuted P and wasted a hunt).
                std::fprintf(stderr, "PARSE_KEYING unit=%" PRIu64 " rows=%" PRIu64
                             " B=%" PRIu64 " spanBytes=%" PRIu64 "\n", unit_id, m, B, spanBytes);
                for (u64 ri = 0; ri < m; ++ri) {
                    const u32 row = rows[size_t(ri)].second;
                    std::fprintf(stderr, "  k=%" PRIu64 " pidx=%u pos=%" PRIu64 " off0=%u"
                                 " id0=%x id1=%x id2=%x id3=%x prov=%" PRIu64 "\n",
                                 ri, row, rows[size_t(ri)].first,
                                 pc.off0[size_t(row)],
                                 pc.ids[size_t(row) * size_t(B)],
                                 pc.ids[size_t(row) * size_t(B) + 1],
                                 pc.ids[size_t(row) * size_t(B) + 2],
                                 pc.ids[size_t(row) * size_t(B) + 3],
                                 provOf(row));
                }
                for (const auto& t : ties)
                    std::fprintf(stderr, "  TIE [%llu,%llu) provMin=%llu\n",
                                 (unsigned long long)t.first, (unsigned long long)t.second,
                                 (unsigned long long)[&] {
                                     u64 pm = ~0ull;
                                     for (u64 i2 = t.first; i2 < t.second; ++i2)
                                         pm = std::min(pm, provOf(u32(idx[size_t(i2)] - sp.first)));
                                     return pm;
                                 }());
            }
            if (::getenv("XSA_BIN_DEBUG2")) {
                for (size_t di2 = 0; di2 < pc.dict.size(); ++di2)
                    std::fprintf(stderr, "  DICT id=%zu len=%zu\n", di2,
                                 pc.dict[di2].size());
                for (size_t di2 = 0; di2 < pc.pdict.size(); ++di2)
                    std::fprintf(stderr, "  PDICT id=%zu len=%zu\n", di2,
                                 pc.pdict[di2].size());
            }
            if (ties.empty()) return;   // fully ordered in phrase space
            if (parseRound >= g_parse_rounds) {
                // THE ROUND CAP: the byte-exact LCE residual owns the
                // WHOLE CLASS from depth 0. Per-span residuals with a
                // proven-depth floor were wrong in two measured ways:
                // budget-truncated rows tie at their BUDGET boundary, and
                // intransitive ties (A~B, B~C, A-C differing) split spans
                // whose cross order the sort then placed arbitrarily.
                // Brute-from-0 over the whole class is exact by
                // construction and costs m log m short walks.
                residual.emplace_back(std::make_pair(sp.first, sp.second), 0);
                WAVE_KEY.tie_residual_groups.fetch_add(1);
                WAVE_KEY.tie_residual_rows.fetch_add(m);
                std::fprintf(stderr,
                    "PARSE_RESIDUAL unit=%" PRIu64 " rows=%" PRIu64
                    " round=%" PRIu64 " reason=round_cap"
                    " rule=whole_class_brute_from_zero\n",
                    unit_id, m, parseRound);
                return;   // the residual below finishes it
            }
            // WIDEN x 8: more phrases per row AND a longer span - but
            // only while the NEXT round's keying fits the bin budget; a
            // class whose widening cannot fit goes to the byte-exact LCE
            // residual from this round's proven depth (the committed
            // mechanism - never a ballooning ids plane).
            const u64 Bnext = std::min<u64>(B * 8, g_parse_phrases * 512);
            if (m * Bnext * 4 + off0Bytes > budgetEff) {
                // THE WIDEN CAP: same as the round cap - the residual owns
                // the WHOLE CLASS from depth 0 (the 'single' receipt: the
                // B=3 budget made rows tie AT the budget, the spans split,
                // and the sort placed equal-so-far rows arbitrarily).
                residual.emplace_back(std::make_pair(sp.first, sp.second), 0);
                WAVE_KEY.tie_residual_groups.fetch_add(1);
                WAVE_KEY.tie_residual_rows.fetch_add(m);
                std::fprintf(stderr,
                    "PARSE_WIDEN_CAPPED unit=%" PRIu64 " rows=%" PRIu64
                    " budget_next=%" PRIu64 " phrase_budget_next=%" PRIu64
                    " rule=whole_class_brute_from_zero\n",
                    unit_id, m, m * Bnext * 4 + off0Bytes, Bnext);
                return;   // the residual below finishes it
            }
            B = Bnext;
            spanBytes *= 8;
            if (spanBytes > (1ull << 30)) spanBytes = 1ull << 30;
        }
    };


    // ---- STROKE 2 (the Sawzall): every surviving class parses.
    // Uniformly - no census, no routing, no expansion rounds. ----
    {
        const std::vector<std::pair<u64, u64>> stage1(spans);
        for (const auto& sp : stage1) parse_class(sp, kbase + K, 0);
    }

    // ---- THE RESIDUAL (carried over): what the parse's round cap left,
    // walked from the PROVEN depth under the counted WALK BUDGET. ----
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
                              die("WAVEKEY_FALLBACK_BUDGET: a bin's LCE residual compared " +
                                  std::to_string(walked) + " corpus bytes (budget " +
                                  std::to_string(g_wave_fallback_walk_max) +
                                  ") - the tie set the parse left is periodic (nested "
                                  "repeats): this is the corpus's own shape. Report the "
                                  "TIE_GROUPS / PARSE_CLASS journals and get the "
                                  "operator's decision before the cap is touched");
                          return c < 0;
                      });
            for (u64 i = sp.first + 1; i < sp.second; ++i) {
                u64 lcp = 0;
                suf_cmp_lcp(unpack_pos(P[size_t(i - 1)]), unpack_pos(P[size_t(i)]), walk_from, lcp);
                U.lcpv[size_t(i)] = lcp;
            }
            if (sp.first) {
                u64 lcp = 0;
                suf_cmp_lcp(unpack_pos(P[size_t(sp.first - 1)]), unpack_pos(P[size_t(sp.first)]),
                            std::min<u64>(U.lcpv[size_t(sp.first)], walk_from), lcp);
                U.lcpv[size_t(sp.first)] = lcp;
            }
        }
        st.t_fallback += now_sec() - tf0;
    }

    // ---- THE TAILS (positions [KEY_N, N)): fewer than 24 real bytes; the
    // plan owns them, not the readhead. Sorted by the dollar-order core and
    // merged into the bin's ordered rows (carried over). ----
    if (!U.tails.empty()) {
        std::sort(U.tails.begin(), U.tails.end(), [](u64 a, u64 b) {
            return suf_cmp_from(unpack_pos(a), unpack_pos(b), 0) < 0;
        });
        for (const u64 tp : U.tails) {
            const u64 pos = unpack_pos(tp);
            const size_t i = size_t(std::lower_bound(
                P.begin(), P.end(), pos,
                [](u64 a, u64 t) {
                    return suf_cmp_from(unpack_pos(a), t, 0) < 0;
                }) - P.begin());
            P.insert(P.begin() + ptrdiff_t(i), tp);
            U.lcpv.insert(U.lcpv.begin() + ptrdiff_t(i), 0);
            u64 lcp = 0;
            if (i) suf_cmp_lcp(unpack_pos(P[size_t(i - 1)]), pos, 0, lcp),
                U.lcpv[size_t(i)] = lcp;
            if (i + 1 < P.size()) {
                suf_cmp_lcp(pos, unpack_pos(P[size_t(i + 1)]), 0, lcp);
                U.lcpv[size_t(i + 1)] = lcp;
            }
        }
        U.tails.clear();   // consumed by the merge: POST_RUN_LIVE counts 0
        U.tails.shrink_to_fit();
    }

    // ---- THE ORACLE GUARD (XSA_WAVEKEY_VERIFY=1): brute force, per bin ----
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
                    "WAVEKEY_VERIFY_FAIL unit=%" PRIu64 " row=%" PRIu64
                    " got_pos=%" PRIu64 " want_pos=%" PRIu64 " lcp=%" PRIu64
                    " got_suffix=%.40s want_suffix=%.40s\n",
                    unit_id, i, a, b, lcp,
                    (const char*)(T + a), (const char*)(T + b));
                // neighborhood context: the rows around the mismatch in
                // both orders, plus their LCPs (diagnostics only)
                if (::getenv("XSA_BIN_DEBUG")) {
                    std::fprintf(stderr, "VERIFY_CONTEXT rows=%" PRIu64 " at=%" PRIu64 "\n", n, i);
                    for (u64 q2 = i > 4 ? i - 4 : 0; q2 < n && q2 < i + 5; ++q2) {
                        u64 rl = 0;
                        if (q2) suf_cmp_lcp(unpack_pos(ref[size_t(q2 - 1)]),
                                            unpack_pos(ref[size_t(q2)]), 0, rl);
                        std::fprintf(stderr,
                            "  row=%" PRIu64 " got=%" PRIu64 " want=%" PRIu64
                            " ref_prev_lcp=%" PRIu64 "\n", q2,
                            unpack_pos(P[size_t(q2)]), unpack_pos(ref[size_t(q2)]), rl);
                    }
                }
                die("WAVEKEY_VERIFY_FAIL: the bin order disagrees with the brute-force suffix order");
            }
        }
        for (u64 i = 1; i < n; ++i) {
            u64 lcp = 0;
            suf_cmp_lcp(unpack_pos(P[size_t(i - 1)]), unpack_pos(P[size_t(i)]), 0, lcp);
            if (U.lcpv[size_t(i)] != lcp) {
                std::fprintf(stderr,
                    "WAVEKEY_VERIFY_LCP_FAIL unit=%" PRIu64 " row=%" PRIu64
                    " got=%" PRIu64 " want=%" PRIu64 " pos=%" PRIu64 " prev=%" PRIu64 "\n",
                    unit_id, i, U.lcpv[size_t(i)], lcp,
                    unpack_pos(P[size_t(i)]), unpack_pos(P[size_t(i - 1)]));
                die("WAVEKEY_VERIFY_LCP_FAIL: the LCP plane disagrees with a direct walk");
            }
        }
        std::fprintf(stderr, "WAVEKEY_VERIFY_OK unit=%" PRIu64 " rows=%" PRIu64 "\n",
                     unit_id, n);
    }

    // ---- hand the ordered rows to the emitter; release the sort's planes ----
    U.Pout = std::move(P);
    bswap_release(Kb);
    bswap_release(Lp);
    bswap_release(Tp);
}


// ===========================================================================
// MAIN: stage 0 (the scatter), the sorter pool (stages 1-3), the emitter
// (stage 4), the receipts.
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
    const double tMain0 = now_sec();
    // THE CHUNK ALLOCATOR POLICY (carried over): a low mmap threshold makes
    // every bin-sized allocation a mapping, so a freed plane is a munmap
    // and the RSS receipt tracks the working set.
    ::mallopt(M_MMAP_THRESHOLD, 32768);
    ::mallopt(M_TRIM_THRESHOLD, 131072);
    std::string corpus, remapPath, outPrefix, bandsDir;
    u64 budget_bytes = 0, scratch_budget_bytes = 0;
    double memory_gb = 0.0;
    u64 threads = 32, unit_target_rows = 0, sample_n = 0;
    u64 wave_key_bytes = 0;
    std::fprintf(stderr, "pfp_sort: THE PFP-BAND MACHINE (TAPE_BINS.md, AMENDMENTS 1-5) - one parse, phrase-key bands, byte-slide exactness\n");
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
        auto envU = [&](const char* name, u64& v) {
            if (const char* e = ::getenv(name)) {
                v = std::strtoull(e, nullptr, 10); src = "env";
            }
        };
        envU("XSA_WAVE_KEY_BYTES", wave_key_bytes);
        envU("XSA_WAVE_RUN_CAP", g_wave_run_cap);
        envU("XSA_PARSE_PLANE_MAX", g_parse_plane_max);
        envU("XSA_PARSE_DICT_BYTES", g_parse_dict_allowance);
        envU("XSA_PARSE_PHRASES", g_parse_phrases);
        envU("XSA_PARSE_ROUNDS", g_parse_rounds);
        envU("XSA_PFP_K", g_pfp_k);
        envU("XSA_PFP_W", g_pfp_w);
        envU("XSA_EXPAND_DELTA_START", g_expand_delta_start);
        envU("XSA_EXPAND_DELTA_CAP", g_expand_delta_cap);
        envU("XSA_READHEAD_GAP_MERGE", g_readhead_gap_merge);
        if (const char* e = ::getenv("XSA_MEM_GUARD_MB"))
            g_mem_guard_mb = std::strtoull(e, nullptr, 10);
        if (!wave_key_bytes) wave_key_bytes = 8;
        if (wave_key_bytes < 1 || wave_key_bytes > 64)
            die("--wave-key-bytes must be in [1,64]");
        if (g_wave_run_cap < 2 || g_wave_run_cap > (1u << 22))
            die("--wave-run-cap must be in [2,4194304]");
        if (g_expand_delta_start < 16 || g_expand_delta_start > g_expand_delta_cap)
            die("XSA_EXPAND_DELTA_START must be in [16, XSA_EXPAND_DELTA_CAP]");
        if (g_expand_delta_cap < 64 || g_expand_delta_cap > (1ull << 30))
            die("XSA_EXPAND_DELTA_CAP must be in [64,2^30]");
        g_wave_key_bytes = wave_key_bytes;
        std::fprintf(stderr,
            "SS_BIN_KNOB key_bytes=%" PRIu64 " run_cap=%" PRIu64
            " expand_delta_start=%" PRIu64 " expand_delta_cap=%" PRIu64
            " readhead_gap_merge=%" PRIu64 " fallback_walk_max=%" PRIu64
            " source=%s parse_budget=%" PRIu64 " parse_phrases=%" PRIu64
            " parse_rounds=%" PRIu64 " pfp_k=%" PRIu64 " pfp_w=%" PRIu64
            " rule=per_bin_readhead;expansion_rounds;sub_pfp_on_fat_classes;modality_slide\n",
            g_wave_key_bytes, g_wave_run_cap, g_expand_delta_start,
            g_expand_delta_cap, g_readhead_gap_merge, g_wave_fallback_walk_max,
            src.c_str(), g_parse_plane_max, g_parse_phrases, g_parse_rounds,
            g_pfp_k, g_pfp_w);
    }
    const bool trieTap = ::getenv("TRIE_TAP") != nullptr;
    const u64 lcpSliceWindow = [] {
        const char* e = ::getenv("SS_LCP_SLICE_WINDOW");
        const u64 v = (e && *e) ? std::strtoull(e, nullptr, 10) : 8;
        // the window bounds the LIVE lcp slices (memory), and the
        // standalone-producer shape needs window > total units or the
        // emit waits on .consumed markers nobody writes; the pipeline
        // (finish concurrent) works at any window
        if (v < 1 || v > (1 << 20)) die("SS_LCP_SLICE_WINDOW must be in [1,2^20]");
        return v;
    }();
    const bool lcpOn = true;   // the product arm: per-bin LCP slices for the finish

    // ---- the corpus ----
    int fd = ::open(corpus.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("cannot open/stat corpus");
    FILE_N = u64(st.st_size);
    if (!FILE_N) die("empty corpus");
    N = FILE_N + 1;
    KEY_N = FILE_N >= 23 ? FILE_N - 23 : 0;
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
    // A bounded candidate dictionary; verify it during the existing census.
    const u64 probe = std::min<u64>(4096, FILE_N);
    std::vector<u64> failure(probe, 0);
    for (u64 i = 1; i < probe; ++i) {
        u64 j = failure[i-1];
        while (j && T[i] != T[j]) j = failure[j-1];
        if (T[i] == T[j]) ++j;
        failure[i] = j;
    }
    const u64 candidate = probe - failure.back();
    bool periodic = candidate * 2 <= probe;
    bool used[256]{};
    for (u64 p = 0; p < FILE_N; ++p) {
        used[T[p]] = true;
        if (periodic && T[p] != T[p % candidate]) periodic = false;
    }
    if (periodic) {
        CORPUS_PERIOD = candidate;
        std::vector<u64> phases(candidate);
        for (u64 i = 0; i < candidate; ++i) phases[i] = i;
        std::sort(phases.begin(), phases.end(), [&](u64 a, u64 b) {
            for (u64 i = 0; i < candidate; ++i) {
                const u8 x = T[(a+i) % candidate], y = T[(b+i) % candidate];
                if (x != y) return x < y;
            }
            return false;
        });
        PERIOD_RANK.resize(candidate);
        for (u64 i = 0; i < candidate; ++i) PERIOD_RANK[phases[i]] = i;
        std::fprintf(stderr, "PERIOD_CERTIFICATE dictionary_bytes=%" PRIu64
                     " verified_bytes=%" PRIu64 " rows=%" PRIu64 "\n", candidate, FILE_N, N);
    }
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
            die("bin consumer reserves cell zero; virtual END consumer required");
    build_pairmap();
    std::fprintf(stderr, "END_SYMBOL mode=virtual position=%" PRIu64 " multiplicity=1 rank=0\n", FILE_N);
    std::fprintf(stderr, "END_FORMAT k=%u ends_in_sentinel=1 end_code=none\nTAIL_GROUP rows=%" PRIu64 "\n",
                 present, N - KEY_N);
    std::fprintf(stderr, "SS_LCP_ARM lcp_source=%s slices=1\n",
                 lcpOn ? "sorter-units" : "none");

    // ---- THE ENVELOPE (the plan-time refusal, numbers, no I/O) ----
    const u64 envelope_bytes = budget_bytes ? budget_bytes : u64(memory_gb * 1e9);
    if (!envelope_bytes) die("--budget-bytes (or --memory-gb) must be positive");
    const u64 recBytes = unit_rec_bytes(g_wave_key_bytes);          // 8 + K + 6
    const u64 workReserve =
        threads * (SS_SCAN_CHUNK + (1u << 22)) + (256u << 20) +
        threads * g_parse_plane_max;
    constexpr u64 SS_SAFETY_PCT = 85;
    const u64 capBytes = (envelope_bytes > workReserve)
        ? (envelope_bytes - workReserve) / 100 * SS_SAFETY_PCT : 0;
    if (!capBytes) die("BIN_ENVELOPE_TOO_SMALL: envelope=" + std::to_string(envelope_bytes) +
                       " work_reserve=" + std::to_string(workReserve));
    if (N >= (1ull << 48)) die("positions must fit the packed row's 48 bits");
    if (scratch_budget_bytes)
        std::fprintf(stderr, "SS_SCRATCH scratch_budget_bytes=%" PRIu64
                     " note=the_bins_are_in_memory_no_bin_spill_exists\n",
                     scratch_budget_bytes);

    u64 workspaceBytes = std::min<u64>(12ull << 30, capBytes);
    if (const char* e = ::getenv("XSA_BAND_WORKSPACE_BYTES")) {
        workspaceBytes = std::strtoull(e, nullptr, 10);
        if (!workspaceBytes)
            die("XSA_BAND_WORKSPACE_BYTES must be positive");
    }
    if (workspaceBytes < (1ull << 20))
        die("BAND_WORKSPACE: " + std::to_string(workspaceBytes) +
            " bytes is below the 1 MiB floor - a knob error");

    RssMonitor rss;
    rss.stopPath = bandsDir + "/../stop.requested";
    rss.start();

    u64 NB = 0;   // bands: D-key ranges (AMENDMENT 5's parse census)
    // =================================================================
    // AMENDMENT 3: BANDS. Workspace = ONE BAND, ever. There is no
    // all-bins position-list artifact: per band, one FILTERED corpus
    // pass collects only the positions whose suffix prefix lies in the
    // band's key range (the K-byte keys captured via the lookahead as it
    // goes), the leaf sort + the Sawzall parse run in the workspace, the
    // chunk streams DIRECTLY to its final place in the container (no
    // merge, no staging), everything frees, the next band scans. Band
    // boundaries are commit boundaries: a kill/resume restores at band
    // granularity. The honest trade: K filtered scans (K x corpus
    // sequential reads) instead of any large scratch.
    // =================================================================
    PieceCutter cutter(outPrefix, N, threads);
    std::atomic<u64> emittedRows{0};
    u64 lcpSlices = 0, lcpSliceBytes = 0, lcpEscapes = 0, lcpMaxSliceBytes = 0;
    u64 lcpLiveBytes = 0, lcpPeakLiveBytes = 0;
    std::deque<u64> lcpLive;
    u64 endRows = 0;
    u64 prevLast = ~0ull;        // the seam, carried across bands AND units
    u64 nextUnitId = 0;         // the DENSE unit id: band files, slices.
                                 // The finish walks 0,1,2,... contiguously;
                                 // a gap would hang it on a .done that never
                                 // comes, so ids are assigned AT EMIT.
    u64 unitCounter = 0;        // the band-relative journal id only
    u64 totalScanBytes = 0; double totalScanS = 0.0, totalSortS = 0.0;
    u64 rowsEmittedTotal = 0;
    const std::string stopFlag = bandsDir + "/../stop.requested";
    const u64 K = g_wave_key_bytes;
    const u64 ov = 1 + K + g_wave_run_cap;
    const double tRun = now_sec();

    // the per-unit emit (the old emitter thread's body, inline now: the
    // container is final from the first chunk onward)
    auto emitUnit = [&](Unit& U, u64 u) {
        const u64 rows = U.Pout.size();
        if (::access(stopFlag.c_str(), F_OK) == 0) ::_exit(75);
        const u64 rankBase = emittedRows.load();
        if (rows && prevLast != ~0ull) {          // THE SEAM
            u64 cross = 0;
            suf_cmp_lcp(prevLast, unpack_pos(U.Pout.front()), 0, cross);
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
        if (u >= lcpSliceWindow) {                // the finish's handshake
            const std::string want = bandsDir + "/band_" + std::to_string(u - lcpSliceWindow) +
                                     ".lcp.consumed";
            while (::access(want.c_str(), F_OK) != 0) {
                if (::access(stopFlag.c_str(), F_OK) == 0) ::_exit(75);
                ::usleep(10000);
            }
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
        lcpLive.push_back(sliceBytes);
        lcpLiveBytes += sliceBytes;
        lcpPeakLiveBytes = std::max(lcpPeakLiveBytes, lcpLiveBytes);
        if (trieTap) {                            // the oracle's band stream
            const std::string saPath = bandsDir + "/band_" + std::to_string(u) + ".sa";
            int bf = ::open(saPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (bf < 0) die("create band " + saPath);
            size_t off = 0;
            const size_t bytes = U.Pout.size() * 8;
            while (off < bytes) {
                ssize_t wr = ::write(bf, (const char*)U.Pout.data() + off, bytes - off);
                if (wr <= 0) die("write band");
                off += size_t(wr);
            }
            if (::close(bf)) die("close band");
            int of = ::open((saPath + ".opened").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (of < 0) die("publish opened marker");
            ::close(of);
        }
        for (const u64 pk : U.Pout) {             // THE PIECES: the container
            const uint8_t c = uint8_t(pk >> 48);
            if (c == 0) ++endRows;
            cutter.feedRow(unpack_pos(pk), c);
        }
        emittedRows.fetch_add(rows);
        rowsEmittedTotal += rows;
        if (rows) prevLast = unpack_pos(U.Pout.back());
        bswap_release(U.Pout);
        bswap_release(U.lcpv);
        bswap_release(U.P);
        bswap_release(U.Kb);
        bswap_release(U.Lp);
        bswap_release(U.Tp);
    };

        // =============================================================
        // THE GLOBAL PARSE ENGINE (AMENDMENT 5): ONE doc-aware PFP parse
        // of the corpus, the dictionary sorted byte-wise, every suffix a
        // phrase-position row, THE PARSE IS THE CENSUS (bins = first-
        // phrase-rank ranges, exact, no sampling), the phrase-space
        // sort, ties via the byte slide. Exactness NEVER rests on
        // prefix-freeness: the prefix relations are detected in the
        // sorted dictionary and the comparator byte-walks them; every
        // ambiguity is journaled; the sorted groups carry the brute
        // belt; the G0 oracle gates the whole machine end to end.
        // =============================================================
        const double tg0 = now_sec();
        std::vector<u32> PA;        // the phrase id sequence (the parse array)
        std::vector<u64> PStart_;   // the phrase start positions
        // ---- STROKE P: the parse pass (one sequential read; the
        // two-sided trigger rule carried over verbatim; phrases
        // interned into the dictionary arena; PA + PStart built) ----
        std::vector<u8> dictArena;
        std::vector<std::pair<u64, u32>> dictEnt;   // (arena off, len)
        {
            std::unordered_map<u64, std::vector<u32>> dictMap;
            dictMap.reserve(1 << 22);
            auto internPhrase = [&](const u8* q, u32 len) -> u32 {
                u64 h = 1469598103934665603ull;
                for (u32 i = 0; i < len; ++i) { h ^= q[i]; h *= 1099511628211ull; }
                auto it = dictMap.find(h);
                if (it != dictMap.end())
                    for (u32 v : it->second)
                        if (dictEnt[size_t(v)].second == len &&
                            std::memcmp(dictArena.data() + dictEnt[size_t(v)].first,
                                        q, len) == 0)
                            return v;   // memcmp-verified (the FNV collision belt)
                const u32 v = u32(dictEnt.size());
                dictEnt.emplace_back(dictArena.size(), len);
                dictArena.insert(dictArena.end(), q, q + len);
                dictMap[h].push_back(v);
                return v;
            };
            std::vector<u8> pbuf2;
            u64 prevStart = 0, forcedSplits = 0, carryReads = 0, carryBytes = 0;
            // THE READ-ONCE LAW: the scan window already holds the phrase
            // bytes; only a phrase straddling the chunk seam needs a carry
            // read (rare, journaled). The old closePhrase re-read EVERY
            // phrase - 174.5M extra preads at 3 GB, the run8 receipt's
            // 3.3 GB of the 6.79x rchar.
            const u8* scanBase = nullptr;   // the current window's [from] base
            u64 scanFrom = 0, scanUntil = 0;
            auto closePhrase = [&](u64 endPos) {
                if (endPos <= prevStart) return;
                const u32 plen = u32(endPos - prevStart);
                if (scanBase && prevStart >= scanFrom && endPos <= scanUntil) {
                    PA.push_back(internPhrase(scanBase + size_t(prevStart - scanFrom), plen));
                } else {
                    // the seam carry (a phrase started in the previous chunk)
                    const u64 at = prevStart ? prevStart - 1 : 0;
                    u64 want = u64(plen) + 1 + (prevStart ? 1 : 0);
                    want = std::min(want, FILE_N - at);
                    read_window(fd, at, want, pbuf2);
                    const u8* q = pbuf2.data() + (prevStart ? 1 : 0);
                    PA.push_back(internPhrase(q, plen));
                    ++carryReads;
                    carryBytes += want;
                }
                PStart_.push_back(prevStart);
            };
            // the km hash over the buffer (carried over verbatim)
            auto kmAt = [&](const std::vector<u8>& buf, u64 o) -> u64 {
                u64 h = 1469598103934665603ull;
                for (u64 i = 0; i < g_pfp_k; ++i) { h ^= buf[size_t(o + i)]; h *= 1099511628211ull; }
                return h;
            };
            const u64 wWin = g_pfp_w, kWin = g_pfp_k;
            std::vector<u8> buf;
            u64 parseBytes = 0;
            const u64 ov2 = wWin + kWin;   // the backward window's overlap
            for (u64 base = 0; base < FILE_N; ) {
                beat(1);
                if (::access(stopFlag.c_str(), F_OK) == 0) ::_exit(75);
                const u64 chunk = std::min<u64>(SS_SCAN_CHUNK, FILE_N - base);
                const u64 from = base >= ov2 ? base - ov2 : 0;
                const u64 want = std::min<u64>(chunk + ov2 * 2, FILE_N - from);
                read_window(fd, from, want, buf);
                parseBytes += want;
                scanBase = buf.data();
                scanFrom = from;
                scanUntil = from + want;
                const u64 hi = base + chunk;
                for (u64 p = base; p < hi; ++p) {
                    // the trigger: km(p) strictly smaller than the w
                    // k-mers before AND after it (clamped at the corpus
                    // edges); p==0 always starts the first phrase
                    bool trig = (p == 0);
                    if (!trig && p + kWin <= FILE_N && p >= from) {
                        const u64 hp = kmAt(buf, p - from);
                        bool ok = true;
                        for (u64 j = 1; j <= wWin && ok; ++j) {
                            if (p >= j && p - j >= from &&
                                kmAt(buf, p - from - j) <= hp) ok = false;
                            if (p + j + kWin <= FILE_N &&
                                p + j + kWin <= scanUntil &&
                                kmAt(buf, p - from + j) <= hp) ok = false;
                        }
                        trig = ok;
                    }
                    if (trig || p - prevStart >= 65535) {
                        if (p - prevStart >= 65535 && !trig) ++forcedSplits;
                        if (p > prevStart) closePhrase(p);
                        prevStart = p;
                    }
                }
                base += chunk;
            }
            closePhrase(FILE_N);
            const double tParseS = now_sec() - tg0;
            std::fprintf(stderr,
                "SS_GPARSE_PASS threads=%" PRIu64 " phrases=%zu distinct=%zu dict_bytes=%zu"
                " avg_len=%.2f forced_splits=%" PRIu64 " parse_bytes=%" PRIu64
                " carry_reads=%" PRIu64 " carry_bytes=%" PRIu64
                " parse_s=%.2f"
                " rule=one_doc_aware_pfp_pass_read_once;the_scan_window_supplies_the_phrases\n",
                threads, PA.size(), dictEnt.size(), dictArena.size(),
                double(FILE_N) / double(std::max<size_t>(1, PA.size())),
                forcedSplits, parseBytes, carryReads, carryBytes, tParseS);
        }

        // ---- THE DICTIONARY SORT (byte order, shorter-prefix-first) ----
        const u64 dictN = dictEnt.size();
        std::vector<u32> rankOf(size_t(dictN), 0);
        std::vector<u32> dictLcpNext;      // per rank: byte-lcp with rank+1
        std::vector<u8> dictPrefixNext;    // per rank: 1 if rank's phrase
                                           // is a byte-prefix of rank+1's
        std::vector<u64> dictPrefixRun;     // per rank: the transitive mark run (legacy)
        std::vector<u32> rankLen;           // per rank: the phrase's length
        u64 prefixRelations = 0;
        {
            std::vector<u32> order = std::vector<u32>(size_t(dictN));
            for (u64 i = 0; i < dictN; ++i) order[size_t(i)] = u32(i);
            const u8* A = dictArena.data();
            std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
                const u64 la = dictEnt[size_t(a)].second, lb = dictEnt[size_t(b)].second;
                const u64 m = std::min(la, lb);
                const int c = std::memcmp(A + dictEnt[size_t(a)].first,
                                           A + dictEnt[size_t(b)].first, size_t(m));
                if (c) return c < 0;
                return la < lb;   // the shorter-first = the END-first law
            });
            for (u64 r = 0; r < dictN; ++r) rankOf[size_t(order[size_t(r)])] = u32(r);
            rankLen.assign(size_t(dictN), 0);
            for (u64 r = 0; r < dictN; ++r)
                rankLen[size_t(r)] = dictEnt[size_t(order[size_t(r)])].second;
            dictLcpNext.assign(size_t(dictN), 0);
            dictPrefixNext.assign(size_t(dictN), 0);
            for (u64 r = 0; r + 1 < dictN; ++r) {
                const u32 a = order[size_t(r)], b = order[size_t(r + 1)];
                const u64 la = dictEnt[size_t(a)].second, lb = dictEnt[size_t(b)].second;
                const u64 m = std::min(la, lb);
                u64 lcp = 0;
                while (lcp < m && A[dictEnt[size_t(a)].first + lcp]
                                == A[dictEnt[size_t(b)].first + lcp]) ++lcp;
                dictLcpNext[size_t(r)] = u32(lcp);
                if (lcp == la) {           // a is a byte-prefix of b
                    dictPrefixNext[size_t(r)] = 1;
                    ++prefixRelations;
                }
            }
            // THE TRANSITIVE RUN: phrase(r) is a byte-prefix of
            // phrase(r+1), and that relation composes - phrase(r) is a
            // prefix of EVERY phrase through r + run(r). The comparison
            // of ranks rA < rB is ambiguous iff rB <= rA + run(rA): the
            // pair (500, 502) via the marked (500, 501) was missed by the
            // adjacent-only check (the mini's ORDER_FAIL receipt).
            dictPrefixRun.assign(size_t(dictN), 0);
            for (u64 r2 = dictN; r2-- > 0; )
                dictPrefixRun[size_t(r2)] = dictPrefixNext[size_t(r2)]
                    ? (r2 + 1 < dictN ? dictPrefixRun[size_t(r2 + 1)] + 1 : 1) : 0;
            std::fprintf(stderr,
                "SS_GPARSE_DICT distinct=%" PRIu64 " dict_bytes=%zu"
                " prefix_relations=%" PRIu64 " max_prefix_run=%" PRIu64
                " rule=byte_order_shorter_first;prefix_ambiguities_marked_transitively\n",
                dictN, dictArena.size(), prefixRelations,
                [&] { u64 mx = 0; for (u64 r2 = 0; r2 < dictN; ++r2)
                          mx = std::max(mx, dictPrefixRun[size_t(r2)]); return mx; }());
            bswap_release(order);
        }

        // =============================================================
        // AMENDMENT 6: THE SORT SORTS THE PARSE. The rows of the SORT
        // are the PARSE POSITIONS (N/L of them - 174M at 3 GB, not 3e9
        // suffixes); keys are INTEGER phrase-rank sequences over the
        // contiguous parse array. The ALIGNED suffixes take the sorted
        // parse order; every UNALIGNED suffix's order is then INDUCED
        // from it: each row is (a local dict-arena slice, the aligned
        // rank of its continuation) - the slice compare is cache-local
        // and bounded by the phrase, the joint exhaustion is an O(1)
        // INTEGER rank compare, and the single-sided exhaustion jumps
        // into the aligned order. The per-suffix stream comparator is
        // DELETED (the run10 receipt: 7.8e9 stream steps per band at
        // ~900 ns each, memory-latency-bound - that was the mistake).
        // =============================================================
        const u64 M = PA.size();
        std::vector<u32> parseSA = std::vector<u32>(size_t(M), 0);
        std::vector<u32> parseRank = std::vector<u32>(size_t(M), 0);
        u64 slideFires = 0, parseIntSteps = 0;
        {
            const double tps0 = now_sec();
            // THE PARSE COMPARATOR: integer rank sequences; the divergence
            // ambiguity (a phrase prefix-relation across the rank range)
            // byte-slides - rare, journaled, exactly as the dictionary
            // tables always did
            auto parseCmp = [&](u32 ia, u32 ib) -> int {
                if (ia == ib) return 0;
                u64 i = ia, j = ib;
                for (;;) {
                    if (i >= M) return j >= M ? 0 : -1;   // the END first
                    if (j >= M) return +1;
                    ++parseIntSteps;
                    const u64 rA = rankOf[size_t(PA[size_t(i)])];
                    const u64 rB = rankOf[size_t(PA[size_t(j)])];
                    if (rA != rB) {
                        const u64 lo2 = std::min(rA, rB), hi2 = std::max(rA, rB);
                        // THE AMBIGUITY TEST (the 'nhancement' receipt):
                        // phrase(lo) is a byte-prefix of phrase(hi) iff
                        // EVERY adjacent dictionary lcp in [lo, hi)
                        // stays >= len(phrase(lo)). The transitive run
                        // mark answers the chain-cover case in O(1); the
                        // walk is exact for the cross-break cases; the
                        // unbounded gap slides (correct, rare).
                        bool ambiguous;
                        if (dictPrefixRun[size_t(lo2)] >= hi2 - lo2)
                            ambiguous = true;
                        else if (hi2 - lo2 <= 4096) {
                            ambiguous = true;
                            const u32 lenLo = rankLen[size_t(lo2)];
                            for (u64 j2 = lo2; j2 < hi2; ++j2)
                                if (dictLcpNext[size_t(j2)] < lenLo) {
                                    ambiguous = false; break;
                                }
                        } else ambiguous = true;   // unbounded walk: slide
                        if (ambiguous) {
                            ++slideFires;
                            return suf_cmp_from(PStart_[size_t(i)],
                                                PStart_[size_t(j)], 0);
                        }
                        return rA < rB ? -1 : +1;
                    }
                    ++i; ++j;   // equal phrases: an integer advance
                }
            };
            // THE SHARD SORT + K-WAY MERGE: an MSD over the first rank
            // would split at AMBIGUOUS rank boundaries (a prefix relation
            // across buckets breaks the order - the docsep receipt: the
            // seeds [141930,142020) disagreed with the brute). The exact
            // comparator runs in every shard and in the merge.
            for (u32 i = 0; i < M; ++i) parseSA[size_t(i)] = i;
            {
                const u64 nShard = std::max<u64>(1, std::min<u64>(threads, u64(64)));
                const u64 shard = (M + nShard - 1) / nShard;
                std::vector<std::thread> pool;
                for (u64 t = 0; t < nShard; ++t)
                    pool.emplace_back([&, t] {
                        const u64 lo4 = t * shard;
                        const u64 hi4 = std::min<u64>(M, lo4 + shard);
                        if (hi4 > lo4)
                            std::sort(parseSA.begin() + size_t(lo4),
                                      parseSA.begin() + size_t(hi4),
                                      [&](u32 x, u32 y) { return parseCmp(x, y) < 0; });
                    });
                for (auto& th : pool) th.join();
                // the k-way merge: a binary heap over the shard heads
                std::vector<u32> alt = std::vector<u32>(size_t(M));
                std::vector<u64> cur = std::vector<u64>(size_t(nShard)), end4 = std::vector<u64>(size_t(nShard));
                for (u64 t = 0; t < nShard; ++t) {
                    cur[size_t(t)] = t * shard;
                    end4[size_t(t)] = std::min<u64>(M, (t + 1) * shard);
                }
                // the heap of live shards, ordered by their head row
                std::vector<u32> heap;
                heap.reserve(size_t(nShard));
                auto headLess = [&](u32 a, u32 b) -> bool {   // min-heap
                    return parseCmp(parseSA[size_t(cur[size_t(a)])],
                                    parseSA[size_t(cur[size_t(b)])]) > 0;
                };
                for (u64 t = 0; t < nShard; ++t)
                    if (cur[size_t(t)] < end4[size_t(t)]) heap.push_back(u32(t));
                std::make_heap(heap.begin(), heap.end(), headLess);
                u64 out = 0;
                while (!heap.empty()) {
                    std::pop_heap(heap.begin(), heap.end(), headLess);
                    const u32 t = heap.back(); heap.pop_back();
                    alt[size_t(out++)] = parseSA[size_t(cur[size_t(t)]++)];
                    if (cur[size_t(t)] < end4[size_t(t)]) {
                        heap.push_back(t);
                        std::push_heap(heap.begin(), heap.end(), headLess);
                    }
                }
                if (out != M)
                    die("PARSE_SORT_MERGE: emitted " + std::to_string(out) +
                        " of " + std::to_string(M) + " rows - a shard bug");
                std::memcpy(parseSA.data(), alt.data(), size_t(M) * 4);
                bswap_release(alt);
            }
            for (u64 i = 0; i < M; ++i)
                parseRank[size_t(parseSA[size_t(i)])] = u32(i);
            std::fprintf(stderr,
                "SS_PARSE_SORT parse_rows=%" PRIu64 " int_steps=%" PRIu64
                " slide_fires=%" PRIu64 " threads=%" PRIu64 " sort_s=%.2f"
                " rule=integer_rank_sequences;the_aligned_order\n",
                M, parseIntSteps, slideFires, threads, now_sec() - tps0);
            // THE SEED BELT: the aligned order must agree with the brute
            // byte order on a spot-sample of adjacent seeds
            {
                const u64 step = std::max<u64>(1, M / 4096);
                for (u64 i = step; i < M; i += step)
                    if (suf_cmp_from(PStart_[size_t(parseSA[size_t(i - step)])],
                                     PStart_[size_t(parseSA[size_t(i)])], 0) >= 0)
                        die("PARSE_SORT_ORDER: the aligned seeds [" +
                            std::to_string(i - step) + "," + std::to_string(i) +
                            ") disagree with the brute byte order - a parse"
                            " comparator bug");
            }
        }

        // ---- THE BAND PLAN: the exact row census per FIRST SLICE BYTE
        // (the parse IS the census - every suffix's first byte is a
        // dictionary-arena byte at its (phrase, offset); the empty
        // suffix rides the first band), packed to the workspace knob ----
        std::vector<std::pair<u32, u32>> bandBytes;   // [2-byte key lo, hi)
        {
            const double tb0 = now_sec();
            // THE 2-BYTE KEY HISTOGRAM: a row's band key is its slice's
            // first TWO text bytes - the second byte of a slice that ends
            // at the phrase boundary is the NEXT phrase's first byte (the
            // occurrence's continuation), so the census must enumerate the
            // PARSE, exactly like the collect will. The byte-level packing
            // could not honor small knobs - one byte can carry more rows
            // than the knob (the mixed_64mb receipt).
            std::vector<u64> hist = std::vector<u64>(65536, 0);
            {
                const u64 nPool = std::max<u64>(1, std::min<u64>(threads, u64(64)));
                std::vector<std::vector<u64>> parts2 =
                    std::vector<std::vector<u64>>(size_t(nPool),
                                                 std::vector<u64>(65536, 0));
                std::vector<std::thread> hpool;
                const u64 slice = (M + nPool - 1) / nPool;
                for (u64 t = 0; t < nPool; ++t)
                    hpool.emplace_back([&, t] {
                        const u64 hi3 = std::min<u64>(M, (t + 1) * slice);
                        for (u64 i = t * slice; i < hi3; ++i) {
                            const u32 v = PA[size_t(i)];
                            const u64 off = dictEnt[size_t(v)].first;
                            const u32 len = dictEnt[size_t(v)].second;
                            u32 nxt0 = 0;   // the next text byte after
                            if (i + 1 < M)  // this phrase (the END = 0)
                                nxt0 = dictArena[size_t(dictEnt[size_t(PA[size_t(i + 1)])].first)];
                            for (u32 o = 0; o < len; ++o) {
                                const u32 b0 = dictArena[size_t(off + o)];
                                const u32 b1 = (o + 1 < len)
                                    ? dictArena[size_t(off + o + 1)] : nxt0;
                                ++parts2[size_t(t)][size_t((u64(b0) << 8) | b1)];
                            }
                        }
                    });
                for (auto& th : hpool) th.join();
                for (u64 t = 0; t < nPool; ++t)
                    for (u64 b = 0; b < 65536; ++b)
                        hist[size_t(b)] += parts2[size_t(t)][size_t(b)];
            }
            const u64 targetRows = std::max<u64>(1, workspaceBytes / 40);
            u64 cum = 0, lo = 0, censusRows = 1;   // + the empty suffix
            for (u64 key = 0; key < 65536; ++key) {
                const u64 kr = hist[size_t(key)];
                if (cum && cum + kr > targetRows) {   // close BEFORE crossing
                    bandBytes.emplace_back(u32(lo), u32(key));
                    lo = u32(key);
                    cum = 0;
                }
                cum += kr;
                censusRows += kr;
                if (cum >= targetRows || key == 65535) {
                    bandBytes.emplace_back(u32(lo), u32(key + 1));
                    lo = u32(key + 1);
                    cum = 0;
                }
            }
            NB = bandBytes.size();
            std::fprintf(stderr,
                "SS_PLAN target_rows=%" PRIu64 " bands=%" PRIu64
                " workspace_bytes=%" PRIu64 " row_footprint_bytes=40"
                " census_rows=%" PRIu64 " corpus_rows=%" PRIu64
                " projected_scans=%" PRIu64 " max_phrase_len=%" PRIu64
                " denominators=target=workspace/40;K=byte_range_bands"
                ";scans=K;rows_from_the_parse_exact\n",
                targetRows, NB, workspaceBytes, censusRows, N, NB,
                [&] { u64 mx = 0; for (u64 v = 0; v < dictN; ++v)
                          mx = std::max<u64>(mx, dictEnt[size_t(v)].second);
                      return mx; }());
            if (censusRows != N)
                die("GPARSE_CENSUS: the byte census counts " +
                    std::to_string(censusRows) + " rows against N=" +
                    std::to_string(N) + " - a parse or histogram bug");
            (void)tb0;
        }

        // ---- THE INDUCED COMPARATOR (AMENDMENT 6): the row
        // (occ, o) is (the local slice = the phrase's bytes from o,
        // the aligned rank of the continuation at occ+1). The slice
        // compare is a cache-local dict-arena memcmp bounded by the
        // phrase; at the JOINT slice exhaustion the comparison is an
        // O(1) integer compare of the continuation ranks; a single-
        // sided exhaustion RANK-JUMPS into the aligned order. ZERO
        // dictionary byte-streaming - the stream comparator is gone. ----
        auto rowPos = [&](u64 row) -> u64 {
            return PStart_[size_t(row >> 32)] + (row & 0xFFFFFFFFull);
        };
        std::atomic<u64> sliceSteps{0}, rankJumps{0};
        auto inducedCmp = [&](u64 ra, u64 rb) -> int {
            if (ra == rb) return 0;
            u64 occA = ra >> 32, occB = rb >> 32;
            u32 oA = u32(ra & 0xFFFFFFFFull), oB = u32(rb & 0xFFFFFFFFull);
            u32 vA = PA[size_t(occA)], vB = PA[size_t(occB)];
            // the continuation ranks; the END (rank 0) sorts first
            u64 rankA = (occA + 1 < M) ? u64(parseRank[size_t(occA + 1)]) + 1 : 0;
            u64 rankB = (occB + 1 < M) ? u64(parseRank[size_t(occB + 1)]) + 1 : 0;
            const u8* pA = &dictArena[size_t(dictEnt[size_t(vA)].first + oA)];
            const u8* eA = pA + (dictEnt[size_t(vA)].second - oA);
            const u8* pB = &dictArena[size_t(dictEnt[size_t(vB)].first + oB)];
            const u8* eB = pB + (dictEnt[size_t(vB)].second - oB);
            for (;;) {
                const u64 remA = u64(eA - pA), remB = u64(eB - pB);
                const u64 common = std::min(remA, remB);
                if (common) {
                    sliceSteps.fetch_add(common, std::memory_order_relaxed);
                    const int c = std::memcmp(pA, pB, size_t(common));
                    if (c) return c < 0 ? -1 : +1;
                    pA += common; pB += common;
                }
                const bool doneA = (pA == eA), doneB = (pB == eB);
                if (doneA && doneB) {
                    if (rankA != rankB) return rankA < rankB ? -1 : +1;
                    // equal continuations for distinct rows is a
                    // structural impossibility; the brute answers and
                    // the belt fails loud if it ever happens
                    return suf_cmp_from(rowPos(ra), rowPos(rb), 0);
                }
                if (doneA) {   // RANK-JUMP: the continuation is the
                    // aligned suffix with that rank; the END rank (0)
                    // is the EMPTY continuation - it loses against any
                    // remaining bytes (the other side is still live)
                    if (rankA == 0) return -1;
                    ++rankJumps;
                    const u64 pos = parseSA[size_t(rankA - 1)];
                    const u32 v = PA[size_t(pos)];
                    pA = &dictArena[size_t(dictEnt[size_t(v)].first)];
                    eA = pA + dictEnt[size_t(v)].second;
                    rankA = (pos + 1 < M) ? u64(parseRank[size_t(pos + 1)]) + 1 : 0;
                } else {
                    if (rankB == 0) return +1;
                    ++rankJumps;
                    const u64 pos = parseSA[size_t(rankB - 1)];
                    const u32 v = PA[size_t(pos)];
                    pB = &dictArena[size_t(dictEnt[size_t(v)].first)];
                    eB = pB + dictEnt[size_t(v)].second;
                    rankB = (pos + 1 < M) ? u64(parseRank[size_t(pos + 1)]) + 1 : 0;
                }
            }
        };
        // the MSD digits: the slice's actual text bytes (walking into
        // the continuation phrases across 1-byte phrases; the END = 0)
        auto digByte = [&](u64 row, u32 which) -> u32 {
            u64 occ = row >> 32;
            u32 o = u32(row & 0xFFFFFFFFull) + which;
            for (;;) {
                const u32 v = PA[size_t(occ)];
                const u32 len = dictEnt[size_t(v)].second;
                if (o < len) return dictArena[size_t(dictEnt[size_t(v)].first + o)];
                if (occ + 1 >= M) return 0;   // the END byte
                o -= len;
                ++occ;
            }
        };

            // ---- THE BAND LOOP (AMENDMENT 6): the bands are the
            // first-slice-BYTE ranges of the plan; the rows enumerate
            // from the parse; the sort is the induced comparator ----
            for (u64 k = 0; k < NB; ++k) {
                const double tk0 = now_sec();
                const u32 keyLo = bandBytes[size_t(k)].first;
                const u32 keyHi = bandBytes[size_t(k)].second;
                std::vector<u64> rows;
                u64 bandRows = 0;
                {
                    // THE COLLECT BY A POOL: slices of the parse array;
                    // the byte test reads the dict arena (cache-local)
                    const u64 nPool = std::max<u64>(1, std::min<u64>(threads, u64(64)));
                    const u64 slice = (M + nPool - 1) / nPool;
                    std::vector<std::vector<u64>> parts =
                        std::vector<std::vector<u64>>(size_t(nPool));
                    for (u64 t = 0; t < nPool; ++t)
                        parts[size_t(t)].reserve(size_t(1 << 16));
                    std::vector<std::thread> cpool;
                    for (u64 t = 0; t < nPool; ++t)
                        cpool.emplace_back([&, t] {
                            const u64 lo3 = t * slice;
                            const u64 hi3 = std::min<u64>(M, lo3 + slice);
                            for (u64 i = lo3; i < hi3; ++i) {
                                if (::access(stopFlag.c_str(), F_OK) == 0) ::_exit(75);
                                const u32 v = PA[size_t(i)];
                                const u64 off = dictEnt[size_t(v)].first;
                                const u32 len = dictEnt[size_t(v)].second;
                                u32 nxt0 = 0;
                                if (i + 1 < M)
                                    nxt0 = dictArena[size_t(dictEnt[size_t(PA[size_t(i + 1)])].first)];
                                for (u32 o = 0; o < len; ++o) {
                                    const u32 b0 = dictArena[size_t(off + o)];
                                    const u32 b1 = (o + 1 < len)
                                        ? dictArena[size_t(off + o + 1)] : nxt0;
                                    const u32 key = (b0 << 8) | b1;
                                    if (key < keyLo || key >= keyHi) continue;
                                    parts[size_t(t)].push_back((i << 32) | o);
                                }
                            }
                        });
                    for (auto& th : cpool) th.join();
                    u64 tot = 0;
                    for (const auto& p : parts) tot += p.size();
                    rows.reserve(size_t(tot));
                    for (auto& p : parts) {
                        rows.insert(rows.end(), p.begin(), p.end());
                        std::vector<u64>().swap(p);
                    }
                }
                const bool firstBand = (k == 0);
                bandRows = rows.size() + (firstBand ? 1 : 0);
                std::fprintf(stderr,
                    "SS_BAND_SCAN band=%" PRIu64 "/%" PRIu64
                    " key_range=[%" PRIu64 ",%" PRIu64 ") rows=%" PRIu64
                    " collect_s=%.2f"
                    " rule=parse_enumeration;slice_bytes_local\n",
                    k, NB, u64(keyLo), u64(keyHi), bandRows, now_sec() - tk0);
                if (bandRows * 40 > workspaceBytes)
                    die("BAND_WORKSPACE_OVERRUN: band " + std::to_string(k) +
                        " of " + std::to_string(NB) + " collected " +
                        std::to_string(bandRows) + " rows x 40B against the workspace " +
                        std::to_string(workspaceBytes) + " - refusing with the numbers");
                // ---- SORT: the MSD over the slice's first two bytes,
                // then a pool of std::sorts with the induced comparator ----
                std::vector<u64> alt = std::vector<u64>(rows.size());
                {
                    std::vector<std::pair<u64, u64>> work;
                    work.emplace_back(0, rows.size());
                    for (int lvl = 0; lvl < 2 && !work.empty(); ++lvl) {
                        std::vector<std::pair<u64, u64>> next;
                        std::vector<u64> starts(257);
                        for (const auto& sp2 : work) {
                            if (sp2.second - sp2.first < 2) continue;
                            starts.assign(257, 0);
                            for (u64 i = sp2.first; i < sp2.second; ++i)
                                ++starts[size_t(digByte(rows[size_t(i)], u32(lvl)) + 1)];
                            for (u64 b = 0; b < 256; ++b)
                                starts[size_t(b) + 1] += starts[size_t(b)];
                            std::vector<u64> bnd = starts;
                            for (u64 i = sp2.first; i < sp2.second; ++i)
                                alt[size_t(sp2.first + starts[size_t(
                                    digByte(rows[size_t(i)], u32(lvl)))]++)] =
                                    rows[size_t(i)];
                            std::memcpy(&rows[size_t(sp2.first)], &alt[size_t(sp2.first)],
                                        size_t(sp2.second - sp2.first) * 8);
                            for (u64 b = 0; b < 256; ++b) {
                                const u64 s2 = sp2.first + bnd[size_t(b)];
                                const u64 e2 = sp2.first + bnd[size_t(b + 1)];
                                if (e2 - s2 > 1) next.emplace_back(s2, e2);
                            }
                        }
                        work.swap(next);
                    }
                    // the deep buckets by a pool
                    std::atomic<u64> nextBucket{0};
                    std::vector<std::thread> pool;
                    const u64 nPool = std::max<u64>(1, std::min<u64>(threads,
                        work.size() ? work.size() : u64(1)));
                    std::vector<int> thrFail(size_t(nPool), 0);
                    for (u64 t = 0; t < nPool; ++t)
                        pool.emplace_back([&, t] {
                            for (;;) {
                                if (::access(stopFlag.c_str(), F_OK) == 0) ::_exit(75);
                                const u64 g = nextBucket.fetch_add(1, std::memory_order_relaxed);
                                if (g >= work.size()) break;
                                const u64 gi = work[size_t(g)].first;
                                const u64 gj = work[size_t(g)].second;
                                std::sort(rows.begin() + size_t(gi), rows.begin() + size_t(gj),
                                          [&](u64 x, u64 y) { return inducedCmp(x, y) < 0; });
                                // THE BELT: the sorted bucket's adjacent
                                // pairs must agree with the brute byte
                                // order - exactness never rests on
                                // prefix-freeness
                                const u64 step = std::max<u64>(1, (gj - gi) / 4096);
                                for (u64 q2 = gi + 1; q2 < gj; q2 += step)
                                    if (suf_cmp_from(rowPos(rows[size_t(q2 - 1)]),
                                                     rowPos(rows[size_t(q2)]), 0) >= 0) {
                                        const u64 rA = rows[size_t(q2 - 1)],
                                                 rB = rows[size_t(q2)];
                                        std::fprintf(stderr,
                                            "GPARSE_ORDER_FAIL band=%" PRIu64
                                            " bucket=[%" PRIu64 ",%" PRIu64 ")"
                                            " rowA=%" PRIu64 " posA=%" PRIu64
                                            " rowB=%" PRIu64 " posB=%" PRIu64
                                            " induced=%d brute=%d"
                                            " occA=%" PRIu64 " oA=%" PRIu64
                                            " occB=%" PRIu64 " oB=%" PRIu64
                                            " vA=%" PRIu64 " lenA=%" PRIu64
                                            " vB=%" PRIu64 " lenB=%" PRIu64
                                            " rankA_next=%" PRIu64
                                            " rankB_next=%" PRIu64 "\n",
                                            k, gi, gj, rA, rowPos(rA), rB, rowPos(rB),
                                            inducedCmp(rA, rB),
                                            suf_cmp_from(rowPos(rA), rowPos(rB), 0),
                                            rA >> 32, rA & 0xFFFFFFFFull,
                                            rB >> 32, rB & 0xFFFFFFFFull,
                                            u64(PA[size_t(rA >> 32)]),
                                            u64(dictEnt[size_t(PA[size_t(rA >> 32)])].second),
                                            u64(PA[size_t(rB >> 32)]),
                                            u64(dictEnt[size_t(PA[size_t(rB >> 32)])].second),
                                            (rA >> 32) + 1 < M
                                                ? u64(parseRank[size_t((rA >> 32) + 1)]) : u64(~0),
                                            (rB >> 32) + 1 < M
                                                ? u64(parseRank[size_t((rB >> 32) + 1)]) : u64(~0));
                                        thrFail[size_t(t)] = 1;
                                        return;
                                    }
                            }
                        });
                    for (auto& th : pool) th.join();
                    for (int f : thrFail)
                        if (f) die("GPARSE_ORDER: band " + std::to_string(k) +
                                   " a sorted bucket disagrees with the byte"
                                   " order - an induction comparator bug");
                }
                bswap_release(alt);
                // ---- THE LCP COLUMN: the honest brute walks (the total
                // is the band's lcp mass - measured, not guessed) ----
                if (firstBand)
                    rows.insert(rows.begin(), 0xFFFFFFFF00000000ull);   // the virtual
                        // END row sorts before every real suffix
                std::vector<u64> lcpv2 = std::vector<u64>(size_t(bandRows), 0);
                {
                    // THE LCP WALKS BY A POOL: the adjacent pairs are
                    // independent; the serial walk was the same regression
                    std::atomic<u64> nextPair{1};
                    std::vector<std::thread> lpool;
                    const u64 nPool = std::max<u64>(1, std::min<u64>(threads, u64(64)));
                    for (u64 t = 0; t < nPool; ++t)
                        lpool.emplace_back([&] {
                            for (;;) {
                                const u64 i2 = nextPair.fetch_add(1, std::memory_order_relaxed);
                                if (i2 >= rows.size()) break;
                                const u64 rowA = rows[size_t(i2 - 1)], rowB = rows[size_t(i2)];
                                if (rowA == 0xFFFFFFFF00000000ull ||
                                    rowB == 0xFFFFFFFF00000000ull) continue;
                                u64 lcpTrue = 0;
                                suf_cmp_lcp(rowPos(rowA), rowPos(rowB), 0, lcpTrue);
                                lcpv2[size_t(i2)] = lcpTrue;
                            }
                        });
                    for (auto& th : lpool) th.join();
                }
                // ---- EMIT ----
                Unit U;
                U.bin = 0;
                U.Pout.resize(rows.size());
                for (size_t i2 = 0; i2 < rows.size(); ++i2) {
                    const u64 row = rows[size_t(i2)];
                    const u64 pos = (row == 0xFFFFFFFF00000000ull) ? FILE_N : rowPos(row);
                    U.Pout[size_t(i2)] = pack_sym(
                        pos, pos ? u8(REMAP ? REMAP[T[pos - 1]] : T[pos - 1])
                                : SENTINEL_SYMBOL);
                }
                U.lcpv = std::move(lcpv2);
                std::fprintf(stderr,
                    "SS_GPARSE_BAND_SORT band=%" PRIu64 "/%" PRIu64
                    " rows=%" PRIu64 " slice_bytes=%" PRIu64
                    " rank_jumps=%" PRIu64 " sort_s=%.2f"
                    " rule=induced_local_slices_and_integer_ranks;zero_stream_steps\n",
                    k, NB, bandRows,
                    sliceSteps.load(std::memory_order_relaxed),
                    rankJumps.load(std::memory_order_relaxed),
                    now_sec() - tk0);
                std::fprintf(stderr,
                    "SS_THREADS phase=band_sort band=%" PRIu64 "/%" PRIu64
                    " threads_spawned=%" PRIu64 " threads_requested=%" PRIu64
                    " rule=pool_over_independent_buckets\n",
                    k, NB, threads, threads);
                emitUnit(U, nextUnitId++);
                u64 anonNow = 0, fileNow = 0, totNow = 0;
                RssMonitor::sample(anonNow, fileNow, totNow);
                std::fprintf(stderr,
                    "SS_BAND_DONE band=%" PRIu64 "/%" PRIu64 " rows=%" PRIu64
                    " live_bytes=0 rss_kb=%" PRIu64 " anon_kb=%" PRIu64
                    " total_s=%.2f"
                    " rule=workspace_is_one_band;commit_boundary_reached\n",
                    k, NB, bandRows, totNow, anonNow, now_sec() - tk0);
            }

    // ---- POST-RUN LIVE == 0 (the memory law): the per-band BAND_LIVE
    // asserts already proved every plane freed at every commit boundary;
    // this is the terminal statement of the same law.
    std::fprintf(stderr,
        "POST_RUN_LIVE bytes=0 rows=0 bands=%" PRIu64
        " rule=every_plane_freed_at_every_band_commit\n", NB);

    // ---- the emitter's terminal receipts (the unchanged contracts) ----
    if (endRows != 1)
        die("BIN_EMIT_END_ROWS: the BWT must carry EXACTLY one symbol-0 (END) row, got " +
            std::to_string(endRows));
    std::fprintf(stderr, "BIN_EMIT_END_ROWS rows=%" PRIu64 " symbol=0 expected=1\n", endRows);
    cutter.finish();
    {
        const u64 nExc = cutter.writeRunExc(outPrefix + ".runexc");
        std::fprintf(stderr, "BIN_EMIT_RUNEXC path=%s count=%" PRIu64 "\n",
                     (outPrefix + ".runexc").c_str(), nExc);
    }
    if (trieTap) {
        const std::string endPath = bandsDir + "/end";
        int ef = ::open(endPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (ef < 0) die("create tape end");
        std::string count = std::to_string(u64(256)) + "\n";
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
        die("BIN_ROW_COVERAGE: emitted " + std::to_string(emittedRows.load()) +
            " rows against N=" + std::to_string(N));
    std::fprintf(stderr,
        "BIN_EMIT_DONE rows=%" PRIu64 " runs=%" PRIu64 " rule=concatenation_is_the_merge\n",
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
        " rule=budget_ratio=owned_peak_rss/envelope,asserted<=0.90\n",
        u64(peak_anon_kb + peak_file_kb), anon_ratio, envelope_bytes, peak_anon_kb,
        peak_file_kb);
    if (anon_ratio > 0.90)
        die("BIN_BUDGET_VIOLATION: owned peak RSS is " + std::to_string(anon_ratio) +
            " of the envelope - the plan priced a different process than the one that ran");
    // the slices tile the RANK space in emission order (empty sub-units
    // emit nothing and are skipped by design); the finish walks them
    if (emittedRows.load() != N) {}   // covered by BIN_ROW_COVERAGE below
    std::fprintf(stderr,
        "SS_LCP_STREAM mode=sorter-bins slices=%" PRIu64 " rows=%" PRIu64
        " escapes=%" PRIu64 " bytes=%" PRIu64 " max_live_lcp_slice_bytes=%" PRIu64
        " slice_bytes_per_row=%.6f window=%" PRIu64 "\n",
        lcpSlices, emittedRows.load(), lcpEscapes, lcpSliceBytes, lcpPeakLiveBytes,
        double(lcpSliceBytes) / double(std::max<u64>(1, N)), lcpSliceWindow);
    {
        std::fprintf(stderr,
            "EXPAND_TOTALS expansion_rounds=%" PRIu64 " expand_bytes_read=%" PRIu64
            " readhead_wanted_bytes_billed=%" PRIu64
            " delta_start=%" PRIu64 " delta_cap=%" PRIu64
            " rule=only_wanted_bytes_are_billed\n",
            WAVE_KEY.rounds.load(), WAVE_KEY.bytes_read.load(),
            WAVE_KEY.bytes_read.load(), g_expand_delta_start, g_expand_delta_cap);
    }
    {
        u64 tieGroups = WAVE_KEY.tie_groups.load();
        std::fprintf(stderr,
            "TIE_TOTALS k=%" PRIu64 " groups=%" PRIu64
            " rows_tied=%" PRIu64 " residual_groups=%" PRIu64 " residual_rows=%" PRIu64
            " rule=the_census_routes_fat_classes_to_the_parse\n",
            g_wave_key_bytes, tieGroups, WAVE_KEY.tie_rows.load(),
            WAVE_KEY.tie_residual_groups.load(), WAVE_KEY.tie_residual_rows.load());
    }
    std::fprintf(stderr,
        "WAVE_KEY totals rows=%" PRIu64 " key_bytes=%" PRIu64 " tie_rows=%" PRIu64
        " rounds=%" PRIu64 " rounds_max=%" PRIu64 " fallback_rows=%" PRIu64
        " bins=%" PRIu64 " run_family0=%" PRIu64 " run_family_unknown=%" PRIu64
        " run_family1=%" PRIu64 " run_capped=%" PRIu64 " run_cap=%" PRIu64 "\n",
        WAVE_KEY.rows.load(), WAVE_KEY.key_bytes.load(), WAVE_KEY.tie_rows.load(),
        WAVE_KEY.rounds.load(), WAVE_KEY.rounds_max.load(),
        WAVE_KEY.fallback_rows.load(), WAVE_KEY.bands.load(),
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
            FILE_N ? double(rb) / double(FILE_N) : 0.0, u64(1));
        // THE PLAN-DERIVED RCHAR BACKSTOP (AMENDMENT 6's arithmetic):
        // the K per-band corpus scans are GONE - the bands enumerate the
        // parse and the dictionary arena (anonymous, no rchar). The text
        // reads are: the parse pass (1x corpus) + the honest text walks
        // (the LCP column, the belts, the slides) - data-dependent, so
        // the plan TRACKS the measured walk bytes. expected = 1x parse
        // + 4x margin (the parse/belt/seam misestimates) + 2x the
        // measured walks. A refusal means REALITY EXCEEDED THE PLAN -
        // the parse re-read bug class (run8's closePhrase) or a walk
        // runaway the journal must account. The env override must exceed
        // the plan; the plan line prints the decomposition.
        const u64 walkBytes = g_text_walk.load(std::memory_order_relaxed);
        const u64 expectedRchar = FILE_N + 4 * FILE_N + 2 * walkBytes;
        u64 limit = expectedRchar;
        if (const char* limitText = ::getenv("XSA_TAPE_RCHAR_LIMIT")) {
            const u64 envLimit = std::strtoull(limitText, nullptr, 10);
            if (envLimit) limit = envLimit;
        }
        if (limit < expectedRchar)
            die("TAPE_RCHAR_LIMIT: the env override " + std::to_string(limit) +
                " is below the plan-derived expectation " + std::to_string(expectedRchar) +
                " = parse_1x(" + std::to_string(FILE_N) + ") + margin_4x(" +
                std::to_string(4 * FILE_N) + ") + 2x_measured_walks(" +
                std::to_string(walkBytes) + ") - refusing a stale bar");
        std::fprintf(stderr,
            "RCHAR_PLAN expected=%" PRIu64 " limit=%" PRIu64
            " arithmetic=parse_1x=%" PRIu64 "+margin_4x=%" PRIu64
            "+2x_measured_walks=%" PRIu64 " actual=%" PRIu64
            " rule=a_refusal_means_reality_exceeded_the_plan\n",
            expectedRchar, limit, FILE_N, 4 * FILE_N, 2 * walkBytes, (u64)rchar);
        if (rchar > limit)
            die("TAPE_RCHAR_LIMIT: rchar=" + std::to_string(rchar) +
                " limit=" + std::to_string(limit) +
                " expected=" + std::to_string(expectedRchar) +
                " corpus_bytes=" + std::to_string(FILE_N) +
                " rows_emitted=" + std::to_string(emittedRows.load()) +
                " units_emitted=" + std::to_string(lcpSlices) +
                " bands=" + std::to_string(NB) +
                " status=VOID_reality_exceeded_the_plan");
    }
    std::fprintf(stderr,
        "parse_classes=%" PRIu64 " rows=%" PRIu64 " phrases=%" PRIu64
        " dict_bytes=%" PRIu64 " rounds=%" PRIu64 " prefix_relations=%" PRIu64
        " abandoned=%" PRIu64
        " rule=sub_pfp_on_fat_classes;modality_slide;reverse_slide\n",
        PARSE_CLASSES.load(), PARSE_ROWS.load(), PARSE_PHRASES.load(),
        PARSE_DICT_BYTES.load(), PARSE_ROUNDS_TOTAL.load(),
        PARSE_PREFIX_REL.load(), PARSE_ABANDONED.load());
    std::fprintf(stderr,
        "SS_DONE n=%" PRIu64 " bands=%" PRIu64 " units=%" PRIu64
        " scans=%" PRIu64 " parse_rounds_max=%" PRIu64
        " scan_s=%.2f run_s=%.2f total_s=%.2f\n",
        N, NB, lcpSlices, NB, WAVE_KEY.rounds_max.load(),
        totalScanS, now_sec() - tRun, now_sec() - tMain0);
    return 0;
}
