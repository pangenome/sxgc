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
    u64 klo, khi, pos;
};

static const u8* T = nullptr;
static u64 N = 0;
static const u8* REMAP = nullptr;   // optional: raw byte -> remapped symbol
static inline u64 pack_sym(u64 p, u8 sym) { return p | (u64(sym) << 48); }
static inline u64 unpack_pos(u64 v) { return v & ((1ull << 48) - 1); }

// The 16-byte padded key at p, big-endian packed.
static inline void key_at(u64 p, u64& klo, u64& khi) {
    u8 b[16];
    const u64 avail = p < N ? N - p : 0;
    const u64 take = avail < 16 ? avail : 16;
    if (take) std::memcpy(b, T + p, take);
    if (take < 16) std::memset(b + take, 0, 16 - take);
    u64 lo, hi;
    std::memcpy(&lo, b, 8);
    std::memcpy(&hi, b + 8, 8);
    klo = bswap64(lo);
    khi = bswap64(hi);
}

// Full suffix comparison in dollar mode (shorter is smaller).
static inline int suf_cmp(u64 a, u64 b) {
    const u64 cap = std::min(N - a, N - b);
    u64 i = 0;
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
    if (a == b) return 0;
    return (N - a) < (N - b) ? -1 : 1;
}

int main(int argc, char** argv) {
    std::string corpus, out, scratch = "/tmp/ss_scratch";
    double ram_gb = 1.0;
    u64 threads = 32;
    u64 par_bands = 1;
    u64 sample_n = 4000000;
    bool keep_rows = false;
    std::string sort_mode = "std";
    std::string remapPath;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() { if (i + 1 >= argc) die("missing value for " + a); return std::string(argv[++i]); };
        if (a == "--corpus") corpus = nxt();
        else if (a == "--out") out = nxt();
        else if (a == "--scratch") scratch = nxt();
        else if (a == "--ram-gb") ram_gb = std::atof(nxt().c_str());
        else if (a == "--threads") threads = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--par-bands") par_bands = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--sample") sample_n = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--keep-rows") keep_rows = true;
        else if (a == "--sort") sort_mode = nxt();
        else if (a == "--remap") remapPath = nxt();
        else die("unknown arg " + a);
    }
    if (corpus.empty() || out.empty()) die("--corpus and --out required");

    int fd = ::open(corpus.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("cannot open/stat corpus");
    N = (u64)st.st_size;
    if (N == 0) die("empty corpus");
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

    const u64 ram_bytes = (u64)(ram_gb * 1e9);
    const u64 row_bytes = sizeof(Row);
    // par_bands bands are sorted CONCURRENTLY, each with ram_bytes/par_bands of
    // RAM, so concurrency does not multiply the memory budget.
    if (par_bands < 1) par_bands = 1;
    u64 rows_per_band = std::max<u64>(1, ram_bytes / (row_bytes * par_bands));
    u64 bands = (N + rows_per_band - 1) / rows_per_band;
    if (bands < 1) bands = 1;

    std::fprintf(stderr,
        "SS_START n=%" PRIu64 " ram_gb=%.3f row_bytes=%zu rows_per_band=%" PRIu64
        " bands=%" PRIu64 " threads=%" PRIu64 "\n",
        N, ram_gb, row_bytes, rows_per_band, bands, threads);

    // ---------------- 1. SAMPLE: quantile boundaries ----------------
    const double t0 = now_sec();
    u64 stride = std::max<u64>(1, N / std::max<u64>(1, sample_n));
    std::vector<Row> samp;
    samp.reserve(N / stride + 2);
    // Jittered stride: (k * stride) + r, r in [0, stride). The jitter kills
    // periodicity aliasing with document/markup structure while keeping the
    // walk page-sequential.
    u64 rng = 0x243f6a8885a308d3ull;
    for (u64 p = 0; p < N; p += stride) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        u64 j = (rng >> 11) % stride;
        u64 q = p + j;
        if (q >= N) q = N - 1;
        Row r{};
        key_at(q, r.klo, r.khi);
        r.pos = q;
        samp.push_back(r);
    }
    std::sort(samp.begin(), samp.end(), [](const Row& a, const Row& b) {
        if (a.klo != b.klo) return a.klo < b.klo;
        return a.khi < b.khi;
    });
    std::vector<Row> bounds;   // upper bounds of each band except the last
    bounds.reserve(bands);
    for (u64 b = 1; b < bands; ++b) {
        u64 idx = (u64)((long double)samp.size() * (long double)b / (long double)bands);
        if (idx >= samp.size()) idx = samp.size() - 1;
        bounds.push_back(samp[idx]);
    }
    // Drop duplicate boundaries (they'd create empty bands) - a sampled
    // quantile can repeat when a key value is very frequent.
    bounds.erase(std::unique(bounds.begin(), bounds.end(), [](const Row& a, const Row& b) {
        return a.klo == b.klo && a.khi == b.khi;
    }), bounds.end());
    const u64 nb = bounds.size() + 1;
    const double t_sample = now_sec() - t0;
    std::fprintf(stderr, "SS_SAMPLE samples=%zu stride=%" PRIu64 " bounds=%zu bands=%" PRIu64
                         " wall=%.2f\n", samp.size(), stride, bounds.size(), nb, t_sample);
    samp.clear();
    samp.shrink_to_fit();

    // ---------------- 2. SCATTER ----------------
    const double t1 = now_sec();
    std::string cmd = "mkdir -p '" + scratch + "' && rm -f '" + scratch + "'/b*";
    if (std::system(cmd.c_str()) != 0) die("scratch prep failed");

    const u64 nparts = std::max<u64>(1, threads);
    std::vector<std::vector<int>> fds(nb, std::vector<int>(nparts, -1));
    // Per (band, thread) buffer: bounded so that nb * nparts * buf stays sane.
    u64 bufsz = 1u << 20;
    while (nb * nparts * bufsz > (1ull << 31) && bufsz > (1u << 12)) bufsz >>= 1;
    std::vector<std::vector<std::vector<u8>>> bufs(
        nparts, std::vector<std::vector<u8>>(nb));
    for (u64 t = 0; t < nparts; ++t)
        for (u64 b = 0; b < nb; ++b) bufs[t][b].reserve(bufsz);

    std::atomic<u64> done_parts{0};
    auto flush_buf = [&](u64 t, u64 b) {
        auto& v = bufs[t][b];
        if (v.empty()) return;
        if (fds[b][t] < 0) {
            char name[512];
            std::snprintf(name, sizeof name, "%s/b%05" PRIu64 ".p%05" PRIu64, scratch.c_str(), b, t);
            int f = ::open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (f < 0) die("create band part");
            fds[b][t] = f;
        }
        size_t off = 0;
        while (off < v.size()) {
            ssize_t w = ::write(fds[b][t], v.data() + off, v.size() - off);
            if (w <= 0) die("write band part");
            off += size_t(w);
        }
        v.clear();
    };

    auto worker = [&](u64 t) {
        const u64 lo = N * t / nparts, hi = N * (t + 1) / nparts;
        for (u64 p = lo; p < hi; ++p) {
            Row r{};
            key_at(p, r.klo, r.khi);
            // Carry the row's BWT symbol IN THE ROW. M[p-1] is free here: the
            // scatter is walking the text in order, so the byte behind p is
            // already in cache. Position 0 is the padding row whose symbol is 2,
            // matching the sink's own `p ? at(p-1) : 2`. Without this the
            // emission would pay one LOCKED corpus read per row.
            {
                u8 raw = (p == 0) ? 0 : T[p - 1];
                u8 sy = (p == 0) ? u8(2) : (REMAP ? REMAP[raw] : raw);
                r.pos = pack_sym(p, sy);
            }
            // band = upper_bound(bounds, key): the first bound >= key
            u64 b = (u64)(std::upper_bound(bounds.begin(), bounds.end(), r,
                [](const Row& key, const Row& bd) {
                    if (key.klo != bd.klo) return key.klo < bd.klo;
                    return key.khi < bd.khi;
                }) - bounds.begin());
            auto& v = bufs[t][b];
            v.insert(v.end(), (const u8*)&r, (const u8*)&r + sizeof(Row));
            if (v.size() >= bufsz) flush_buf(t, b);
        }
        for (u64 b = 0; b < nb; ++b) flush_buf(t, b);
        for (u64 b = 0; b < nb; ++b)
            if (fds[b][t] >= 0) { ::close(fds[b][t]); fds[b][t] = -1; }
        done_parts.fetch_add(1);
    };

    std::vector<std::thread> ts;
    for (u64 t = 0; t < nparts; ++t) ts.emplace_back(worker, t);
    for (auto& th : ts) th.join();
    const double t_scatter = now_sec() - t1;
    std::fprintf(stderr, "SS_SCATTER parts=%" PRIu64 " wall=%.2f rows=%" PRIu64 "\n",
                 nparts, t_scatter, N);

    // ---------------- 3+4. SORT each band (parallel), EMIT in order --------
    const double t2 = now_sec();
    std::atomic<u64> next_band{0};
    std::atomic<u64> ties_total{0};
    std::atomic<u64> rows_total{0};
    std::vector<double> band_wall(nb, 0.0);

    auto band_worker = [&]() {
        std::vector<Row> rows;
        std::vector<u8> obuf;
        obuf.reserve(1u << 20);
        for (;;) {
            const u64 b = next_band.fetch_add(1);
            if (b >= nb) return;
            const double bt0 = now_sec();
            rows.clear();
            for (u64 t = 0; t < nparts; ++t) {
                char name[512];
                std::snprintf(name, sizeof name, "%s/b%05" PRIu64 ".p%05" PRIu64,
                              scratch.c_str(), b, t);
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
                        ssize_t r = ::read(f, ((u8*)rows.data()) + at * sizeof(Row) + off,
                                           bytes - off);
                        if (r <= 0) die("read band part");
                        off += size_t(r);
                    }
                }
                ::close(f);
                if (!keep_rows) ::unlink(name);
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                if (a.klo != b.klo) return a.klo < b.klo;
                if (a.khi != b.khi) return a.khi < b.khi;
                return suf_cmp(unpack_pos(a.pos), unpack_pos(b.pos)) < 0;   // the tie set
            });
            u64 ties = 0;
            for (size_t i = 1; i < rows.size(); ++i)
                if (rows[i].klo == rows[i - 1].klo && rows[i].khi == rows[i - 1].khi) ++ties;
            ties_total.fetch_add(ties);
            rows_total.fetch_add(rows.size());
            // ordered positions -> this band's part file
            char oname[512];
            std::snprintf(oname, sizeof oname, "%s/out%06" PRIu64, scratch.c_str(), b);
            int of = ::open(oname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (of < 0) die("create band out");
            char sname[512];
            std::snprintf(sname, sizeof sname, "%s/sym%06" PRIu64, scratch.c_str(), b);
            int sf = ::open(sname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (sf < 0) die("create band sym");
            obuf.clear();
            for (const Row& r : rows) {
                const u64 p = unpack_pos(r.pos);
                const u8 sy = (u8)(r.pos >> 48);
                if (::write(sf, &sy, 1) != 1) die("write band sym byte");
                obuf.insert(obuf.end(), (const u8*)&p, (const u8*)&p + 8);
                if (obuf.size() >= (1u << 20)) {
                    size_t off = 0;
                    while (off < obuf.size()) {
                        ssize_t w = ::write(of, obuf.data() + off, obuf.size() - off);
                        if (w <= 0) die("write band out");
                        off += size_t(w);
                    }
                    obuf.clear();
                }
            }
            {
                size_t off = 0;
                while (off < obuf.size()) {
                    ssize_t w = ::write(of, obuf.data() + off, obuf.size() - off);
                    if (w <= 0) die("write band out");
                    off += size_t(w);
                }
            }
            ::fsync(of);
            ::close(of);
            ::fsync(sf);
            ::close(sf);
            rows.clear();
            rows.shrink_to_fit();
            band_wall[b] = now_sec() - bt0;
        }
    };

    {
        std::vector<std::thread> bs;
        const u64 np = std::min<u64>(par_bands, nb);
        for (u64 i = 0; i < np; ++i) bs.emplace_back(band_worker);
        for (auto& th : bs) th.join();
    }
    const double t_sort = now_sec() - t2;

    // concatenate the band parts IN BAND ORDER: bands are key ranges, so this
    // concatenation IS suffix order.
    int ofd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ofd < 0) die("create out");
    u64 hdr[6] = {0x434C5853ull /*'SXLC'*/, 1, N, N, 0, 0};
    if (::write(ofd, hdr, 48) != 48) die("write header");
    const std::string symPath = out + ".sym";
    int sfd = ::open(symPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (sfd < 0) die("create sym stream");
    std::vector<u8> cat(1u << 22);
    std::vector<u8> symcat(1u << 22);
    for (u64 b = 0; b < nb; ++b) {
        char oname[512];
        std::snprintf(oname, sizeof oname, "%s/out%06" PRIu64, scratch.c_str(), b);
        int f = ::open(oname, O_RDONLY);
        if (f < 0) die("open band out");
        for (;;) {
            ssize_t r = ::read(f, cat.data(), cat.size());
            if (r < 0) die("read band out");
            if (r == 0) break;
            size_t off = 0;
            while (off < size_t(r)) {
                ssize_t w = ::write(ofd, cat.data() + off, size_t(r) - off);
                if (w <= 0) die("write out");
                off += size_t(w);
            }
        }
        // the symbol stream for this band, in the same rank order
        std::snprintf(oname, sizeof oname, "%s/sym%06" PRIu64, scratch.c_str(), b);
        int sf = ::open(oname, O_RDONLY);
        if (sf < 0) die("open band sym");
        for (;;) {
            ssize_t r = ::read(sf, symcat.data(), symcat.size());
            if (r < 0) die("read band sym");
            if (r == 0) break;
            size_t off = 0;
            while (off < size_t(r)) {
                ssize_t w = ::write(sfd, symcat.data() + off, size_t(r) - off);
                if (w <= 0) die("write sym");
                off += size_t(w);
            }
        }
        ::close(sf);
        ::unlink(oname);
        ::close(f);
        ::unlink(oname);
    }
    ::fsync(ofd);
    ::close(ofd);
    ::fsync(sfd);
    ::close(sfd);
    const u64 ties = ties_total.load(), sorted_rows = rows_total.load();

    std::fprintf(stderr,
        "SS_DONE n=%" PRIu64 " bands=%" PRIu64 " rows=%" PRIu64 " ties=%" PRIu64
        " sample_s=%.2f scatter_s=%.2f sortemit_s=%.2f total_s=%.2f\n",
        N, nb, sorted_rows, ties, t_sample, t_scatter, t_sort, now_sec() - t0);
    return 0;
}
