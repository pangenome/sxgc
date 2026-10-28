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
// N IS THE TEXT LENGTH AND INCLUDES THE DECLARED END SYMBOL: the text is
// T' = corpus . END with the single occurrence at index FILE_N. FILE_N is what is
// on disc; the END is a virtual index served by code() below.
//
// THE ALPHABET IS k = 257 DECLARED CODES: code 0 is END, codes 1..256 are the 256
// byte values. The verification is 8-BIT everywhere (stream symbols, remap
// entries); a CODE is computed as an unsigned integer only to order two symbols.
// Two REALIZATIONS, both DECLARED by the caller (--end-mode), never inferred:
//   free-byte     the table maps a never-occurring byte (0xF5 by default) to
//                 stream symbol 0: END is directly representable, data symbols
//                 are >= 1, code = stream value.
//   declared-code the table is the identity: data codes are exactly 1..256
//                 (code = byte + 1) and END-0 exists only in the frame.
static u64 N = 0, FILE_N = 0;
static constexpr unsigned DECLARED_K = 257, CODE_END = 0;
static u8 END_BYTE = 0xF5;
static bool DECLARED_CODE = false;
static unsigned SHIFT = 0;
static bool DOLLAR = true;

// The declared CODE of text index i (0 = END, below every data code).
static inline unsigned code(u64 i) {
    if (i >= FILE_N) return CODE_END;
    return unsigned(RMP ? RMP[T[i]] : T[i]) + SHIFT;
}

// Compare suffix(a) vs suffix(b) under the active convention. Gives -1/0/+1.
// Per-byte on the PRODUCT SYMBOLS: with the sentinel the alphabet terminates
// every comparison, so the finite order needs no length rule beyond the one the
// virtual byte already implements. (The per-byte form is deliberate: a verifier
// reads at most one page past nothing and never depends on mapping slack.)
static inline int suf_cmp(u64 a, u64 b) {
    if (a == b) return 0;
    if (DOLLAR) {
        // Comparable DATA symbols: min(|a|,|b|) - 1, because the last symbol of
        // every suffix is the declared END, which is not a byte on disc.
        const u64 cap = std::min(N - a, N - b) - 1;
        for (u64 i = 0; i < cap; ++i) {
            const unsigned xb = code(a + i), yc = code(b + i);
            if (xb != yc) return xb < yc ? -1 : 1;
        }
        // THE END RULE: the shorter suffix sorts first (its next symbol is END,
        // code 0, below every data code). Two distinct suffixes cannot be
        // prefix-related with a unique END, so a!=b implies unequal lengths here.
        return (N - a) < (N - b) ? -1 : 1;
    }
    for (u64 i = 0; i < N; ++i) {
        const unsigned xb = code((a + i < N) ? a + i : a + i - N);
        const unsigned yc = code((b + i < N) ? b + i : b + i - N);
        if (xb != yc) return xb < yc ? -1 : 1;
    }
    return a < b ? -1 : 1;   // ascending source position breaks equal rotations
}

int main(int argc, char** argv) {
    std::string corpus, sapath, remapPath, endMode;
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
        else if (a == "--end-mode") endMode = nxt();
        else if (a == "--end-byte") END_BYTE = u8(std::strtoul(nxt().c_str(), nullptr, 0));
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
    }
    // THE REALIZATION IS DECLARED AND THEN PROVED. Exactly one mode is chosen;
    // a corpus that fits neither is an internal error, not a fallback.
    if (endMode.empty()) endMode = "free-byte";
    if (endMode == "free-byte") {
        DECLARED_CODE = false;
        SHIFT = 0;
        if (RMP) {
            if (RMP[END_BYTE] != 0) die("free-byte: the remap must map the end byte to symbol 0", 2);
            for (unsigned c = 0; c < 256; ++c)
                if (c != END_BYTE && RMP[c] == 0) die("free-byte: a data byte maps onto END", 2);
        }
    } else if (endMode == "declared-code") {
        DECLARED_CODE = true;
        SHIFT = 1;
        if (RMP)
            for (unsigned c = 0; c < 256; ++c)
                if (RMP[c] != u8(c)) die("declared-code: the remap must be the identity", 2);
    } else {
        die("--end-mode must be free-byte or declared-code", 2);
    }
    std::fprintf(stderr, "END_SYMBOL mode=%s k=%u end_symbol=%u end_positional=%u\n",
                 DECLARED_CODE ? "declared-code" : "free-byte", DECLARED_K, CODE_END,
                 DECLARED_CODE ? 1u : 0u);
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
    // THE END ROW IS EMITTED AT RANK 0: the only suffix that starts with the
    // declared END, and it must be the minimum of the whole order.
    if (DOLLAR && SA[0] != N - 1) {
        std::printf("SA_CHECK_END_ROW_FAIL rank=0 pos=%" PRIu64 " expected=%" PRIu64 "\n",
                    SA[0], N - 1);
        return 3;
    }
    std::printf("SA_CHECK_PASS entries=%" PRIu64 " dollar=%d end_row_rank=0 end_pos=%" PRIu64
                " mode=%s k=%u\n", N, DOLLAR ? 1 : 0, N - 1,
                DECLARED_CODE ? "declared-code" : "free-byte", DECLARED_K);
    return 0;
}
