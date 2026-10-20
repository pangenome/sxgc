// sa_anchor_verify - independent verification of a SAMPLED-SA anchor file at
// FIXTURE scale (charter Phase 4's correctness arm, and the counterpart of the
// tiny brute oracle: sa_anchor_oracle proves the emit stream against a brute SA,
// this proves a real 100 MB / 1 GB anchor file against the corpus itself).
//
//   sa_anchor_verify ANCHOR_FILE CORPUS REMAP [--full]
//
// Always checked (any k):
//   * the header describes itself: magic, version, pad=10 (the container's
//     padding offset: container_row = emit_rank + 10), k >= 1,
//     count == (rows-1)/k+1, rows == corpus length, file size exact;
//   * every entry is a position in [0, n);
//   * THE ORDER CHECK: the first symbol of the suffix at each anchor,
//     M[SA[j*k]] = REMAP[corpus[SA[j*k]]], is NON-DECREASING in j. Suffixes in
//     SA order have non-decreasing first symbols, so this is a necessary
//     condition on every anchor and it reads only the corpus - it shares no code
//     with the producer's ordering. (It is a 1-symbol projection, so it is
//     necessary but not sufficient at k > 1.)
//
// With --full (only meaningful when k == 1, i.e. the file IS the whole SA):
//   * the array is a PERMUTATION of [0, n) (a 1-bit-per-position bitmap);
//   * the suffixes are STRICTLY INCREASING in the file's order, verified by an
//     unbounded LCE-compare of each adjacent pair over the remapped corpus.
//     This is the sledgehammer statement made checkable: the k=1 anchor file is
//     a valid suffix array of the corpus.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void die(const char* m) { std::fprintf(stderr, "SA_VERIFY_FAIL %s\n", m); std::exit(2); }

static constexpr uint32_t MAGIC = 0x41535853u;   // 'SXSA'
static constexpr uint32_t VERSION = 1;
static constexpr uint32_t HDR = 48;

struct Mapped {
    int fd = -1; const uint8_t* p = nullptr; uint64_t n = 0;
    void open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) die("open corpus");
        struct stat st{};
        if (::fstat(fd, &st)) die("stat corpus");
        n = uint64_t(st.st_size);
        p = (const uint8_t*)::mmap(nullptr, n, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) die("mmap corpus");
    }
    ~Mapped() { if (p && p != MAP_FAILED) ::munmap((void*)p, n); if (fd >= 0) ::close(fd); }
};

