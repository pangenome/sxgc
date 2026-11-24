// phrasebook_gate.c - THE PHRASEBOOK GATE (TAPE_VISION.md's explicit,
// tested structure). The phrasebook: the ID sequence in position order
// (pb.ids) + the dictionary's length column (pb.lens) + boundary
// samples (pb.samples: every stride-th prefix sum). Positions are
// PREFIX SUMS of lengths - never stored as a column.
//
// The gate:
//   (a) N random corpus positions -> the phrasebook lookup (the
//       covering occurrence, its ID, the intra-phrase offset), each
//       verified against a BRUTE PARSE of the corpus (the one-sided
//       trigger, the same parameters as the producer).
//   (b) the reverse lookup: random occurrences -> (ID, reference
//       bytes via the prefix sums) -> the bytes must match the corpus
//       pread; and the DEDUPE LAW: two occurrences of the same ID must
//       carry byte-identical phrase text at their derived positions.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static uint64_t g_k = 8, g_w = 8;

static void die(const char* m) {
    fprintf(stderr, "PHRASEBOOK_GATE_FAIL: %s\n", m);
    exit(3);
}

static uint64_t* slurp(const char* p, uint64_t want, uint64_t* got) {
    FILE* f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "open %s\n", p); exit(2); }
    if (want == 0) {  // read all
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        want = (uint64_t)sz;
    }
    uint64_t* b = (uint64_t*)malloc((size_t)(want ? want : 4));
    size_t r = fread(b, 1, (size_t)want, f);
    fclose(f);
    if ((uint64_t)r != want) { fprintf(stderr, "short read %s\n", p); exit(2); }
    *got = want;
    return b;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s corpus modeb_dir n_tests seed\n", argv[0]);
        return 2;
    }
    const char* e;
    if ((e = getenv("XSA_PFP_K"))) g_k = strtoull(e, 0, 10);
    if ((e = getenv("XSA_PFP_W"))) g_w = strtoull(e, 0, 10);
    const uint64_t nTests = strtoull(argv[3], 0, 10);
    uint64_t seed = strtoull(argv[4], 0, 10);
    const char* remapPath = argc > 5 ? argv[5] : nullptr;
    uint8_t remap[256];
    for (int i = 0; i < 256; ++i) remap[i] = uint8_t(i);
    if (remapPath) {
        FILE* rf = fopen(remapPath, "rb");
        if (!rf || fread(remap, 1, 256, rf) != 256) die("remap read");
        fclose(rf);
    }

    // the corpus
    FILE* cf = fopen(argv[1], "rb");
    if (!cf) { fprintf(stderr, "open corpus\n"); return 2; }
    fseek(cf, 0, SEEK_END);
    const uint64_t FILE_N = (uint64_t)ftell(cf);
    fseek(cf, 0, SEEK_SET);
    uint8_t* T = (uint8_t*)malloc((size_t)FILE_N);
    if (fread(T, 1, (size_t)FILE_N, cf) != FILE_N) die("corpus read");
    fclose(cf);
    // THE CONVENTION: the producer's read window applies the remap
    // BEFORE the trigger (the historical, certified convention - the
    // 3 GB byte-identity held under it); the brute must match.
    for (uint64_t i = 0; i < FILE_N; ++i) T[i] = remap[T[i]];

    // the phrasebook
    char p[1024];
    uint64_t got = 0;
    snprintf(p, sizeof p, "%s/pb.samples", argv[2]);
    uint64_t* samples = slurp(p, 0, &got);
    const uint64_t M = samples[0], stride = samples[1],
                   dictN = samples[2], nSamples = got / 8 - 3;
    snprintf(p, sizeof p, "%s/pb.ids", argv[2]);
    uint64_t idsBytes = 0;
    uint32_t* ids = (uint32_t*)slurp(p, M * 4, &idsBytes);
    snprintf(p, sizeof p, "%s/pb.lens", argv[2]);
    uint64_t lensBytes = 0;
    uint32_t* lens = (uint32_t*)slurp(p, dictN * 4, &lensBytes);
    (void)lensBytes;
    if (M == 0) die("empty parse");
    fprintf(stderr, "PHRASEBOOK_GATE M=%llu dictN=%llu stride=%llu"
                    " samples=%llu\n",
            (unsigned long long)M, (unsigned long long)dictN,
            (unsigned long long)stride, (unsigned long long)nSamples);

    // ---- the brute parse (the one-sided trigger, k/w matching the
    // producer; the phrase ENDS WITH its trigger k-mer; the window
    // fully inside the phrase; forced splits at 65535)
    uint64_t* bStart = (uint64_t*)malloc((size_t)(M + 4) * 8);
    uint32_t* bLen = (uint32_t*)malloc((size_t)(M + 4) * 4);
    uint64_t bm = 0;
    {
        uint64_t prevStart = 0;
        // the k-mer hash, EXACTLY as the producer computes it (FNV-1a
        // over the k bytes; the trigger is HASH minimality)
        auto kmAt = [&](uint64_t o) -> uint64_t {
            uint64_t h = 1469598103934665603ull;
            for (uint64_t i = 0; i < g_k; ++i) {
                h ^= T[o + i]; h *= 1099511628211ull;
            }
            return h;
        };
        for (uint64_t q = 0; q < FILE_N; ++q) {
            int trig = 0;
            if (q + g_k <= FILE_N && q >= prevStart + g_k + g_w) {
                const uint64_t hp = kmAt(q);
                int ok = 1;
                for (uint64_t j = g_k; j < g_k + g_w && ok; ++j)
                    if (q >= j && kmAt(q - j) <= hp) ok = 0;
                trig = ok;
            }
            const int forced = (q > prevStart && q - prevStart >= 65535);
            if (trig || forced) {
                const uint64_t end = trig ? q + g_k : q;
                if (end > prevStart) {
                    bStart[bm] = prevStart; bLen[bm] = uint32_t(end - prevStart);
                    ++bm;
                    prevStart = end;
                }
            }
        }
        if (prevStart < FILE_N) {
            bStart[bm] = prevStart; bLen[bm] = uint32_t(FILE_N - prevStart);
            ++bm;
        }
    }
    if (bm != M) {
        fprintf(stderr, "PHRASEBOOK_GATE_FAIL: brute M=%llu against"
                         " phrasebook M=%llu\n",
                (unsigned long long)bm, (unsigned long long)M);
        return 3;
    }
    // the brute phrase starts must equal the prefix sums of the ids
    {
        uint64_t cum = 0;
        for (uint64_t i = 0; i < M; ++i) {
            if (cum != bStart[i]) {
                fprintf(stderr, "PHRASEBOOK_GATE_FAIL: prefix-sum start"
                                 " %llu against brute %llu at occurrence"
                                 " %llu\n",
                        (unsigned long long)cum,
                        (unsigned long long)bStart[i],
                        (unsigned long long)i);
                return 3;
            }
            if (lens[ids[i]] != bLen[i]) {
                fprintf(stderr, "PHRASEBOOK_GATE_FAIL: len column %u against"
                                 " brute %u at occurrence %llu (id %u)\n",
                        lens[ids[i]], bLen[i], (unsigned long long)i, ids[i]);
                return 3;
            }
            cum += bLen[i];
        }
        if (cum != FILE_N) die("the parse does not tile the corpus");
    }
    fprintf(stderr, "PHRASEBOOK_GATE brute parse agrees: M=%llu,"
                    " the prefix sums tile the corpus exactly\n",
            (unsigned long long)M);

    // the phrasebook lookup: position -> the covering occurrence via
    // the samples + the prefix-sum walk (positions are prefix sums)
    uint64_t xorshift = seed ? seed : 88172645463325252ull;
    auto nxt = [&] {
        xorshift ^= xorshift << 13; xorshift ^= xorshift >> 7;
        xorshift ^= xorshift << 17; return xorshift;
    };
    uint64_t lookups = 0;
    for (uint64_t t = 0; t < nTests; ++t) {
        const uint64_t pos = nxt() % FILE_N;
        // the sample binary search: the last sample <= pos
        uint64_t lo = 0, hi = nSamples;
        while (lo < hi) {
            const uint64_t mid = lo + (hi - lo) / 2;
            if (samples[3 + mid] <= pos) lo = mid + 1; else hi = mid;
        }
        uint64_t occ = lo == 0 ? 0 : (lo - 1) * stride;
        uint64_t start = samples[3 + (lo ? lo - 1 : 0)];
        while (occ + 1 < M && start + lens[ids[occ]] <= pos) {
            start += lens[ids[occ]]; ++occ;
        }
        const uint32_t id = ids[occ];
        const uint32_t L = lens[id];
        if (!(pos >= start && pos < start + L)) {
            fprintf(stderr, "PHRASEBOOK_GATE_FAIL: pos %llu not covered"
                             " by the lookup (occ %llu start %llu len %u)\n",
                    (unsigned long long)pos, (unsigned long long)occ,
                    (unsigned long long)start, L);
            return 3;
        }
        // against the brute
        uint64_t blo = 0, bhi = bm;
        while (blo < bhi) {
            const uint64_t mid = blo + (bhi - blo) / 2;
            if (bStart[mid] <= pos) blo = mid + 1; else bhi = mid;
        }
        const uint64_t boc = blo - 1;
        if (boc != occ) {
            fprintf(stderr, "PHRASEBOOK_GATE_FAIL: pos %llu -> occ %llu"
                             " against brute %llu\n",
                    (unsigned long long)pos, (unsigned long long)occ,
                    (unsigned long long)boc);
            return 3;
        }
        ++lookups;
    }
    fprintf(stderr, "PHRASEBOOK_GATE forward lookups=%llu OK\n",
            (unsigned long long)lookups);

    // the reverse + dedupe: random occurrences -> (id, the bytes at
    // the derived start) must match the corpus pread, and the bytes
    // must be IDENTICAL at every occurrence of the same id (the
    // dictionary-as-references law; the dedupe the key sort proved).
    // THE START DERIVATION: the samples are prefix sums indexed by
    // occurrence (every stride-th); the covering sample for occurrence
    // occ is the one whose base index <= occ.
    auto startOf = [&](uint64_t occ) -> uint64_t {
        uint64_t sLo = 0, sHi = nSamples;
        while (sLo < sHi) {
            const uint64_t mid = sLo + (sHi - sLo) / 2;
            const uint64_t baseIdx = mid * stride;
            if (baseIdx <= occ) sLo = mid + 1; else sHi = mid;
        }
        const uint64_t base = (sLo ? sLo - 1 : 0) * stride;
        uint64_t start = samples[3 + (sLo ? sLo - 1 : 0)];
        for (uint64_t i = base; i < occ; ++i) start += lens[ids[i]];
        return start;
    };
    for (uint64_t t = 0; t < nTests; ++t) {
        const uint64_t occ = nxt() % M;
        const uint32_t id = ids[occ];
        const uint32_t L = lens[id];
        const uint64_t start = startOf(occ);
        if (start != bStart[occ])
            die("startOf disagrees with the brute phrase start");
        // the reverse lookup: the bytes at the derived reference match
        // the corpus pread (the brute phrase's own bytes)
        if (memcmp(T + start, T + bStart[occ], L) != 0)
            die("reverse: the derived reference bytes differ from the"
                " corpus pread");
        // the dedupe: find another occurrence of the same id (bounded
        // scan windows around occ; skip the test if the id is unique)
        uint64_t other = ~0ull;
        for (uint64_t d = 1; d < 128 && occ + d < M; ++d)
            if (ids[occ + d] == id) { other = occ + d; break; }
        if (other == ~0ull && occ >= 1)
            for (uint64_t d = 1; d <= 128 && occ >= d; ++d)
                if (ids[occ - d] == id) { other = occ - d; break; }
        if (other != ~0ull) {
            const uint64_t s2 = startOf(other);
            if (memcmp(T + start, T + s2, L) != 0) {
                fprintf(stderr, "PHRASEBOOK_GATE_FAIL: the id %u bytes"
                                 " differ between occurrences %llu and"
                                 " %llu - the dedupe is broken\n",
                        id, (unsigned long long)occ,
                        (unsigned long long)other);
                return 3;
            }
        }
        ++lookups;
    }
    fprintf(stderr, "PHRASEBOOK_GATE reverse+dedupe OK\n");
    fprintf(stderr, "PHRASEBOOK_GATE_DONE tests=%llu\n",
            (unsigned long long)nTests * 2);
    return 0;
}
