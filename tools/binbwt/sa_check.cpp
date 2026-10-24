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
static u64 N = 0;
static bool DOLLAR = true;

// Compare suffix(a) vs suffix(b) under the active convention. Gives -1/0/+1.
static inline int suf_cmp(u64 a, u64 b) {
    const u64 cap = DOLLAR ? std::min(N - a, N - b) : N;
    u64 i = 0;
    // 8-byte chunks from the tape while both stay in range
    while (i + 8 <= cap) {
        u64 x, y;
        if (DOLLAR) {
            std::memcpy(&x, T + a + i, 8);
            std::memcpy(&y, T + b + i, 8);
        } else {
            // cyclic: bytes may wrap; read per-byte for the tail
            for (int j = 0; j < 8; ++j) {
                u64 xa = a + i + j, yb = b + i + j;
                u8 xb = T[xa < N ? xa : xa - N];
                u8 yc = T[yb < N ? yb : yb - N];
                if (xb != yc) return xb < yc ? -1 : 1;
            }
            i += 8;
            continue;
        }
        if (x != y) {
            if (RMP) {
                u8 xs[8], ys[8];
                std::memcpy(xs, &x, 8);
                std::memcpy(ys, &y, 8);
                for (int j = 0; j < 8; ++j)
                    if (RMP[xs[j]] != RMP[ys[j]]) return RMP[xs[j]] < RMP[ys[j]] ? -1 : 1;
            }
            u8 xs[8], ys[8];
            std::memcpy(xs, &x, 8);
            std::memcpy(ys, &y, 8);
            for (int j = 0; j < 8; ++j)
                if (xs[j] != ys[j]) return xs[j] < ys[j] ? -1 : 1;
        }
        i += 8;
    }
    for (; i < cap; ++i) {
        u8 xb, yc;
        if (DOLLAR) {
            xb = T[a + i];
            yc = T[b + i];
        } else {
            u64 xa = a + i, yb = b + i;
            xb = T[xa < N ? xa : xa - N];
            yc = T[yb < N ? yb : yb - N];
        }
        if (xb != yc) return xb < yc ? -1 : 1;
    }
    if (DOLLAR) {
        if (a == b) return 0;
        return (N - a) < (N - b) ? -1 : 1;   // shorter suffix is smaller
    }
    return a == b ? 0 : (a < b ? -1 : 1);
}

int main(int argc, char** argv) {
    std::string corpus, sapath, remapPath;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() { if (i + 1 >= argc) die("missing value", 2); return std::string(argv[++i]); };
        if (a == "--corpus") corpus = nxt();
        else if (a == "--sa") sapath = nxt();
        else if (a == "--dollar") DOLLAR = std::atoi(nxt().c_str()) != 0;
        else if (a == "--remap") remapPath = nxt();
        else die(("unknown arg " + a).c_str(), 2);
    }
    if (corpus.empty() || sapath.empty()) die("--corpus and --sa required", 2);

    int fd = ::open(corpus.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("corpus", 2);
    N = (u64)st.st_size;
    T = (const u8*)::mmap(nullptr, (size_t)N, PROT_READ, MAP_PRIVATE, fd, 0);
    if (T == MAP_FAILED) die("mmap corpus", 2);

    std::vector<u8> rmpBuf;
    if (!remapPath.empty()) {
        int rf = ::open(remapPath.c_str(), O_RDONLY);
        if (rf < 0) die("remap", 2);
        rmpBuf.resize(256);
        if (::read(rf, rmpBuf.data(), 256) != 256) die("remap short", 2);
        ::close(rf);
        RMP = rmpBuf.data();
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
                " dollar=%d\n", N, count, rows_field, DOLLAR ? 1 : 0);
    const u64* SA = R + 6;

    if (count != N) {
        std::printf("SA_CHECK_PERMUTATION_FAIL count=%" PRIu64 " n=%" PRIu64 "\n", count, N);
        return 4;
    }
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
