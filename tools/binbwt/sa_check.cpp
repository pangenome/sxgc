// sa_check - verify an emitted SA stream: permutation + exact suffix order.
//
//   sa_check --corpus C --sa F [--dollar 0|1] [--threads T]
//
// Checks, under ONE convention:
//   (1) PERMUTATION: every position in [0,n) appears exactly once.
//   (2) ORDER: SA[r] < SA[r+1] under the convention, i.e. the suffix at SA[r]
//       is strictly smaller. Ties cannot exist: two distinct positions never
//       have equal suffixes under either convention (dollar: the shorter is
//       smaller; cyclic: distinct rotations of a string ending in a byte that
//       occurs once are distinct - and if they tie, this tool reports it).
//
// Conventions (matching tools/binbwt/oracle.cpp's brute()):
//   dollar=1 : suffix(p) = M[p..n)            (shorter is smaller)
//   dollar=0 : suffix(p) = M[p..n) + M[0..p)  (cyclic rotation)
//
// Exit 0 = PASS, 3 = ORDER fail, 4 = PERMUTATION fail.

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

static void die(const char* m, int code) {
    std::fprintf(stderr, "sa_check: %s\n", m);
    std::exit(code);
}

static const u8* T = nullptr;
static const u8* RMP = nullptr;   // optional raw -> remapped (the PRODUCT order)
// Domain 0..FILE_N includes the virtual END suffix at rank zero.
static u64 N = 0, FILE_N = 0;
static bool DOLLAR = true;

// The product symbol at text index i: the corpus bytes through the remap, and the
// appended sentinel past the end of the file.
static inline u8 sym(u64 i) {
    if (i >= FILE_N) die("virtual END has no byte code", 2);
    return RMP ? RMP[T[i]] : T[i];
}

// Compare suffix(a) vs suffix(b) under the active convention. Gives -1/0/+1.
// Per-byte on the PRODUCT SYMBOLS: with the sentinel the alphabet terminates
// every comparison, so the finite order needs no length rule beyond the one the
// virtual byte already implements. (The per-byte form is deliberate: a verifier
// reads at most one page past nothing and never depends on mapping slack.)
static inline int suf_cmp(u64 a, u64 b) {
    if (a == b) return 0;
    const u64 cap = DOLLAR ? std::min(FILE_N - a, FILE_N - b) : N;
    for (u64 i = 0; i < cap; ++i) {
        const u8 xb = DOLLAR ? sym(a + i) : sym((a + i < N) ? a + i : a + i - N);
        const u8 yc = DOLLAR ? sym(b + i) : sym((b + i < N) ? b + i : b + i - N);
        if (xb != yc) return xb < yc ? -1 : 1;
    }
    // END RULE: exhaustion sorts below every byte, including raw zero.
    // Finite comparison never materializes END in the byte alphabet.
    // Cyclic mode retains its source-position tie break.
    if (DOLLAR) return (N - a) < (N - b) ? -1 : 1;
    return a < b ? -1 : 1;
}

