// kgram_survey - the tape design's planning survey.
//
// TWO MEASUREMENTS, both from sequential passes over the corpus (no sorting,
// no positions materialized, no scratch):
//
//   (A) EXACT prefix counts for k = 1, 2, 3 bytes. One sequential read, dense
//       histograms (256 / 64K / 16.7M counters). At the measured 6.89 GB/s this
//       is ~16 s over 107 GB and ~190 s over the 1.31 TB pile. This is the
//       window-plan input: it says exactly which byte(-pair/-triple) ranges are
//       fat, hence how a target-size window split must fall.
//
//   (B) SAMPLED group-size shape for deeper k (default 4, 8, 12, 16 bytes).
//       Every `stride`-th text position contributes one k-byte key; keys are
//       counted in a flat open-addressing table keyed by a 64-bit hash
//       (collision probability ~ N^2/2^65, negligible at these N). The result
//       answers the tape design's one real unknown: after taking radix levels
//       to depth k, HOW BIG are the groups that remain, i.e. how much of the
//       corpus sits in ties that a k-byte key cannot separate. Groups are
//       estimated from a 1-in-stride sample, which is accurate for exactly the
//       large groups that matter (Binomial(G, 1/stride)).
//
// Output is line-oriented KEY=VALUE so results can be filed in the ledger
// verbatim.
//
//   kgram_survey --corpus C [--exact 1] [--ks 4,8,12,16] [--samples 50000000]
//                [--top 12] [--leaf 1048576] [--primes 1000000,10000000,100000000]
//
// The cyclic-vs-sentinel convention does not matter here: keys are read as
// bytes AFTER the position, zero-padded past EOF, which matches how a tape scan
// sees the end of the material.

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
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
using u32 = uint32_t;
using u64 = uint64_t;
using u128 = unsigned __int128;

static void die(const char* m) {
    std::fprintf(stderr, "kgram_survey: %s\n", m);
    std::exit(2);
}

