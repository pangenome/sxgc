// subdiv_oracle - the brute-force suffix-array oracle for WAVE_SUBDIVISION.md's G2.
//
//   subdiv_oracle --corpus C --sa F [--expect N] [--quiet]
//
// Convention (the tape's): dollar mode with a VIRTUAL END suffix. The empty
// suffix at position n is the smallest object and is rank 0; every other rank is
// the suffix corpus[p..n). This is the order the sorter's band stream claims, so
// the oracle sorts the suffixes outright - n log n string comparisons, no clever
// anything - and compares RANK BY RANK against the emitted stream. It is slow and
// obviously correct on purpose: it exists to disagree with the sorter when the
// sorter is wrong, not to be fast.
//
// Exit 0 = agree, 3 = disagree, 2 = usage/IO.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using u8 = uint8_t;
using u64 = uint64_t;

static void die(const std::string& m) { std::fprintf(stderr, "subdiv_oracle: %s\n", m.c_str()); std::exit(2); }

int main(int argc, char** argv) {
    std::string corpus, sa;
    bool quiet = false;
    u64 expect = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto nxt = [&]() { if (i + 1 >= argc) die("missing value for " + a); return std::string(argv[++i]); };
        if (a == "--corpus") corpus = nxt();
        else if (a == "--sa") sa = nxt();
        else if (a == "--expect") expect = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--quiet") quiet = true;
        else die("unknown arg " + a);
    }
    if (corpus.empty() || sa.empty()) die("--corpus and --sa required");
    int fd = ::open(corpus.c_str(), O_RDONLY);
    if (fd < 0) die("open corpus");
    struct stat st{};
    if (::fstat(fd, &st)) die("stat corpus");
    const u64 n = u64(st.st_size);
    if (!n) die("empty corpus");
    const u8* T = (const u8*)::mmap(nullptr, size_t(n), PROT_READ, MAP_PRIVATE, fd, 0);
    if (T == MAP_FAILED) die("mmap corpus");

    // THE BRUTE ORDER: positions 0..n-1 plus the virtual END (position n, the
    // empty suffix, which sorts first because every suffix is longer than it).
    std::vector<u64> ord;
    ord.reserve(size_t(n) + 1);
    for (u64 p = 0; p < n; ++p) ord.push_back(p);
    std::sort(ord.begin(), ord.end(), [&](u64 a, u64 b) {
        const u64 cap = std::min(n - a, n - b);
        const int c = cap ? std::memcmp(T + a, T + b, size_t(cap)) : 0;
        if (c) return c < 0;
        return (n - a) < (n - b);          // the shorter finite suffix is smaller
    });
    ord.insert(ord.begin(), n);            // the virtual END suffix, rank 0

    // THE EMITTED STREAM: 48-byte header, then u64 ranks.
    int sf = ::open(sa.c_str(), O_RDONLY);
    if (sf < 0) die("open sa");
    struct stat st2{};
    if (::fstat(sf, &st2)) die("stat sa");
    if (u64(st2.st_size) < 48) die("sa stream shorter than its header");
    const u64 rows = (u64(st2.st_size) - 48) / 8;
    if (rows != u64(st2.st_size - 48) / 8 || (u64(st2.st_size) - 48) % 8) die("sa stream misaligned");
    u64 mism = 0;
    u64 firstBadRank = ~0ull, firstBadGot = ~0ull, firstBadWant = ~0ull;
    {
        const size_t chunk = 1u << 16;
        std::vector<u64> buf(chunk);
        u64 rank = 0, at = 48;
        while (rank < rows) {
            const size_t want = size_t(std::min<u64>(chunk, rows - rank));
            size_t got = 0;
            while (got < want) {
                ssize_t r = ::pread(sf, ((u8*)buf.data()) + got * 8, want * 8 - got * 8, off_t(at + got * 8));
                if (r <= 0) die("short read sa");
                got += size_t(r) / 8;
            }
            for (size_t i = 0; i < want; ++i, ++rank) {
                const u64 wantPos = ord[size_t(rank)];
                if (buf[i] != wantPos) {
                    ++mism;
                    if (firstBadRank == ~0ull) { firstBadRank = rank; firstBadGot = buf[i]; firstBadWant = wantPos; }
                }
            }
            at += want * 8;
        }
    }
    const u64 wantEntries = n + 1;
    if (rows != wantEntries)
        std::fprintf(stderr,
            "SUBDIV_ORACLE_SHAPE rows=%" PRIu64 " expected=%" PRIu64 " corpus_n=%" PRIu64 "\n",
            rows, wantEntries, n);
    if (!quiet)
        std::printf("SUBDIV_ORACLE corpus_n=%" PRIu64 " entries=%" PRIu64 " expected=%" PRIu64
                    " mismatches=%" PRIu64 " first_bad_rank=%" PRIu64
                    " got=%" PRIu64 " want=%" PRIu64 "\n",
                    n, rows, expect ? expect : wantEntries, mism, firstBadRank, firstBadGot, firstBadWant);
    if (rows != wantEntries || mism) {
        std::printf("SUBDIV_ORACLE_FAIL rows=%" PRIu64 " mismatches=%" PRIu64 "\n", rows, mism);
        return 3;
    }
    std::printf("SUBDIV_ORACLE_PASS rows=%" PRIu64 " mismatches=0 (brute-force sort, virtual END rank 0)\n", rows);
    return 0;
}
