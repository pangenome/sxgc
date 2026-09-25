// sa_decode.cpp — decode the saV4 run-end sample convention of a .ri4 v4
// against the flat text, using only .ri4-internal structure (LF from runs).
//   Test A: saV4[r] as plain stream position: flat[saV4[r]-1] == a[r]?
//   Test B: LF-relation: run r with end row e; run r' contains LF(e):
//           d = saV4[r'] - saV4[r]; plain -> -1 (LF decrements position),
//           reversed (c_i - X) -> +1; string boundaries excepted.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <fstream>

static uint64_t rd64(FILE* f) { uint64_t v; fread(&v, 8, 1, f); return v; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <ri4> <flat> [probes]\n", argv[0]); return 1; }
    const char* ri4Path = argv[1];
    const char* flatPath = argv[2];
    uint64_t probes = argc > 3 ? strtoull(argv[3], nullptr, 10) : 4096;

    FILE* f = fopen(ri4Path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", ri4Path); return 1; }
    uint32_t magic, version; uint64_t n, k, R;
    fread(&magic, 4, 1, f); fread(&version, 4, 1, f);
    if (magic != 0x52585349u || version != 4) { fprintf(stderr, "bad ri4 header\n"); return 1; }
    n = rd64(f); k = rd64(f); R = rd64(f);
    std::vector<uint64_t> C(256); fread(C.data(), 8, 256, f);
    std::vector<uint8_t> a(R); fread(a.data(), 1, R, f);
    std::vector<uint32_t> l(R); fread(l.data(), 4, R, f);
    uint64_t bits = rd64(f); uint8_t width;
    fread(&width, 1, 1, f);
    if (width == 0 || width > 64 || bits != R * width) {
        fprintf(stderr, "bad sa array (bits=%llu width=%u R=%llu)\n",
                (unsigned long long)bits, (unsigned)width, (unsigned long long)R);
        return 1;
    }
    size_t nwords = (size_t)((bits + 63) / 64);
    std::vector<uint64_t> words(nwords);
    fread(words.data(), 8, nwords, f);
    fclose(f);
    auto sa_at = [&](uint64_t r) -> uint64_t {
        uint64_t bitpos = r * width, w = bitpos >> 6, off = bitpos & 63;
        uint64_t v = (words[w] >> off);
        if (off + width > 64) v |= (words[w + 1] << (64 - off));
        return v & ((width == 64) ? ~0ULL : ((1ULL << width) - 1));
    };
    fprintf(stderr, "ri4: n=%llu k=%llu R=%llu sa_w=%u\n",
            (unsigned long long)n, (unsigned long long)k, (unsigned long long)R, (unsigned)width);

    // run starts + LF support
    std::vector<uint64_t> starts(R);
    { uint64_t acc = 0; for (uint64_t i = 0; i < R; ++i) { starts[i] = acc; acc += l[i]; } }
    std::vector<uint64_t> Cless(256, 0), Ctot(256, 0);
    for (uint64_t i = 0; i < R; ++i) Ctot[a[i]] += l[i];
    { uint64_t acc = 0; for (int c = 0; c < 256; ++c) { Cless[c] = acc; acc += Ctot[c]; } }
    std::vector<std::vector<uint32_t>> charRuns(256);
    std::vector<std::vector<uint64_t>> charSum(256);
    for (uint64_t i = 0; i < R; ++i) charRuns[a[i]].push_back((uint32_t)i);
    for (int c = 0; c < 256; ++c) {
        if (charRuns[c].empty()) continue;
        charSum[c].resize(charRuns[c].size() + 1, 0);
        for (size_t t = 0; t < charRuns[c].size(); ++t)
            charSum[c][t + 1] = charSum[c][t] + l[charRuns[c][t]];
    }
    auto run_of = [&](uint64_t row) -> uint64_t {
        return (uint64_t)(std::upper_bound(starts.begin(), starts.end(), row) - starts.begin() - 1);
    };
    auto lf_row = [&](uint64_t row) -> uint64_t {
        uint64_t r = run_of(row);
        uint8_t ch = a[r];
        uint64_t o = row - starts[r];
        auto& vr = charRuns[ch];
        uint64_t kk = (uint64_t)(std::lower_bound(vr.begin(), vr.end(), (uint32_t)r) - vr.begin());
        return Cless[ch] + charSum[ch][kk] + o;
    };

    std::ifstream flat(flatPath, std::ios::binary);
    if (!flat) { fprintf(stderr, "cannot open flat %s\n", flatPath); return 1; }
    auto flat_at = [&](uint64_t p) -> int {
        if (p == 0) return -1;
        flat.seekg((std::streamoff)(p - 1));
        char c; flat.read(&c, 1);
        return (uint8_t)c;
    };

    // ---- Test A: plain-position hypothesis ----
    uint64_t aOk = 0, aBad = 0, aSkip = 0, aCap = 16;
    // ---- Test B: LF-relation (sign histogram) ----
    uint64_t bPlus = 0, bMinus = 0, bOther = 0, bSkip = 0;
    for (uint64_t t = 0; t < probes; ++t) {
        uint64_t r = (t + 0.5) * R / probes;
        uint64_t e = starts[r] + l[r] - 1;           // end row of run r
        if (l[r] == 0) continue;
        // Test A
        uint64_t s = sa_at(r);
        int c = s ? flat_at(s) : -1;
        if (c < 0 || a[r] == 0x0A || (uint8_t)c == 0x0A) aSkip++;
        else if ((uint8_t)c == a[r]) aOk++;
        else {
            aBad++;
            if (aCap--) {
                char c1 = 0, c2 = 0;
                if (s >= 2) { flat.seekg((std::streamoff)(s - 2)); flat.read(&c1, 1); }
                flat.seekg((std::streamoff)s); flat.read(&c2, 1);
                fprintf(stderr, "A-bad run=%llu sa=%llu a=%02x flat[sa-1]=%02x "
                        "flat[sa-2]=%02x flat[sa]=%02x\n",
                        (unsigned long long)r, (unsigned long long)s, a[r],
                        (uint8_t)c, (uint8_t)c1, (uint8_t)c2);
            }
        }
        // Test B
        if (e + 1 < n) {
            uint64_t lf = lf_row(e);
            uint64_t rp = run_of(lf);
            if (rp != r) {
                int64_t d = (int64_t)sa_at(rp) - (int64_t)sa_at(r);
                if (d == -1) bMinus++;
                else if (d == +1) bPlus++;
                else bOther++;
            }
        }
    }
    fprintf(stderr, "Test A (plain pos): ok=%llu bad=%llu skip=%llu\n",
            (unsigned long long)aOk, (unsigned long long)aBad, (unsigned long long)aSkip);
    fprintf(stderr, "Test B (LF-relation): minus=%llu plus=%llu other=%llu\n",
            (unsigned long long)bMinus, (unsigned long long)bPlus, (unsigned long long)bOther);
    return 0;
}
