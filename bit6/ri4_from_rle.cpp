// ri4_from_rle.cpp — build a .ri4 v4 (runs-only; samples = INF) from the
// TeraLCP-format rlbwt pair BASE.bwt.heads + BASE.bwt.len (5-byte LE lens).
// Conventions copied verbatim from teralcp_chi.cpp build_runs_files/write:
//   n = sum(l); C[c] = rows with char < c (cumulative-before); K = total
//   count of 0x0A symbols in the BWT; sa_w = bits(n-1); samples = all-ones.
#include <cstdio>
#include <cstdint>
#include <vector>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <rlbwt-base> <out.ri4>\n", argv[0]);
        return 1;
    }
    std::ifstream hf(std::string(argv[1]) + ".bwt.heads", std::ios::binary);
    std::ifstream lf(std::string(argv[1]) + ".bwt.len", std::ios::binary);
    if (!hf || !lf) { fprintf(stderr, "cannot open rlbwt %s.*\n", argv[1]); return 1; }
    std::vector<unsigned char> heads((std::istreambuf_iterator<char>(hf)),
                                     std::istreambuf_iterator<char>());
    const uint64_t R = heads.size();
    std::vector<uint32_t> l(R);
    uint64_t n = 0;
    for (uint64_t i = 0; i < R; ++i) {
        char b[5];
        if (!lf.read(b, 5)) { fprintf(stderr, "short len file\n"); return 1; }
        uint64_t L = 0;
        for (int k = 0; k < 5; ++k) L |= (uint64_t)(unsigned char)b[k] << (8 * k);
        if (L == 0 || L > 0xFFFFFFFFULL) { fprintf(stderr, "bad run len %llu\n", (unsigned long long)L); return 1; }
        l[i] = (uint32_t)L;
        n += L;
    }
    std::vector<uint64_t> C(256, 0);
    uint64_t K = 0;
    for (uint64_t i = 0; i < R; ++i) {
        C[heads[i]] += l[i];
        if (heads[i] == 0x0A) K += l[i];
    }
    uint64_t tot = 0;
    for (int c = 0; c < 256; ++c) { uint64_t t = C[c]; C[c] = tot; tot += t; }
    uint8_t w = 1;
    while (w < 64 && ((uint64_t)1 << w) <= (n ? n - 1 : 1)) ++w;

    FILE* f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    uint32_t magic = 0x52585349, version = 4;
    fwrite(&magic, 4, 1, f); fwrite(&version, 4, 1, f);
    fwrite(&n, 8, 1, f); fwrite(&K, 8, 1, f); fwrite(&R, 8, 1, f);
    fwrite(C.data(), 8, 256, f);
    fwrite(heads.data(), 1, R, f);
    fwrite(l.data(), 4, R, f);
    // sdsl int_vector header: u64 size-in-bits, u8 width, then words LSB-first
    uint64_t bits = R * w;
    fwrite(&bits, 8, 1, f); fwrite(&w, 1, 1, f);
    size_t nwords = (size_t)((bits + 63) / 64);
    std::vector<uint64_t> words(nwords, ~0ULL);   // INF samples
    fwrite(words.data(), 8, nwords, f);
    fclose(f);
    fprintf(stderr, "wrote %s: n=%llu K=%llu R=%llu sa_w=%u (samples INF)\n",
            argv[2], (unsigned long long)n, (unsigned long long)K,
            (unsigned long long)R, (unsigned)w);
    return 0;
}
