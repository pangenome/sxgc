#pragma once
#include "overlap_finish_row_fence.hpp"
#include <cstddef>

// This predicts the *reported logical load geometry* of the unmodified v8
// LcpColumn, not RSS. Only for contiguous maximal runs, no seams, and one
// fresh Band per lceJob. The two coalescers delay two *runs*, not two rows.
namespace rowfence {
template<std::size_t MaxSlices> class Fold {
    const U n, band, slice, limit;
    std::array<U,MaxSlices+1> prefix{}; // cumulative marker+escape bytes at whole SXLS ends (no headers)
    U rows=0,runs=0,last=257,older=0,newer=0,esc=0,closed=0,frontier=0;
    bool pending=false,finished=false;
    U pending_len=0,pending_base=0;
    void observe(U len,U start,U bytes){
        need(start<=bytes,"fold source prefix");U load=len+(bytes-start);
        need(load<=limit,"fold live slice geometry");peak=std::max(peak,load);
    }
    // One load uses [previous_end-1,end), except at rank 0. Exactly the
    // copied slices intersecting that range are still live before retireFront.
    void stage(U end){
        need(frontier<=end && end<=rows,"fold rank frontier");if(frontier==end)return;
        U base=frontier?frontier-1:0,len=end-base,first=base/slice,stop=(end-1)/slice;
        reads+=1+stop-first;staged+=len;++loads;max_slices=std::max(max_slices,1+stop-first);
        U start=prefix[first];if(stop<closed)observe(len,start,prefix[stop+1]);
        else {
            need(stop==closed,"fold ahead of current slice");observe(len,start,rows+16*esc);
            // A piece can close BEFORE its last slice is complete. The band
            // may allocate len now; the reader cannot copy the rest until
            // .lcp.done. At that slice's close check the largest len-start
            // over *all* folds ending here (signed differences via cross sums).
            if(!pending || len+pending_base>pending_len+start){pending_len=len;pending_base=start;}pending=true;
        }
        frontier=end;
    }
public:
    U loads=0,reads=0,staged=0,peak=0,max_slices=0;
    Fold(U rows_expected,U runs_per_piece,U rows_per_slice,U cap):n(rows_expected),band(runs_per_piece),slice(rows_per_slice),limit(cap){
        need(n && n<=(U(1)<<43) && band && slice && (n-1)/slice<MaxSlices,"fold dimensions");
    }
    void add(U b,U h){
        need(!finished && rows<n && b<=256,"fold row extent");
        if(b!=last){
            // This lookahead cuts the preceding full piece BEFORE the sink
            // appends this row to SXLS. older is the start of run r-2.
            if(runs && runs%band==0)stage(runs>2?older:0);
            older=newer;newer=rows;++runs;last=b;
        }
        ++rows;esc+=h>=255;
        if(rows%slice==0 || rows==n){
            ++closed;prefix[closed]=rows+16*esc;
            if(pending)observe(pending_len,pending_base,prefix[closed]);
            pending=false;
        }
    }
    void end(){need(!finished && rows==n && !pending,"incomplete fold");stage(runs>2?older:0);stage(n);finished=true;}
};
}
