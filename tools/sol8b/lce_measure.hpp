// Included only in the generated measurement translation unit.
#include <cstdint>
#include <unordered_map>
#include <atomic>
struct Sol8bTail { uint64_t comparisons=0, symbols=0; };
static std::unordered_map<uint64_t,Sol8bTail> sol8bTails[3];
static std::atomic<bool> sol8bEnabled{false};
template<class K> struct Sol8bGuard {
    const K* keys; uint64_t p,q,before; bool enabled;
    Sol8bGuard(const K* k,uint64_t a,uint64_t b):keys(k),p(a),q(b),before(k->bud->symbols),enabled(sol8bEnabled.load(std::memory_order_relaxed)){}
    ~Sol8bGuard() {
        if(!enabled)return;
        uint64_t a=0,b=0;
        // Dollar-mode only: short finite suffixes cannot share a full d prefix.
        for(unsigned j=0;j<8;++j) {
            if(p+j>=keys->n||q+j>=keys->n)break;
            a=(a<<8)|keys->m2->at(p+j);b=(b<<8)|keys->m2->at(q+j);
            if(a!=b)break;
            unsigned d=j+1;
            if(d==4||d==6||d==8) {
                unsigned index=d==4?0:d==6?1:2;
                auto& tail=sol8bTails[index][a];++tail.comparisons;
                tail.symbols+=keys->bud->symbols-before-d;
            }
        }
    }
};
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
// A measurement of literal independent lower_bounds, on REPAIRED chunk lists.
// Corpus-position sampling chooses keys only; no sample position is retained.
template<class P,class K,class S>
static void sol8b_boundary_price(const P& pos,const K& keys,const S& sides) {
    for(unsigned d:{4,6,8}) {
        if(keys.n<d)continue;
        std::vector<uint64_t> targets;
        for(uint64_t j=0;j<1024;++j) {
            uint64_t p=(j*982451653ULL+314159ULL)%(keys.n-d+1),word=0;
            for(unsigned k=0;k<d;++k)word=(word<<8)|keys.m2->at(p+k);
            targets.push_back(word);
        }
        std::sort(targets.begin(),targets.end());targets.erase(std::unique(targets.begin(),targets.end()),targets.end());
        // Key order models the requested ascending boundary walk; each bound
        // still independently searches its chunk's complete row range.
        uint64_t probes=0,bytes=0,checksum=0;
        auto start=std::chrono::steady_clock::now();
        for(uint64_t target:targets)for(size_t side=0;side<pos.size();++side) {
            uint64_t lo=0,hi=sides[side].n;
            while(lo<hi) {
                uint64_t mid=lo+(hi-lo)/2,p=pos[side]->at(mid),word=0;++probes;
                unsigned length=std::min<uint64_t>(d,keys.n-p);
                for(unsigned j=0;j<d;++j){word=(word<<8)|(j<length?keys.m2->at(p+j):0);bytes+=j<length;}
                // Finite end precedes any extension, including NUL. The
                // target is always a full-width key.
                bool less=word<target||(word==target&&length<d);
                if(less)lo=mid+1;else hi=mid;
            }
            checksum+=lo;
        }
        double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        std::printf("{\"measurement\":\"independent_boundary_search\",\"d\":%u,\"sample_unique_keys\":%zu,\"chunks\":%zu,\"searches\":%zu,\"position_probes\":%llu,\"prefix_bytes\":%llu,\"seconds\":%.9f,\"checksum\":%llu}\n",d,targets.size(),pos.size(),targets.size()*pos.size(),(unsigned long long)probes,(unsigned long long)bytes,seconds,(unsigned long long)checksum);
        std::fflush(stdout);
    }
}
