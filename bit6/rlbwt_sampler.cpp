// rlbwt_sampler.cpp — sxgc grl-route (rung 4): SA samples at run ends from a
// grlBWT BCR BWT, via per-string LF walks. Zero text access: needs only the
// RLBWT (grlbwt2rle .syms/.len) + the revlines sidecar (string count, lengths,
// forward-flat starts).
//
// Semantics (validated vs brute force on a 30-string collection, 712/712 run
// ends): BWT positions 0..k-1 are the bare-sentinel suffixes in string order;
// walking LF from position i visits every suffix of string i at its exact BWT
// position, j = len..0. A run-end position is sampled as
//   sa_sample[r] = fstart_i + (len_i - 1 - j)   (forward-flat suffix start)
// so the query reports forward positions as pos = sa_sample - m + 1.
//
// Output .ri4 (v4): "SXRI" u32, version=4, n u64 (BWT length incl sentinels),
// k u64 (strings), R u64, C[256] u64, run_char[R] u8, run_len[R] u32,
// sa_sample[R] u64 (packed int_vector bits as v3 when width < 64).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <atomic>
#include <sdsl/int_vector.hpp>

struct Runs {
    std::vector<unsigned char> a;   // run symbols
    std::vector<uint32_t> l;       // run lengths
    std::vector<uint64_t> starts;  // BWT offset of each run
    uint64_t n = 0;                // total BWT length
    uint64_t R = 0;
};

static void die(const char* msg) { fprintf(stderr, "rlbwt_sampler: %s\n", msg); exit(1); }

