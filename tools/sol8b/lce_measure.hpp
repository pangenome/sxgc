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
