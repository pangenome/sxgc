// Producer-only certificates: carried bytes == BWT(SA), and inverse BWT == input.
// END is the row whose suffix position is zero; no byte value is reserved.
// Usage: end_stream_check CORPUS SA [REMAP]
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
using U = uint64_t;
using B = uint8_t;
static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "END_STREAM_FAIL %s\n", message); std::exit(1); }
}
struct File {
    int fd; size_t size; const B* data;
    explicit File(const char* name) {
        fd = open(name, O_RDONLY); struct stat st{};
        require(fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0, "open/stat");
        size = size_t(st.st_size);
        data = static_cast<const B*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
        require(data != MAP_FAILED, "mmap");
    }
    ~File() { munmap(const_cast<B*>(data), size); close(fd); }
};
int main(int argc, char** argv) {
    require(argc == 3 || argc == 4, "usage: end_stream_check CORPUS SA [REMAP]");
    File corpus(argv[1]), sa(argv[2]);
    std::vector<char> name(std::strlen(argv[2]) + 5);
    std::snprintf(name.data(), name.size(), "%s.sym", argv[2]);
    File bwt(name.data());
    const U n = corpus.size + 1;
    require(corpus.size <= 150000001, "fixture exceeds 150 MB gate limit");
    require(sa.size == 48 + 8*n && bwt.size == n, "stream length");
    const U* positions = reinterpret_cast<const U*>(sa.data + 48);
    std::array<B, 256> map{}, inverse{};
    for (unsigned c = 0; c < 256; ++c) map[c] = inverse[c] = B(c);
    if (argc == 4) {
        File remap(argv[3]); require(remap.size == 256, "remap length");
        std::array<bool, 256> seen{};
        for (unsigned c = 0; c < 256; ++c) {
            map[c] = remap.data[c]; require(!seen[map[c]], "remap permutation");
            seen[map[c]] = true; inverse[map[c]] = B(c);
        }
    }
    U endRank = n, mismatches = 0, runs = 0;
    std::array<U, 256> counts{};
    std::vector<U> seen((n + 63)/64);
    for (U r = 0; r < n; ++r) {
        const U p = positions[r]; require(p < n, "position bounds");
        U& bits = seen[p >> 6]; const U bit = U(1) << (p & 63);
        require(!(bits & bit), "permutation duplicate"); bits |= bit;
        if (p == 0) endRank = r;
        const B expected = p == 0 ? 0 : map[corpus.data[p-1]];
        mismatches += bwt.data[r] != expected;
        if (p) ++counts[inverse[bwt.data[r]]];
        // Adjacent END and a data-zero row are distinct symbols even though
        // their byte transport cells can be equal.
        if (!r || p == 0 || positions[r-1] == 0 || bwt.data[r] != bwt.data[r-1]) ++runs;
    }
    require(endRank < n && positions[0] == corpus.size, "END frame ranks");
    std::printf("STREAM_BWT_SA rows=%llu mismatches=%llu runs=%llu END_rank=%llu\n",
        (unsigned long long)n, (unsigned long long)mismatches,
        (unsigned long long)runs, (unsigned long long)endRank);
    require(!mismatches, "stream disagrees with BWT(SA)");
    std::array<U, 256> cursor{};
    U total = 1; // rank zero is the END suffix
    for (unsigned c = 0; c < 256; ++c) { cursor[c] = total; total += counts[c]; }
    require(total == n, "symbol totals");
    std::vector<U> lf(n);
    for (U r = 0; r < n; ++r)
        lf[r] = r == endRank ? 0 : cursor[inverse[bwt.data[r]]]++;
    U rank = 0, errors = 0;
    for (U i = corpus.size; i > 0; --i) {
        require(rank != endRank && rank < n, "premature END in decode");
        errors += inverse[bwt.data[rank]] != corpus.data[i-1];
        rank = lf[rank];
    }
    require(rank == endRank && lf[rank] == 0, "decode closure");
    std::printf("PRODUCER_DECODE bytes=%llu mismatches=%llu\n",
        (unsigned long long)corpus.size, (unsigned long long)errors);
    require(!errors, "decode mismatch");
}
