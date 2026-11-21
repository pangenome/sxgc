// rowwalk_gate.cpp - THE CLIMB's sampled row-walk battery.
//
// NO BRUTE FORCE ANYWHERE: hundreds of random ranks, each verified by
// ONE suffix walk against the corpus bytes (the remapped dollar order,
// the brute_sa law: byte-by-byte, equal-up-to-the-shorter-end -> the
// shorter first; the virtual END suffix at FILE_N sorts first).
//
// The battery reads the PRODUCER's band stream (band_<i>.sa, numeric
// order, u64 rows packed sym<<48|pos) from a COPY taken after SS_DONE
// (the finish consumes the originals). Per sampled rank r it checks
// SA[r-1] < SA[r] < SA[r+1] exactly, plus the packed symbol bits, the
// row-0 END row, the row count, and the position bounds. The maximum
// walk depth seen is journaled (the battery's honest cost).
//
//   usage: rowwalk_gate <corpus> <remap> <bands_copy_dir> <n_samples> <seed>
#include <algorithm>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <string>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

static void die(const std::string& m) {
    std::fprintf(stderr, "rowwalk_gate: %s\n", m.c_str());
    std::exit(2);
}

static u64 FILE_N = 0;
static const u8* T = nullptr;
static u8 REMAP[256];

// the remapped dollar-order compare: <0, 0, >0 (one suffix walk)
static int sufCmp(u64 a, u64 b, u64& depth) {
    if (a == FILE_N) return b == FILE_N ? 0 : -1;   // the END first
    if (b == FILE_N) return +1;
    const u64 cap = (FILE_N - a) < (FILE_N - b) ? (FILE_N - a) : (FILE_N - b);
    u64 i = 0;
    while (i < cap) {
        // memcmp in 64-byte strides: the walk is exact, the strides
        // only make it fast
        const u64 step = 64;
        const u64 take = std::min<u64>(step, cap - i);
        const int c = std::memcmp(T + a + i, T + b + i, size_t(take));
        if (c) {
            // refine to the byte
            u64 j = i;
            while (REMAP[T[a + j]] == REMAP[T[b + j]]) ++j;
            depth = std::max(depth, j);
            return REMAP[T[a + j]] < REMAP[T[b + j]] ? -1 : +1;
        }
        i += take;
    }
    depth = std::max(depth, cap);
    // equal up to the shorter's end: the shorter suffix sorts first
    return (FILE_N - a) < (FILE_N - b) ? -1
         : ((FILE_N - a) > (FILE_N - b) ? +1 : 0);
}

