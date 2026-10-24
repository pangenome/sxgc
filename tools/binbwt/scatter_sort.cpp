// scatter_sort - THE TAPE MACHINE, prototype.
//
// Replaces "sort positions with random-access LCE at every level" with
// "scatter rows into key-ordered bands once, then sort each band in memory".
//
//   scatter_sort --corpus C --out SA [--ram-gb G] [--threads T]
//                [--sample N] [--scratch DIR] [--keep-rows]
//
// Pipeline:
//   1. SAMPLE   jittered-stride sample of 16-byte padded keys -> sorted ->
//               quantile BOUNDARIES. A sample suffices: bands only have to be
//               balanced, and every row is routed by a key COMPARISON, so a
//               sampling error can only unbalance a band, never misorder a row.
//   2. SCATTER  ONE sequential pass over the tape (T threads over contiguous
//               text ranges). Each row's 16-byte key is compared against the
//               boundaries and the row (key, position) is appended to that
//               band's file. Text is read exactly once and never again during
//               sorting; the sort is pure in-memory work on fixed-width rows.
//   3. SORT     per band, in RAM: compare keys as two big-endian u64s; only
//               rows whose 16-byte keys are EQUAL fall through to a real
//               suffix comparison. On web text that tie set is ~1% (and 97% of
//               that is uniform-byte runs) - see tools/binbwt/kgram_survey.
//   4. EMIT     bands are key ranges, so emitting them in boundary order IS
//               suffix order: the output is a plain concatenation.
//
// Output: the SA stream in the emitted-SSA format (48-byte header + u64/rank),
// which is exactly what --sa-sample-k 1 writes, so `sa_check` and the banked
// containers can verify it directly.
//
// Convention: dollar (plain suffix order, shorter suffix smaller) - established
// empirically against the bin machine's own emitted SA on a 4 MB slice.

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

static void die(const std::string& m) {
    std::fprintf(stderr, "scatter_sort: %s\n", m.c_str());
    std::exit(2);
}
static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}
static inline u64 bswap64(u64 x) { return __builtin_bswap64(x); }

// One row of the intermediate: a 16-byte big-endian-padded key + the position.
// (Big-endian packing makes the key comparison a plain integral compare; the
// bytes past the text end are zero, which is exactly the dollar convention's
// "shorter is smaller".)
struct Row {
    u64 klo, khi, kq, pos;   // 24-byte key (3 big-endian words) + packed pos/sym
};

static const u8* T = nullptr;
static u64 N = 0;
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

