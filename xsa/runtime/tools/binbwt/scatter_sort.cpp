// scatter_sort - THE TAPE MACHINE, prototype.
//
// Replaces "sort positions with random-access LCE at every level" with
// "scatter rows into key-ordered bands once, then sort each band in memory".
//
//   scatter_sort --corpus C --out SA [--ram-gb G] [--threads T]
//                [--sample N] [--scratch DIR] [--keep-rows]
//
// Pipeline:
//   1. SAMPLE   jittered-stride sample of complete 24-byte keys -> sorted ->
//               quantile BOUNDARIES. A sample suffices: bands only have to be
//               balanced, and every row is routed by a key COMPARISON, so a
//               sampling error can only unbalance a band, never misorder a row.
//   2. SCATTER  ONE sequential pass over the tape (T threads over contiguous
//               text ranges). Each row's 24-byte key is compared against the
//               boundaries and the row (key, position) is appended to that
//               band's file. Text is read exactly once and never again during
//               sorting; the sort is pure in-memory work on fixed-width rows.
//   3. SORT     per band, in RAM: compare keys as two big-endian u64s; only
//               rows whose 24-byte keys are EQUAL fall through to a real
//               suffix comparison. On web text that tie set is ~1% (and 97% of
//               that is uniform-byte runs) - see tools/binbwt/kgram_survey.
//   4. EMIT     bands are key ranges, so emitting them in boundary order IS
//               suffix order: the output is a plain concatenation.
//
// Output: the SA stream in the emitted-SSA format (48-byte header + u64/rank),
// which is exactly what --sa-sample-k 1 writes, so `sa_check` and the banked
// containers can verify it directly.
//
// THE LCP COLUMN IS A BAND STREAM, NOT A FILE. On the tape route (--tape-bands)
// every band carries its own LCP slice next to its SA stream - band_<i>.lcp,
// written before band_<i>.done - with the same lifecycle as the band itself:
// slice published -> consumer folds it -> slice deleted -> band_<i>.lcp.consumed.
// The merge that sorts a band already knows every adjacent LCP (it IS the
// comparison), so the whole column costs one store per row on top of the sort it
// was doing anyway, and residency stays K bands of slice, never n: a single
// 1.34n column file cannot be opened by a streaming consumer that only needs its
// own band (and at 10 GB that file was 13.39 GB resident against a 2n law).
//
// Convention: dollar (plain suffix order, shorter suffix smaller) - established
// empirically against the bin machine's own emitted SA on a 4 MB slice.

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dirent.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <fstream>
#include <sstream>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

static void die(const std::string& m) {
    std::fprintf(stderr, "scatter_sort: %s\n", m.c_str());
    std::exit(2);
}

// This is an address-space probe, not the band storage (which uses vectors).
// No explicit huge pages are requested here. Optional reservation flags must
// never force a smaller work unit before ordinary anonymous mmap is tried.
static bool tape_arena_reserve(u64 bytes) {
    const int plain = MAP_PRIVATE | MAP_ANONYMOUS;
    int flags = plain | MAP_NORESERVE;
    for (unsigned fallback = 0; fallback != 2; ++fallback) {
        void* probe = ::mmap(nullptr, (size_t)bytes, PROT_READ | PROT_WRITE,
                            flags, -1, 0);
        if (probe != MAP_FAILED) {
            ::munmap(probe, (size_t)bytes);
            std::fprintf(stderr, "SS_TAPE_ARENA bytes=%" PRIu64
                         " mechanism=anon fallback=%u role=reservation-probe\n", bytes, fallback);
            return true;
        }
        const int error = errno; // preserve before stdio or /proc reads
        struct rlimit limit{};
        ::getrlimit(RLIMIT_AS, &limit);
        unsigned long long vm_pages = 0;
        if (FILE* f = std::fopen("/proc/self/statm", "r")) {
            if (std::fscanf(f, "%llu", &vm_pages) != 1) vm_pages = 0;
            std::fclose(f);
        }
        std::fprintf(stderr,
            "SS_TAPE_ARENA_FAILED bytes=%" PRIu64
            " mechanism=anon flags=%s errno=%d strerror=%s"
            " rlimit_as_bytes=%llu vm_bytes=%llu\n",
            bytes, fallback ? "MAP_PRIVATE|MAP_ANONYMOUS" :
                "MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE",
            error, std::strerror(error), (unsigned long long)limit.rlim_cur,
            vm_pages * (unsigned long long)::sysconf(_SC_PAGESIZE));
        if (!fallback) {
            std::fprintf(stderr, "SS_TAPE_ARENA_FALLBACK reason=mmap-noreserve-failed"
                         " bytes=%" PRIu64 " errno=%d\n", bytes, error);
            flags = plain;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// LIVE DESCRIPTORS. The scatter holds band-part files and every band worker
// holds the bands it reads; when the count exceeds the limit the failure is a
// std::system_error out of a worker (rc=134), not a bad_alloc - the second
// 100 GB attempt died exactly there. /proc/self/fd is the only honest source
// (a hand counter drifts the moment an open fails), and it is one opendir.
// ---------------------------------------------------------------------------
static std::atomic<u64> FD_PEAK{0};
static u64 liveFdCount() {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return 0;
    u64 n = 0;
    while (struct dirent* e = ::readdir(d))
        if (e->d_name[0] != '.') ++n;
    ::closedir(d);
    return n;                        // the opendir's own fd is counted, by one
}
static u64 noteFdPeak() {
    const u64 n = liveFdCount();
    if (n > FD_PEAK.load(std::memory_order_relaxed)) FD_PEAK.store(n);
    return n;
}
static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}
static inline u64 bswap64(u64 x) { return __builtin_bswap64(x); }

// One keyed row carries 24 real bytes and a packed position/symbol.
// Finite tails are held separately and never receive a padded key.
struct Row {
    u64 klo, khi, kq, pos;   // 24-byte key (3 big-endian words) + packed pos/sym
};

static const u8* T = nullptr;
// N includes one structural END suffix at FILE_N. Its realization is chosen
// by preflight; comparisons exhaust the finite corpus before accessing END.
// Byte transport cells remain eight bits; position zero identifies the END BWT row.
static u64 N = 0, FILE_N = 0;
static u64 KEY_N = 0; // only positions with 24 real bytes may enter the key sorter
static constexpr u8 SENTINEL_SYMBOL = 0;
static const u8* REMAP = nullptr;   // optional: raw byte -> remapped symbol
// Two raw bytes -> their two remapped bytes, packed big-endian. A tie comparison
// then consumes 2 bytes per table lookup instead of 1.
static uint16_t PAIRMAP[65536];
static bool PAIRMAP_READY = false;
static inline void build_pairmap() {
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned b = 0; b < 256; ++b) {
            const unsigned ma = REMAP ? REMAP[a] : a;
            const unsigned mb = REMAP ? REMAP[b] : b;
            PAIRMAP[(a << 8) | b] = (uint16_t)((ma << 8) | mb);
        }
    PAIRMAP_READY = true;
}
static inline u64 pack_sym(u64 p, u8 sym) { return p | (u64(sym) << 48); }
static inline u64 unpack_pos(u64 v) { return v & ((1ull << 48) - 1); }

// A complete 24-byte key, big-endian packed. Tail positions are forbidden.
static inline void key_at(u64 p, u64& klo, u64& khi, u64& kq) {
    if (p >= KEY_N) die("tail position passed to key_at");
    u8 b[24];
    std::memcpy(b, T + p, 24);
    if (REMAP) for (u64 i = 0; i < 24; ++i) b[i] = REMAP[b[i]];
    u64 lo, hi, q;
    std::memcpy(&lo, b, 8);
    std::memcpy(&hi, b + 8, 8);
    std::memcpy(&q, b + 16, 8);
    klo = bswap64(lo);
    khi = bswap64(hi);
    kq = bswap64(q);
}

// Full suffix comparison in dollar mode (shorter is smaller).
// `from` is safe to pass as 24: the comparator only reaches here when the first
// 24 key bytes are EQUAL, so comparing them again is pure waste (and it is the
// dominant cost of the tie set).
// The same comparison, but it also reports HOW FAR the two suffixes agree. The
// caller is the merge sort, where that number IS the adjacent-rank LCP - the value
// the finish otherwise re-derives later with 1.28e11 random text reads.
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
    const u64 cap0 = cap;
    if (!REMAP) {   // fast path: identity alphabet, 8 bytes at a time
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
    } else {        // product alphabet: compare remapped symbols
        for (u64 i = from; i < cap0; ++i) {
            const u8 xb = REMAP[T[a + i]], yb = REMAP[T[b + i]];
            if (xb != yb) return xb < yb ? -1 : 1;
        }
    }
    if (a == b) return 0;
    return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
}



// ---------------------------------------------------------------------------
// THE POOL: threads and buffers live for the WHOLE run.
//
// The first wave implementation spawned `threads` threads per wave and allocated
// `nparts x nbands` staging buffers per wave. With ~20 waves at the 2n cap that is
// 20 thread-spawn storms and 20 rounds of allocator churn - measured 34% of the
// wall as sys time. A wave should cost its work and nothing else.
// ---------------------------------------------------------------------------
struct Pool {
    std::vector<std::thread> ts;
    std::mutex mu;
    std::condition_variable cv, done;
    std::function<void(unsigned)> fn;
    u64 gen = 0;
    unsigned pending = 0;
    bool stop = false;
    explicit Pool(unsigned n) {
        for (unsigned i = 0; i < n; ++i) ts.emplace_back([this, i] { loop(i); });
    }
    ~Pool() {
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cv.notify_all();
        for (auto& t : ts) t.join();
    }
    void run(const std::function<void(unsigned)>& f) {
        {
            std::unique_lock<std::mutex> lk(mu);
            fn = f;
            ++gen;
            pending = (unsigned)ts.size();
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lk(mu);
        done.wait(lk, [&] { return pending == 0; });
    }
    void loop(unsigned i) {
        u64 seen = 0;
        for (;;) {
            std::function<void(unsigned)> f;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return stop || gen != seen; });
                if (stop) return;
                seen = gen;
                f = fn;
            }
            f(i);
            {
                std::lock_guard<std::mutex> lk(mu);
                if (--pending == 0) done.notify_all();
            }
        }
    }
};

// ---------------------------------------------------------------------------
// WAVES: the scratch cap is a DESIGN CONSTRAINT, not a hope.
//
// The first version wrote every band to disc before sorting any of them, so peak
// scratch was the whole intermediate: 32 B x n = 42 TB at pile. Wrong by design.
// Now the work runs in WAVES: each wave owns a contiguous KEY RANGE big enough
// that (rows in wave) x (row bytes) <= scratch_cap, and each wave's rows are
// written, sorted, emitted and DELETED before the next wave is written. Peak
// scratch is therefore one wave, i.e. the cap, and the corpus is re-read once
// per wave (190 s at pile - nothing against the win).
//
// Order is preserved because a wave is a key range and waves run in ascending key
// order, so concatenating the waves' outputs IS suffix order.
// ---------------------------------------------------------------------------
// =========================== THE LCP-CARRYING MERGE ==========================
// Sorting a band with a comparison sort throws away the one thing the finish
// needs: how far each adjacent pair agreed. A bottom-up merge does not - the
// comparison that decides who goes next IS the cross-run LCP, and the classic
// merge identity finishes the job:
//
//   emitting e from run A, having last emitted from B:
//       lcp(e) = min( internal_lcp_of_B_head , cross_lcp )
//
// so the whole adjacent-LCP array costs ONE STORE per element on top of the
// comparisons the sort was making anyway. No inversion, no random scatter, no
// re-derivation: the finish's 1.28e11 random reads become a sequential stream.
//
// v[] is sorted in place; out_lcp[k] = LCP(v[k-1], v[k]) (out_lcp[0] = 0).
// Values >= 255 are recorded in `esc` as (rank, value).
// Exact row comparison that also yields the LCP. The three key words are
// big-endian, so an integer compare IS the byte order, and clz of the xor gives
// the number of leading EQUAL bytes for free. Only a full 24-byte tie falls
// through to the text.
static inline int row_cmp_lcp(const Row& a, const Row& b, u64& lcp) {
    if (a.klo != b.klo) {
        lcp = u64(__builtin_clzll(a.klo ^ b.klo)) >> 3;
        return a.klo < b.klo ? -1 : 1;
    }
    if (a.khi != b.khi) {
        lcp = 8 + (u64(__builtin_clzll(a.khi ^ b.khi)) >> 3);
        return a.khi < b.khi ? -1 : 1;
    }
    if (a.kq != b.kq) {
        lcp = 16 + (u64(__builtin_clzll(a.kq ^ b.kq)) >> 3);
        return a.kq < b.kq ? -1 : 1;
    }
    return suf_cmp_lcp(unpack_pos(a.pos), unpack_pos(b.pos), 24, lcp);
}

static void merge_sort_lcp(std::vector<Row>& v, std::vector<uint8_t>& out_lcp,
                           std::vector<std::pair<u64, u64>>& esc, u64 rank_base) {
    const size_t n = v.size();
    if (n < 2) { out_lcp.assign(n ? n : 0, 0); return; }
    std::vector<Row> buf(n);
    // FULL u64 values are carried through the levels: an element's index in an
    // intermediate buffer is NOT its final rank, so recording escapes at that point
    // attached them to the wrong ranks (measured: rank 279 with an escape of 297
    // against a true value of 14). Convert once, at the end, by final rank.
    std::vector<u64> lcp_a(n, 0), lcp_b(n, 0);
    Row* a = v.data();
    Row* b = buf.data();
    u64* la = lcp_a.data();
    u64* lb = lcp_b.data();
    for (size_t w = 1; w < n; w <<= 1) {
        for (size_t i0 = 0; i0 < n; i0 += 2 * w) {
            const size_t i1 = std::min(i0 + w, n), i2 = std::min(i0 + 2 * w, n);
            if (i1 >= i2) { for (size_t k = i0; k < i2; ++k) { b[k] = a[k]; lb[k] = la[k]; } continue; }
            size_t i = i0, j = i1, k = i0;
            int prev = 0;
            u64 l = 0;   // LCP(last emitted, other run's current head) == last cross
            while (i < i1 && j < i2) {
                u64 cross = 0;
                const int c = row_cmp_lcp(a[i], a[j], cross);
                if (c < 0) {
                    b[k] = a[i];
                    lb[k] = (k == i0 || prev == 1) ? la[i] : l;
                    prev = 1; ++i;
                } else {
                    b[k] = a[j];
                    lb[k] = (k == i0 || prev == 2) ? la[j] : l;
                    prev = 2; ++j;
                }
                l = cross;
                ++k;
            }
            while (i < i1) { b[k] = a[i]; lb[k] = (prev == 1) ? la[i] : l; prev = 1; ++i; ++k; }
            while (j < i2) { b[k] = a[j]; lb[k] = (prev == 2) ? la[j] : l; prev = 2; ++j; ++k; }
        }
        std::swap(a, b);
        std::swap(la, lb);
    }
    if (a != v.data()) {
        std::memcpy(v.data(), a, n * sizeof(Row));
        std::memcpy(lcp_a.data(), la, n * sizeof(u64));
    }
    out_lcp.assign(n, 0);
    for (size_t k = 0; k < n; ++k) {
        const u64 val = lcp_a[k];
        if (val >= 255) { esc.emplace_back(rank_base + k, val); out_lcp[k] = 255; }
        else out_lcp[k] = uint8_t(val);
    }
}