int main(int argc, char** argv) {
    std::string corpus, sapath, remapPath;
    bool sample = false;
    u64 sampleFirst = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() { if (i + 1 >= argc) die("missing value", 2); return std::string(argv[++i]); };
        if (a == "--sample-first-rank") { sample = true; sampleFirst = std::strtoull(nxt().c_str(), nullptr, 10); }
        else if (a == "--corpus") corpus = nxt();
        else if (a == "--sa") sapath = nxt();
        else if (a == "--dollar") DOLLAR = std::atoi(nxt().c_str()) != 0;
        else if (a == "--remap") remapPath = nxt();
        else die(("unknown arg " + a).c_str(), 2);
    }
    if (corpus.empty() || sapath.empty()) die("--corpus and --sa required", 2);

    int fd = ::open(corpus.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("corpus", 2);
    FILE_N = (u64)st.st_size;
    N = FILE_N + 1;              // the appended end-of-text byte
    T = (const u8*)::mmap(nullptr, (size_t)FILE_N, PROT_READ, MAP_PRIVATE, fd, 0);
    if (T == MAP_FAILED) die("mmap corpus", 2);

    std::vector<u8> rmpBuf;
    if (!remapPath.empty()) {
        int rf = ::open(remapPath.c_str(), O_RDONLY);
        if (rf < 0) die("remap", 2);
        rmpBuf.resize(256);
        if (::read(rf, rmpBuf.data(), 256) != 256) die("remap short", 2);
        ::close(rf);
        RMP = rmpBuf.data();
        bool seen[256]{};
        for (unsigned b = 0; b < 256; ++b) {
            if (seen[RMP[b]]) die("remap must be a bijection", 2);
            seen[RMP[b]] = true;
        }
    }
    int sfd = ::open(sapath.c_str(), O_RDONLY);
    struct stat ss{};
    if (sfd < 0 || ::fstat(sfd, &ss)) die("sa", 2);
    const u64 bytes = (u64)ss.st_size;
    if (bytes < 48) die("sa too short", 2);
    u64* R = (u64*)::mmap(nullptr, (size_t)bytes, PROT_READ, MAP_PRIVATE, sfd, 0);
    if (R == MAP_FAILED) die("mmap sa", 2);
    // header: magic, version, n, rows, escapes, max_lcp, flags (48 B)
    const u64 rows_field = R[3];
    const u64 count = (bytes - 48) / 8;
    std::printf("SA_CHECK corpus_n=%" PRIu64 " sa_entries=%" PRIu64 " rows=%" PRIu64
                " dollar=%d convention=virtual-END corpus_bytes=%" PRIu64 "\n", N, count, rows_field, DOLLAR ? 1 : 0, FILE_N);
    const u64* SA = R + 6;

    if (sample) {
        if (!count || count > 1048576 || sampleFirst >= N || count > N - sampleFirst ||
            R[0] != 0x0000000141535853ull || R[1] != N || R[2] != 1 || R[3] != count || bytes != 48 + count * 8)
            die("invalid bounded order sample", 2);
        std::vector<u64> positions(SA, SA + count);
        for (u64 p : positions) if (p >= N) die("sample position out of range", 4);
        std::sort(positions.begin(), positions.end());
        if (std::adjacent_find(positions.begin(), positions.end()) != positions.end()) die("duplicate sample position", 4);
        for (u64 r = 0; r + 1 < count; ++r)
            if (suf_cmp(SA[r], SA[r + 1]) >= 0) die("sample order failure", 3);
        std::printf("SA_CHECK_SAMPLE_PASS first_rank=%" PRIu64 " entries=%" PRIu64 " adjacent_pairs=%" PRIu64 " global_permutation=not_checked\n", sampleFirst, count, count - 1);
        return 0;
    }
    if (count != N) {
        std::printf("SA_CHECK_PERMUTATION_FAIL count=%" PRIu64 " n=%" PRIu64 "\n", count, N);
        return 4;
    }
    if (DOLLAR && SA[0] != FILE_N) die("rank zero must be virtual END position n", 4);
    // permutation: a dense seen-bitmap over n
    std::vector<u64> seen((N + 63) / 64, 0);
    for (u64 r = 0; r < N; ++r) {
        const u64 p = SA[r];
        if (p >= N) {
            std::printf("SA_CHECK_PERMUTATION_FAIL rank=%" PRIu64 " pos=%" PRIu64 " out_of_range\n", r, p);
            return 4;
        }
        u64& w = seen[p >> 6];
        const u64 bit = 1ull << (p & 63);
        if (w & bit) {
            std::printf("SA_CHECK_PERMUTATION_FAIL rank=%" PRIu64 " pos=%" PRIu64 " repeated\n", r, p);
            return 4;
        }
        w |= bit;
    }
    // order
    for (u64 r = 0; r + 1 < N; ++r) {
        const int c = suf_cmp(SA[r], SA[r + 1]);
        if (c >= 0) {
            std::printf("SA_CHECK_ORDER_FAIL rank=%" PRIu64 " a=%" PRIu64 " b=%" PRIu64
                        " cmp=%d\n", r, SA[r], SA[r + 1], c);
            return 3;
        }
    }
    std::printf("SA_CHECK_PASS entries=%" PRIu64 " dollar=%d\n", N, DOLLAR ? 1 : 0);
    return 0;
}
