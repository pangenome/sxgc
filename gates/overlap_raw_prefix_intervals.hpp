#pragma once
// Experimental equality subroutine for ONE immutable private raw buffer. No
// sharing across files. Does not certify adjacency, suffix order, END or rank.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
namespace prefixproof {
using U=std::uint64_t;
inline void require(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
inline U mix(U x){x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;x=(x^(x>>27))*0x94d049bb133111ebull;return x^(x>>31);}
struct Counts {U requests=0,asked=0,compared=0,skipped=0,lookups=0,full_hits=0,evictions=0;};
// (delta,lo,hi) proves raw[i]==raw[i+delta] for EVERY i in [lo,hi).
// The hash selects a bucket only. Ranges on a different diagonal prove nothing.
template<unsigned Sets=4096,unsigned Ways=4,U Threshold=1024> class Verified {
    static_assert(Sets && !(Sets&(Sets-1)) && Ways && Ways<=8 && Threshold);
    struct Entry {U d=0,lo=0,hi=0;};
    std::span<const std::uint8_t> raw;
    std::array<std::array<Entry,Ways>,Sets> set{};
    std::array<std::uint8_t,Sets> victim{};
    Counts c;
public:
    static_assert(sizeof(Entry)==24);
    static constexpr U table_bytes=sizeof(set)+sizeof(victim);
    explicit Verified(std::span<const std::uint8_t> bytes):raw(bytes){}
    Verified(const Verified&)=delete;Verified& operator=(const Verified&)=delete;
    const Counts& counts() const{return c;}
    // Check a deadline periodically in long raw comparisons. Short checks and
    // hits take bounded work; the enclosing verifier still checks at row packets.
    template<class Guard> bool equal(U a,U b,U h,Guard guard){
        if(a>b)std::swap(a,b);
        require(b<=raw.size() && h<=raw.size()-b && (!h || a<b),"invalid raw prefix extent");
        require(h<=UINT64_MAX-c.asked && c.requests!=UINT64_MAX,"raw prefix work overflow");++c.requests;c.asked+=h;
        const U d=b-a,end=a+h;auto gap=[&](U lo,U hi){while(lo<hi){U z=std::min<U>(hi-lo,65536);if(z==65536)guard();c.compared+=z;
            if(std::memcmp(raw.data()+lo,raw.data()+lo+d,z))return false;
            lo+=z;}return true;};
        if(h<Threshold)return gap(a,end);
        ++c.lookups;auto index=mix(d)&(Sets-1);auto& slots=set[index];std::array<Entry,Ways> covers;unsigned n=0;
        for(const auto& x:slots)if(x.d==d && x.hi>a && x.lo<end){
            if(x.lo<=a && x.hi>=end){++c.full_hits;c.skipped+=h;return true;}
            // At most Ways intervals. Sort by start, not by recency/hash.
            unsigned j=n++;while(j && covers[j-1].lo>x.lo){covers[j]=covers[j-1];--j;}covers[j]=x;}
        U pos=a,saved=0;
        for(unsigned j=0;j<n && pos<end;++j){U lo=std::max(pos,covers[j].lo),hi=std::min(end,covers[j].hi);if(hi<=lo)continue;
            if(!gap(pos,lo))return false;
            saved+=hi-lo;pos=hi;}
        if(!gap(pos,end))return false;
        c.skipped+=saved;
        // Commit only AFTER all gaps succeed. Previously committed ranges on a
        // diagonal are disjoint/non-touching; union actual overlaps or touching
        // endpoints, never an unchecked gap, even one byte wide.
        U lo=a,hi=end;unsigned dest=Ways;
        for(unsigned j=0;j<Ways;++j){auto& x=slots[j];if(x.d==d && x.hi>=a && x.lo<=end){lo=std::min(lo,x.lo);hi=std::max(hi,x.hi);x.d=0;dest=j;}
            else if(!x.d && dest==Ways)dest=j;}
        if(dest==Ways){dest=victim[index];victim[index]=static_cast<std::uint8_t>((dest+1)%Ways);++c.evictions;}
        slots[dest]={d,lo,hi};return true;
    }
};
}