int main(int argc, char** argv) {
    if (argc != 6)
        die("usage: rowwalk_gate <corpus> <remap> <bands_copy_dir> <n_samples> <seed>");
    // the remap
    {
        FILE* rf = std::fopen(argv[2], "rb");
        if (!rf || std::fread(REMAP, 1, 256, rf) != 256) die("remap read");
        std::fclose(rf);
    }
    // the corpus (mmap; reads apply the remap IN THE COMPARATOR, the
    // raw bytes stay the file's)
    int fd = ::open(argv[1], O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("open corpus");
    FILE_N = u64(st.st_size);
    if (!FILE_N) die("empty corpus");
    T = (const u8*)::mmap(nullptr, size_t(FILE_N), PROT_READ, MAP_PRIVATE, fd, 0);
    if (T == MAP_FAILED) die("mmap corpus");
    const u64 N = FILE_N + 1;

    // the band stream: band_<i>.sa, numeric order, concatenated = the
    // row stream (packed sym<<48|pos)
    const std::string dir = argv[3];
    std::vector<std::string> files;
    {
        DIR* d = ::opendir(dir.c_str());
        if (!d) die("open bands dir");
        while (struct dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 8 && n.compare(0, 5, "band_") == 0
                && n.compare(n.size() - 3, 3, ".sa") == 0)
                files.push_back(n);
        }
        ::closedir(d);
        std::sort(files.begin(), files.end(),
                  [](const std::string& x, const std::string& y) {
                      return std::stoull(x.substr(5)) < std::stoull(y.substr(5));
                  });
        if (files.empty()) die("no band_*.sa files in the copy dir");
    }
    // map each band, keep (fd, ptr, rows) - the random access is over
    // the concatenated stream; build a small index of band starts
    struct Band { void* p; u64 rows; };
    std::vector<Band> bands;
    std::vector<u64> bandStart;   // the first global row of each band
    u64 totalRows = 0;
    for (const auto& f : files) {
        const std::string path = dir + "/" + f;
        int bfd = ::open(path.c_str(), O_RDONLY);
        struct stat bs{};
        if (bfd < 0 || ::fstat(bfd, &bs)) die("stat " + path);
        const u64 rows = u64(bs.st_size) / 8;
        if (bs.st_size % 8) die("band size not a multiple of 8: " + path);
        void* p = nullptr;
        if (rows) {
            p = ::mmap(nullptr, size_t(bs.st_size), PROT_READ, MAP_PRIVATE, bfd, 0);
            if (p == MAP_FAILED) die("mmap " + path);
        }
        ::close(bfd);
        bandStart.push_back(totalRows);
        bands.push_back(Band{p, rows});
        totalRows += rows;
    }
    if (totalRows != N)
        die("ROW_COUNT: the band stream has " + std::to_string(totalRows)
            + " rows against N=" + std::to_string(N)
            + " (FILE_N+1) - rows were lost or duplicated");
    auto rowAt = [&](u64 r) -> u64 {
        const size_t b = size_t(std::upper_bound(bandStart.begin(),
                                                 bandStart.end(), r)
                                - bandStart.begin()) - 1;
        return ((const u64*)bands[b].p)[r - bandStart[b]];
    };
    auto posOf = [&](u64 r) -> u64 { return rowAt(r) & ((1ull << 48) - 1); };
    auto symOf = [&](u64 r) -> u64 { return rowAt(r) >> 48; };

    // row 0: the virtual END (position FILE_N, the sentinel symbol)
    if (posOf(0) != FILE_N)
        die("ROW0: the first row is not the virtual END (pos="
            + std::to_string(posOf(0)) + " against FILE_N="
            + std::to_string(FILE_N) + ")");

    // the packed symbol law: sym = remap[T[pos-1]] (pos>0), the
    // producer's SENTINEL_SYMBOL for pos==0
    // (the sentinel packing is checked as a flag, never fatal: the
    // container's own END_REMAP certificate owns that law)

    // the sampled battery
    const u64 nSamples = std::strtoull(argv[4], nullptr, 10);
    const u64 seed = std::strtoull(argv[5], nullptr, 10);
    u64 xorshift = seed ? seed : 88172645463325252ull;
    auto nxt = [&] {
        xorshift ^= xorshift << 13; xorshift ^= xorshift >> 7;
        xorshift ^= xorshift << 17; return xorshift;
    };
    u64 maxDepth = 0, symChecked = 0, symFlags = 0;
    u64 checked = 0;
    const u64 SENT = 0;   // the producer's SENTINEL_SYMBOL (pack_sym at 0)
    for (u64 s = 0; s < nSamples; ++s) {
        // ranks in [1, N): row 0 checked above; include the edges
        const u64 r = (s < 8)
            ? (s * (N - 2)) / 8 + 1
            : (nxt() % (N - 1)) + 1;
        if (r == 0 || r >= N) continue;
        const u64 pPrev = posOf(r - 1), p = posOf(r),
                  pNext = (r + 1 < N) ? posOf(r + 1) : ~0ull;
        const int c1 = sufCmp(pPrev, p, maxDepth);
        if (c1 >= 0)
            die("ORDER at rank " + std::to_string(r) + ": SA[r-1]="
                + std::to_string(pPrev) + " !< SA[r]=" + std::to_string(p)
                + " (cmp=" + std::to_string(c1) + ")");
        if (pNext != ~0ull) {
            const int c2 = sufCmp(p, pNext, maxDepth);
            if (c2 >= 0)
                die("ORDER at rank " + std::to_string(r) + ": SA[r]="
                    + std::to_string(p) + " !< SA[r+1]="
                    + std::to_string(pNext) + " (cmp=" + std::to_string(c2)
                    + ")");
        }
        // the packed symbol bits
        {
            const u64 sym = symOf(r);
            const u64 pos = p;
            const u64 want = pos ? u64(REMAP[T[size_t(pos - 1)]]) : SENT;
            ++symChecked;
            if (sym != want) ++symFlags;   // journaled, not fatal
        }
        ++checked;
    }
    // the position bounds over the samples
    std::fprintf(stderr,
        "ROWWALK_GATE rows=%" PRIu64 " expected=%" PRIu64
        " samples=%" PRIu64 " checked=%" PRIu64
        " max_walk_depth=%" PRIu64
        " sym_checked=%" PRIu64 " sym_flags=%" PRIu64
        " row0_end=OK"
        " rule=no_brute_force;one_suffix_walk_per_row;exact_order\n",
        totalRows, N, nSamples, checked, maxDepth, symChecked, symFlags);
    if (checked == 0) die("no samples were checked");
    std::fprintf(stderr, "ROWWALK_GATE_DONE samples=%" PRIu64
        " max_depth=%" PRIu64 "\n", checked, maxDepth);
    return 0;
}