int main(int argc, char** argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <prefix from grlbwt2rle> <sidecar.tsv> <out.ri4> [threads]\n", argv[0]);
        return 1;
    }
    std::string prefix = argv[1];
    std::string sidecar = argv[2];
    std::string out = argv[3];
    int nthreads = argc > 4 ? atoi(argv[4]) : (int)std::thread::hardware_concurrency();
    if (nthreads < 1) nthreads = 1;

    // --- load runs
    Runs R;
    {
        FILE* f = fopen((prefix + ".syms").c_str(), "rb");
        if (!f) die("cannot open .syms");
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        R.a.resize(sz);
        if (fread(R.a.data(), 1, sz, f) != (size_t)sz) die("short read .syms");
        fclose(f);
        R.R = R.a.size();
        f = fopen((prefix + ".len").c_str(), "rb");
        if (!f) die("cannot open .len");
        R.l.resize(R.R);
        if (fread(R.l.data(), 4, R.R, f) != R.R) die("short read .len");
        fclose(f);
        R.starts.resize(R.R);
        uint64_t s = 0;
        for (uint64_t r = 0; r < R.R; ++r) { R.starts[r] = s; s += R.l[r]; }
        R.n = s;
    }

    // --- sidecar: cname \t fstart \t len  (string index = row order = line order)
    std::vector<uint64_t> fstart, slen;
    {
        FILE* f = fopen(sidecar.c_str(), "r");
        if (!f) die("cannot open sidecar");
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            uint64_t fs, ln;
            char* tab1 = strchr(line, '\t'); if (!tab1) die("sidecar format");
            char* tab2 = strchr(tab1 + 1, '\t'); if (!tab2) die("sidecar format");
            fs = strtoull(tab1 + 1, nullptr, 10);
            ln = strtoull(tab2 + 1, nullptr, 10);
            fstart.push_back(fs); slen.push_back(ln);
        }
        fclose(f);
    }
    const uint64_t k = fstart.size();
    if (k == 0) die("empty sidecar");
    if (k > R.n) die("more strings than BWT positions");

    // --- derived rank structures (same layout as rindex_query)
    static const int SIG = 256;
    std::vector<std::vector<uint32_t>> cruns(SIG);
    std::vector<std::vector<uint64_t>> csum(SIG);
    std::vector<uint64_t> total(SIG, 0);
    {
        std::vector<uint64_t> acc(SIG, 0);
        for (uint64_t r = 0; r < R.R; ++r) {
            unsigned char c = R.a[r];
            cruns[c].push_back((uint32_t)r);
            csum[c].push_back(acc[c]);
            acc[c] += R.l[r];
        }
        for (int c = 0; c < SIG; ++c) { total[c] = acc[c]; csum[c].push_back(total[c]); }
    }
    std::vector<uint64_t> C(SIG, 0);
    {
        uint64_t cum = 0;
        for (int c = 0; c < SIG; ++c) { C[c] = cum; cum += total[c]; }
    }

    // rank/LF over the runs (const after setup; thread-safe)
    auto run_of = [&](uint64_t i) -> uint64_t {
        uint64_t lo = 0, hi = R.R - 1;
        while (lo < hi) { uint64_t mid = (lo + hi + 1) / 2; if (R.starts[mid] <= i) lo = mid; else hi = mid - 1; }
        return lo;
    };
    auto rank = [&](unsigned char c, uint64_t i) -> uint64_t {
        if (i == 0) return 0;
        if (i >= R.n) return total[c];
        const auto& v = cruns[c];
        if (v.empty()) return 0;
        // last c-run with start < i (manual binary search)
        uint64_t j = 0, hi2 = v.size();
        while (j < hi2) { uint64_t mid = (j + hi2) / 2; if (R.starts[v[mid]] < i) j = mid + 1; else hi2 = mid; }
        if (j == 0) return 0;
        uint64_t r = v[j - 1];
        uint64_t within = i - R.starts[r];
        if (within > R.l[r]) within = R.l[r];
        return csum[c][j - 1] + within;
    };
    auto LF = [&](uint64_t p) -> uint64_t {
        uint64_t r = run_of(p);
        unsigned char c = R.a[r];
        return C[c] + rank(c, p);
    };

    // --- parallel per-string walks; sample run ends
    std::vector<uint64_t> sa_sample(R.R, 0);
    std::vector<unsigned char> sampled(R.R, 0);
    std::atomic<uint64_t> done{0};
    std::atomic<bool> fail{false};

    auto worker = [&](int tid) {
        const bool dbg = (tid == 0 && getenv("SXGC_SAMPLER_DBG"));
        for (uint64_t i = tid; i < k && !fail.load(); i += nthreads) {
            uint64_t p = i;  // bare-sentinel suffix of string i (string order)
            uint64_t L = slen[i];
            if (dbg) fprintf(stderr, "dbg string %llu: L=%llu fstart=%llu walk:", (unsigned long long)i, (unsigned long long)L, (unsigned long long)fstart[i]);
            for (uint64_t j = L + 1; j-- > 0; ) {  // j = L .. 0
                uint64_t r = run_of(p);
                if (dbg && (L - j) < 10) fprintf(stderr, " %llu", (unsigned long long)p);
                if (p == R.starts[r] + R.l[r] - 1) {  // run end
                    sa_sample[r] = fstart[i] + (L - j);
                    sampled[r] = 1;
                }
                if (j > 0) p = LF(p);
            }
            if (dbg) fprintf(stderr, "\n");
            done.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int t = 1; t < nthreads; ++t) threads.emplace_back(worker, t);
    worker(0);
    for (auto& t : threads) t.join();
    if (fail.load()) die("walk diverged (should not happen — no text check here)");

    uint64_t nsampled = 0;
    for (uint64_t r = 0; r < R.R; ++r) nsampled += sampled[r];
    fprintf(stderr, "walks: %llu strings, %llu positions; sampled %llu/%llu run ends\n",
            (unsigned long long)k, (unsigned long long)R.n,
            (unsigned long long)nsampled, (unsigned long long)R.R);
    if (nsampled != R.R) { fprintf(stderr, "FATAL: unsampled run ends — walk coverage incomplete\n"); return 1; }

    // --- emit .ri4
    std::ofstream f(out, std::ios::binary);
    if (!f) die("cannot open output");
    uint32_t magic = 0x5258'5349, version = 4;
    f.write((char*)&magic, 4); f.write((char*)&version, 4);
    f.write((char*)&R.n, 8); f.write((char*)&k, 8); f.write((char*)&R.R, 8);
    f.write((char*)C.data(), 256 * 8);
    f.write((char*)R.a.data(), R.R);
    f.write((char*)R.l.data(), 4 * R.R);
    // pack sa samples to bits(n)
    uint8_t w = 1;
    while (w < 64 && ((uint64_t)1 << w) <= (R.n ? R.n - 1 : 1)) ++w;
    sdsl::int_vector<> sa_iv(R.R, 0, w);
    for (uint64_t r = 0; r < R.R; ++r) sa_iv[r] = sa_sample[r];
    sa_iv.serialize(f);
    f.close();
    fprintf(stderr, "wrote %s (R=%llu, sa_w=%u)\n", out.c_str(), (unsigned long long)R.R, w);
    return 0;
}
