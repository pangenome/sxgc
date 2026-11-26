// One trigger scan. External naming sorts finite phrase occurrences, never
// corpus suffixes. Only references leave memory. The ranker consumes parse.u64
// directly; phrases.ref is its lexical alphabet with exact occurrence counts.
#include "pages.hpp"
#include <type_traits>
#include "../rolling/pfp_trigger.hpp"
#include <omp.h>
#include <queue>

namespace pfp {
constexpr U W = xsa_pfp::width, BLOCK = 65536, FETCH = 128;
struct Occ { U pos, stride, ordinal; };
static_assert(sizeof(Occ) == 24);

// Positions/strides are always in *symbols*. Only file access uses bytes.
template<class Symbol> struct Compare {
    Cache left,right;
    Compare(const File& f,U n,U slots):left{f,n,std::vector<Page>(slots)},right{f,n,std::vector<Page>(slots)} {}
    int word(Occ a,Occ b) {
        if (a.pos==b.pos) return 0;
        constexpr U S=sizeof(Symbol); U n=left.n/S;
        bool ae = a.pos+a.stride==n, be = b.pos+b.stride==n;
        U x=S*a.pos,y=S*b.pos,al=a.stride+(ae?0:W),bl=b.stride+(be?0:W);
        for (U rest=S*std::min(al,bl); rest;) {
            U l=std::min({rest,PAGE-x%PAGE,PAGE-y%PAGE});
            auto p=left.at(x), q=right.at(y);
            if constexpr (S==1) { int c=std::memcmp(p,q,l); if (c) return c; }
            else for (U j=0;j<l;j+=S) {
                Symbol v,w; std::memcpy(&v,p+j,S); std::memcpy(&w,q+j,S);
                if (v!=w) return v<w ? -1:1;
            }
            x+=l; y+=l; rest-=l;
        }
        if (al!=bl) return al<bl ? -1:1;
        return int(ae)-int(be); // raw tie: finite word vs same bytes followed by virtual END
    }
    bool before(Occ a,Occ b) {
        int c=word(a,b); return c ? c<0 : a.ordinal<b.ordinal;
    }
};

// The very same scan covers short inputs, overlapping hits, and triggers
// straddling a read. BLOCK bounds I/O only: it is never a phrase-length cap.
template<class Symbol,class Hit> void scan(const File& f,U n,std::array<U,256>& hist,U& max_id,Hit hit) {
    constexpr U S=sizeof(Symbol), chunk=BLOCK/S;
    std::array<Symbol,chunk+W-1> buf;
    U carry=0,start=0,ordinal=0;
    for (U at=0;at<n;) {
        U take=std::min(chunk,n-at);
        f.read(S*at,buf.data()+carry,S*take);
        for (U j=carry;j<carry+take;++j) {
            if constexpr (S==1) ++hist[buf[j]];
            else max_id=std::max(max_id,buf[j]);
        }
        U size=carry+take;
        for (U j=0;j+W<=size;++j) {
            U p=at-carry+j;
            if (p && xsa_pfp::trigger(buf.data()+j)) { hit(Occ{start,p-start,ordinal++}); start=p; }
        }
        carry=std::min(W-1,size); std::memmove(buf.data(),buf.data()+size-carry,S*carry); at+=take;
    }
    hit(Occ{start,n-start,ordinal});
}

// A run is an extent in a single temporary file, or an extent of the final
// in-memory batch. No file descriptor or resident table per input run.
struct Cursor {
    const File* file; const Occ* memory; U at,end,base=0,fill=0;
    std::vector<Occ> buf;
    Cursor(const File* f,const Occ* mem,U lo,U hi):file(f),memory(mem),at(lo),end(hi),buf(f?FETCH:0) { load(); }
    void load() {
        if (file && at!=end && (!fill || at==base+fill)) {
            fill=std::min(FETCH,end-at); base=at; file->read(at*sizeof(Occ),buf.data(),fill*sizeof(Occ));
        }
    }
    const Occ& value() const { return file ? buf[at-base] : memory[at]; }
    bool advance() { ++at; load(); return at<end; }
};
template<class Symbol,class Emit> void merge(const File* file,const Occ* mem,U lo,U end,U stride,Compare<Symbol>& cmp,Emit emit) {
    std::vector<Cursor> runs; runs.reserve(ceildiv(end-lo,stride));
    for (U i=lo;i<end;i+=stride) runs.emplace_back(file,mem,i,std::min(end,i+stride));
    auto after=[&](size_t a,size_t b){ return cmp.before(runs[b].value(),runs[a].value()); };
    std::priority_queue<size_t,std::vector<size_t>,decltype(after)> q(after);
    for (size_t i=0;i<runs.size();++i) q.push(i);
    while (!q.empty()) { auto i=q.top(); q.pop(); emit(runs[i].value()); if (runs[i].advance()) q.push(i); }
}

template<class Symbol> void parse(const fs::path& path,const fs::path& dir,U workers,U memory,U cap) {
    constexpr bool ids_level=std::is_same_v<Symbol,U>;
    File raw(path); U bytes=raw.size(),n=bytes/sizeof(Symbol);
    check(bytes%sizeof(Symbol)==0,"partial input symbol");
    check(n && n < (U(1)<<43),"input must be nonempty and fit row43");
    check(fs::is_directory(dir) && fs::is_empty(dir),"expected fresh empty scratch directory");
    check(workers && workers<=U(omp_get_num_procs()),"invalid worker count");
    U cp=std::min({U(1024),ceildiv(bytes,PAGE),memory/(8*workers*2*sizeof(Page))});
    U ip=std::min({U(4096),ceildiv(8*n,PAGE),memory/(8*sizeof(IdPage))});
    U fan=std::min<U>(32,memory/(8*(FETCH*sizeof(Occ)+128)));
    // Charge even non-overlapping scan and merge buffers. memory is a sort-
    // workspace budget, not a claim about libc/OpenMP or the OS file cache.
    U fixed=BLOCK+(32<<10)+workers*(8192+2*cp*sizeof(Page))+ip*sizeof(IdPage)+fan*(FETCH*sizeof(Occ)+256);
    check(cp && ip && fan>=2 && memory>fixed && (memory-fixed)/sizeof(Occ)>=workers,"insufficient PFP naming workspace");
    U span=std::min(ceildiv(n,workers),(memory-fixed)/sizeof(Occ)/workers),batch=span*workers;
    std::vector<Compare<Symbol>> readers; readers.reserve(workers);
    for (U i=0;i<workers;++i) readers.emplace_back(raw,bytes,cp);
    std::vector<Occ> records; records.reserve(batch);
    std::array<fs::path,2> names{dir/"naming.0",dir/"naming.1"}; int src=0; File spilled;
    Scratch scratch{cap}; U disk_rows=0,m=0,passes=0;
    auto order=[&]{
        U parts=ceildiv(records.size(),span);
        #pragma omp parallel for schedule(static) num_threads(workers) if(parts>1)
        for (U j=0;j<parts;++j) {
            auto& cmp=readers[omp_get_thread_num()]; auto lo=records.begin()+j*span;
            std::sort(lo,lo+std::min<U>(span,records.end()-lo),[&](Occ a,Occ b){return cmp.before(a,b);});
        }
    };
    auto spill=[&]{
        order(); if (spilled.fd<0) spilled=File(names[src],true);
        U bytes=records.size()*sizeof(Occ); scratch.take(bytes);
        spilled.write(disk_rows*sizeof(Occ),records.data(),bytes); disk_rows+=records.size(); records.clear();
    };
    std::array<U,256> hist{}; U max_id=0;
    scan<Symbol>(raw,n,hist,max_id,[&](Occ x){if (records.size()==batch) spill(); records.push_back(x); ++m;});
    if constexpr (!ids_level) check(std::find(hist.begin(),hist.end(),0)!=hist.end(),"all 256 bytes are present: v10 needs an absent END code");
    U original_runs=ceildiv(m,span);
    if (spilled.fd<0) { order(); }
    else {
        spill(); check(disk_rows==m,"incomplete naming runs"); std::vector<Occ>().swap(records);
        while (ceildiv(m,span)>fan) {
            File dest(names[1-src],true); scratch.take(m*sizeof(Occ)); Writer<Occ> out{dest,{}};
            U step=span*fan;
            for (U i=0;i<m;i+=step) merge(&spilled,nullptr,i,std::min(m,i+step),span,readers[0],[&](Occ x){out.put(x);});
            out.flush(); check(out.pos==m*sizeof(Occ),"short merge pass"); scratch.retire(names[src],spilled);
            src=1-src; spilled=std::move(dest); span=step; ++passes;
        }
    }
    scratch.take(8*m+(ids_level?sizeof(Census):sizeof(hist))); File ids(dir/"parse.u64",true),dict(dir/"phrases.ref",true);
    ids.resize(8*m);
    if constexpr (!ids_level) File(dir/"histogram",true).write(0,hist.data(),sizeof(hist));
    Scatter tape{ids,m,std::vector<IdPage>(std::min(ip,ceildiv(8*m,PAGE)))};
    Writer<Phrase> refs{dict,{}}; Occ head{}; U k=0,count=0,owned=0,coverage=0,terms=0,placed=0;
    auto finish_word=[&]{ if (count) { refs.put({head.pos,head.stride,count}); owned+=head.stride; coverage+=head.stride*count; terms+=(head.pos+head.stride==n); } };
    merge(spilled.fd>=0?&spilled:nullptr,records.data(),0,m,span,readers[0],[&](Occ x){
        if (!count || readers[0].word(head,x)) {
            finish_word(); head=x; scratch.take(sizeof(Phrase)); ++k; count=0;
        }
        tape.set(x.ordinal,k-1); ++count; ++placed;
    });
    finish_word(); refs.flush(); tape.finish();
    check(terms==1 && placed==m && coverage==n && refs.pos==k*sizeof(Phrase),"PFP naming coverage");
    if (spilled.fd>=0) scratch.retire(names[src],spilled);
    if constexpr (ids_level) { Census c{n,m,k,owned,max_id}; File(dir/"census",true).write(0,&c,sizeof(c)); }
    std::fprintf(stderr,"%s=%" PRIu64 " trigger_scans=1 occurrences=%" PRIu64 " words=%" PRIu64
        " owned_offsets=%" PRIu64 " initial_phrase_runs=%" PRIu64 " external_merge_passes=%" PRIu64
        " scratch_peak=%" PRIu64 " scratch_final=%" PRIu64 "\n",ids_level?"PFP_ID_PARSE symbols":"PFP_PARSE bytes",n,m,k,owned,original_runs,passes,scratch.peak,scratch.live);
}
} // namespace pfp
int main(int argc,char** argv) try {
    using namespace pfp;
    bool ids=argc==7 && std::string_view(argv[1])=="ids";
    check(argc==6 || ids,"usage: pfp_parse [ids] INPUT EMPTY_DIR WORKERS SORT_BYTES SCRATCH_BYTES");
    if (ids) { ++argv; parse<U>(argv[1],argv[2],number(argv[3]),number(argv[4]),number(argv[5])); }
    else parse<uint8_t>(argv[1],argv[2],number(argv[3]),number(argv[4]),number(argv[5]));
    return 0;
} catch (const std::exception& e) { pfp::fail(e.what()); }
