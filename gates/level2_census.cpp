// level2_census.cpp - THE STREAMING LEVEL-2 CENSUS (TAPE_VISION.md):
// parse the parse - the same one-sided trigger over ID k-mers,
// doc-contained - and MEASURE the condensation yield: the level-2
// dictionary size and the phrase lengths. THE CENSUS DECIDES THE LEVEL,
// once per corpus, by number (the blade never decides anything).
//
// usage: level2_census corpus remap.bin modeb_dir [k] [w]
// The doc boundaries: the level-1 phrases whose corpus span contains a
// 0x1E byte (the pile's document separator); the level-2 parse units
// are the spans between them (the NO-CROSS law at every level).
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <utility>

static void die(const char* m) {
    std::fprintf(stderr, "LEVEL2_CENSUS_FAIL: %s\n", m);
    std::exit(3);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: %s corpus remap.bin modeb_dir [k] [w]\n", argv[0]);
        return 2;
    }
    const uint64_t K = argc > 4 ? strtoull(argv[4], 0, 10) : 8;
    const uint64_t W = argc > 5 ? strtoull(argv[5], 0, 10) : 8;

    // the corpus (for the doc boundaries + the byte accounting)
    FILE* cf = std::fopen(argv[1], "rb");
    if (!cf) die("open corpus");
    std::fseek(cf, 0, SEEK_END);
    const uint64_t FILE_N = uint64_t(std::ftell(cf));
    std::fseek(cf, 0, SEEK_SET);
    uint8_t* T = (uint8_t*)malloc(size_t(FILE_N));
    if (std::fread(T, 1, size_t(FILE_N), cf) != FILE_N) die("corpus read");
    std::fclose(cf);
    uint8_t remap[256];
    for (int i = 0; i < 256; ++i) remap[i] = uint8_t(i);
    {
        FILE* rf = std::fopen(argv[2], "rb");
        if (!rf || std::fread(remap, 1, 256, rf) != 256) die("remap read");
        std::fclose(rf);
    }

    char p[1024];
    uint64_t M = 0, stride = 0, dictN = 0, nSamples = 0;
    uint64_t* samples = nullptr;
    uint32_t* ids = nullptr;
    uint32_t* lens = nullptr;
    {
        snprintf(p, sizeof p, "%s/pb.samples", argv[3]);
        FILE* f = std::fopen(p, "rb");
        if (!f) die("open pb.samples");
        uint64_t hdr[3];
        if (std::fread(hdr, 8, 3, f) != 3) die("samples header");
        M = hdr[0]; stride = hdr[1]; dictN = hdr[2];
        std::fseek(f, 0, SEEK_END);
        nSamples = uint64_t(std::ftell(f)) / 8 - 3;
        std::fseek(f, 24, SEEK_SET);
        samples = (uint64_t*)malloc(size_t((nSamples + 1) * 8));
        if (std::fread(samples, 8, nSamples, f) != nSamples) die("samples");
        std::fclose(f);
        snprintf(p, sizeof p, "%s/pb.ids", argv[3]);
        f = std::fopen(p, "rb");
        ids = (uint32_t*)malloc(size_t(M * 4));
        if (std::fread(ids, 4, M, f) != M) die("ids");
        std::fclose(f);
        snprintf(p, sizeof p, "%s/pb.lens", argv[3]);
        f = std::fopen(p, "rb");
        lens = (uint32_t*)malloc(size_t(dictN * 4));
        if (std::fread(lens, 4, dictN, f) != dictN) die("lens");
        std::fclose(f);
    }
    std::fprintf(stderr, "LEVEL2_CENSUS M=%llu dictN=%llu\n",
                 (unsigned long long)M, (unsigned long long)dictN);

    // the phrase start of occurrence i (prefix sums + the samples)
    auto startOf = [&](uint64_t occ) -> uint64_t {
        uint64_t sLo = 0, sHi = nSamples;
        while (sLo < sHi) {
            const uint64_t mid = sLo + (sHi - sLo) / 2;
            if (mid * stride <= occ) sLo = mid + 1; else sHi = mid;
        }
        const uint64_t base = (sLo ? sLo - 1 : 0) * stride;
        uint64_t start = samples[sLo ? sLo - 1 : 0];
        for (uint64_t i = base; i < occ; ++i) start += lens[ids[i]];
        return start;
    };

    // the doc boundaries: the level-1 phrases whose span contains 0x1E
    // (the pile separator, in the RAW corpus bytes; the level-1 doc
    // ends there - the NO-CROSS law carries to level 2)
    std::vector<uint64_t> docEnd;   // the last occurrence index of each doc
    {
        uint64_t prevPhrase = 0;
        for (uint64_t q = 0; q < FILE_N; ++q) {
            if (T[q] != 0x1E) continue;
            // the covering occurrence
            uint64_t lo = 0, hi = M;
            while (lo < hi) {
                const uint64_t mid = lo + (hi - lo) / 2;
                if (startOf(mid) <= q) lo = mid + 1; else hi = mid;
            }
            const uint64_t occ = lo - 1;
            if (docEnd.empty() || occ != docEnd.back()) docEnd.push_back(occ);
            (void)prevPhrase;
        }
    }
    std::fprintf(stderr, "LEVEL2_CENSUS docs=%llu\n",
                 (unsigned long long)docEnd.size());

    // THE STREAMING LEVEL-2 PARSE: the one-sided trigger over ID
    // k-mers (FNV over the k ids), the window fully inside the
    // level-2 phrase, suppressed in each doc's first w positions,
    // forced splits at 65535 ids. THE MEASUREMENT (the census):
    //   the level-2 phrase count, distinct count (a bounded advisory
    //   cache is fine for the COUNT measurement - the census needs
    //   the distinct count: a sort-based dedupe below), the
    //   dictionary size in ID-entries, the average length.
    const uint64_t L2MAX = 65535;
    uint64_t prevStart = 0, phrases2 = 0, forced2 = 0;
    // the phrase spans (start, len) for the dedupe measurement
    std::vector<std::pair<uint64_t, uint32_t>> ph2;
    auto kmAt = [&](uint64_t o) -> uint64_t {
        uint64_t h = 1469598103934665603ull;
        for (uint64_t i = 0; i < K; ++i) {
            h ^= ids[o + i]; h *= 1099511628211ull;
        }
        return h;
    };
    // the doc boundary lookup: the first doc end >= q
    std::vector<uint64_t>::iterator dIt = docEnd.begin();
    uint64_t docStart = 0;   // the first occurrence index of the doc
    auto inDocWindow = [&](uint64_t q) -> bool {
        // the trigger needs q >= prevStart + K + W AND q inside the doc
        return q + K <= M && q >= prevStart + K + W && q >= docStart + K + W;
    };
    for (uint64_t q = 0; q < M; ++q) {
        // advance the doc boundary: the phrase q-1 ended a doc ->
        // the doc window resets at q
        while (dIt != docEnd.end() && q > *dIt) {
            docStart = *dIt + 1;
            // a phrase ends at the doc boundary: the level-2 parse
            // closes there (the NO-CROSS law)
            if (prevStart < q) {
                ph2.emplace_back(prevStart, uint32_t(q - prevStart));
                ++phrases2;
                prevStart = q;
            }
            ++dIt;
        }
        bool trig = false;
        if (inDocWindow(q)) {
            const uint64_t hp = kmAt(q);
            bool ok = true;
            for (uint64_t j = K; j < K + W && ok; ++j) {
                if (q >= j && q - j >= prevStart && kmAt(q - j) <= hp)
                    ok = false;
            }
            trig = ok;
        }
        const bool forced = (q > prevStart && q - prevStart >= L2MAX);
        if (trig || forced) {
            const uint64_t end = trig ? q + K : q;
            if (end > prevStart) {
                if (forced && !trig) ++forced2;
                ph2.emplace_back(prevStart, uint32_t(end - prevStart));
                ++phrases2;
                prevStart = end;
            }
        }
    }
    if (prevStart < M) {
        ph2.emplace_back(prevStart, uint32_t(M - prevStart));
        ++phrases2;
    }
    // the distinct count: sort the (start, len) by the ID-sequence key
    std::sort(ph2.begin(), ph2.end(),
        [&](const std::pair<uint64_t, uint32_t>& a,
            const std::pair<uint64_t, uint32_t>& b) {
            const uint64_t m = std::min(a.second, b.second);
            const int c = std::memcmp(ids + a.first, ids + b.first,
                                      size_t(m * 4));
            if (c) return c < 0;
            return a.second < b.second;
        });
    uint64_t distinct2 = 0;
    for (uint64_t i = 0; i < ph2.size(); ++i)
        if (i == 0 ||
            ph2[i].second != ph2[i-1].second ||
            std::memcmp(ids + ph2[i].first, ids + ph2[i-1].first,
                        size_t(ph2[i].second * 4)) != 0)
            ++distinct2;
    uint64_t totalLen2 = 0, maxLen2 = 0;
    for (const auto& e : ph2) {
        totalLen2 += e.second;
        if (e.second > maxLen2) maxLen2 = e.second;
    }
    std::fprintf(stderr,
        "SS_LEVEL2_CENSUS level1_phrases=%llu level2_phrases=%llu"
        " level2_distinct=%llu level2_avg_len=%.2f level2_max_len=%llu"
        " level2_dict_ids=%llu condensation=%.3fx docs=%llu"
        " forced=%llu"
        " rule=the_same_trigger_over_id_kmers;doc_contained;the_census"
        "_decides_the_level\n",
        (unsigned long long)M, (unsigned long long)phrases2,
        (unsigned long long)distinct2,
        double(M) / double(std::max<uint64_t>(1, phrases2)),
        (unsigned long long)maxLen2,
        (unsigned long long)totalLen2,
        double(M) / double(std::max<uint64_t>(1, phrases2)),
        (unsigned long long)docEnd.size(),
        (unsigned long long)forced2);
    return 0;
}
