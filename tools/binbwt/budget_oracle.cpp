// budget_oracle - the brute-force suffix-array oracle for the BUDGET work.
//
//   budget_oracle <corpus> <out.txt>
//
// Writes one line per rank: "<rank> <position> <symbol-hex>", where the symbol is
// the byte preceding the suffix (0 at position 0, i.e. the structural END), in
// the exact format scatter_sort --selftest reads. Dollar convention: a shorter
// suffix sorts first (a proper prefix is smaller).
//
// This is the tiny-scale arbiter for the sizer: the band plan may not change the
// SA, the tie order, the carried symbols or the adjacent LCP, so every
// adversarial shape here is compared by brute force before any fixture gate runs.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

static std::string T;

static int cmp_suffix(unsigned long long a, unsigned long long b) {
    const unsigned long long n = T.size();
    const unsigned long long cap = std::min(n - a, n - b);
    int c = std::memcmp(T.data() + a, T.data() + b, (size_t)cap);
    if (c != 0) return c;
    if (a == b) return 0;
    return (n - a) < (n - b) ? -1 : 1;
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: budget_oracle <corpus> <out>\n"); return 2; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "open %s\n", argv[1]); return 2; }
    char buf[1 << 16];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) T.append(buf, got);
    std::fclose(f);
    if (T.empty()) { std::fprintf(stderr, "empty corpus\n"); return 2; }
    const unsigned long long n = T.size();
    std::vector<unsigned long long> sa(n);
    std::iota(sa.begin(), sa.end(), 0ull);
    std::sort(sa.begin(), sa.end(), [](unsigned long long a, unsigned long long b) { return cmp_suffix(a, b) < 0; });
    FILE* o = std::fopen(argv[2], "w");
    if (!o) { std::fprintf(stderr, "create %s\n", argv[2]); return 2; }
    std::fprintf(o, "# rank position symbol  (brute force, dollar convention)\n");
    // RANK 0 IS THE STRUCTURAL END SUFFIX: the empty suffix at position n is the
    // smallest suffix under the dollar convention, and its predecessor (the
    // carried BWT symbol) is the LAST byte of the text.
    std::fprintf(o, "0 %llu %x\n", n, (unsigned)(unsigned char)T[n - 1]);
    for (unsigned long long r = 0; r < n; ++r) {
        const unsigned long long p = sa[r];
        const unsigned sym = p == 0 ? 0u : (unsigned char)T[p - 1];
        std::fprintf(o, "%llu %llu %x\n", r + 1, p, sym);
    }
    std::fclose(o);
    std::printf("ORACLE n=%llu rows=%llu\n", n, n + 1);
    return 0;
}