int main(int argc, char** argv) {
    bool full = false;
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--full")) full = true;
    if (argc < 4) { std::fprintf(stderr, "usage: sa_anchor_verify ANCHOR CORPUS REMAP [--full]\n"); return 2; }
    Mapped anchor, corpus, remap;
    anchor.open(argv[1]); corpus.open(argv[2]); remap.open(argv[3]);
    if (remap.n != 256) die("remap must be 256 bytes");

    if (anchor.n < HDR) die("anchor file shorter than its header");
    uint32_t magic, version; uint64_t n, k, count, rows, pad;
    std::memcpy(&magic, anchor.p + 0, 4);
    std::memcpy(&version, anchor.p + 4, 4);
    std::memcpy(&n, anchor.p + 8, 8);
    std::memcpy(&k, anchor.p + 16, 8);
    std::memcpy(&count, anchor.p + 24, 8);
    std::memcpy(&rows, anchor.p + 32, 8);
    std::memcpy(&pad, anchor.p + 40, 8);
    if (magic != MAGIC) die("bad magic");
    if (version != VERSION) die("bad version");
    if (pad != 10) die("pad != 10 (the container's 1+9 padding rows before rank 0)");
    if (k < 1) die("k < 1");
    if (rows != corpus.n) die("header rows != corpus length");
    if (n != corpus.n) die("header n != corpus length");
    const uint64_t want = rows ? (rows - 1) / k + 1 : 0;
    if (count != want) die("count != (rows-1)/k+1");
    if (anchor.n != uint64_t(HDR) + count * 8) die("file size != header + count*8");

    const uint64_t* sa = (const uint64_t*)(anchor.p + HDR);
    // ---- every entry is a position, and the first symbols are non-decreasing --
    uint64_t prevSym = 0, firsts = 0, ascents = 0, equalFirst = 0;
    for (uint64_t j = 0; j < count; ++j) {
        const uint64_t p = sa[j];
        if (p >= corpus.n) die("anchor position out of range");
        const uint8_t sym = remap.p[corpus.p[p]];
        if (j) {
            if (sym < prevSym) {
                std::fprintf(stderr,
                    "SA_VERIFY_FAIL first-symbol descent at j=%llu (rank %llu): %u < %llu\n",
                    (unsigned long long)j, (unsigned long long)(j * k),
                    unsigned(sym), (unsigned long long)prevSym);
                return 2;
            }
            if (sym > prevSym) ++ascents; else ++equalFirst;
        }
        prevSym = sym;
        ++firsts;
    }
    std::fprintf(stderr,
        "SA_VERIFY_ORDER ok=1 anchors=%llu k=%llu ranks_covered=%llu first_symbol_ascents=%llu "
        "equal_first_symbol=%llu (necessary condition: SA order => non-decreasing first symbol)\n",
        (unsigned long long)count, (unsigned long long)k,
        (unsigned long long)((count - 1) * k + 1), (unsigned long long)ascents,
        (unsigned long long)equalFirst);
    if (!full) {
        std::fprintf(stdout, "SA_VERIFY_PASS mode=order-only anchors=%llu k=%llu\n",
                     (unsigned long long)count, (unsigned long long)k);
        return 0;
    }
    if (k != 1) die("--full requires k == 1 (the file must BE the suffix array)");
    if (count != corpus.n) die("--full requires count == n");
    // ---- PERMUTATION + STRICT INCREASE --------------------------------------
    std::vector<uint8_t> seen((corpus.n + 7) / 8, 0);
    for (uint64_t r = 0; r < count; ++r) {
        const uint64_t p = sa[r];
        if (seen[p >> 3] & (1u << (p & 7))) die("not a permutation: duplicate position");
        seen[p >> 3] |= uint8_t(1u << (p & 7));
    }
    uint64_t cmpSymbols = 0, maxLcp = 0;
    for (uint64_t r = 1; r < count; ++r) {
        const uint64_t a = sa[r - 1], b = sa[r];
        // unbounded suffix compare over the remapped corpus (dollar semantics:
        // the corpus is the text, no sentinel; the suffix order the producer
        // builds is the DOLLAR order, so a proper prefix sorts first).
        uint64_t d = 0;
        const uint64_t lim = corpus.n - (a > b ? a : b);
        while (d < lim && remap.p[corpus.p[a + d]] == remap.p[corpus.p[b + d]]) ++d;
        cmpSymbols += d + 1;
        if (d > maxLcp) maxLcp = d;
        const bool orderOk =
            (d == lim) ? ((corpus.n - a) < (corpus.n - b))          // one is a prefix
                       : (remap.p[corpus.p[a + d]] < remap.p[corpus.p[b + d]]);
        if (!orderOk) {
            std::fprintf(stderr, "SA_VERIFY_FAIL order at rank %llu: SA=%llu next=%llu lcp=%llu\n",
                         (unsigned long long)(r - 1), (unsigned long long)a,
                         (unsigned long long)b, (unsigned long long)d);
            return 2;
        }
    }
    std::fprintf(stderr,
        "SA_VERIFY_FULL ok=1 ranks=%llu permutation=1 strictly_increasing=1 "
        "adjacent_compare_symbols=%llu mean_lcp=%.3f max_lcp=%llu\n",
        (unsigned long long)count, (unsigned long long)cmpSymbols,
        double(cmpSymbols) / double(count ? count : 1), (unsigned long long)maxLcp);
    std::fprintf(stdout, "SA_VERIFY_PASS mode=full-sa anchors=%llu k=1\n",
                 (unsigned long long)count);
    return 0;
}