// The 16-byte padded key at p, big-endian packed.
static inline void key_at(u64 p, u64& klo, u64& khi, u64& kq) {
    u8 b[24];
    const u64 avail = p < N ? N - p : 0;
    const u64 take = avail < 24 ? avail : 24;
    if (take) std::memcpy(b, T + p, take);
    if (take < 24) std::memset(b + take, 0, 24 - take);
    // THE KEY LIVES IN THE PRODUCT ALPHABET. remap[] is not order-preserving
    // (remap[1]=255, remap[2]=253, ...), so building the key from raw bytes sorts
    // in the wrong order - the bug that made the 150 MB stream differ from the
    // bin machine's while the 4 MB slice (which happens to contain none of the
    // remapped bytes) matched exactly.
    if (REMAP) for (int i = 0; i < 24; ++i) b[i] = REMAP[b[i]];
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
static inline int suf_cmp_from(u64 a, u64 b, u64 from) {
    const u64 cap = std::min(N - a, N - b);
    if (from >= cap) {
        if (a == b) return 0;
        return (N - a) < (N - b) ? -1 : 1;
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
        return (N - a) < (N - b) ? -1 : 1;
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
    return (N - a) < (N - b) ? -1 : 1;
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
int main(int argc, char** argv) {
    std::string corpus, out, scratch = "/tmp/ss_scratch";
    double ram_gb = 1.0;
    double scratch_cap_gb = 0.0;      // 0 = 2n (the operator's law)
    u64 threads = 32, par_bands = 1, sample_n = 4000000;
    bool keep_rows = false;
    std::string remapPath, sort_mode = "std";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() { if (i + 1 >= argc) die("missing value for " + a); return std::string(argv[++i]); };
        if (a == "--corpus") corpus = nxt();
        else if (a == "--out") out = nxt();
        else if (a == "--scratch") scratch = nxt();
        else if (a == "--ram-gb") ram_gb = std::atof(nxt().c_str());
        else if (a == "--scratch-cap-gb") scratch_cap_gb = std::atof(nxt().c_str());
        else if (a == "--threads") threads = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--par-bands") par_bands = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--sample") sample_n = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--keep-rows") keep_rows = true;
        else if (a == "--sort") sort_mode = nxt();
        else if (a == "--remap") remapPath = nxt();
        else die("unknown arg " + a);
    }
    (void)sort_mode;
    if (corpus.empty() || out.empty()) die("--corpus and --out required");

    int fd = ::open(corpus.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("cannot open/stat corpus");
    N = (u64)st.st_size;
    if (!N) die("empty corpus");
    T = (const u8*)::mmap(nullptr, (size_t)N, PROT_READ, MAP_PRIVATE, fd, 0);
    if (T == MAP_FAILED) die("mmap corpus");
    ::madvise((void*)T, (size_t)N, MADV_WILLNEED);
    std::vector<u8> remapBuf;
    if (!remapPath.empty()) {
        int rf = ::open(remapPath.c_str(), O_RDONLY);
        if (rf < 0) die("cannot open remap");
        remapBuf.resize(256);
        if (::read(rf, remapBuf.data(), 256) != 256) die("remap short read");
        ::close(rf);
        REMAP = remapBuf.data();
    }
    build_pairmap();

    const u64 row_bytes = sizeof(Row);
    const u64 ram_bytes = (u64)(ram_gb * 1e9);
    if (par_bands < 1) par_bands = 1;
    u64 rows_per_band = std::max<u64>(1, ram_bytes / (row_bytes * par_bands));
    // THE CAP CAN BE THE BINDING CONSTRAINT: with a 2n scratch cap a wave is often
    // SMALLER than one RAM-sized band, which left each wave with a single band and
    // therefore a single-threaded sort - 20 waves of serial sorting, measured at
    // 370 s for a 1 GB corpus against 40 s unwaved. Bands exist to bound RAM, so
    // size them from whichever budget is smaller: RAM/par or the wave/par.
    // THE SCRATCH LAW: default cap is 2n bytes.
    const u64 cap_bytes = scratch_cap_gb > 0.0 ? (u64)(scratch_cap_gb * 1e9) : 2 * N;
    // Waves are filled greedily by byte class, so a wave can overshoot the target
    // by one class (measured 2.06n against a 2.0n cap). Fill to 95% of the cap and
    // the overshoot stays inside the law instead of breaking it.
    u64 rows_per_wave = std::max<u64>(1, (cap_bytes - cap_bytes / 20) / row_bytes);
    // BANDS MUST OUTNUMBER WORKERS. With bands == par_bands the work-stealing
    // queue is a 1:1 map: each worker gets exactly one band and the wall is set by
    // the fattest one (measured: 2.43 s of sort summed in a 1.14 s wave = ~2 cores
    // busy of 8). Oversubscribe 4x so stealing can balance the fat bands.
    if (rows_per_wave / (4 * par_bands) < rows_per_band)
        rows_per_band = std::max<u64>(1, rows_per_wave / (4 * par_bands));
    u64 waves = (N + rows_per_wave - 1) / rows_per_wave;
    if (waves < 1) waves = 1;

    std::fprintf(stderr,
        "SS_START n=%" PRIu64 " row_bytes=%zu cap_bytes=%" PRIu64 " (%.2fn) "
        "rows_per_wave=%" PRIu64 " waves=%" PRIu64 " ram_gb=%.2f rows_per_band=%" PRIu64
        " par_bands=%" PRIu64 " threads=%" PRIu64 "\n",
        N, row_bytes, cap_bytes, double(cap_bytes) / double(N), rows_per_wave, waves,
        ram_gb, rows_per_band, par_bands, threads);

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
                for (u64 p = lo; p < hi; ++p) ++h[REMAP ? REMAP[T[p]] : T[p]];
            });
        for (auto& th : ts) th.join();
        for (u64 t = 0; t < nt; ++t)
            for (int i = 0; i < 256; ++i) hist[i] += lh[t][i];
    }
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
                // split this byte by its second byte
                std::vector<u64> h2(256, 0);
                for (u64 p = 0; p < N; ++p) {
                    if ((REMAP ? REMAP[T[p]] : T[p]) != c) continue;
                    h2[p + 1 < N ? (REMAP ? REMAP[T[p + 1]] : T[p + 1]) : 0]++;
                }
                unsigned l2 = 0; u64 a2 = 0;
                for (unsigned d = 0; d < 256; ++d) {
                    if (a2 && a2 + h2[d] > rows_per_wave) {
                        wl.push_back({c, c + 1, true, c, l2, d}); l2 = d; a2 = 0;
                    }
                    a2 += h2[d];
                }
                if (l2 < 256) wl.push_back({c, c + 1, true, c, l2, 256});
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
        if (q >= N) q = N - 1;
        u64 a, b, c;
        key_at(q, a, b, c);
        sk.push_back(a);
    }
    std::sort(sk.begin(), sk.end());
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

    int ofd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ofd < 0) die("create out");
    u64 hdr[6] = {0x434C5853ull /*'SXLC'*/, 1, N, N, 0, 0};
    if (::write(ofd, hdr, 48) != 48) die("write header");
    const std::string symPath = out + ".sym";
    int sfd = ::open(symPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (sfd < 0) die("create sym stream");

    std::vector<u8> cat(1u << 22), symcat(1u << 22);
    u64 rows_done = 0, ties_total = 0, emitted = 0, wave_rows = 0;
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
        std::vector<std::vector<int>> fds(nbands, std::vector<int>(nparts, -1));
        u64 bufsz = 1u << 20;
        while (nbands * nparts * bufsz > (1ull << 31) && bufsz > (1u << 12)) bufsz >>= 1;
        std::vector<std::vector<std::vector<u8>>> bufs(nparts, std::vector<std::vector<u8>>(nbands));
        for (u64 t = 0; t < nparts; ++t)
            for (u64 b = 0; b < nbands; ++b) bufs[t][b].reserve(bufsz);
        auto flush_buf = [&](u64 t, u64 b) {
            auto& v = bufs[t][b];
            if (v.empty()) return;
            if (fds[b][t] < 0) {
                char name[512];
                std::snprintf(name, sizeof name, "%s/w%" PRIu64 ".b%05" PRIu64 ".p%05" PRIu64,
                              scratch.c_str(), w, b, t);
                int f = ::open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (f < 0) die("create band part");
                fds[b][t] = f;
            }
            size_t off = 0;
            while (off < v.size()) {
                ssize_t wr = ::write(fds[b][t], v.data() + off, v.size() - off);
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
                // look-ahead so a 16-byte key and the symbol are always in range
                const u64 from = (base ? base - 1 : base);
                const u64 want = std::min<u64>(chunk + 1 + 48, N - from);
                size_t got = 0;
                while (got < want) {
                    ssize_t r = ::pread(fd, buf.data() + got, want - got, off_t(from + got));
                    if (r <= 0) die("pread scan");
                    got += size_t(r);
                }
                // ZERO-PAD PAST EOF: key_at's contract is that bytes past the end
                // of the text are 0 (the dollar convention's "shorter is smaller").
                // A buffer read stops at EOF, so the tail must be zeroed or the
                // last positions get STALE bytes as their key - which is exactly
                // what failed SA_CHECK_ORDER_FAIL at rank 1,776,069.
                if (from + want == N && want < buf.size())
                    std::memset(buf.data() + want, 0, buf.size() - want);
                // REMAP THE CHUNK ONCE, in place, instead of 24 lookups per row.
                // Per-row key building with byte-by-byte REMAP[] was the real cost:
                // ~400 ns/row for the included rows (~43M per wave at 1 GB), which
                // dominated the whole scan. One sequential pass over the chunk
                // makes every key a plain big-endian load.
                if (REMAP)
                    for (u64 i = 0; i < got; ++i) buf[i] = REMAP[buf[i]];
                const u64 off = base - from;   // index in buf of position `base`
                for (u64 j = 0; j < chunk; ++j) {
                    const u64 p = base + j;
                    const u64 bj = off + j;
                    const unsigned c0 = buf[bj];
                    if (c0 < W.lo || c0 >= W.hi) continue;
                    if (W.lvl2) {
                        const unsigned c1 = (p + 1 < N) ? buf[bj + 1] : 0;
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
                    u8 sy;
                    if (p == 0) sy = 2;
                    else {
                        sy = buf[bj - 1];
                    }
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
            for (u64 b = 0; b < nbands; ++b)
                if (fds[b][t] >= 0) { ::close(fds[b][t]); fds[b][t] = -1; }
        };
        pool->run([&](unsigned t) { worker(t); });
        const double tScan = now_sec() - tw;

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
            u64 acc = emitted;
            for (u64 b = 0; b < nbands; ++b) { band_off[b] = acc; acc += band_rows[b]; }
            wave_rows = acc - emitted;
        }
        // ---- sort this wave's bands (parallel), writing at their own offsets ----
        std::atomic<u64> next_band{0};
        std::atomic<u64> ties_w{0};
        std::vector<double> tRead(nbands, 0.0), tSort(nbands, 0.0), tWrite(nbands, 0.0);
        auto band_worker = [&]() {
            std::vector<Row> rows;
            for (;;) {
                const u64 b = next_band.fetch_add(1);
                if (b >= nbands) return;
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
                    if (!keep_rows) ::unlink(name);
                }
                tRead[b] = now_sec() - br0;
                const double bs0 = now_sec();
                std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                    if (a.klo != b.klo) return a.klo < b.klo;
                    if (a.khi != b.khi) return a.khi < b.khi;
                    if (a.kq != b.kq) return a.kq < b.kq;
                    return suf_cmp_from(unpack_pos(a.pos), unpack_pos(b.pos), 24) < 0;
                });
                tSort[b] = now_sec() - bs0;
                const double bw0 = now_sec();
                u64 ties = 0;
                for (size_t i = 1; i < rows.size(); ++i)
                    if (rows[i].klo == rows[i-1].klo && rows[i].khi == rows[i-1].khi &&
                        rows[i].kq == rows[i-1].kq) ++ties;
                ties_w.fetch_add(ties);
                // ordered output: positions to the SA stream, symbols alongside
                std::vector<u64> ob;
                std::vector<u8> sb;
                ob.reserve(rows.size());
                sb.reserve(rows.size());
                for (const Row& r : rows) {
                    ob.push_back(unpack_pos(r.pos));
                    sb.push_back((u8)(r.pos >> 48));
                }
                // direct write at this band's offset in the global streams
                {
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
                rows.clear();
                rows.shrink_to_fit();
            }
        };
        pool->run([&](unsigned) { band_worker(); });
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
                " | band_max read=%.2f sort=%.2f write=%.2f\n",
                w + 1, waves, nbands, now_sec() - tw, tScan,
                rd, so, wr, rdm, som, wrm);
        }
    }

    ::fsync(ofd); ::close(ofd);
    ::fsync(sfd); ::close(sfd);
    std::fprintf(stderr,
        "SS_DONE n=%" PRIu64 " waves=%" PRIu64 " ties=%" PRIu64
        " sample_s=%.2f waves_s=%.2f total_s=%.2f\n",
        N, waves, ties_total, t_sample, now_sec() - t1, now_sec() - t0);
    return 0;
}