// ===========================================================================
// THE BAND BUDGET (see the call site in main for the reasoning). n appears in no
// numerator: the envelope gives the work unit, and the band count and the wave
// count are OUTPUTS.
// ===========================================================================
static constexpr u64 SS_SAFETY_PCT = 85;          // of the arena left after fixed + stage
static constexpr u64 SS_SCAN_CHUNK = 4u << 20;    // one windowed read buffer per thread
static constexpr u64 SS_CTL_BYTES = 64u << 20;    // paths, per-band bookkeeping, allocator overhead
static constexpr u64 SS_STAGE_MIN = 64u << 20;    // the scatter's append staging: a fixed fraction
static constexpr u64 SS_STAGE_MAX = 2u << 30;     // of the envelope, clamped (the same numbers the
static constexpr u64 SS_STAGE_DIVISOR = 24;       // scatter below allocates with)
static constexpr u64 SS_OVERSUBSCRIBE = 4;        // bands per worker per wave (work stealing needs >1)
static constexpr u64 SS_ROWS_FLOOR = 65536;       // one fold band: below this the budget is broken
static constexpr u64 SS_BUDGET_RATIO_MAX = 90;    // the exit assertion: owned RSS <= 90% of the envelope

struct TapeBudget {
    u64 envelope = 0;        // the arena envelope this process may commit
    u64 bytes_per_row = 0;   // the sort's own footprint of ONE row, term by term
    u64 bands_in_flight = 0; // K: band arenas that can hold rows at the same time
    u64 fixed = 0;           // read windows + the sorted sample + histograms + control
    u64 stage = 0;           // the scatter's append staging
    u64 arena = 0;           // what the band columns may use
    u64 cap_rows = 0;        // rows admitted across all bands at once (= arena / bpr)
    u64 rows_per_band = 0;   // THE WORK UNIT: the envelope's, capped by the window
    u64 rows_per_band_budget = 0; // the envelope's own unit (arena / (bpr x K))
    u64 rows_per_band_wave = 0;   // what the scratch window allows (>= budget -> budget binds)
    u64 arena_planned = 0;   // K bands of the unit: the rows the gate admits at most
    u64 rows_per_wave = 0;   // an OUTPUT
    u64 bands_per_wave = 0;  // an OUTPUT
    u64 waves = 0;           // an OUTPUT (n / rows_per_wave)
};

static TapeBudget tape_budget_plan(u64 envelope, u64 threads, u64 k_in_flight, bool lcp_on,
                                   u64 sample_slots, u64 n, u64 disc_cap_bytes, u64 row_bytes,
                                   u64 override_rows_per_band = 0) {
    TapeBudget b;
    b.envelope = envelope;
    // EVERYTHING the sorter holds per row. The wire row is 32 B and the sort alone
    // is 81 B; the arena has to carry both plus the write staging.
    b.bytes_per_row = lcp_on ? (2 * row_bytes + 2 * 8 + 1 + 8 + 1) : (row_bytes + 8 + 1);
    b.bands_in_flight = std::max<u64>(1, k_in_flight);
    // The sample array can never be longer than the corpus, and the control
    // reserve is a function of the envelope (a 50 MB envelope cannot carry a
    // 64 MiB control term - that would price a process nobody runs).
    const u64 sample_bytes = (std::min<u64>(sample_slots, n) + 2) * 8;
    const u64 ctl = std::max<u64>(1u << 20, std::min<u64>(SS_CTL_BYTES, envelope / 64));
    b.fixed = threads * (SS_SCAN_CHUNK + 4096) + threads * 256 * 8 + sample_bytes + ctl;
    b.stage = envelope / SS_STAGE_DIVISOR;
    if (b.stage < SS_STAGE_MIN) b.stage = SS_STAGE_MIN;
    if (b.stage > SS_STAGE_MAX) b.stage = SS_STAGE_MAX;
    const u64 overhead = b.fixed + b.stage;
    const u64 avail = envelope > overhead ? envelope - overhead : 0;
    b.arena = avail / 100 * SS_SAFETY_PCT;
    const u64 denom = std::max<u64>(1, b.bytes_per_row * b.bands_in_flight);
    // THE BUDGET UNIT: the arena divided by what can be in flight. This is the
    // number the 100 GB run got wrong (it derived it from 2n instead).
    const u64 budget_rows = std::max<u64>(1, b.arena / denom);
    // THE SCRATCH WINDOW (2n by law) is the other side of the same coin: a wave
    // must fit on disc. It decides how MANY budget-sized bands a wave can hold
    // (work stealing wants more than one, so up to SS_OVERSUBSCRIBE per worker) -
    // never how big a band may be. When the window cannot even hold one
    // budget-sized band, the band shrinks to the window (a tiny corpus cannot use
    // a big envelope) and the plan says so.
    const u64 disc_rows = std::max<u64>(1, (disc_cap_bytes - disc_cap_bytes / 20) / row_bytes);
    u64 nbands_target = disc_rows / budget_rows;
    if (nbands_target < 1) nbands_target = 1;
    if (nbands_target > SS_OVERSUBSCRIBE * b.bands_in_flight)
        nbands_target = SS_OVERSUBSCRIBE * b.bands_in_flight;
    b.rows_per_band = std::max<u64>(1, std::min(budget_rows, disc_rows / nbands_target));
    b.rows_per_band_budget = budget_rows;
    b.rows_per_band_wave = b.rows_per_band < budget_rows ? b.rows_per_band : budget_rows;
    if (override_rows_per_band) b.rows_per_band = std::min(b.rows_per_band, override_rows_per_band);
    b.cap_rows = std::max<u64>(b.rows_per_band, b.arena / std::max<u64>(1, b.bytes_per_row));
    // THE ARENA AS PLANNED: K bands of the unit (what the gate admits at most).
    b.arena_planned = std::min(b.bands_in_flight, std::max<u64>(1, nbands_target)) * b.rows_per_band;
    b.rows_per_wave = std::max<u64>(1, std::min(disc_rows,
        nbands_target * b.rows_per_band));
    b.bands_per_wave = std::max<u64>(1, (b.rows_per_wave + b.rows_per_band - 1) / b.rows_per_band);
    b.waves = std::max<u64>(1, (n + b.rows_per_wave - 1) / b.rows_per_wave);
    return b;
}

// ---------------------------------------------------------------------------
// THE ROW GATE: rows admitted into band arenas at once. This is what makes the
// arena bound hold when the DATA makes one band fatter than the quantile cut
// promised: a worker blocks until its band fits, so an oversize band costs
// concurrency, never budget. A band larger than the whole cap is admitted alone
// (nothing else is live) - the one case where the arena is a band's, not the
// plan's - and `peak` makes that visible.
// ---------------------------------------------------------------------------
struct RowGate {
    std::mutex mu;
    std::condition_variable cv;
    u64 cap, live = 0, peak = 0, waits = 0;
    explicit RowGate(u64 c) : cap(std::max<u64>(1, c)) {}
    void admit(u64 rows) {
        std::unique_lock<std::mutex> lk(mu);
        if (!(live == 0 || live + rows <= cap)) ++waits;
        cv.wait(lk, [&] { return live == 0 || live + rows <= cap; });
        live += rows;
        if (live > peak) peak = live;
    }
    void release(u64 rows) {
        {
            std::lock_guard<std::mutex> lk(mu);
            live -= rows;
        }
        cv.notify_all();
    }
    void shrink_to(u64 new_cap) {
        {
            std::lock_guard<std::mutex> lk(mu);
            cap = std::max<u64>(SS_ROWS_FLOOR, new_cap);
        }
        cv.notify_all();
    }
    u64 cap_now() {
        std::lock_guard<std::mutex> lk(mu);
        return cap;
    }
    u64 wait_count() {
        std::lock_guard<std::mutex> lk(mu);
        return waits;
    }
};

// ---------------------------------------------------------------------------
// THE RSS RECEIPT. ru_maxrss is the peak of EVERYTHING the process has mapped,
// which includes the corpus mapping - clean, reclaimable page cache that can
// never OOM the build but DOES scale with n. The budget assertion is therefore
// stated on the OWNED (anon) bytes, sampled from /proc/self/smaps_rollup, and the
// mapped figure is reported beside it so nothing is hidden.
// ---------------------------------------------------------------------------
struct RssMonitor {
    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<u64> peak_anon{0}, peak_file{0}, peak_total{0};
    // /proc/self/status carries the split natively (VmRSS / RssAnon / RssFile)
    // and costs a fraction of smaps_rollup's page-table walk on a 100 GB mapping.
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
                }
                for (int i = 0; i < 5 && !stop.load(std::memory_order_relaxed); ++i) ::usleep(100000);
            }
        });
    }
    void finish() {
        if (!th.joinable()) return;
        stop.store(true);
        th.join();
        u64 a = 0, f = 0, t = 0;   // one last sample: the receipt must include the tail
        if (sample(a, f, t)) {
            if (a > peak_anon.load()) peak_anon.store(a);
            if (f > peak_file.load()) peak_file.store(f);
            if (t > peak_total.load()) peak_total.store(t);
        }
    }
};

// Drop whatever the random tie walks left resident in the corpus mapping. The
// bytes stay in PAGE CACHE (this is MAP_PRIVATE on a clean file: nothing can be
// lost, no writeback happens), so a re-touch is a minor fault, not a read - but
// the pages leave the process's RSS, which is the number the budget is stated in.
static void madv_release(const u8* base, u64 bytes) {
    if (!base || !bytes) return;
    const u64 page = 4096;
    const u64 lo = (u64)(uintptr_t)base;
    if (bytes < page) return;
    const u64 span = bytes / page * page;
    (void)lo;
    ::madvise((void*)base, (size_t)span, MADV_DONTNEED);
}

// Read text bytes through a WINDOW: buffered sequential pread into a reused
// buffer, remapped once in place, so the pages live in PAGE CACHE and not in this
// process's RSS (a mapped sequential walk faults a page every 4 KB: measured
// 19 ns/position against 6.89 GB/s for buffered sequential reads).
static void read_window(int fd, u64 from, u64 want, std::vector<u8>& buf) {
    if (buf.size() < want) buf.resize(size_t(want));
    size_t got = 0;
    while (got < want) {
        ssize_t r = ::pread(fd, buf.data() + got, size_t(want) - got, off_t(from + got));
        if (r <= 0) die("pread text window");
        got += size_t(r);
    }
    if (REMAP) for (u64 i = 0; i < want; ++i) buf[i] = REMAP[buf[i]];
}

// Free a vector WITHOUT asking the allocator for anything (shrink_to_fit may
// reallocate; swap with an empty vector only releases). Used on the shrink path,
// where the process is already out of memory.
template <class V>
static void bswap_release(V& v) {
    V().swap(v);
}

// THE LOW-MEMORY BAND ARM (33 B/row: the rows themselves + the u8 column).
// In-place comparison sort, then the adjacent LCP by DIRECT comparison of the
// two neighbours - no ping-pong buffer, no u64 LCP arrays. It produces exactly
// the slice merge_sort_lcp produces (out_lcp[k] = LCP(v[k-1], v[k]), escapes for
// values >= 255), which the byte-identity gates check: the same run forced onto
// this arm must publish the same container. This is the arm a band falls back to
// when its planned arena cannot be allocated - a slower answer, never an abort.
static void sort_band_inplace(std::vector<Row>& v, std::vector<uint8_t>& out_lcp,
                              std::vector<std::pair<u64, u64>>& esc, u64 rank_base) {
    std::sort(v.begin(), v.end(), [](const Row& a, const Row& b) {
        if (a.klo != b.klo) return a.klo < b.klo;
        if (a.khi != b.khi) return a.khi < b.khi;
        if (a.kq != b.kq) return a.kq < b.kq;
        return suf_cmp_from(unpack_pos(a.pos), unpack_pos(b.pos), 24) < 0;
    });
    out_lcp.assign(v.size(), 0);
    esc.clear();
    for (size_t k = 1; k < v.size(); ++k) {
        u64 lcp = 0;
        suf_cmp_lcp(unpack_pos(v[k - 1].pos), unpack_pos(v[k].pos), 0, lcp);
        if (lcp >= 255) { esc.emplace_back(rank_base + k, lcp); out_lcp[k] = 255; }
        else out_lcp[k] = uint8_t(lcp);
    }
}

// Refine equal cached keys by the next byte, using the same count/prefix/scatter
// subdivision as the bin builder. American-flag swaps avoid a second row arena.
// Large tie groups split proactively; elapsed time is a tuning signal only.
static void sort_tape_keys(std::vector<Row>& rows, u64 band) {
    const double started = now_sec();
    const char* setting = ::getenv("SS_SORT_SPLIT_S");
    const double threshold = setting && *setting ? std::strtod(setting, nullptr) : 1.0;
    if (!(threshold > 0)) die("SS_SORT_SPLIT_S must be positive");
    auto keyLess = [](const Row& a, const Row& b) {
        if (a.klo != b.klo) return a.klo < b.klo;
        if (a.khi != b.khi) return a.khi < b.khi;
        return a.kq < b.kq;
    };
    std::sort(rows.begin(), rows.end(), keyLess);
    struct Task { size_t lo, hi; u64 depth; };
    std::vector<Task> stack;
    u64 splits = 0, maxDepth = 0, largest = 0;
    auto leaf = [&](size_t lo, size_t hi, u64 depth) {
        std::sort(rows.begin() + lo, rows.begin() + hi, [depth](const Row& a, const Row& b) {
            return suf_cmp_from(unpack_pos(a.pos), unpack_pos(b.pos), depth) < 0;
        });
    };
    for (size_t lo = 0; lo < rows.size();) {
        size_t hi = lo + 1;
        while (hi < rows.size() && !keyLess(rows[lo], rows[hi])) ++hi;
        // A byte-at-a-time walk through a long uniform run is quadratic.
        // Scan each disjoint corpus run once in position order, then partition
        // by its next differing byte. The cached key fields are scratch only
        // within this equal-key group and are restored before returning.
        const Row saved = rows[lo];
        if (hi - lo > 1024 && saved.klo == saved.khi && saved.klo == saved.kq &&
            saved.klo == (saved.klo & 255) * 0x0101010101010101ull) {
            std::sort(rows.begin() + lo, rows.begin() + hi, [](const Row& a, const Row& b) {
                return unpack_pos(a.pos) < unpack_pos(b.pos);
            });
            u64 runEnd = 0;
            const unsigned symbol = saved.klo & 255;
            for (size_t i = lo; i < hi; ++i) {
                const u64 pos = unpack_pos(rows[i].pos);
                if (pos >= runEnd) {
                    runEnd = pos + 24;
                    while (runEnd < FILE_N && unsigned(REMAP ? REMAP[T[runEnd]] : T[runEnd]) == symbol)
                        ++runEnd;
                }
                rows[i].klo = runEnd - pos;
                rows[i].khi = runEnd == FILE_N || unsigned(REMAP ? REMAP[T[runEnd]] : T[runEnd]) < symbol ? 0 : 1;
                maxDepth = std::max(maxDepth, rows[i].klo);
            }
            std::sort(rows.begin() + lo, rows.begin() + hi, [](const Row& a, const Row& b) {
                if (a.khi != b.khi) return a.khi < b.khi;
                if (a.klo != b.klo) return a.khi == 0 ? a.klo < b.klo : a.klo > b.klo;
                return suf_cmp_from(unpack_pos(a.pos), unpack_pos(b.pos), a.klo) < 0;
            });
            for (size_t i = lo; i < hi; ++i) {
                rows[i].klo = saved.klo; rows[i].khi = saved.khi; rows[i].kq = saved.kq;
            }
            ++splits; largest = std::max<u64>(largest, hi - lo);
            std::fprintf(stderr, "SS_SPLIT band=%llu rows=%zu kind=uniform-run-next-byte max_depth=%llu\n",
                (unsigned long long)band, hi - lo, (unsigned long long)maxDepth);
            lo = hi; continue;
        }
        if (hi - lo > 1) {
            if (hi - lo > 1024 || now_sec() - started >= threshold)
                stack.push_back({lo, hi, 24});
            else leaf(lo, hi, 24);
            while (!stack.empty()) {
                const Task task = stack.back(); stack.pop_back();
                size_t count[257] = {}, begin[257], next[257];
                auto code = [&](const Row& row) -> unsigned {
                    const u64 p = unpack_pos(row.pos);
                    if (task.depth >= FILE_N - p) return 0;
                    const u8 c = T[p + task.depth];
                    return 1 + unsigned(REMAP ? REMAP[c] : c);
                };
                for (size_t i = task.lo; i < task.hi; ++i) ++count[code(rows[i])];
                size_t offset = task.lo;
                for (unsigned c = 0; c < 257; ++c) {
                    begin[c] = next[c] = offset; offset += count[c];
                }
                for (unsigned c = 0; c < 257; ++c) {
                    const size_t end = begin[c] + count[c];
                    while (next[c] < end) {
                        const unsigned target = code(rows[next[c]]);
                        if (target == c) ++next[c];
                        else std::swap(rows[next[c]], rows[next[target]++]);
                    }
                }
                ++splits; maxDepth = std::max(maxDepth, task.depth + 1);
                largest = std::max<u64>(largest, task.hi - task.lo);
                // END bucket has at most one distinct finite suffix.
                for (unsigned c = 1; c < 257; ++c) {
                    if (count[c] > 1024) stack.push_back({begin[c], begin[c] + count[c], task.depth + 1});
                    else if (count[c] > 1) leaf(begin[c], begin[c] + count[c], task.depth + 1);
                }
            }
        }
        lo = hi;
    }
    if (splits) std::fprintf(stderr,
        "SS_SPLIT band=%llu nodes=%llu largest_rows=%llu max_depth=%llu wall_s=%.6f threshold_s=%.3f policy=next-byte-large-ties-or-elapsed\n",
        (unsigned long long)band, (unsigned long long)splits, (unsigned long long)largest,
        (unsigned long long)maxDepth, now_sec() - started, threshold);
}

