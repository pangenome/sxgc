// reader_bench.cpp — A/B the AGC random-access readers on identical workloads.
//
//   ./reader_bench <archive.agc> <sidecar.names.tsv> [nfetch] [win_kb] [mode]
//     mode: ragc | agcpp | agcpp-prefetch
//
// Workload: uniform-random 64-KiB window fetches over the contigs named in the
// sidecar (the same distribution the sA oracle's window cache produces).
// Warmup 50 fetches, then timed. Reports ms/fetch and the effective MB/s.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <random>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>

#ifdef SXGC_AGCPP_BACKEND
#include <agc-api.h>
#endif

struct Row { std::string cname; uint64_t off; uint64_t len; };

static std::vector<Row> load_sidecar(const char* path)
{
    std::vector<Row> rows;
    std::ifstream f(path);
    std::string line;
    while(std::getline(f, line))
    {
        if(line.empty()) continue;
        if(!line.empty() && line.back()=='\r') line.pop_back();
        auto t1 = line.find('\t'); if(t1 == std::string::npos) continue;
        auto t2 = line.find('\t', t1+1); if(t2 == std::string::npos) continue;
        rows.push_back({line.substr(0, t1),
                        std::stoull(line.substr(t1+1, t2-t1-1)),
                        std::stoull(line.substr(t2+1))});
    }
    return rows;
}

// ----- ragc FFI backend -----
typedef void* agc_handle;
typedef agc_handle (*fn_open)(const char*);
typedef int (*fn_range)(agc_handle, const char*, uint64_t, uint64_t, unsigned char*);
typedef void (*fn_close)(agc_handle);

int main(int argc, char** argv)
{
    if(argc < 3){ printf("usage: %s <archive.agc> <sidecar.tsv> [nfetch=2000] [win_kb=64] [mode=ragc]\n", argv[0]); return 1; }
    const char* archive = argv[1];
    const char* sidecar = argv[2];
    long nfetch = argc > 3 ? atol(argv[3]) : 2000;
    size_t win = argc > 4 ? (size_t)atol(argv[4]) * 1024 : 64 * 1024;
    std::string mode = argc > 5 ? argv[5] : "ragc";

    auto rows = load_sidecar(sidecar);
    if(rows.empty()){ fprintf(stderr, "no sidecar rows\n"); return 1; }
    // keep only contigs large enough for a full window
    std::vector<Row> big;
    for(auto& r : rows) if(r.len >= win) big.push_back(r);
    printf("contigs: %zu usable (%zu total), window: %zu KiB, fetches: %ld, mode: %s\n",
           big.size(), rows.size(), win/1024, nfetch, mode.c_str());
    if(big.empty()) return 1;

    std::vector<unsigned char> buf(win);
    std::mt19937_64 rng(12345);

#ifdef SXGC_AGCPP_BACKEND
    if(mode.rfind("agcpp", 0) == 0)
    {
        CAGCFile agc;
        bool pf = (mode == "agcpp-prefetch");
        if(!agc.Open(archive, pf)){ fprintf(stderr, "open failed\n"); return 1; }
        // contig name -> sample map (some names repeat across samples)
        std::vector<std::string> samples;
        agc.ListSample(samples);
        std::unordered_map<std::string,std::string> cname2sample;
        for(auto& s : samples)
        {
            std::vector<std::string> ctgs;
            agc.ListCtg(s, ctgs);
            for(auto& c : ctgs) cname2sample[c] = s;
        }
        printf("samples: %zu, mapped contigs: %zu\n", samples.size(), cname2sample.size());
        // warmup
        for(int i = 0; i < 50; ++i)
        {
            Row& r = big[rng() % big.size()];
            uint64_t s0 = rng() % (r.len - win + 1);
            std::string seq;
            agc.GetCtgSeq(cname2sample[r.cname], r.cname, (int)s0, (int)(s0 + win - 1), seq);
        }
        auto t0 = std::chrono::high_resolution_clock::now();
        uint64_t sum = 0;
        for(long i = 0; i < nfetch; ++i)
        {
            Row& r = big[rng() % big.size()];
            uint64_t s0 = rng() % (r.len - win + 1);
            std::string seq;
            agc.GetCtgSeq(cname2sample[r.cname], r.cname, (int)s0, (int)(s0 + win - 1), seq);
            sum += (uint64_t)seq.size();
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("agcpp(%s): total %.1f ms, %.3f ms/fetch, %.1f MiB/s, sum=%llu\n",
               mode.c_str(), ms, ms / nfetch, (double)nfetch * win / 1048576.0 / (ms / 1000.0),
               (unsigned long long)sum);
        agc.Close();
        return 0;
    }
#endif

    // ragc FFI backend
    void* lib = dlopen("/home/erikg/sxgc/ragc-ffi/target/release/libragc_ffi.so", RTLD_NOW);
    if(!lib){ fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    fn_open fo = (fn_open)dlsym(lib, "sxgc_agc_open");
    fn_range fr = (fn_range)dlsym(lib, "sxgc_agc_range");
    fn_close fc = (fn_close)dlsym(lib, "sxgc_agc_close");
    agc_handle h = fo(archive);
    if(!h){ fprintf(stderr, "sxgc_agc_open failed\n"); return 1; }
    for(int i = 0; i < 50; ++i)
    {
        Row& r = big[rng() % big.size()];
        uint64_t s0 = rng() % (r.len - win + 1);
        fr(h, r.cname.c_str(), s0, s0 + win, buf.data());
    }
    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t sum = 0;
    for(long i = 0; i < nfetch; ++i)
    {
        Row& r = big[rng() % big.size()];
        uint64_t s0 = rng() % (r.len - win + 1);
        int got = fr(h, r.cname.c_str(), s0, s0 + win, buf.data());
        sum += (uint64_t)got;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("ragc: total %.1f ms, %.3f ms/fetch, %.1f MiB/s, sum=%llu\n",
           ms, ms / nfetch, (double)nfetch * win / 1048576.0 / (ms / 1000.0),
           (unsigned long long)sum);
    fc(h);
    return 0;
}
