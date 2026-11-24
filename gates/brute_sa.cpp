// brute_sa.cpp - the G0 oracle's reference suffix array (TAPE_BINS.md G0).
//
// A standalone, memory-honest brute force: positions sorted by a fresh
// dollar-order suffix comparator over the mmap'd corpus (O(N) memory -
// the position vector + the sort's log-depth recursion), the SA written
// as little-endian u64. Deliberately INDEPENDENT of bins_sort.cpp: its
// own comparator, its own main; nothing is shared but the corpus bytes.
//
// The python band-concatenation oracle that OOM'd the box (2026-11-17,
// 22:28) materialized c[p:]+b'\x00' for every position at once - O(N^2)
// bytes, petabyte-scale on a 6 MB corpus. This tool is the fix: the
// reference comes from a comparison sort that never copies the text.
//
//   usage: brute_sa <corpus> <out.sa>
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
#include <vector>

using u8 = uint8_t;
using u64 = uint64_t;

static const u8* T = nullptr;
static u64 FILE_N = 0;

static void die(const char* m) {
    std::fprintf(stderr, "brute_sa: %s\n", m);
    std::exit(2);
}

int main(int argc, char** argv) {
    if (argc != 3) die("usage: brute_sa <corpus> <out.sa>");
    int fd = ::open(argv[1], O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("cannot open corpus");
    FILE_N = u64(st.st_size);
    if (!FILE_N) die("empty corpus");
    T = (const u8*)::mmap(nullptr, size_t(FILE_N), PROT_READ, MAP_PRIVATE, fd, 0);
    if (T == MAP_FAILED) die("mmap corpus");
    const u64 N = FILE_N + 1;   // the virtual END suffix (position FILE_N)

    std::vector<u64> sa;
    sa.resize(size_t(N));
    for (u64 p = 0; p < N; ++p) sa[size_t(p)] = p;
    // dollar order: the shorter prefix first at the text end; byte-by-byte
    std::sort(sa.begin(), sa.end(), [](u64 a, u64 b) {
        if (a == FILE_N) return b != FILE_N;   // the empty suffix is first
        if (b == FILE_N) return false;
        const u64 cap = (FILE_N - a) < (FILE_N - b) ? (FILE_N - a) : (FILE_N - b);
        for (u64 i = 0; i < cap; ++i) {
            const u8 x = T[a + i], y = T[b + i];
            if (x != y) return x < y;
        }
        // equal up to the shorter's end: the shorter suffix sorts first
        return (FILE_N - a) < (FILE_N - b);
    });

    int out = ::open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) die("create out.sa");
    std::vector<u64> buf;
    buf.reserve(1 << 20);
    for (u64 i = 0; i < N; ++i) {
        buf.push_back(sa[size_t(i)]);
        if (buf.size() == (1u << 20)) {
            if (::write(out, buf.data(), buf.size() * 8) != (ssize_t)(buf.size() * 8))
                die("write sa");
            buf.clear();
        }
    }
    if (!buf.empty())
        if (::write(out, buf.data(), buf.size() * 8) != (ssize_t)(buf.size() * 8))
            die("write sa tail");
    if (::close(out)) die("close sa");
    std::fprintf(stderr, "BRUTE_SA_DONE rows=%" PRIu64 " corpus_bytes=%" PRIu64 "\n",
                 N, FILE_N);
    return 0;
}