int main(int argc, char** argv) {
    std::string corpus, out, scratch = "/tmp/ss_scratch", lcpOut, isaOut;
    u64 isaK = 1024;
    double ram_gb = 1.0;
    double scratch_cap_gb = 0.0;      // 0 = 2n (the operator's law)
    u64 threads = 32, par_bands = 0, sample_n = 4000000;
    bool keep_rows = false;
    std::string selftestOracle;
    std::string remapPath, sort_mode = "std", tapeBands, orderSample;
    bool noLcpSlices = false;   // A/B arm: the finish walks text (XSA_LCE_VERIFY=1)
    bool force_inplace = false; // A/B arm: the low-memory (in-place + LCE) band sort
    u64 sampleLo = 0, sampleCount = 4096;
    u64 pendingBands = 8;
    u64 budget_bytes = 0;       // THE ARENA ENVELOPE: the only input that sizes work
    double memory_gb = 0.0;     // the phase's --memory-gb, journaled (not used to size)

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() { if (i + 1 >= argc) die("missing value for " + a); return std::string(argv[++i]); };
        if (a == "--order-sample-out") orderSample = nxt();
        else if (a == "--order-sample-lo") sampleLo = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--order-sample-count") sampleCount = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--tape-bands") tapeBands = nxt();
        else if (a == "--pending-bands") pendingBands = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--selftest") selftestOracle = nxt();
        else if (a == "--corpus") corpus = nxt();
        else if (a == "--out") out = nxt();
        else if (a == "--scratch") scratch = nxt();
        else if (a == "--ram-gb") ram_gb = std::atof(nxt().c_str());
        else if (a == "--budget-bytes") budget_bytes = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--memory-gb") memory_gb = std::atof(nxt().c_str());
        else if (a == "--scratch-cap-gb") scratch_cap_gb = std::atof(nxt().c_str());
        else if (a == "--threads") threads = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--par-bands") par_bands = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--sample") sample_n = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--keep-rows") keep_rows = true;
        else if (a == "--sort") sort_mode = nxt();
        else if (a == "--remap") remapPath = nxt();
        else if (a == "--lcp-out") lcpOut = nxt();
        else if (a == "--no-lcp-slices") noLcpSlices = true;
        else if (a == "--force-inplace-sort") force_inplace = true;
        else if (a == "--isa-sample-out") isaOut = nxt();
        else if (a == "--isa-sample-k") isaK = std::strtoull(nxt().c_str(), nullptr, 10);
        else die("unknown arg " + a);
    }
    (void)sort_mode;
    if (corpus.empty() || (out.empty() && tapeBands.empty())) die("--corpus and --out required");

    if (!selftestOracle.empty() && !tapeBands.empty()) die("--selftest requires --out");
    if (!tapeBands.empty() && (!pendingBands || keep_rows))
        die("tape bands require K>0 and no keep-rows");
    // ================= THE DESCRIPTOR ENVELOPE ==============================
    // The second 100 GB attempt died with
    //     terminate called after throwing an instance of 'std::system_error'  (rc=134)
    // while the budget law itself was right (rows_per_band 659,875 came from the
    // envelope): the run was out of DESCRIPTORS, because it held
    // nbands x nparts band-part files open at once (4x K bands x --threads
    // parts) AND K was the thread count. Both halves are fixed here and in the
    // band workers / the scatter (one open part file per scan thread, closed the
    // moment the thread moves to another band).
    //
    // This is the third leg: raise RLIMIT_NOFILE to what the run needs, bounded by
    // the HARD limit and a sane maximum, and journal the three numbers - a soft
    // limit that cannot hold the projection is a CONFIGURATION error, loud here,
    // never a std::system_error out of a worker thread later.
    u64 fd_soft = 0, fd_hard = 0, fd_wanted = 0;
    {
        struct rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) != 0) die("getrlimit(RLIMIT_NOFILE)");
        const u64 soft0 = (u64)rl.rlim_cur, hard = (u64)rl.rlim_max;
        // One descriptor per pool thread (the scan's part file), one per band a
        // worker is reading or publishing, and the long-lived ones (corpus, remap,
        // out, sym, manifest, guides). The wave's band count is at most
        // SS_OVERSUBSCRIBE x K, which is what the second term prices.
        fd_wanted = SS_OVERSUBSCRIBE * std::max<u64>(1, pendingBands) + 2 * std::max<u64>(1, threads) + 64;
        const u64 sane_max = 1u << 20;
        const u64 ceiling = std::min<u64>(hard, sane_max);
        const u64 want = std::min<u64>(std::max<u64>(soft0, fd_wanted), ceiling);
        if (want > soft0) {
            struct rlimit nr = rl;
            nr.rlim_cur = (rlim_t)want;
            if (::setrlimit(RLIMIT_NOFILE, &nr) != 0 && fd_wanted > soft0)
                die("RLIMIT_NOFILE: this run projects " + std::to_string(fd_wanted) +
                    " descriptors, the soft limit is " + std::to_string(soft0) +
                    " and raising it to " + std::to_string(want) + " failed (hard " +
                    std::to_string(hard) + ")");
        }
        struct rlimit got{};
        if (::getrlimit(RLIMIT_NOFILE, &got) != 0) die("getrlimit(RLIMIT_NOFILE) after raise");
        fd_soft = (u64)got.rlim_cur;
        fd_hard = (u64)got.rlim_max;
        std::fprintf(stderr,
            "SS_TAPE_FDS soft=%" PRIu64 " hard=%" PRIu64 " wanted=%" PRIu64
            " soft0=%" PRIu64 " fd_ceiling=%" PRIu64 " rule=raise to wanted, bounded by "
            "min(hard,%" PRIu64 ") bands_per_wave_max=%" PRIu64 " threads=%" PRIu64 "\n",
            fd_soft, fd_hard, fd_wanted, soft0, std::min<u64>(hard, u64(1) << 20), u64(1) << 20,
            SS_OVERSUBSCRIBE * std::max<u64>(1, pendingBands), std::max<u64>(1, threads));
        noteFdPeak();
    }
    int fd = ::open(corpus.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("cannot open/stat corpus");
    FILE_N = (u64)st.st_size;
    if (!FILE_N) die("empty corpus");
    N = FILE_N + 1; // the END suffix exists in both realizations
    KEY_N = FILE_N >= 24 ? FILE_N - 23 : 0;
    T = (const u8*)::mmap(nullptr, (size_t)FILE_N, PROT_READ, MAP_PRIVATE, fd, 0);
    if (T == MAP_FAILED) die("mmap corpus");
    // NO MADV_WILLNEED: it pulls the WHOLE corpus into this process's RSS before a
    // single row is sorted, which is precisely the n-scaling term the budget
    // forbids. Every full-corpus pass below reads through a WINDOW (pread into a
    // reused buffer) and the mapping is only touched by the RANDOM tie walks of
    // the sort, which are followed by madv_release().
    std::vector<u8> remapBuf;
    if (!remapPath.empty()) {
        int rf = ::open(remapPath.c_str(), O_RDONLY);
        if (rf < 0) die("cannot open remap");
        remapBuf.resize(256);
        if (::read(rf, remapBuf.data(), 256) != 256) die("remap short read");
        ::close(rf);
        REMAP = remapBuf.data();
    }
    // END is virtual: byte cells carry only corpus bytes. SA position zero
    // identifies the exceptional BWT row, independently of its transport cell.
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
    // The inherited finisher still interprets cell zero as END. Refuse an
    // ambiguous tape until that consumer supports the structural END row.
    if (!tapeBands.empty())
        for (unsigned c = 0; c < 256; ++c)
            if (used[c] && (REMAP ? REMAP[c] : c) == 0)
                die("tape consumer reserves cell zero; virtual END consumer required");
    if (!tapeBands.empty() && !lcpOut.empty())
        die("--lcp-out is the one-file column (non-tape); tape bands emit per-band slices");
    // THE DEFAULT PATH: the tape route always produces the LCP column, as one
    // slice per band. --no-lcp-slices is the A/B arm that reproduces the old
    // LCE-verify route (the finish then walks the corpus for every endpoint).
    const bool lcpOn = tapeBands.empty() ? !lcpOut.empty() : !noLcpSlices;
    std::fprintf(stderr, "SS_LCP_ARM lcp_source=%s slices=%d\n",
                 lcpOn ? (tapeBands.empty() ? "column-file" : "sorter-bands") : "none",
                 int(lcpOn && !tapeBands.empty()));
    std::fprintf(stderr, "END_SYMBOL mode=virtual position=%" PRIu64 " multiplicity=1 rank=0\n", FILE_N);
    std::fprintf(stderr, "END_FORMAT k=%u ends_in_sentinel=1 end_code=none\nTAIL_GROUP rows=%" PRIu64 "\n", present, N - KEY_N);
    build_pairmap();
    // SYMBOL ACCESS ON THE SENTINEL-TERMINATED TEXT: the corpus bytes through the
    // product remap, and index FILE_N = the sentinel symbol 0.
    auto sym_at = [](u64 p) -> unsigned {
        if (p >= FILE_N) return SENTINEL_SYMBOL;
        return REMAP ? REMAP[T[p]] : T[p];
    };

    const u64 row_bytes = sizeof(Row);            // the WIRE row (band part files)
    // THE SCRATCH LAW: default cap is 2n bytes.
    const u64 cap_bytes = scratch_cap_gb > 0.0 ? (u64)(scratch_cap_gb * 1e9) : 2 * N;
    if (!tapeBands.empty() && (cap_bytes > 2 * N || N >= (1ull << 48)))
        die("tape bands require wave cap <= 2n and n < 2^48");
    // ===================== THE BAND BUDGET ==================================
    // THE WORK UNIT COMES FROM THE ENVELOPE, NEVER FROM THE DATA.
    //
    // The 100 GB run died on
    //     SS_TAPE_CREDIT sa_window=8 rows_per_band=185,721,167
    // with a bad_alloc inside the unwind (rc=134). 185,721,167 was not a memory
    // figure: it was rows_per_wave/(4*par_bands) = 0.95 x 2n / 32 / 32, i.e. the
    // band size was DERIVED FROM THE CORPUS. It grew 10x when the corpus grew
    // 10x while --memory-gb stayed 96, the arena it asked for (rows x 32 B x the
    // 32 bands a wave carried) outgrew the envelope, and the sorter ABORTED
    // instead of shrinking the unit.
    //
    // The rule that replaces it, and the one thing this section implements:
    //     rows_per_band = (envelope - fixed - stage) x SAFETY / (bpr x K)
    // n appears in NO numerator. Everything else is an OUTPUT of the plan:
    //     bands_per_wave = ceil(rows_per_wave / rows_per_band)
    //     waves          = ceil(n / rows_per_wave)
    // so a bigger corpus buys MORE BANDS AND MORE WAVES, never a bigger arena.
    //
    // bytes_per_row is what the sorter actually holds per row, term by term:
    //   rows 32 | ping-pong 32 | lcp_a 8 | lcp_b 8 | slice column 1 | ob 8 | sb 1
    // LCP arm = 90 B/row; the wire row is 32 and the sort alone is 81. Pricing
    // either of those two numbers is exactly how the model ran 2.8x short.
    //
    // THE ENVELOPE is the arena this process may commit, given by the front door
    // (--budget-bytes; --ram-gb is the legacy decimal-GB spelling). fixed and
    // stage are the terms that are NOT the band arena and MUST be paid first:
    //   fixed = one windowed read buffer per thread + the sorted sample array
    //           (8 B x --sample) + the per-thread byte histograms + control
    //   stage = the scatter's per-(thread,band) append buffers, a fixed FRACTION
    //           of the envelope (the same fraction the scatter allocates)
    const bool plan_lcp = lcpOn;
    const u64 envelope_bytes = budget_bytes ? budget_bytes : (u64)(ram_gb * 1e9);
    if (!envelope_bytes) die("--budget-bytes (or --ram-gb) must be positive");
    // THE PARALLELISM P, AND WHY IT IS THE THREAD COUNT.
    // Two numbers were confused and they are NOT the same thing:
    //   P = bands LIVE at once (parallelism). The band sort is single-threaded, so
    //       P is what keeps the pool busy: P band arenas are admitted at a time and
    //       each is sorted by one worker.
    //   K = the band WINDOW of the tape handshake (--pending-bands: one band per
    //       slot, consumed before the next is published). K bounds how many band
    //       FILES are outstanding, never how many hold rows in RAM.
    // The first 100 GB attempt did not die of P=96: it priced rows_per_band=185M
    // (from the CORPUS) WITH P=96, i.e. 96 x 185M x 41 B = 728 GB. The budget law
    // below is the fix - rows_per_band = arena/(bpr x P) - and it is memory-legal
    // for ANY P, so P stays the thread count. The second attempt died of
    // DESCRIPTORS (nbands x nparts part files held open at once), which is fixed in
    // the scatter and with RLIMIT_NOFILE, independently of P.
    // --par-bands stays as the explicit override it always was.
    const u64 k_in_flight = par_bands ? par_bands
                                      : std::max<u64>(1, threads);
    const char* k_source = par_bands ? "operator --par-bands"
                                     : "thread count (P = --threads; K=--pending-bands is the tape window)";
    TapeBudget budget = tape_budget_plan(envelope_bytes, std::max<u64>(1, threads), k_in_flight,
                                         plan_lcp, sample_n, N, cap_bytes, row_bytes);
    // THE PLAN IS PRE-FLIGHTED AGAINST THE PROCESS'S OWN ADDRESS SPACE: a reserve
    // of the whole arena (never touched, so it costs nothing) fails HERE, at plan
    // time, if RLIMIT_AS cannot hold it - and then the unit SHRINKS instead of the
    // run dying with a bad_alloc in a worker thread. Bounded, journaled halvings;
    // only a unit below the floor is a real configuration error.
    const u64 floor_rows = std::min<u64>(SS_ROWS_FLOOR, std::max<u64>(1, N));
    for (unsigned attempt = 0;; ++attempt) {
        if (budget.rows_per_band < floor_rows) {
            std::fprintf(stderr,
                "SS_TAPE_CONFIG per_band_rows=%" PRIu64 " < floor=%" PRIu64
                " budget_bytes=%" PRIu64 " bytes_per_row=%" PRIu64 " bands_in_flight=%" PRIu64
                " attempt=%u\n",
                budget.rows_per_band, floor_rows, envelope_bytes, budget.bytes_per_row,
                budget.bands_in_flight, attempt);
            die("TAPE_BUDGET_TOO_SMALL: the arena envelope cannot hold one band of " +
                std::to_string(floor_rows) + " rows at " + std::to_string(budget.bytes_per_row) +
                " B/row x K=" + std::to_string(budget.bands_in_flight) + " in flight. This is a "
                "CONFIGURATION error, not a data-dependent shrink: raise --memory-gb, lower "
                "--threads (K is the in-flight band count), or shard the corpus");
        }
        const u64 arena_bytes = budget.bands_in_flight * budget.rows_per_band * budget.bytes_per_row;
        if (tape_arena_reserve(arena_bytes)) break;
        if (attempt >= 24) die("SS_TAPE_SHRINK exhausted after 24 halvings: the arena cannot be reserved");
        std::fprintf(stderr,
            "SS_TAPE_SHRINK why=arena-reserve-failed rows_per_band=%" PRIu64 " -> %" PRIu64
            " arena_bytes=%" PRIu64 " budget_bytes=%" PRIu64 " attempt=%u (halving the work unit; "
            "the band plan is recomputed, nothing has been written)\n",
            budget.rows_per_band, std::max<u64>(1, budget.rows_per_band / 2), arena_bytes,
            envelope_bytes, attempt);
        budget = tape_budget_plan(envelope_bytes, std::max<u64>(1, threads), k_in_flight,
                                  plan_lcp, sample_n, N, cap_bytes, row_bytes,
                                  /*override_rows_per_band=*/std::max<u64>(1, budget.rows_per_band / 2));
    }
    const u64 sort_row_bytes = budget.bytes_per_row;
    // THE LIVE WORK UNIT. A band that cannot be allocated at the planned unit
    // HALVES it for every later wave (the wave's band plan is recomputed from this
    // number at the top of each wave), which is the "halve per_band_rows, recompute
    // the band plan, retry" rule - applied where it is legal: a wave already
    // scattered cannot be re-cut (its rank offsets are fixed), so the wave in
    // flight finishes on the low-memory arm below and the NEXT one is planned
    // smaller. Bounded by SS_ROWS_FLOOR, and journaled on every step.
    u64 rows_per_band = budget.rows_per_band;
    std::mutex shrink_mu;
    std::atomic<bool> prefer_inplace{force_inplace};
    const u64 rows_per_wave = budget.rows_per_wave;
    u64 waves = budget.waves;
    // THE ROW GATE: the rows admitted into band arenas at once. Admission
    // control - not the band count - is what keeps the arena inside the plan when
    // the DATA makes a band fatter than the quantile cut promised: a worker waits
    // until its band fits, and a band larger than the whole cap still goes alone
    // (nothing else is live), which is the only case where the arena is the
    // band's and not the plan's.
    RowGate gate(budget.cap_rows);
    // THE TEXT RESIDENCY: the corpus mapping is the only per-process footprint
    // that can scale with n. Its pages are CLEAN page cache (the kernel reclaims
    // them under pressure, so they can never OOM this process), but a peak-RSS
    // receipt that is supposed to be a function of the budget must not silently
    // carry them. Every full-corpus pass therefore reads through a WINDOW
    // (pread into a reused buffer) and drops the mapping afterwards; the RSS
    // receipt reports the anon (owned) and file (mapped, reclaimable) peaks
    // separately, and the BUDGET ASSERTION is on the owned arena.
    RssMonitor rss;
    rss.start();

    std::fprintf(stderr,
        "SS_START n=%" PRIu64 " row_bytes=%zu sort_row_bytes=%" PRIu64 " cap_bytes=%" PRIu64
        " (%.2fn) rows_per_wave=%" PRIu64 " waves=%" PRIu64 " ram_gb=%.2f rows_per_band=%" PRIu64
        " par_bands=%" PRIu64 " threads=%" PRIu64 " in_band_arena_bytes=%" PRIu64 " (%.3fn)\n",
        N, row_bytes, sort_row_bytes, cap_bytes, double(cap_bytes) / double(N), rows_per_wave,
        waves, ram_gb, rows_per_band, budget.bands_in_flight, threads,
        budget.bands_in_flight * rows_per_band * sort_row_bytes,
        double(budget.bands_in_flight * rows_per_band * sort_row_bytes) / double(N));
    std::fprintf(stderr,
        "SS_TAPE_BUDGET memory_gb=%.2f budget_bytes=%" PRIu64 " bytes_per_row=%" PRIu64
        " bpr_terms=rows:%zu+pingpong:%zu+lcp:%u+slice:%u+stage:%u safety=%.2f"
        " bands_in_flight=%" PRIu64 " rows_per_band=%" PRIu64 " rows_per_band_budget=%" PRIu64
        " rows_per_band_window=%" PRIu64 " binding=%s bands=%" PRIu64
        " waves=%" PRIu64 " rows_per_wave=%" PRIu64 " arena_bytes=%" PRIu64
        " arena_planned_rows=%" PRIu64 " cap_rows=%" PRIu64 " fixed_bytes=%" PRIu64
        " stage_bytes=%" PRIu64
        " source=%s k_source=%s fd_soft=%" PRIu64 " fd_hard=%" PRIu64
        " parallel_bands=%" PRIu64 " live_rows=%" PRIu64 " budget_rows=%" PRIu64
        " ratio=%.4f\n"
        " (rows_per_band = arena/(bpr x parallel_bands): the work unit is derived from the envelope, n is"
        " in NO numerator; parallel_bands is the count of bands LIVE at once - the tape window K"
        " (--pending-bands) bounds band FILES, never live arenas; live_rows = parallel_bands x"
        " rows_per_band, budget_rows = envelope/bpr, ratio = live_rows x bpr / envelope)\n",
        (memory_gb > 0 ? memory_gb : ram_gb), envelope_bytes, budget.bytes_per_row,
        row_bytes, plan_lcp ? row_bytes : 0, plan_lcp ? 16u : 0u, plan_lcp ? 1u : 0u, 9u,
        double(SS_SAFETY_PCT) / 100.0,
        budget.bands_in_flight, rows_per_band, budget.rows_per_band_budget,
        budget.rows_per_band_wave,
        rows_per_band == budget.rows_per_band_budget ? "budget" : "scratch-window",
        budget.bands_per_wave, waves, rows_per_wave,
        budget.arena, budget.arena_planned, budget.cap_rows, budget.fixed, budget.stage,
        budget_bytes ? "operator --budget-bytes" : "legacy --ram-gb", k_source, fd_soft, fd_hard,
        budget.bands_in_flight, budget.bands_in_flight * budget.rows_per_band,
        envelope_bytes / std::max<u64>(1, budget.bytes_per_row),
        double(budget.bands_in_flight) * double(budget.rows_per_band) * double(budget.bytes_per_row)
            / double(envelope_bytes));

    // ---- exact first-byte histogram: one parallel pass, 256 counters. This is
    // what makes the per-wave FILTER cheap: a rejected position costs one load
    // instead of a 24-byte key computation (16 waves x n key computations was
    // 4.6x the whole sort at 150 MB).
    std::vector<u64> hist(256, 0);
    {
        const u64 nt = std::max<u64>(1, threads);
        std::vector<std::vector<u64>> lh(nt, std::vector<u64>(256, 0));
        std::vector<std::thread> ts;
        for (u64 t = 0; t < nt; ++t)
            ts.emplace_back([&, t]() {
                const u64 lo = N * t / nt, hi = N * (t + 1) / nt;
                auto& h = lh[t];
                std::vector<u8> buf(SS_SCAN_CHUNK + 64);
                for (u64 p = lo; p < hi;) {
                    const u64 chunk = std::min<u64>(SS_SCAN_CHUNK, hi - p);
                    const u64 real = std::min<u64>(chunk, FILE_N > p ? FILE_N - p : 0);
                    if (real) {
                        read_window(fd, p, real, buf);   // remapped in place
                        for (u64 i = 0; i < real; ++i) ++h[buf[i]];
                    }
                    // the virtual END suffix (position FILE_N) counts as symbol 0
                    if (chunk > real) ++h[SENTINEL_SYMBOL];
                    p += chunk;
                }
            });
        for (auto& th : ts) th.join();
        for (u64 t = 0; t < nt; ++t)
            for (int i = 0; i < 256; ++i) hist[i] += lh[t][i];
    }
    // Whatever this pass faulted in leaves the process's RSS: the receipt is a
    // function of the budget, not of n (see RssMonitor).
    madv_release(T, FILE_N);
    // ---- wave list: greedy byte ranges, each <= rows_per_wave rows. A byte whose
    // class ALONE exceeds the cap is split by its second byte (2-level wave).
    struct Wave { unsigned lo, hi; bool lvl2; unsigned b; unsigned lo2, hi2; };
    std::vector<Wave> wl;
    {
        unsigned lo = 0;
        u64 acc = 0;
        for (unsigned c = 0; c < 256; ++c) {
            if (hist[c] > rows_per_wave) {
                if (lo < c) { wl.push_back({lo, c, false, 0, 0, 0}); lo = c; acc = 0; }
                // split this byte by its second byte: a WINDOWED pass (the first
                // version walked the mapping twice, which cost 19 ns/position and
                // left the whole corpus resident)
                std::vector<u64> h2(256, 0);
                {
                    const u64 nt = std::max<u64>(1, threads);
                    std::vector<std::vector<u64>> l2(nt, std::vector<u64>(256, 0));
                    std::vector<std::thread> ts2;
                    for (u64 t = 0; t < nt; ++t)
                        ts2.emplace_back([&, t]() {
                            const u64 xlo = N * t / nt, xhi = std::min<u64>(FILE_N, N * (t + 1) / nt);
                            auto& h = l2[t];
                            std::vector<u8> buf(SS_SCAN_CHUNK + 64);
                            for (u64 p = xlo; p < xhi;) {
                                const u64 chunk = std::min<u64>(SS_SCAN_CHUNK, xhi - p);
                                // one byte of lookahead for the second symbol (0 past the
                                // corpus end, exactly as sym_at(p+1) reports it)
                                const u64 want = std::min<u64>(chunk + 1, FILE_N - p);
                                read_window(fd, p, want, buf);
                                for (u64 i = 0; i < chunk; ++i)
                                    if (buf[i] == c) ++h[i + 1 < want ? buf[i + 1] : SENTINEL_SYMBOL];
                                p += chunk;
                            }
                        });
                    for (auto& th : ts2) th.join();
                    for (u64 t = 0; t < nt; ++t)
                        for (int i = 0; i < 256; ++i) h2[i] += l2[t][i];
                }
                unsigned l2lo = 0; u64 a2 = 0;
                for (unsigned d = 0; d < 256; ++d) {
                    if (a2 && a2 + h2[d] > rows_per_wave) {
                        wl.push_back({c, c + 1, true, c, l2lo, d}); l2lo = d; a2 = 0;
                    }
                    if (!tapeBands.empty() && h2[d] > rows_per_wave) die("byte-pair class exceeds wave cap");
                    a2 += h2[d];
                }
                if (l2lo < 256) wl.push_back({c, c + 1, true, c, l2lo, 256});
                lo = c + 1; acc = 0;
                continue;
            }
            if (acc && acc + hist[c] > rows_per_wave) {
                wl.push_back({lo, c, false, 0, 0, 0}); lo = c; acc = 0;
            }
            acc += hist[c];
        }
        if (lo < 256) wl.push_back({lo, 256, false, 0, 0, 0});
    }
    waves = wl.size();
    std::fprintf(stderr, "SS_WAVES count=%" PRIu64 " (byte-range waves)\n", waves);

    // ---------------- SAMPLE once: band edges inside each wave ----------------
    const double t0 = now_sec();
    const u64 stride = std::max<u64>(1, N / std::max<u64>(1, sample_n));
    std::vector<u64> sk;
    sk.reserve(N / stride + 2);
    u64 rng = 0x243f6a8885a308d3ull;
    for (u64 p = 0; p < N; p += stride) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        u64 q = p + (rng >> 11) % stride;
        if (q >= KEY_N) continue;
        u64 a, b, c;
        key_at(q, a, b, c);
        sk.push_back(a);
    }
    std::sort(sk.begin(), sk.end());
    madv_release(T, FILE_N);   // the jittered sample walk touched n/stride pages
    const double t_sample = now_sec() - t0;
    std::fprintf(stderr, "SS_SAMPLE samples=%zu stride=%" PRIu64 " wall=%.2f\n",
                 sk.size(), stride, t_sample);

    // quantile index helper on the sampled keys
    auto qkey = [&](double frac) -> u64 {
        if (sk.empty()) return ~0ull;
        double idx = frac * double(sk.size() - 1);
        if (idx < 0) idx = 0;
        u64 i = (u64)idx;
        if (i >= sk.size()) i = sk.size() - 1;
        return sk[i];
    };

    std::unique_ptr<Pool> pool(new Pool((unsigned)std::max<u64>(1, threads)));
    std::string cmd = "mkdir -p '" + scratch + "'";
    if (std::system(cmd.c_str()) != 0) die("scratch prep failed");

    // SAMPLED INVERSE: ISA[p] for every k-th POSITION. Anchors are keyed by
    // position, which is invariant under the finish's repair (run indices are not,
    // and run lengths change), so an anchor lets the finish RESYNC its rank cursor.
    // 10 GB at pile with k=1024, against 10.5 TB for the full inverse.
    int ifd = -1;
    if (!isaOut.empty()) {
        ifd = ::open(isaOut.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (ifd < 0) die("create sampled ISA");
        u64 ih[4] = {1363761235ull /*'SXIQ'*/, 1ull, N, isaK};
        if (::write(ifd, ih, 32) != 32) die("write ISA header");
        const u64 ne = (N + isaK - 1) / isaK;
        if (::ftruncate(ifd, off_t(32 + ne * 8)) != 0) die("size sampled ISA");
    }
    int sampleFd = -1;
    if (!orderSample.empty()) {
        if (!sampleCount || sampleCount > 1048576 || sampleLo >= N || sampleCount > N - sampleLo)
            die("order sample range invalid (maximum 1048576 rows)");
        sampleFd = ::open(orderSample.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (sampleFd < 0) die("create bounded order sample");
        u64 sh[6] = {0x0000000141535853ull, N, 1, sampleCount, N, 10};
        if (::write(sampleFd, sh, 48) != 48) die("write order sample header");
    }
    u64 globalBand = 0;
    auto bandPath = [&](u64 b, const char* suffix) {
        return tapeBands + "/band_" + std::to_string(b) + suffix;
    };
    auto producerState = [&](u64 band, const char* phase) {
        if (tapeBands.empty()) return;
        const std::string path = bandPath(band, ".producer-state");
        { std::ofstream out(path + ".tmp"); out << phase << "\n";
          if (!out.good()) die("write producer state " + path); }
        if (::rename((path + ".tmp").c_str(), path.c_str())) die("publish producer state " + path);
    };
    const u64 reorderRowCapacity = std::max<u64>(1, budget.stage / 8);
    auto marker = [&](const std::string& path) {
        int f = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (f < 0) die("create tape marker " + path);
        ::close(f);
    };
    // ============ THE TWO CONSUMERS, AND WHY THEIR WINDOWS DIFFER ============
    // band_<i>.sa has ONE consumer: cross_lcp_merge's tape emit absorbs the band,
    // unlinks it and stamps band_<i>.consumed - a per-band handshake at the
    // sorter's own pace, so K=pending_bands bounds it.
    // band_<i>.lcp has a DIFFERENT consumer: xsa_finish's rank-cursor fold. The
    // fold stages ONE PIECE BAND at a time and a piece band is 65536 BWT RUNS -
    // not rows - so it cannot retire its first slice until it owns 65536 runs
    // worth of slice, i.e. 65536 x (rows per run) ROWS. Holding the slice to the
    // SA's K-band handshake deadlocked exactly when K x rows_per_band was smaller
    // than that (measured at 4 MB: 8 x 7421 = 59,368 rows < one fold band): the
    // sorter waited for band_0.lcp.consumed while the finish waited for the piece
    // of a band the sorter had not published. The credit below is MEASURED from
    // the run structure this producer already emits, never assumed.
    std::atomic<u64> lcpRowsSeen{0}, lcpRunsSeen{0};
    auto lcpFoldRows = [&]() -> u64 {                 // ROWS spanned by one 65536-RUN piece band
        const u64 seenRows = lcpRowsSeen.load(std::memory_order_relaxed);
        const u64 seenRuns = std::max<u64>(1, lcpRunsSeen.load(std::memory_order_relaxed));
        const double rowsPerRun = seenRows ? double(seenRows) / double(seenRuns) : 1.0;
        return u64(65536.0 * rowsPerRun) + 1;
    };
    // THE CREDIT IS A ROW BUDGET OF UNCONSUMED SLICE, and it is set from the
    // measured fold-band span. Measured at the 4 MB fixture (rows_per_run 2.48,
    // band 7421 rows): 400,000 rows stalls the ring, 672,000 completes - i.e. the
    // required slack is several fold bands, not one, because the emit absorbs rows
    // into ITS current piece band before the finish can fold and retire. The rule
    // below is 8 fold bands; if that is ever too small the wait GROWS it (doubling,
    // journaled) instead of hanging - the ring always breaks, and LCP_SLICE still
    // reports what was actually live.
    std::atomic<u64> lcpCreditRows{0};
    lcpCreditRows.store([&] {
        const char* e = ::getenv("SS_LCP_CREDIT_ROWS");
        const u64 want = (e && *e) ? std::strtoull(e, nullptr, 10) : 0;
        if (want && want < std::max<u64>(u64(65536), rows_per_band))
            die("SS_LCP_CREDIT_ROWS below one fold band (65536 runs) would deadlock the fold");
        return want ? want : 8 * lcpFoldRows();
    }());
    auto lcpWindowNow = [&]() -> u64 {
        const u64 rows = lcpCreditRows.load(std::memory_order_relaxed);
        const u64 need = (rows + rows_per_band - 1) / rows_per_band + 1;
        return std::max<u64>(pendingBands, need);
    };
    // Band waits diagnose ownership periodically. Diagnostics neither return
    // credits nor terminate the producer; external process ownership handles
    // failed peers. The legacy slice-credit arm below is not covered by this
    // protocol's progress argument (see DEADLOCK_FREEDOM.md).
    const double bandWaitDeadlineS = [] {
        const char* e = ::getenv("XSA_TAPE_WAIT_S");
        if (!e || !*e) e = ::getenv("SS_BAND_WAIT_TIMEOUT_S");
        if (!e || !*e) return 1800.0;
        const double v = std::strtod(e, nullptr);
        if (!(v > 0)) die("XSA_TAPE_WAIT_S / SS_BAND_WAIT_TIMEOUT_S must be positive");
        return v;
    }();
    std::mutex lcpLiveMu;
    std::deque<u64> lcpLive;                       // bytes of each PUBLISHED slice not yet retired
    u64 lcpPublished = 0, lcpLiveBytes = 0, lcpPeakLiveBytes = 0, lcpPeakLiveSlices = 0;
    // Retirement is in RANK ORDER (the finish unlinks slice j only after it folded
    // it), so once band_<back>.lcp.consumed exists every slice <= back is gone:
    // that makes the live figure below exact at the sampling instant, and between
    // two waits live only grows - so the max over waits IS the peak.
    auto retireUpTo = [&](u64 back) {
        std::lock_guard<std::mutex> lk(lcpLiveMu);
        while (lcpPublished - u64(lcpLive.size()) <= back && !lcpLive.empty()) {
            lcpLiveBytes -= lcpLive.front();
            lcpLive.pop_front();
        }
        lcpPeakLiveBytes = std::max(lcpPeakLiveBytes, lcpLiveBytes);
        lcpPeakLiveSlices = std::max<u64>(lcpPeakLiveSlices, u64(lcpLive.size()));
    };
    // Name an engaged wait after two seconds; report every completed duration
    // and periodically dump state without changing scheduling or capacity.
    auto waitMarker = [&](const std::string& path, const char* what, const char* who) {
        const double t0 = now_sec();
        bool said = false;
        double nextDiagnostic = bandWaitDeadlineS;
        while (::access(path.c_str(), F_OK) != 0) {
            const double waitedS = now_sec() - t0;
            if (!said && waitedS > 2.0) {
                said = true;
                std::fprintf(stderr,
                    "SS_WAIT producer=scatter_sort blocks_on=%s path=%s writer=%s wall_s=%.1f\n",
                    what, path.c_str(), who, waitedS);
            }
            if (waitedS > nextDiagnostic) {
                nextDiagnostic += bandWaitDeadlineS;
                const std::string stem = path.substr(0, path.size() - std::strlen(".consumed"));
                struct stat st{};
                const bool exists = ::stat((stem + ".sa").c_str(), &st) == 0;
                std::ifstream stateFile(tapeBands + "/merge.state");
                std::string state; std::getline(stateFile, state);
                const bool opened = ::access((stem + ".opened").c_str(), F_OK) == 0;
                std::ifstream producerFile(stem + ".producer-state");
                std::string producerPhase; std::getline(producerFile, producerPhase);
                std::fprintf(stderr, "SS_DIAGNOSTIC path=%s writer=%s band_exists=%d band_bytes=%llu merge_opened=%d producer_phase=%s merge_state={%s}\n",
                    path.c_str(), who, int(exists), (unsigned long long)(exists ? st.st_size : 0),
                    int(opened), producerPhase.empty() ? "unclaimed" : producerPhase.c_str(), state.empty() ? "unavailable" : state.c_str());

            }
            ::usleep(10000);
        }
        std::fprintf(stderr, "SS_WAIT_DONE path=%s wall_s=%.3f\n", path.c_str(), now_sec() - t0);
    };
    // .consumed      <- cross_lcp_merge's tape emit, at READ time (payload in its buffer)
    // .lcp.consumed  <- xsa_finish, immediately after copying a slice into private memory
    auto waitBandConsumed = [&](u64 back) {
        waitMarker(bandPath(back, ".consumed"), "band.consumed", "emit-tape(cross_lcp_merge)");
    };
    // Reservation covers sorting plus publication. Return ANY read credit;
    // serial band-index credits would idle all workers behind one slow sort.
    // Called only while claim_mu is held; workers never need that mutex to
    // finish sorting, publish, or release their arena.
    std::deque<u64> bandReservations;
    auto reserveBand = [&](u64 band) {
        const double started = now_sec();
        double diagnostic = bandWaitDeadlineS;
        bool waiting = false;
        while (bandReservations.size() >= pendingBands) {
            for (auto it = bandReservations.begin(); it != bandReservations.end();) {
                if (::access(bandPath(*it, ".consumed").c_str(), F_OK) == 0)
                    it = bandReservations.erase(it);
                else ++it;
            }
            if (bandReservations.size() < pendingBands) break;
            const double elapsed = now_sec() - started;
            if (!waiting && elapsed >= 2) {
                waiting = true;
                std::fprintf(stderr, "SS_WAIT producer=scatter_sort blocks_on=band.queue band=%llu reservations=%zu wall_s=%.3f\n",
                    (unsigned long long)band, bandReservations.size(), elapsed);
            }
            if (elapsed >= diagnostic) {
                diagnostic += bandWaitDeadlineS;
                std::fprintf(stderr, "SS_DIAGNOSTIC queue=band capacity=%llu reservations=%zu next=%llu wall_s=%.3f\n",
                    (unsigned long long)pendingBands, bandReservations.size(), (unsigned long long)band, elapsed);
                for (u64 held : bandReservations) {
                    std::ifstream pf(bandPath(held, ".producer-state"));
                    std::string phase; std::getline(pf, phase);
                    std::fprintf(stderr, "SS_DIAGNOSTIC_BAND band=%llu phase=%s done=%d consumed=%d\n",
                        (unsigned long long)held, phase.c_str(),
                        int(::access(bandPath(held, ".done").c_str(), F_OK) == 0),
                        int(::access(bandPath(held, ".consumed").c_str(), F_OK) == 0));
                }
                std::ifstream mf(tapeBands + "/merge.state");
                std::string state; std::getline(mf, state);
                std::fprintf(stderr, "SS_DIAGNOSTIC_MERGE state={%s}\n", state.c_str());
            }
            ::usleep(10000);
        }
        std::fprintf(stderr, "SS_WAIT_DONE queue=band band=%llu wall_s=%.6f\n",
            (unsigned long long)band, now_sec() - started);
        bandReservations.push_back(band);
    };
    // The slice credit, with GROWTH instead of a hang: if the fold has not retired
    // the slice this band needs for SS_LCP_GROW_S (default 30 s), the credit
    // doubles and the wait re-targets a later slice. Every growth is journaled.
    const double lcpGrowS = [] {
        const char* e = ::getenv("SS_LCP_GROW_S");
        if (!e || !*e) return 30.0;
        const double v = std::strtod(e, nullptr);
        if (!(v > 0)) die("SS_LCP_GROW_S must be positive");
        return v;
    }();
    auto waitSliceCredit = [&](u64 tapeBand) {
        const double t0 = now_sec();
        for (;;) {
            const u64 lw = lcpWindowNow();
            if (tapeBand < lw) return;                       // inside the credit: go
            const u64 back = tapeBand - lw;
            if (::access(bandPath(back, ".lcp.consumed").c_str(), F_OK) == 0) {
                retireUpTo(back);
                return;
            }
            const double waitedS = now_sec() - t0;
            if (waitedS > lcpGrowS) {
                const u64 was = lcpCreditRows.load();
                if (was < N) {
                    lcpCreditRows.store(std::min<u64>(N, was * 2 + 1));
                    std::fprintf(stderr,
                        "SS_LCP_CREDIT_GROW rows=%llu -> %llu window=%llu waited_s=%.1f "
                        "blocked_on=band_%llu.lcp.consumed writer=finish(xsa_finish fold)\n",
                        (unsigned long long)was, (unsigned long long)lcpCreditRows.load(),
                        (unsigned long long)lcpWindowNow(), waitedS, (unsigned long long)back);
                }
            }
            if (waitedS > bandWaitDeadlineS)
                die("no consumer progress on slice.lcp.consumed band_" + std::to_string(back) +
                    " for " + std::to_string(int(waitedS)) + " s (finish); credit_rows=" +
                    std::to_string(lcpCreditRows.load()) + " of n=" + std::to_string(N));
            ::usleep(10000);
        }
    };
    if (!tapeBands.empty()) {
        struct stat ts{};
        if (::stat(tapeBands.c_str(), &ts) != 0 || !S_ISDIR(ts.st_mode)) die("tape directory must exist and be fresh");
        if (::access((tapeBands + "/manifest").c_str(), F_OK) == 0) die("stale tape manifest");
        int mf = ::open((tapeBands + "/manifest").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (mf < 0) die("create tape manifest");
        std::string meta = std::to_string(N) + (lcpOn ? " 1\n" : " 2 " + std::to_string(pendingBands) + " " + std::to_string(reorderRowCapacity) + "\n");
        if (::write(mf, meta.data(), meta.size()) != (ssize_t)meta.size()) die("write tape manifest");
        ::close(mf);
        marker(tapeBands + "/ready");
        std::fprintf(stderr, "SS_TAPE K=%llu packed_row_bytes=8 wave_cap=%llu\n", (unsigned long long)pendingBands, (unsigned long long)cap_bytes);
        std::fprintf(stderr,
                     "SS_TAPE_CREDIT sa_window=%llu lcp_window0=%llu lcp_credit_rows0=%llu "
                     "rows_per_band=%llu wait_timeout_s=%.0f grow_s=%.0f\n",
                     (unsigned long long)pendingBands, (unsigned long long)lcpWindowNow(),
                     (unsigned long long)lcpCreditRows.load(), (unsigned long long)rows_per_band,
                     bandWaitDeadlineS, lcpGrowS);
    }
    int ofd = tapeBands.empty() ? ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (tapeBands.empty() && ofd < 0) die("create out");
    // THE STREAM IS IN THE PRODUCT'S OWN SAMPLED-SA ANCHOR FORMAT ('SXSA'), so
    // kasai_column and every existing tool can consume it directly. The old header
    // here was 'SXLC' (the COLUMN magic) with n at the wrong offset, which made the
    // LCP column unbuildable from a tape SA - i.e. it silently disabled the Kasai
    // arm of the finish, the one thing that removes the finish's LCE wall.
    {
        u64 h[6];
        const uint32_t magic = 0x41535853u, ver = 1;   // 'SXSA', version 1
        std::memcpy(h, &magic, 4);
        std::memcpy((char*)h + 4, &ver, 4);
        h[1] = N;    // n
        h[2] = 1;    // k (full SA)
        h[3] = N;    // count
        h[4] = N;    // rows
        h[5] = 0;    // SA_ANCHOR_ROW_PAD: no transport frame (sentinel text)
        if (ofd >= 0 && ::write(ofd, h, 48) != 48) die("write header");
    }
    const std::string symPath = out + ".sym";
    int sfd = tapeBands.empty() ? ::open(symPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (tapeBands.empty() && sfd < 0) die("create sym stream");

    // the LCP column: n bytes of adjacent-rank LCP + escapes, in the SXLC format
    // the finish already reads via --lcp-column.
    int lfd = -1;
    if (lcpOn && tapeBands.empty()) {
        lfd = ::open(lcpOut.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (lfd < 0) die("create lcp column");
        std::vector<u8> lh(48, 0);
        const uint32_t magic = 0x434C5853u, ver = 1;
        std::memcpy(lh.data() + 0, &magic, 4);
        std::memcpy(lh.data() + 4, &ver, 4);
        const u64 rows = N, zero = 0, flags = 1;
        std::memcpy(lh.data() + 8, &rows, 8);
        std::memcpy(lh.data() + 16, &rows, 8);
        std::memcpy(lh.data() + 24, &zero, 8);
        std::memcpy(lh.data() + 32, &zero, 8);
        std::memcpy(lh.data() + 40, &flags, 8);
        if (::write(lfd, lh.data(), 48) != 48) die("write lcp header");
        if (::ftruncate(lfd, off_t(48 + N)) != 0) die("size lcp column");
    }
    std::vector<std::pair<u64,u64>> gEsc;
    std::mutex escMu;
    // THE BAND-STREAM RECEIPT: how much LCP slice this run produced and the
    // largest one it ever held (the residency the 2n law is checked against).
    u64 lcpSlices = 0, lcpSliceBytes = 0, lcpRows = 0, lcpEscapes = 0, lcpMaxSliceBytes = 0;
    std::vector<u64> bandFirstPos, bandLastPos;
    std::vector<u8> cat(1u << 22), symcat(1u << 22);
    u64 rows_done = 0, ties_total = 0, emitted = 0, wave_rows = 0;
    u64 prev_wave_last = ~0ull;   // last row position emitted by the previous wave
    const double t1 = now_sec();

    for (u64 w = 0; w < waves; ++w) {
        const double tw = now_sec();
        const Wave& W = wl[w];
        // Bands inside the wave: rows_per_band each, from the same sample.
        // BAND EDGES MUST BE DATA QUANTILES, NOT UNIFORM IN KEY SPACE. Uniform
        // subdivision put nearly every row of a fat wave in ONE band (spaces
        // dominate the alphabet), which serialised the sort: measured 271 s for a
        // single wave of the 10 GB run. The sample is already sorted, so the
        // wave's slice of it gives exact-enough quantiles.
        const u64 nb = (u64)std::max<double>(1.0, double(rows_per_wave) / double(rows_per_band));
        std::vector<u64> bounds;
        {
            // For a TWO-LEVEL wave the population is the wave's two-byte range,
            // not the whole first byte: quantiles taken over the wrong population
            // produce wildly unbalanced bands (measured: one band's sort at 341 s
            // inside a 352 s wave - the space wave).
            u64 klo_s, khi_s;
            if (W.lvl2) {
                klo_s = (u64(W.lo) << 56) | (u64(W.lo2) << 48);
                khi_s = (u64(W.hi) << 56) | (u64(W.hi2 >= 256 ? 255 : W.hi2) << 48) |
                        ((W.hi2 >= 256) ? ~0ull : 0ull);
                if (W.hi2 >= 256) khi_s = ~0ull;
            } else {
                klo_s = (u64(W.lo) << 56);
                khi_s = (W.hi >= 256) ? ~0ull : ((u64(W.hi) << 56) - 1);
            }
            auto lo_it = std::lower_bound(sk.begin(), sk.end(), klo_s);
            auto hi_it = std::upper_bound(sk.begin(), sk.end(), khi_s);
            const u64 n_in = (u64)(hi_it - lo_it);
            for (u64 b = 1; b < nb && n_in > 1; ++b) {
                const u64 idx = (u64)((long double)n_in * b / nb);
                if (idx == 0 || idx >= n_in) continue;
                const u64 k = *(lo_it + idx);
                if (bounds.empty() || k > bounds.back()) bounds.push_back(k);
            }
            if (bounds.empty() && nb > 1) bounds.push_back(khi_s);  // degenerate: one band
        }
        const u64 nbands = bounds.size() + 1;

        // ---- scatter this wave only ----
        const u64 nparts = std::max<u64>(1, threads);
        // ONE OPEN PART FILE PER SCAN THREAD, NOT ONE PER (BAND, PART).
        // The scan is sequential in key order, so a thread appends to a few bands
        // in a row and the descriptor it no longer needs goes back IMMEDIATELY: no
        // descriptor is held for a band this thread has left. The 100 GB run held
        // nbands x nparts of them (4 x K bands x --threads parts, K = the thread
        // count) and died with std::system_error (rc=134). `made` records the parts
        // that already exist so the O_TRUNC happens exactly once per part.
        std::vector<std::vector<u8>> made(nparts, std::vector<u8>(nbands, 0));
        std::vector<int> curFd(nparts, -1);
        std::vector<u64> curBand(nparts, ~0ull);
        u64 bufsz = 1u << 20;
        // THE STAGING IS PRICED (budget.stage), so the scatter may not outgrow the
        // number the planner paid for: the cap is the model's, not a constant.
        while (nbands * nparts * bufsz > budget.stage && bufsz > (1u << 12)) bufsz >>= 1;
        std::vector<std::vector<std::vector<u8>>> bufs(nparts, std::vector<std::vector<u8>>(nbands));
        for (u64 t = 0; t < nparts; ++t)
            for (u64 b = 0; b < nbands; ++b) bufs[t][b].reserve(bufsz);
        auto flush_buf = [&](u64 t, u64 b) {
            auto& v = bufs[t][b];
            if (v.empty()) return;
            if (curFd[t] < 0 || curBand[t] != b) {
                if (curFd[t] >= 0) { if (::close(curFd[t])) die("close band part"); curFd[t] = -1; }
                char name[512];
                std::snprintf(name, sizeof name, "%s/w%" PRIu64 ".b%05" PRIu64 ".p%05" PRIu64,
                              scratch.c_str(), w, b, t);
                const int f = ::open(name, O_WRONLY | O_CREAT | (made[t][b] ? O_APPEND : O_TRUNC), 0666);
                if (f < 0) die(std::string("create band part ") + name);
                curFd[t] = f;
                curBand[t] = b;
                made[t][b] = 1;
                noteFdPeak();
            }
            size_t off = 0;
            while (off < v.size()) {
                ssize_t wr = ::write(curFd[t], v.data() + off, v.size() - off);
                if (wr <= 0) die("write band part");
                off += size_t(wr);
            }
            v.clear();
        };
        // THE SCAN USES pread INTO A REUSABLE BUFFER, not a faulting mmap walk.
        // Measured: the mmap scan cost ~19 ns per position per wave (a page fault
        // every 4 KB and a TLB walk), against 6.89 GB/s for buffered sequential
        // reads. The mmap stays for the RANDOM tie comparisons in suf_cmp, where
        // it is the right tool.
        auto worker = [&](u64 t) {
            const u64 lo = N * t / nparts, hi = N * (t + 1) / nparts;
            const u64 CH = 4u << 20;
            std::vector<u8> buf(CH + 64);
            for (u64 base = lo; base < hi;) {
                const u64 chunk = std::min<u64>(CH, hi - base);
                // one byte of run-up (the symbol byte at p-1) plus 32 bytes of
                // look-ahead so a 24-byte key and the symbol are always in range
                const u64 from = (base ? base - 1 : base);
                // Read real bytes only. Tail positions are handled separately
                // and cannot inspect the unfilled part of this buffer.
                const u64 want = std::min<u64>(chunk + 1 + 48, FILE_N > from ? FILE_N - from : 0);
                size_t got = 0;
                while (got < want) {
                    ssize_t r = ::pread(fd, buf.data() + got, want - got, off_t(from + got));
                    if (r <= 0) die("pread scan");
                    got += size_t(r);
                }
                // REMAP THE CHUNK ONCE, in place, instead of 24 lookups per row.
                // Per-row key building with byte-by-byte REMAP[] was the real cost:
                // ~400 ns/row for the included rows (~43M per wave at 1 GB), which
                // dominated the whole scan. One sequential pass over the chunk
                // makes every complete key a plain big-endian load.
                if (REMAP)
                    for (u64 i = 0; i < got; ++i) buf[i] = REMAP[buf[i]];
                const u64 off = base - from;   // index in buf of position `base`
                for (u64 j = 0; j < chunk; ++j) {
                    const u64 p = base + j;
                    if (p >= KEY_N) continue; // finite tails never receive a key
                    const u64 bj = off + j;
                    const unsigned c0 = buf[bj];
                    if (c0 < W.lo || c0 >= W.hi) continue;
                    if (W.lvl2) {
                        const unsigned c1 = buf[bj + 1];   // 0 past the corpus end
                        if (c1 < W.lo2 || c1 >= W.hi2) continue;
                    }
                    // THE WHOLE 24-BYTE KEY. Leaving kq unset here was a real bug:
                    // the comparator's third word then read uninitialised memory,
                    // and it still passed sa_check once - the verifier catches order
                    // errors, not undefined values that happen to agree.
                    u64 a1, a2, a3;
                    std::memcpy(&a1, buf.data() + bj, 8);
                    std::memcpy(&a2, buf.data() + bj + 8, 8);
                    std::memcpy(&a3, buf.data() + bj + 16, 8);
                    a1 = bswap64(a1); a2 = bswap64(a2); a3 = bswap64(a3);
                    // The carried BWT symbol. Position 0's predecessor is the
                    // appended end-of-text byte: symbol 0, exactly as the
                    // successor rule says (no marker, no rewrite downstream).
                    const u8 sy = p == 0 ? SENTINEL_SYMBOL : buf[bj - 1];
                    Row r{};
                    r.klo = a1;
                    r.khi = a2;
                    r.kq = a3;
                    r.pos = pack_sym(p, sy);
                    u64 b = (u64)(std::upper_bound(bounds.begin(), bounds.end(), a1) - bounds.begin());
                    if (b >= nbands) b = nbands - 1;
                    auto& v = bufs[t][b];
                    v.insert(v.end(), (const u8*)&r, (const u8*)&r + sizeof(Row));
                    if (v.size() >= bufsz) flush_buf(t, b);
                }
                base += chunk;
            }
            for (u64 b = 0; b < nbands; ++b) flush_buf(t, b);
            if (curFd[t] >= 0) { if (::close(curFd[t])) die("close band part"); curFd[t] = -1; }
            noteFdPeak();
        };
        pool->run([&](unsigned t) { worker(t); });
        const double tScan = now_sec() - tw;

        std::vector<std::vector<Row>> tailBands(nbands);
        for (u64 p = KEY_N; p < N; ++p) {
            const unsigned c0 = sym_at(p);
            if (c0 < W.lo || c0 >= W.hi) continue;
            if (W.lvl2 && (sym_at(p + 1) < W.lo2 || sym_at(p + 1) >= W.hi2)) continue;
            auto beforeFence = [&](u64 fence) {
                for (u64 i = 0; i < 8; ++i) {
                    if (p + i >= FILE_N) return true; // END < every byte
                    const unsigned c = sym_at(p + i);
                    const unsigned d = unsigned((fence >> (56 - 8 * i)) & 255);
                    if (c != d) return c < d;
                }
                return false; // equal eight-byte prefix belongs above fence
            };
            u64 b = 0;
            while (b < bounds.size() && !beforeFence(bounds[b])) ++b;
            Row r{};
            r.pos = pack_sym(p, p == 0 ? SENTINEL_SYMBOL : u8(sym_at(p - 1)));
            tailBands[b].push_back(r);
        }
        for (auto& tail : tailBands)
            std::sort(tail.begin(), tail.end(), [](const Row& a, const Row& b) {
                return suf_cmp_from(unpack_pos(a.pos), unpack_pos(b.pos), 0) < 0;
            });

        // band row counts are known BEFORE sorting (file sizes / row bytes), so
        // every band's offset in the global streams is computable up front and
        // each band can write DIRECTLY there. No append copy, no serial tail.
        std::vector<u64> band_rows(nbands, 0), band_off(nbands, 0);
        for (u64 b = 0; b < nbands; ++b) {
            for (u64 t = 0; t < nparts; ++t) {
                char name[512];
                std::snprintf(name, sizeof name, "%s/w%" PRIu64 ".b%05" PRIu64 ".p%05" PRIu64,
                              scratch.c_str(), w, b, t);
                struct stat bs{};
                if (::stat(name, &bs) == 0) band_rows[b] += u64(bs.st_size) / row_bytes;
            }
        }
        {
            for (u64 b = 0; b < nbands; ++b) band_rows[b] += tailBands[b].size();
            u64 acc = emitted;
            for (u64 b = 0; b < nbands; ++b) { band_off[b] = acc; acc += band_rows[b]; }
            wave_rows = acc - emitted;
        }
        // ---- sort this wave's bands (parallel), writing at their own offsets ----
        std::atomic<u64> next_band{0};
        std::atomic<u64> ties_w{0};
        // Published AFTER a band is sorted: 0 = not published, 1 = empty band,
        // else last-row position + 2. This is what a band needs to compute its own
        // seam without a post-wave patch racing the consumer.
        std::unique_ptr<std::atomic<u64>[]> bandPub;
        if (lcpOn) {
            bandPub.reset(new std::atomic<u64>[nbands]);
            for (u64 b = 0; b < nbands; ++b) bandPub[b].store(0, std::memory_order_relaxed);
        }
        const u64 prevWaveLastLocal = prev_wave_last;
        std::vector<double> tRead(nbands, 0.0), tSort(nbands, 0.0), tWrite(nbands, 0.0);
        bandFirstPos.assign(nbands, 0);
        bandLastPos.assign(nbands, 0);
        // CLAIM ORDER == ADMISSION ORDER. A band computes its seam against EARLIER
        // bands, so a worker that holds arena must never wait on a band that is
        // still waiting for arena: admission therefore follows the claim, and the
        // live set is always a PREFIX of the claimed bands, which is what makes the
        // gate deadlock-free (the head of the prefix always finishes).
        std::mutex claim_mu;
        auto band_worker = [&]() {
            std::vector<Row> rows;
            std::vector<u64> ob;
            std::vector<u8> sb;
            std::vector<u8> bandLcp;
            std::vector<std::pair<u64, u64>> bandEsc;
            for (;;) {
                u64 b = 0;
                {
                    // Only the next claim waits for disk credit. Future claims
                    // must not time out before their prerequisite is produced,
                    // or reserve arena while blocked behind downstream work.
                    std::lock_guard<std::mutex> lk(claim_mu);
                    b = next_band.fetch_add(1);
                    if (b >= nbands) return;
                    const u64 nextTapeBand = globalBand + b;
                    producerState(nextTapeBand, "credit");
                    if (!tapeBands.empty() && !lcpOn) {
                        reserveBand(nextTapeBand);
                    } else if (!tapeBands.empty() && nextTapeBand >= pendingBands) {
                        waitBandConsumed(nextTapeBand - pendingBands);
                        if (lcpOn) waitSliceCredit(nextTapeBand);
                    }
                    gate.admit(band_rows[b]);
                }
                const u64 admitted = band_rows[b];
                const u64 tapeBand = globalBand + b;
                producerState(tapeBand, "read");
                // ---- THE BAND BODY: EVERYTHING THAT ALLOCATES, AND THE SHRINK ----
                // A std::bad_alloc inside a worker is std::terminate (rc=134, the
                // exact 100 GB signature: "terminate called recursively"), so the
                // allocation path is inside a try and the answer is a SMALLER ARM,
                // not a dead process:
                //   arm 0  the planned merge-with-LCP   (2 rows + 2 u64 LCP arrays = 90 B/row)
                //   arm 1  in-place sort + LCP by direct adjacent comparison (33 B/row)
                // Arm 1 needs no extra arena at all, so it cannot fail the way arm 0
                // did, and it produces the SAME slice (the adjacent LCP it computes
                // is the one the merge propagates - verified by byte-identity, see
                // SS_TAPE_ARM in the selftest gate). A failure also halves the work
                // unit for every LATER wave, journaled.
                bool have_rows = false;
                for (unsigned attempt = 0; !have_rows; ++attempt) {
                    const bool inplace = attempt > 0 || prefer_inplace.load();
                    try {
                        rows.clear();
                        const double br0 = now_sec();
                        for (u64 t = 0; t < nparts; ++t) {
                            char name[512];
                            std::snprintf(name, sizeof name, "%s/w%" PRIu64 ".b%05" PRIu64 ".p%05" PRIu64,
                                          scratch.c_str(), w, b, t);
                            int f = ::open(name, O_RDONLY);
                            if (f < 0) continue;
                            struct stat fs{};
                            ::fstat(f, &fs);
                            const u64 bytes = (u64)fs.st_size;
                            if (bytes) {
                                const size_t at = rows.size();
                                rows.resize(at + bytes / sizeof(Row));
                                size_t off = 0;
                                while (off < bytes) {
                                    ssize_t r = ::read(f, ((u8*)rows.data()) + at * sizeof(Row) + off, bytes - off);
                                    if (r <= 0) die("read band part");
                                    off += size_t(r);
                                }
                            }
                            ::close(f);
                        }
                        tRead[b] = now_sec() - br0;
                        // every part descriptor of this band is closed by now: the
                        // band's files are consumed, so nothing is held for it.
                        noteFdPeak();
                        const double bs0 = now_sec();
                        producerState(tapeBand, "sort");
                        bandLcp.clear();
                        bandEsc.clear();
                        if (lcpOn) {
                            if (inplace) sort_band_inplace(rows, bandLcp, bandEsc, band_off[b]);
                            else merge_sort_lcp(rows, bandLcp, bandEsc, band_off[b]);
                        } else {
                            // product path: the plain comparison sort. The merge-with-LCP
                            // is opt-in until its propagation is verified exact.
                            sort_tape_keys(rows, tapeBand);
                        }
                        // Insert the <=24 separately sorted finite suffixes. Only this
                        // cold path uses a full comparison against keyed rows.
                        // LCP values of unaffected adjacent rows remain unchanged.
                        if (!tailBands[b].empty()) {
                            std::vector<u64> lcps(rows.size(), 0);
                            if (lcpOn) {
                                for (size_t i = 0; i < rows.size(); ++i) lcps[i] = bandLcp[i];
                                for (const auto& e : bandEsc) lcps[e.first - band_off[b]] = e.second;
                            }
                            for (const Row& tail : tailBands[b]) {
                                auto it = std::lower_bound(rows.begin(), rows.end(), tail,
                                    [](const Row& a, const Row& t) {
                                        return suf_cmp_from(unpack_pos(a.pos), unpack_pos(t.pos), 0) < 0;
                                    });
                                const size_t i = size_t(it - rows.begin());
                                rows.insert(it, tail);
                                if (lcpOn) {
                                    u64 lcp = 0;
                                    if (i) suf_cmp_lcp(unpack_pos(rows[i-1].pos), unpack_pos(tail.pos), 0, lcp);
                                    lcps.insert(lcps.begin() + i, lcp);
                                    if (i + 1 < rows.size()) {
                                        suf_cmp_lcp(unpack_pos(tail.pos), unpack_pos(rows[i+1].pos), 0, lcp);
                                        lcps[i+1] = lcp;
                                    }
                                }
                            }
                            if (lcpOn) {
                                bandLcp.resize(rows.size()); bandEsc.clear();
                                for (size_t i = 0; i < lcps.size(); ++i) {
                                    bandLcp[i] = u8(std::min<u64>(lcps[i], 255));
                                    if (lcps[i] >= 255) bandEsc.emplace_back(band_off[b] + i, lcps[i]);
                                }
                            }
                        }
                        if (lcpOn) {
                            // ---- THE SEAM --------------------------------------------------
                            // Byte 0 of a band is the LCP with the PREVIOUS row in rank
                            // order, i.e. the last row of the previous non-empty band - a
                            // pair that belongs to neither band's own merge. It is computed
                            // HERE, before the slice is published, because a post-wave patch
                            // would race the consumer that already holds the band.
                            //
                            // The wait is on an EARLIER band (claimed earlier by the ordered
                            // work queue), so the dependency graph is acyclic: with W workers
                            // band b only ever waits for bands < b, and every band's worker
                            // finishes without waiting on a band >= b.
                            if (!rows.empty()) {
                                const u64 firstPos = unpack_pos(rows.front().pos);
                                u64 prevLast = ~0ull;
                                for (u64 j = b; j-- > 0;) {
                                    u64 v = 0;
                                    for (;;) {
                                        v = bandPub[j].load(std::memory_order_acquire);
                                        if (v) break;
                                        ::usleep(50);
                                    }
                                    if (v > 1) { prevLast = v - 2; break; }
                                }
                                if (prevLast == ~0ull) prevLast = prevWaveLastLocal;
                                if (prevLast != ~0ull) {
                                    u64 cross = 0;
                                    suf_cmp_lcp(prevLast, firstPos, 0, cross);
                                    // the band's own block-start value is replaced, never
                                    // duplicated: the escape list must stay rank-strict.
                                    for (size_t e = bandEsc.size(); e-- > 0;)
                                        if (bandEsc[e].first == band_off[b]) bandEsc.erase(bandEsc.begin() + e);
                                    bandLcp[0] = u8(std::min<u64>(cross, 255));
                                    if (cross >= 255) bandEsc.emplace_back(band_off[b], cross);
                                }
                            }
                            std::sort(bandEsc.begin(), bandEsc.end());
                        }
                        // THE WRITE STAGING IS ALLOCATED HERE, BEFORE ANYTHING IS
                        // PUBLISHED: the publish path below is then allocation-free, so
                        // a refusal can never leave a half-published band on disc.
                        ob.resize(rows.size());
                        sb.resize(rows.size());
                        tSort[b] += now_sec() - bs0;
                        have_rows = true;
                        if (inplace && (attempt > 0 || (force_inplace && b < 4)))
                            std::fprintf(stderr,
                                "SS_TAPE_ARM arm=inplace band=%" PRIu64 " rows=%zu attempt=%u "
                                "bytes_per_row=%" PRIu64 " (33 B/row: in-place sort + adjacent LCP by "
                                "direct comparison)\n",
                                b, rows.size(), attempt, u64(row_bytes + 1 + 8 + 1));
                    } catch (const std::bad_alloc&) {
                        // keep the work unit for the NEXT wave smaller, and redo THIS
                        // band on the low-memory arm (arm 1 needs no second row array)
                        bswap_release(rows); bswap_release(ob); bswap_release(sb);
                        bswap_release(bandLcp); bswap_release(bandEsc);
                        {
                            std::lock_guard<std::mutex> lk(shrink_mu);
                            const u64 was = rows_per_band;
                            rows_per_band = std::max<u64>(floor_rows, was / 2);
                            prefer_inplace.store(true);
                            std::fprintf(stderr,
                                "SS_TAPE_SHRINK why=band-alloc-failed band=%" PRIu64 " rows=%" PRIu64
                                " attempt=%u rows_per_band=%" PRIu64 " -> %" PRIu64
                                " bytes_per_row=%" PRIu64 " (this band retries on the in-place arm; "
                                "every LATER wave is planned at the smaller unit)\n",
                                b, band_rows[b], attempt, was, rows_per_band, budget.bytes_per_row);
                            if (rows_per_band <= floor_rows && attempt >= 1) {
                                std::fprintf(stderr, "SS_TAPE_CONFIG rows_per_band at the floor=%" PRIu64 "\n",
                                             floor_rows);
                                die("TAPE_BAND_ALLOC_FAILED band=" + std::to_string(b) +
                                    " rows=" + std::to_string(band_rows[b]) +
                                    ": the band could not be allocated even on the in-place arm " +
                                    "(32 B/row) after shrinking per_band_rows to the " +
                                    std::to_string(floor_rows) + " floor. This is a "
                                    "CONFIGURATION error: raise --memory-gb or shard the corpus");
                            }
                        }
                    }
                }
                const double bs0 = now_sec();
                        producerState(tapeBand, "sort");
                if (lcpOn) {
                    if (tapeBands.empty()) {
                        // the one-file arm: the band's LCP stream lands at its own
                        // offset (rank order is band order).
                        const u64 at = 48 + band_off[b];
                        size_t off = 0;
                        while (off < bandLcp.size()) {
                            ssize_t wr = ::pwrite(lfd, bandLcp.data() + off, bandLcp.size() - off,
                                                  off_t(at + off));
                            if (wr <= 0) die("pwrite lcp");
                            off += size_t(wr);
                        }
                        if (!bandEsc.empty()) {
                            std::lock_guard<std::mutex> lk(escMu);
                            for (auto& e : bandEsc) gEsc.push_back(e);
                        }
                    } else {
                        // ---- THE SLICE -------------------------------------------------
                        // band_<i>.lcp: 48-byte header {rows, escapes, max_lcp,
                        // rank_base, flags} + rows bytes + escapes*16. Published by
                        // rename, then .lcp.done, and it is consumed (deleted) by the
                        // fold that reads it -> .lcp.consumed, which is this
                        // producer's flow control for the NEXT K bands.
                        u64 maxL = 0;
                        for (u64 i = 0; i < bandLcp.size(); ++i) maxL = std::max<u64>(maxL, bandLcp[i]);
                        for (const auto& e : bandEsc) maxL = std::max(maxL, e.second);
                        const std::string tmp = bandPath(tapeBand, ".lcp.tmp");
                        const std::string dst = bandPath(tapeBand, ".lcp");
                        int lf = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                        if (lf < 0) die("create lcp slice " + tmp);
                        u8 h[48];
                        std::memset(h, 0, sizeof h);
                        const uint32_t magic = 0x534C5853u, ver = 1;   // 'SXLS', v1
                        std::memcpy(h + 0, &magic, 4);
                        std::memcpy(h + 4, &ver, 4);
                        const u64 rowsN = bandLcp.size(), escN = bandEsc.size(), flags = 1;
                        std::memcpy(h + 8, &rowsN, 8);
                        std::memcpy(h + 16, &escN, 8);
                        std::memcpy(h + 24, &maxL, 8);
                        std::memcpy(h + 32, &band_off[b], 8);
                        std::memcpy(h + 40, &flags, 8);
                        auto wf = [&](const void* p, size_t n2) {
                            const char* c = (const char*)p;
                            size_t gone = 0;
                            while (gone < n2) {
                                ssize_t wr = ::write(lf, c + gone, n2 - gone);
                                if (wr <= 0) die("write lcp slice");
                                gone += size_t(wr);
                            }
                        };
                        wf(h, 48);
                        if (!bandLcp.empty()) wf(bandLcp.data(), bandLcp.size());
                        for (const auto& e : bandEsc) {
                            const u64 pair[2] = {e.first, e.second};
                            wf(pair, 16);
                        }
                        if (::close(lf)) die("close lcp slice");
                        if (::rename(tmp.c_str(), dst.c_str()) != 0) die("publish lcp slice");
                        marker(dst + ".done");
                        const u64 sliceBytes = bandLcp.size() + bandEsc.size() * 16;
                        {
                            std::lock_guard<std::mutex> lk(escMu);
                            lcpSlices += 1;
                            lcpSliceBytes += sliceBytes;
                            lcpRows += bandLcp.size();
                            lcpEscapes += bandEsc.size();
                            if (sliceBytes > lcpMaxSliceBytes) lcpMaxSliceBytes = sliceBytes;
                        }
                        {
                            // live = published - retired; the peak is sampled at every
                            // credit wait (see retireUpTo). Exact bytes, not a ratio.
                            std::lock_guard<std::mutex> lk(lcpLiveMu);
                            ++lcpPublished;
                            lcpLive.push_back(sliceBytes);
                            lcpLiveBytes += sliceBytes;
                            lcpPeakLiveBytes = std::max(lcpPeakLiveBytes, lcpLiveBytes);
                            lcpPeakLiveSlices = std::max<u64>(lcpPeakLiveSlices, u64(lcpLive.size()));
                        }
                    }
                    bandFirstPos[b] = rows.empty() ? ~0ull : unpack_pos(rows.front().pos);
                    bandLastPos[b] = rows.empty() ? ~0ull : unpack_pos(rows.back().pos);
                }
                // PUBLISH the band's last row for the next band's seam. 1 = empty
                // band, else lastPos + 2 (0 stays "not published").
                if (lcpOn) bandPub[b].store(rows.empty() ? 1ull : (unpack_pos(rows.back().pos) + 2),
                                            std::memory_order_release);
                tSort[b] += now_sec() - bs0;
                const double bw0 = now_sec();
                producerState(tapeBand, "publish");
                u64 ties = 0;
                for (size_t i = 1; i < rows.size(); ++i)
                    if (rows[i].klo == rows[i-1].klo && rows[i].khi == rows[i-1].khi &&
                        rows[i].kq == rows[i-1].kq) ++ties;
                ties_w.fetch_add(ties);
                // ordered output: positions to the SA stream, symbols alongside.
                // ob/sb were SIZED inside the band body (before any publish), so this
                // fill only stores: the publish path cannot allocate.
                for (size_t ri = 0; ri < rows.size(); ++ri) {
                    const u64 pp = unpack_pos(rows[ri].pos);
                    ob[ri] = pp;
                    sb[ri] = (u8)(rows[ri].pos >> 48);
                    if (ifd >= 0 && pp % isaK == 0) {
                        const u64 rk = band_off[b] + (u64)ri;   // this row's global rank
                        if (::pwrite(ifd, &rk, 8, off_t(32 + (pp / isaK) * 8)) != 8)
                            die("pwrite ISA anchor");
                    }
                }
                if (lcpOn && !tapeBands.empty()) {
                    // The slice credit needs rows per BWT run (lcpWindowNow): count
                    // the symbol changes this band publishes. The boundary symbol
                    // between two bands is not counted, so runs are UNDER-counted
                    // and rows/run over-estimated - the safe direction.
                    u64 runsHere = sb.empty() ? 0 : 1;
                    for (size_t i = 1; i < sb.size(); ++i) runsHere += sb[i] != sb[i - 1];
                    lcpRowsSeen.fetch_add(u64(sb.size()), std::memory_order_relaxed);
                    lcpRunsSeen.fetch_add(runsHere, std::memory_order_relaxed);
                }
                if (sampleFd >= 0) {
                    const u64 lo = std::max(sampleLo, band_off[b]);
                    const u64 hi = std::min(sampleLo + sampleCount, band_off[b] + u64(ob.size()));
                    if (lo < hi && ::pwrite(sampleFd, ob.data() + (lo - band_off[b]), (hi - lo) * 8,
                                           off_t(48 + (lo - sampleLo) * 8)) != ssize_t((hi - lo) * 8))
                        die("write bounded order sample");
                }
                if (!tapeBands.empty()) {
                    // Compact wire: low 48 bits position, next 8 bits carried symbol.
                    for (size_t i = 0; i < ob.size(); ++i) ob[i] |= u64(sb[i]) << 48;
                    int bf = ::open(bandPath(tapeBand, ".sa").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                    if (bf < 0) die("create tape band");
                    size_t off = 0, bytes = ob.size() * 8;
                    while (off < bytes) {
                        ssize_t wr = ::write(bf, (const char*)ob.data() + off, bytes - off);
                        if (wr <= 0) die("write tape band");
                        off += size_t(wr);
                    }
                    if (::close(bf)) die("close tape band");
                    marker(bandPath(tapeBand, ".done"));
                } else {
                // direct write at this band's offset in the global streams
                    const off_t at = off_t(48 + band_off[b] * 8);
                    size_t off = 0;
                    const size_t bytes = ob.size() * 8;
                    while (off < bytes) {
                        ssize_t wr = ::pwrite(ofd, ((const u8*)ob.data()) + off, bytes - off,
                                             at + off_t(off));
                        if (wr <= 0) die("pwrite band out");
                        off += size_t(wr);
                    }
                    size_t soff = 0;
                    while (soff < sb.size()) {
                        ssize_t wr = ::pwrite(sfd, sb.data() + soff, sb.size() - soff,
                                              off_t(band_off[b] + soff));
                        if (wr <= 0) die("pwrite band sym");
                        soff += size_t(wr);
                    }
                }
                tWrite[b] = now_sec() - bw0;
                std::fprintf(stderr, "SS_BAND band=%llu rows=%zu read_s=%.6f sort_s=%.6f write_s=%.6f\n",
                    (unsigned long long)tapeBand, rows.size(), tRead[b], tSort[b], tWrite[b]);
                producerState(tapeBand, "published");
                // The band is on disc and published: only NOW may its scatter parts
                // go (a retry after a failed allocation re-reads them).
                if (!keep_rows) {
                    for (u64 t = 0; t < nparts; ++t) {
                        char name[512];
                        std::snprintf(name, sizeof name, "%s/w%" PRIu64 ".b%05" PRIu64 ".p%05" PRIu64,
                                      scratch.c_str(), w, b, t);
                        ::unlink(name);
                    }
                }
                bswap_release(rows);
                bswap_release(bandLcp);
                bswap_release(bandEsc);
                bswap_release(ob);
                bswap_release(sb);
                gate.release(admitted);   // the arena this band held goes back
                (void)bs0;
            }
        };
        if (!tapeBands.empty()) {
            if (w + 1 == waves) {
                int ef = ::open((tapeBands + "/end").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (ef < 0) die("create tape end");
                std::string count = std::to_string(globalBand + nbands) + "\n";
                if (::write(ef, count.data(), count.size()) != (ssize_t)count.size()) die("write tape end");
                ::close(ef);
                marker(tapeBands + "/end.ready");
            }
        }
        if (!tapeBands.empty() && !lcpOn) {
            const u64 rowLimit = reorderRowCapacity;
            for (u64 first = 0; first < nbands;) {
                u64 count = 0, rows = 0;
                while (first + count < nbands && count < pendingBands) {
                    const u64 next = band_rows[first + count];
                    if (next > rowLimit) die("band exceeds bounded reorder row budget; reduce work unit");
                    if (next > rowLimit - rows) break;
                    rows += next; ++count;
                }
                const std::string path = tapeBands + "/window_" + std::to_string(globalBand + first);
                { std::ofstream f(path + ".tmp");
                  f << count << "\n";
                  if (!f.good()) die("write tape window"); }
                if (::rename((path + ".tmp").c_str(), path.c_str())) die("publish tape window");
                std::fprintf(stderr, "SS_REORDER_WINDOW first=%llu bands=%llu rows=%llu row_limit=%llu bytes=%llu ratio=%.6f\n",
                    (unsigned long long)(globalBand + first), (unsigned long long)count,
                    (unsigned long long)rows, (unsigned long long)rowLimit,
                    (unsigned long long)(rows * 8), double(rows) / double(rowLimit));
                first += count;
            }
        }
        pool->run([&](unsigned) { band_worker(); });
        // BAND BOUNDARIES are computed INSIDE the band that owns them (each slice
        // is published complete, so no post-wave patch can race a consumer). The
        // only thing the wave must still carry forward is the LAST row position of
        // its last non-empty band: the next wave's first band seams against it
        // (wave seams were not written at all in an earlier version, and 4 of the
        // 22 seams at 150 MB then had a true LCP of 1).
        if (lcpOn)
            for (u64 b = 0; b < nbands; ++b)
                if (bandLastPos[b] != ~0ull) prev_wave_last = bandLastPos[b];
        if (!tapeBands.empty()) {
            // Drain the SA stream before scattering another wave: no cross-wave disk
            // overlap. The LCP slices are NOT drained here: their consumer folds a
            // 65536-RUN piece band, which can be LARGER than one wave of rows (4 MB:
            // 32 x 7421 = 237,500 rows < one fold band's run span), so a wave-aligned
            // drain is the same chicken-and-egg one level up - measured as a full
            // wait cycle with all three tools asleep. The slice stream is bounded by
            // lcpWindowNow() instead, which is measured in rows and is what LCP_SLICE
            // reports.
            for (u64 b = 0; b < nbands; ++b) waitBandConsumed(globalBand + b);
            globalBand += nbands;
        }
        emitted += wave_rows;
        if (false) for (u64 b = 0; b < nbands; ++b) {
            char oname[512];
            std::snprintf(oname, sizeof oname, "%s/w%" PRIu64 ".out%05" PRIu64, scratch.c_str(), w, b);
            int f = ::open(oname, O_RDONLY);
            if (f < 0) die("open band out");
            for (;;) {
                ssize_t r = ::read(f, cat.data(), cat.size());
                if (r < 0) die("read band out");
                if (r == 0) break;
                size_t off = 0;
                while (off < size_t(r)) {
                    ssize_t wr = ::write(ofd, cat.data() + off, size_t(r) - off);
                    if (wr <= 0) die("write out");
                    off += size_t(wr);
                }
            }
            ::close(f);
            ::unlink(oname);
            std::snprintf(oname, sizeof oname, "%s/w%" PRIu64 ".sym%05" PRIu64, scratch.c_str(), w, b);
            int sf = ::open(oname, O_RDONLY);
            if (sf < 0) die("open band sym");
            for (;;) {
                ssize_t r = ::read(sf, symcat.data(), symcat.size());
                if (r < 0) die("read band sym");
                if (r == 0) break;
                size_t off = 0;
                while (off < size_t(r)) {
                    ssize_t wr = ::write(sfd, symcat.data() + off, size_t(r) - off);
                    if (wr <= 0) die("write sym");
                    off += size_t(wr);
                }
            }
            ::close(sf);
            ::unlink(oname);
        }
        rows_done += 0;
        ties_total += ties_w.load();
        {
            double rd = 0, so = 0, wr = 0, rdm = 0, som = 0, wrm = 0;
            for (u64 b = 0; b < nbands; ++b) {
                rd += tRead[b]; so += tSort[b]; wr += tWrite[b];
                rdm = std::max(rdm, tRead[b]); som = std::max(som, tSort[b]); wrm = std::max(wrm, tWrite[b]);
            }
            std::fprintf(stderr,
                "SS_WAVE w=%" PRIu64 "/%" PRIu64 " bands=%" PRIu64 " wall=%.2f"
                " scan=%.2f | band_sum read=%.2f sort=%.2f write=%.2f"
                " | band_max read=%.2f sort=%.2f write=%.2f | fd_live=%" PRIu64
                " fd_peak=%" PRIu64 "\n",
                w + 1, waves, nbands, now_sec() - tw, tScan,
                rd, so, wr, rdm, som, wrm, noteFdPeak(), FD_PEAK.load());
        }
    }

    if (emitted != N) die("scatter row coverage != n");
    if (lfd >= 0) {
        // DEDUPE BY RANK, keeping the LAST value written for a rank: a band's own
        // merge can escape the rank that the seam also escapes (the block-start
        // inheritance), and the finish requires a STRICTLY ordered escape list.
        std::sort(gEsc.begin(), gEsc.end(),
                  [](const std::pair<u64,u64>& a, const std::pair<u64,u64>& b) {
                      return a.first != b.first ? a.first < b.first : a.second < b.second;
                  });
        gEsc.erase(std::unique(gEsc.begin(), gEsc.end(),
                               [](const std::pair<u64,u64>& a, const std::pair<u64,u64>& b) {
                                   return a.first == b.first;
                               }), gEsc.end());
        for (auto& e : gEsc) {
            u64 pair[2] = {e.first, e.second};
            if (::pwrite(lfd, pair, 16, off_t(48 + off_t(N) + off_t(&e - gEsc.data()) * 16)) != 16)
                die("write lcp escape");
        }
        const u64 ne = gEsc.size();
        u64 maxL = 0;
        for (auto& e : gEsc) maxL = std::max(maxL, e.second);
        if (::pwrite(lfd, &ne, 8, 24) != 8) die("patch escapes");
        if (::pwrite(lfd, &maxL, 8, 32) != 8) die("patch maxlcp");
        ::fsync(lfd); ::close(lfd);
    }
    if (sampleFd >= 0) { ::fsync(sampleFd); ::close(sampleFd); }
    if (ofd >= 0) { ::fsync(ofd); ::close(ofd); }
    if (sfd >= 0) { ::fsync(sfd); ::close(sfd); }
    if (!tapeBands.empty()) marker(tapeBands + "/producer.done");
    if (lcpOn && !tapeBands.empty()) {
        // THE RESIDENCY RECEIPT: the largest slice this producer ever held is one
        // band's, and rows must sum to n. Both are gates, not telemetry.
        if (lcpRows != N) die("lcp slices do not cover n");
        std::fprintf(stderr,
            "SS_LCP_STREAM mode=sorter-bands slices=%" PRIu64 " rows=%" PRIu64
            " escapes=%" PRIu64 " bytes=%" PRIu64 " max_live_lcp_slice_bytes=%" PRIu64
            " slice_bytes_per_row=%.6f K=%" PRIu64 "\n",
            lcpSlices, lcpRows, lcpEscapes, lcpSliceBytes, lcpMaxSliceBytes,
            double(lcpSliceBytes) / double(N), pendingBands);
        // THE LCP SLICE FOOTPRINT, in the units the residency law is stated in.
        // live = published-but-not-yet-retired SLICE bytes on disc (the finish
        // unlinks each slice as its rank cursor passes it), sampled at every
        // credit wait - the peak is exact because between two waits live only
        // grows. This is the number that may never approach n: one K-band window
        // of slices, never a resident 1.34n column.
        std::fprintf(stderr,
            "LCP_SLICE max_live_bytes=%" PRIu64 " slices=%" PRIu64 " peak_over_n=%.6f"
            " live_slices_peak=%" PRIu64 " slice_bytes_total=%" PRIu64 " rows=%" PRIu64
            " escapes=%" PRIu64 " rows_per_run=%.4f lcp_window_final=%" PRIu64
            " max_slice_bytes=%" PRIu64 "\n",
            lcpPeakLiveBytes, lcpSlices, double(lcpPeakLiveBytes) / double(N),
            lcpPeakLiveSlices, lcpSliceBytes, lcpRows, lcpEscapes,
            lcpRowsSeen.load() ? double(lcpRowsSeen.load()) / double(std::max<u64>(1, lcpRunsSeen.load())) : 0.0,
            lcpWindowNow(), lcpMaxSliceBytes);
    }
    // ===================== THE BUDGET, PAID =================================
    // The receipt, and the two assertions that keep the model from rotting.
    //   budget_ratio = peak OWNED (anon) RSS / the envelope. It must stay under
    //   SS_BUDGET_RATIO_MAX (0.90); if it does not, the sizer priced a different
    //   process than the one that ran and the build FAILS LOUD instead of printing
    //   a comfortable story.
    //   peak_file_kb is the corpus MAPPING: clean, reclaimable page cache that can
    //   never OOM this process and is not part of the sorter's footprint - reported
    //   beside the anon figure so nothing is hidden, never mixed into the ratio.
    rss.finish();
    const u64 peak_anon_kb = rss.peak_anon.load();
    const u64 peak_file_kb = rss.peak_file.load();
    const u64 peak_total_kb = rss.peak_total.load();
    struct rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    const u64 hwm_kb = (u64)ru.ru_maxrss;
    const double anon_ratio = double(peak_anon_kb) * 1024.0 / double(envelope_bytes);
    const double total_ratio = double(hwm_kb) * 1024.0 / double(envelope_bytes);
    const u64 arena_peak_rows = gate.peak;
    const u64 arena_peak_bytes = arena_peak_rows * budget.bytes_per_row;
    noteFdPeak();
    const u64 fd_peak = FD_PEAK.load();   // the PEAK, not the count at exit
    std::fprintf(stderr,
        "SS_TAPE_FD_PEAK live_fds_peak=%" PRIu64 " fd_soft=%" PRIu64 " fd_hard=%" PRIu64
        " fd_wanted=%" PRIu64 " bands_in_flight=%" PRIu64 " threads=%" PRIu64
        " (peak of /proc/self/fd: one open part file per scan thread, one band at a"
        " time per worker, no descriptor held for a band already read)\n",
        fd_peak, fd_soft, fd_hard, fd_wanted, budget.bands_in_flight, threads);
    if (fd_peak > fd_soft)
        die("TAPE_FD_OVER_LIMIT: the run reached " + std::to_string(fd_peak) +
            " live descriptors against a soft limit of " + std::to_string(fd_soft) +
            " (hard " + std::to_string(fd_hard) + ") - raise RLIMIT_NOFILE or --par-bands");
    std::fprintf(stderr,
        "SS_TAPE_PEAK peak_rss_kb=%" PRIu64 " budget_ratio=%.4f budget_bytes=%" PRIu64
        " peak_anon_kb=%" PRIu64 " peak_file_kb=%" PRIu64 " peak_total_kb=%" PRIu64
        " total_ratio=%.4f arena_peak_rows=%" PRIu64 " arena_peak_bytes=%" PRIu64
        " arena_model_bytes=%" PRIu64 " cap_rows=%" PRIu64 " rows_per_band=%" PRIu64
        " arena_planned_bytes=%" PRIu64 " bands_in_flight=%" PRIu64 " waves_actual=%" PRIu64
        " gate_waits=%" PRIu64
        " memory_gb=%.2f (budget_ratio = peak OWNED RSS / envelope, asserted <= %.2f; "
        "peak_file_kb is the CLEAN reclaimable corpus mapping, reported, never counted)\n",
        hwm_kb, anon_ratio, envelope_bytes, peak_anon_kb, peak_file_kb, peak_total_kb,
        total_ratio, arena_peak_rows, arena_peak_bytes, budget.arena, budget.cap_rows,
        rows_per_band, budget.arena_planned * budget.bytes_per_row, budget.bands_in_flight, waves,
        gate.wait_count(),
        memory_gb > 0 ? memory_gb : ram_gb, double(SS_BUDGET_RATIO_MAX) / 100.0);
    if (anon_ratio > double(SS_BUDGET_RATIO_MAX) / 100.0) {
        std::fprintf(stderr,
            "SS_TAPE_BUDGET_VIOLATION owned_rss=%" PRIu64 " kB = %.4f of the %.3f GiB envelope \n",
            peak_anon_kb, anon_ratio, double(envelope_bytes) / double(1u << 30));
        die("TAPE_BUDGET_VIOLATION: the sorter's OWNED peak RSS exceeded the model "
            "(SS_BUDGET_RATIO_MAX). The band plan no longer prices what the run holds - "
            "fix bytes_per_row or bands_in_flight, do not widen the envelope");
    }
    if (arena_peak_bytes > envelope_bytes) {
        die("TAPE_ARENA_OVER_BUDGET: the admitted band arenas peaked at " +
            std::to_string(arena_peak_bytes) + " B against an envelope of " +
            std::to_string(envelope_bytes) + " B (cap_rows=" + std::to_string(budget.cap_rows) +
            ", rows_per_band=" + std::to_string(rows_per_band) + ")");
    }
    // THE PLAN'S OWN RATIO, ASSERTED EXACTLY (not a monotonic proxy):
    //     live_rows x bpr / envelope  <=  0.90,  live_rows = parallel_bands x rows_per_band
    // This is the memory law the supervisor's mandate names: P live bands of
    // rows_per_band rows at bytes_per_row each, inside the envelope. It is checked
    // BEFORE the RSS receipt is trusted, so a plan that prices more than the
    // envelope can never be reported as a pass.
    {
        const u64 live_rows = budget.bands_in_flight * budget.rows_per_band;
        const double plan_ratio = double(live_rows) * double(budget.bytes_per_row) / double(envelope_bytes);
        const u64 budget_rows_whole = envelope_bytes / std::max<u64>(1, budget.bytes_per_row);
        std::fprintf(stderr,
            "SS_TAPE_PARALLEL parallel_bands=%" PRIu64 " rows_per_band=%" PRIu64
            " live_rows=%" PRIu64 " budget_rows=%" PRIu64 " ratio=%.4f bound=%.2f\n",
            budget.bands_in_flight, rows_per_band, live_rows, budget_rows_whole, plan_ratio,
            double(SS_BUDGET_RATIO_MAX) / 100.0);
        if (plan_ratio > double(SS_BUDGET_RATIO_MAX) / 100.0)
            die("TAPE_PLAN_RATIO: parallel_bands=" + std::to_string(budget.bands_in_flight) +
                " x rows_per_band=" + std::to_string(rows_per_band) + " x " +
                std::to_string(budget.bytes_per_row) + " B = " +
                std::to_string(live_rows * budget.bytes_per_row) + " B of " +
                std::to_string(envelope_bytes) + " B (ratio " + std::to_string(plan_ratio) +
                " > 0.90) - the plan prices more than the envelope it was given");
    }
    std::fprintf(stderr,
        "SS_DONE n=%" PRIu64 " waves=%" PRIu64 " ties=%" PRIu64
        " sample_s=%.2f waves_s=%.2f total_s=%.2f\n",
        N, waves, ties_total, t_sample, now_sec() - t1, now_sec() - t0);
    if (!selftestOracle.empty()) {
        std::ifstream oracle(selftestOracle), sa(out, std::ios::binary), symbols(symPath, std::ios::binary);
        if (!oracle || !sa || !symbols) die("selftest open oracle/output");
        sa.seekg(48);
        u8 inverse[256];
        for (unsigned c = 0; c < 256; ++c) inverse[REMAP ? REMAP[c] : c] = u8(c);
        std::string line;
        u64 rank = 0;
        while (std::getline(oracle, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            u64 expectedRank, expectedPos, expectedSymbol, p;
            unsigned char carried;
            if (!(fields >> expectedRank >> expectedPos >> std::hex >> expectedSymbol))
                die("selftest malformed oracle");
            if (!sa.read(reinterpret_cast<char*>(&p), 8) ||
                !symbols.read(reinterpret_cast<char*>(&carried), 1)) die("selftest truncated output");
            if (p == 0 && carried != SENTINEL_SYMBOL) die("selftest END carry");
            const unsigned symbol = p == 0 ? 0 : inverse[carried];
            if (expectedRank != rank || expectedPos != p || expectedSymbol != symbol)
                die("G6 disagreement rank=" + std::to_string(rank) + " position=" + std::to_string(p));
            ++rank;
        }
        if (rank != N || sa.peek() != EOF || symbols.peek() != EOF) die("selftest row coverage");
        std::printf("G6_PASS mode=%s rows=%" PRIu64 " mismatches=0\n",
                    "virtual", rank);
    }
    return 0;
}
