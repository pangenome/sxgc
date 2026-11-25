#pragma once
// Bounded row-window probe ONLY, not a builder. Three-byte radix masses locate
// complete prefix classes covering true rank intervals; never keep a full SA.
#include "overlap_raw_prefix_intervals.hpp"
#include <limits>
#include <vector>

namespace prefixwindow {
using prefixproof::U;
using prefixproof::require;
using I = std::uint32_t;
using Raw = std::span<const std::uint8_t>;
using Guard = void (*)();
constexpr I Keys = 257u * 257u * 257u;
struct Window { I first, count; }; // also collect rank first-1
struct Cover {
    Window wanted;
    I lo, hi, base, extent;
    std::vector<I> pos;
};
inline I key(Raw t, I p) {
    if (p+2 < t.size()) return ((1u+t[p])*257u + 1u+t[p+1])*257u + 1u+t[p+2];
    I x=0;
    for(unsigned j=0;j<3;++j) x=x*257u+(p+j<t.size()?1u+t[p+j]:0u);
    return x; // END=0 below every byte including NUL=1
}
inline std::vector<Window> ranks(I n) {
    constexpr I w=65536, strata=16;
    require(n==100000503,"fixed real prefix only");
    std::vector<Window> v;
    for(U i=0;i<strata;++i){
        U lo=(n+1ull)*i/strata, hi=(n+1ull)*(i+1)/strata;
        U r=lo+1+prefixproof::mix(0xF6B8D2A4ull+i)%(hi-lo-w);
        v.push_back({I(r),w});
    }
    return v;
}
inline std::vector<Cover> select(Raw t,const std::vector<Window>& plan,I cap,Guard guard){
    require(!t.empty() && t.size()<=100000503 && !plan.empty() && plan.size()<255,"probe extent");
    std::vector<I> cdf(Keys);
    for(I p=0;p<=t.size();++p){if(!(p%524288))guard();++cdf[key(t,p)];}
    I sum=0; for(auto& v:cdf){sum+=v;v=sum;}
    require(sum==t.size()+1,"prefix count coverage");
    std::vector<Cover> bins; U retained=0;
    for(const auto& w:plan){
        require(w.first && w.count && U(w.first)+w.count<=sum,"rank extent");
        auto find=[&](I r){return I(std::upper_bound(cdf.begin(),cdf.end(),r)-cdf.begin());};
        I a=find(w.first-1), b=find(w.first+w.count-1), base=a?cdf[a-1]:0, len=cdf[b]-base;
        require(b<Keys && (bins.empty() || bins.back().hi<a),"overlapping lexical covers");
        retained+=len;require(retained<=cap,"complete prefix cover exceeds position cap before allocation");
        bins.push_back({w,a,b,base,len,{}});
    }
    std::vector<I>().swap(cdf);
    std::vector<std::uint8_t> tag(Keys);
    for(unsigned i=0;i<bins.size();++i){auto& v=bins[i];v.pos.reserve(v.extent);std::fill(tag.begin()+v.lo,tag.begin()+v.hi+1,i+1);}
    for(I p=0;p<=t.size();++p){if(!(p%524288))guard();auto id=tag[key(t,p)];if(id){auto& v=bins[id-1];require(v.pos.size()<v.extent,"cover count changed");v.pos.push_back(p);}}
    for(const auto& v:bins)require(v.pos.size()==v.extent,"incomplete cover");
    return bins;
}
struct Work {
    U bytes=0,limit,next=0;
    Guard guard;
    Work(U cap,Guard g):limit(cap),guard(g){}
    void add(U z){require(z<=limit-bytes,"raw sort/LCP work cap");bytes+=z;if(bytes>=next){guard();next=bytes+(1<<20);}}
};
inline void order(Cover& v,Raw t,Work& work){
    // Full raw suffix order, not just queue order within a three-byte tie.
    std::sort(v.pos.begin(),v.pos.end(),[&](I a,I b){
        if(a==b)return false;
        I left=I(t.size())-std::max(a,b),h=0;
        while(h<left){I z=std::min<I>(h?256:24,left-h);work.add(z);int c=std::memcmp(t.data()+a+h,t.data()+b+h,z);if(c)return c<0;h+=z;}
        return a>b;
    });
}
inline I direct(Raw t,I a,I b,Work& work){
    // Independently discover the FIRST mismatching raw byte, including EOF.
    require(a!=b && std::max(a,b)<=t.size(),"duplicate/bad position");
    I left=I(t.size())-std::max(a,b), h=0;
    while(h<left){I z=std::min<I>(64,left-h);work.add(z);
        if(std::memcmp(t.data()+a+h,t.data()+b+h,z)){
            I j=0;while(j<z && t[a+h+j]==t[b+h+j])++j;
            require(j<z && t[a+h+j]<t[b+h+j],"not strictly ordered raw suffixes");return h+j;
        }
        h+=z;
    }
    require(a+h==t.size(),"wrong shorter suffix");return h;
}
}