static inline u64 splitmix(u64 x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Flat open-addressing counter table: (hash, count), power-of-two capacity.
struct HashTable {
    std::vector<u64> klo, khi;
    std::vector<u32> cnt;
    u64 mask = 0;
    u64 used = 0;

    explicit HashTable(u64 cap_pow2) {
        klo.assign(cap_pow2, 0);
        khi.assign(cap_pow2, 0);
        cnt.assign(cap_pow2, 0);
        mask = cap_pow2 - 1;
    }
    // Empty is cnt==0, NOT a sentinel key: forcing a bit of the hash (an
    // earlier `h|1`) makes the key non-injective and silently merges pairs.
    inline void add(u64 lo, u64 hi) {
        u64 i = splitmix(lo ^ splitmix(hi)) & mask;
        for (;;) {
            if (cnt[i] == 0) {
                klo[i] = lo; khi[i] = hi;
                cnt[i] = 1;
                ++used;
                return;
            }
            if (klo[i] == lo && khi[i] == hi) {
                if (cnt[i] != 0xffffffffu) ++cnt[i];
                return;
            }
            i = (i + 1) & mask;
        }
    }
};

struct KV {
    u64 count;
    u8 bytes[16];
};

int main(int argc, char** argv) {
    std::string corpus;
    int do_exact = 1;
    std::vector<int> ks = {4, 8, 12, 16};
    u64 samples = 50000000ull;
    u64 topN = 12;
    u64 leaf = 1ull << 20;
    std::vector<u64> primes = {1000000ull, 10000000ull, 100000000ull};

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() { if (i + 1 >= argc) die("missing value"); return std::string(argv[++i]); };
        if (a == "--corpus") corpus = nxt();
        else if (a == "--exact") do_exact = std::atoi(nxt().c_str());
        else if (a == "--samples") samples = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--top") topN = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--leaf") leaf = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--ks") {
            ks.clear();
            std::string s = nxt();
            size_t p = 0;
            while (p < s.size()) {
                size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                ks.push_back(std::atoi(s.substr(p, q - p).c_str()));
                p = q + 1;
            }
        } else if (a == "--primes") {
            primes.clear();
            std::string s = nxt();
            size_t p = 0;
            while (p < s.size()) {
                size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                primes.push_back(std::strtoull(s.substr(p, q - p).c_str(), nullptr, 10));
                p = q + 1;
            }
        } else die(("unknown arg " + a).c_str());
    }
    if (corpus.empty()) die("--corpus required");

    int fd = ::open(corpus.c_str(), O_RDONLY);
    if (fd < 0) die("cannot open corpus");
    struct stat st{};
    if (::fstat(fd, &st) != 0) die("cannot stat corpus");
    u64 n = (u64)st.st_size;
    // MAP_POPULATE off: we WANT the kernel's readahead, not a pre-fault storm.
    void* base = ::mmap(nullptr, (size_t)n, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) die("mmap failed");
    const u8* T = (const u8*)base;

    std::fprintf(stderr, "KGRAM_SURVEY_START corpus=%s n=%" PRIu64 "\n", corpus.c_str(), n);

    // ---------------- (A) exact k = 1,2,3 ----------------
    if (do_exact) {
        std::vector<u64> h1(256, 0);
        std::vector<u64> h2(1ull << 16, 0);
        std::vector<u64> h3(1ull << 24, 0);
        for (u64 i = 0; i < n; ++i) {
            u32 c0 = T[i];
            h1[c0]++;
            if (i + 1 < n) h2[(c0 << 8) | T[i + 1]]++;
            if (i + 2 < n) h3[(c0 << 16) | ((u32)T[i + 1] << 8) | T[i + 2]]++;
        }
        auto report = [&](std::vector<u64>& h, int k, const char* tag) {
            u64 total = 0, live = 0;
            for (u64 v : h) { total += v; if (v) ++live; }
            std::vector<u32> idx;
            idx.reserve(live);
            for (u32 i = 0; i < h.size(); ++i) if (h[i]) idx.push_back(i);
            std::sort(idx.begin(), idx.end(), [&](u32 a, u32 b) { return h[a] > h[b]; });
            std::printf("EXACT_K%d distinct=%" PRIu64 " positions=%" PRIu64 "\n", k, live, total);
            u64 shown = std::min<u64>(topN, idx.size());
            for (u64 j = 0; j < shown; ++j) {
                u32 i = idx[j];
                char lit[3 * 8 + 1] = {0};
                int p = 0;
                for (int b = k - 1; b >= 0; --b) {
                    u8 ch = (u8)((i >> (8 * b)) & 0xff);
                    if (ch >= 32 && ch < 127 && ch != '%' && ch != '\\') lit[p++] = (char)ch;
                    else p += std::snprintf(lit + p, 8, "\\x%02x", ch);
                }
                std::printf("EXACT_K%d %s=%u count=%" PRIu64 " pct=%.3f\n", k, tag, i, h[i],
                            100.0 * (double)h[i] / (double)total);
            }
            // oversize ranges at this depth (for the window plan)
            u64 over = 0, overpos = 0;
            for (u64 j = 0; j < idx.size(); ++j)
                if (h[idx[j]] > leaf) { ++over; overpos += h[idx[j]]; }
            std::printf("EXACT_K%d groups_over_leaf(leaf=%" PRIu64 ")=%" PRIu64
                        " positions_in_them=%" PRIu64 " pct=%.3f\n",
                        k, leaf, over, overpos, 100.0 * (double)overpos / (double)total);
        };
        report(h1, 1, "byte");
        report(h2, 2, "pair");
        report(h3, 3, "triple");
    }

    // ---------------- (B) sampled group shape for deeper k ----------------
    for (int k : ks) {
        if (k < 4 || k > 16) die("k must be in [4,16]");
        u64 stride = std::max<u64>(1, n / std::max<u64>(1, samples));
        u64 nsamp = n / stride;
        u64 cap = 1;
        while (cap < nsamp * 2 + 1024) cap <<= 1;
        HashTable H(cap);
        u64 pos = 0;
        for (; pos + (u64)k <= n; pos += stride) {
            u64 lo = 0, hi = 0;
            std::memcpy(&lo, T + pos, 8);
            std::memcpy(&hi, T + pos + 8, 8);
            // NESTING CONTRACT: the k-byte key must be a true prefix of the
            // (k+1)-byte key, or the group-size curve is meaningless. hi MUST
            // be zeroed for k<=8 (the first version forgot, so k=4 and k=8
            // measured 16-byte keys).
            if (k <= 8) {
                hi = 0;
                if (k < 8) lo &= (1ull << (8 * k)) - 1;
            } else {
                if (k < 16) hi &= (1ull << (8 * (k - 8))) - 1;
            }
            H.add(lo, hi);
        }
        // size histogram of sampled groups, and the implied full-corpus sizes
        std::vector<u64> sizes;
        std::vector<u32> idx;
        sizes.reserve(H.used);
        idx.reserve(H.used);
        u64 maxc = 0;
        for (u64 i = 0; i < H.cnt.size(); ++i)
            if (H.cnt[i]) { sizes.push_back(H.cnt[i]); idx.push_back((u32)i); if (H.cnt[i] > maxc) maxc = H.cnt[i]; }
        std::sort(idx.begin(), idx.end(), [&](u32 a, u32 b) { return H.cnt[a] > H.cnt[b]; });
        std::sort(sizes.begin(), sizes.end(), std::greater<u64>());
        u64 sampled_positions = 0;
        for (u64 v : sizes) sampled_positions += v;
        std::printf("SHAPE k=%d stride=%" PRIu64 " samples=%" PRIu64 " distinct_groups=%" PRIu64
                    " largest_sampled=%" PRIu64 " largest_implied=%" PRIu64 "\n",
                    k, stride, sampled_positions, H.used, maxc, maxc * stride);
        {
            u64 shown = std::min<u64>(topN, idx.size());
            for (u64 j = 0; j < shown; ++j) {
                u32 t = idx[j];
                u8 bytes[16];
                std::memcpy(bytes, &H.klo[t], 8);
                std::memcpy(bytes + 8, &H.khi[t], 8);
                char lit[8 * 16 + 1];
                int p = 0;
                for (int b = 0; b < k; ++b) {
                    u8 ch = bytes[b];
                    if (ch >= 32 && ch < 127 && ch != '%' && ch != '\\') lit[p++] = (char)ch;
                    else p += std::snprintf(lit + p, 8, "\\x%02x", ch);
                }
                lit[p] = 0;
                // is it a run of ONE repeated byte? then the tie is resolved by
                // run length alone - no text comparison needed.
                bool uniform = true;
                for (int b = 1; b < k; ++b) if (bytes[b] != bytes[0]) uniform = false;
                std::printf("GROUP k=%d rank=%" PRIu64 " count=%u implied=%" PRIu64
                            " pct=%.4f uniform_byte=%d key=\"%s\"\\n",
                            k, j, H.cnt[t], (u64)H.cnt[t] * stride,
                            100.0 * (double)((u64)H.cnt[t] * stride) / (double)n,
                            uniform ? 1 : 0, lit);
            }
        }
        // "how much of the corpus sits in groups bigger than X"
        for (u64 P : primes) {
            u64 thr_sample = std::max<u64>(1, P / stride);  // X in sampled units
            u64 pos_sample = 0, groups = 0;
            for (u64 v : sizes) if (v >= thr_sample) { pos_sample += v; ++groups; }
            std::printf("SHAPE k=%d group_gt=%" PRIu64 " groups=%" PRIu64
                        " implied_positions=%" PRIu64 " pct=%.4f\n",
                        k, P, groups, pos_sample * stride,
                        100.0 * (double)(pos_sample * stride) / (double)n);
        }
        // the tape design's go/no-go: does a k-byte key reach leaf-sized groups?
        {
            u64 thr_sample = std::max<u64>(1, leaf / stride);
            u64 pos_sample = 0, groups = 0;
            for (u64 v : sizes) if (v >= thr_sample) { pos_sample += v; ++groups; }
            // HOW MUCH OF THE TIE TAIL IS RUNS? A group whose k-byte key is one
            // repeated byte is a run of >= k; ordering it needs only run LENGTH,
            // never a text comparison. That is the tape design's cheapest tail.
            u64 uni_sample = 0, uni_groups = 0, nonuni_sample = 0;
            for (u64 t = 0; t < H.cnt.size(); ++t) {
                if (!H.cnt[t] || H.cnt[t] < thr_sample) continue;
                u8 bytes[16];
                std::memcpy(bytes, &H.klo[t], 8);
                std::memcpy(bytes + 8, &H.khi[t], 8);
                bool uni = true;
                for (int b = 1; b < k; ++b) if (bytes[b] != bytes[0]) uni = false;
                if (uni) { uni_sample += H.cnt[t]; ++uni_groups; }
                else nonuni_sample += H.cnt[t];
            }
            std::printf("SHAPE k=%d leaf=%" PRIu64 " groups_at_or_above_leaf=%" PRIu64
                        " implied_positions=%" PRIu64 " pct=%.4f"
                        " tail_uniform_groups=%" PRIu64 " tail_uniform_pct=%.4f"
                        " tail_nonuniform_pct=%.4f\n",
                        k, leaf, groups, pos_sample * stride,
                        100.0 * (double)(pos_sample * stride) / (double)n,
                        uni_groups,
                        100.0 * (double)(uni_sample * stride) / (double)n,
                        100.0 * (double)(nonuni_sample * stride) / (double)n);
        }
    }

    ::munmap(base, (size_t)n);
    ::close(fd);
    std::fprintf(stderr, "KGRAM_SURVEY_DONE\n");
    return 0;
}
