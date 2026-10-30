// lcp_check - verify an emitted SA stream + LCP column against the text itself.
//
//   lcp_check <corpus> <out.sa> <out.sa.sym> <col>
//
// The column's flags bit 0 means "compact column with escapes": rows bytes of
// adjacent-rank LCP (255 = escaped) followed by escapes x (rank, value) u64
// pairs. Every value is re-derived from the corpus here and compared exactly -
// this is the arbiter for the two band-sort arms (the planned merge and the
// low-memory in-place one), which must produce the SAME column.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using u64 = uint64_t;
using u8 = uint8_t;

static std::string T;
static u64 FILE_N;

static u64 lcp_of(u64 a, u64 b) {
    const u64 cap = std::min(FILE_N - a, FILE_N - b);
    u64 i = 0;
    while (i < cap && T[a + i] == T[b + i]) ++i;
    return i;
}

int main(int argc, char** argv) {
    if (argc != 5) { std::fprintf(stderr, "usage: <corpus> <sa> <sym> <col>\n"); return 2; }
    {
        FILE* f = std::fopen(argv[1], "rb");
        if (!f) { std::fprintf(stderr, "open corpus\n"); return 2; }
        char buf[1 << 16]; size_t got;
        while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) T.append(buf, got);
        std::fclose(f);
        FILE_N = T.size();
    }
    const u64 n = FILE_N + 1;
    std::vector<u64> sa;
    {
        FILE* f = std::fopen(argv[2], "rb");
        if (!f) { std::fprintf(stderr, "open sa\n"); return 2; }
        u64 h[6]; if (std::fread(h, 8, 6, f) != 6) { std::fprintf(stderr, "sa header\n"); return 2; }
        if (h[1] != n) { std::fprintf(stderr, "sa header n=%llu expected %llu\n", (unsigned long long)h[1], (unsigned long long)n); return 2; }
        sa.resize(h[3]);
        if (std::fread(sa.data(), 8, sa.size(), f) != sa.size()) { std::fprintf(stderr, "sa short\n"); return 2; }
        std::fclose(f);
    }
    if (sa.size() != n) { std::fprintf(stderr, "sa rows %zu != n %llu\n", sa.size(), (unsigned long long)n); return 2; }
    std::vector<u8> sym(n);
    {
        FILE* f = std::fopen(argv[3], "rb");
        if (!f) { std::fprintf(stderr, "open sym\n"); return 2; }
        if (std::fread(sym.data(), 1, n, f) != n) { std::fprintf(stderr, "sym short\n"); return 2; }
        std::fclose(f);
    }
    u64 ch[6]; std::vector<u8> col; std::map<u64, u64> esc;
    {
        FILE* f = std::fopen(argv[4], "rb");
        if (!f) { std::fprintf(stderr, "open col\n"); return 2; }
        if (std::fread(ch, 8, 6, f) != 6) { std::fprintf(stderr, "col header\n"); return 2; }
        if (ch[2] != n) { std::fprintf(stderr, "col rows %llu != n %llu\n", (unsigned long long)ch[2], (unsigned long long)n); return 2; }
        col.resize(n);
        if (std::fread(col.data(), 1, n, f) != n) { std::fprintf(stderr, "col short\n"); return 2; }
        std::vector<u64> pair(ch[3] * 2);
        if (ch[3] && std::fread(pair.data(), 8, pair.size(), f) != pair.size()) { std::fprintf(stderr, "col escapes short\n"); return 2; }
        for (u64 i = 0; i < ch[3]; ++i) esc[pair[2 * i]] = pair[2 * i + 1];
        std::fclose(f);
    }
    u64 bad = 0;
    for (u64 r = 0; r < n; ++r) {
        const u64 want = r == 0 ? 0 : lcp_of(sa[r - 1], sa[r]);
        const u64 got = col[r] == 255 ? (esc.count(r) ? esc[r] : ~0ull) : col[r];
        if (got != want) {
            if (bad < 8)
                std::fprintf(stderr, "LCP_MISMATCH rank=%llu want=%llu got=%llu (col=%u)\n",
                             (unsigned long long)r, (unsigned long long)want, (unsigned long long)got, col[r]);
            ++bad;
        }
    }
    if (bad) { std::fprintf(stderr, "LCP_FAIL rows=%llu mismatches=%llu\n", (unsigned long long)n, (unsigned long long)bad); return 1; }
    std::printf("LCP_PASS rows=%llu escapes=%llu mismatches=0\n", (unsigned long long)n, (unsigned long long)esc.size());
    return 0;
}
