// Independent file-extent accounting for the seam-free frozen v8 Kasai sink.
// No producer or finish headers. Histograms suffice; no encoded payload retained.
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>

namespace rowfence {
using U=uint64_t;
inline void need(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
struct Cost {unsigned mode,width,escapes,bytes;};
class Block {
    std::array<unsigned,65> bits{},ones{};
public:
    unsigned n=0;
    void add(U h){
        need(n<1024,"m14 block already full"); ++n;
        unsigned w=std::bit_width(h); ++bits[w];
        if((h & (h+1))==0)++ones[w]; // includes UINT64_MAX; unsigned wrap intended
    }
    Cost cost() const {
        need(n>0,"empty m14 block"); if(bits[0]==n)return {0,0,0,8};
        unsigned above=n;
        std::array<unsigned,65> gt{};
        for(unsigned i=0;i<65;++i){above-=bits[i];gt[i]=above;}
        unsigned mode=1,width=8,esc=gt[8]+ones[8],best=5+n+10*esc;
        for(unsigned w: {1u,2u,3u,4u,5u,6u,7u,8u,10u,12u,16u,20u,24u,32u}){
            // Choice tests width overflow only. ACTUAL span also escapes the
            // all-ones field. Charging just the minimum choice cost is UNSAFE.
            unsigned est=6+(n*w+7)/8+10*gt[w];
            if(est<best){best=est;mode=2;width=w;esc=gt[w]+ones[w];}
        }
        unsigned extent=(mode==1?5+n:6+(n*width+7)/8)+10*esc;
        return {mode,mode==1?0u:width,esc,(extent+7)/8*8};
    }
};
struct Limits {U runs,slice_extent,lcp_payload;};
struct Fence {
    static constexpr U SLICE=262144;
    Limits cap; Block block; U rows=0,runs=0,last=257,slice_esc=0,slice_peak=0,total_esc=0,
        lcp_payload=0,m14_blocks=0,max_h=0,sum_h=0;
    explicit Fence(Limits lim):cap(lim){}
    void close_block(){auto c=block.cost();need(c.bytes<=cap.lcp_payload-lcp_payload,"m14 payload limit");
        lcp_payload+=c.bytes;++m14_blocks;block={};}
    void add(U b,U h){
        // In this profile the used remap is injective with a distinct END.
        // Each maximal-run head contributes its adjacent raw LCP to m14.
        if(b!=last){need(runs<cap.runs,"BWT run limit");++runs;last=b;block.add(h);if(block.n==1024)close_block();}
        if(rows%SLICE==0)slice_esc=0;
        if(h>=255){++slice_esc;++total_esc;}
        U size=48+(rows%SLICE)+1+16*slice_esc;
        need(size<=cap.slice_extent,"SXLS extent limit");slice_peak=std::max(slice_peak,size);
        need(h<=UINT64_MAX-sum_h,"prefix work overflow");sum_h+=h;max_h=std::max(max_h,h);++rows;
    }
    void end(){if(block.n)close_block();}
};
}
