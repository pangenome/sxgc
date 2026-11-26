// One trigger scan. External naming sorts finite phrase occurrences, never
// corpus suffixes. Only references leave memory. The ranker consumes parse.u64
// directly; phrases.ref is its lexical alphabet with exact occurrence counts.
#include "pages.hpp"
#include <sys/mman.h>
#include <type_traits>
#include "../rolling/pfp_trigger.hpp"
#include <omp.h>
#include <queue>

namespace pfp {
constexpr U W = xsa_pfp::width, BLOCK = 65536, FETCH = 128;
// One phrase occurrence. k1/k2 are the first two eight-byte words of the
// finite window [pos, pos+stride+W) (the first two symbols at an ID level),
// big-endian and zero-padded past the window or past the end of the file. A
// proper prefix therefore compares smaller unless the longer window continues
// with zero bytes, and that tie falls through to the exact comparator.
struct Occ { U pos, stride, ordinal, k1, k2; };
static_assert(sizeof(Occ) == 40);

// A read-only mapping of the whole input. A text comparison is a memory read,
// not a page-cache syscall per operand: at 10 GB the paged reader spent 1,041 s
// of the 1,128 s naming phase in pread. Reads never pass the mapping: bytes
// beyond the file are zero on both sides, which is exactly the earlier
// convention, so the comparison stays byte-identical.
struct Map {
    void* ptr=MAP_FAILED; U size=0;
    Map()=default; Map(const Map&)=delete;
    ~Map(){ if(ptr!=MAP_FAILED) munmap(ptr,size); }
    void open(const File& f,U bytes){ size=bytes; ptr=mmap(nullptr,size,PROT_READ,MAP_PRIVATE,f.fd,0); check(ptr!=MAP_FAILED,"map input"); }
    const uint8_t* bytes()const{return (const uint8_t*)ptr;}
};

// Positions/strides are always in *symbols*. Only the map offset uses bytes.
template<class Symbol> struct Compare {
    const uint8_t* text; U bytes;
    Compare(const uint8_t* p,U n):text(p),bytes(n) {}
    // The order of a phrase is the order of its finite window [pos, pos+stride+W).
    // Two different key words decide that order with no text access at all; equal
    // keys fall back to the exact comparison. The key is a filter, never the
    // definition.
    int word(Occ a,Occ b) {
        if (a.pos==b.pos) return 0;
        constexpr U BAD=~U(0);
        if (a.k1!=BAD && b.k1!=BAD) {
            if (a.k1!=b.k1) return a.k1<b.k1 ? -1:1;
            if (a.k2!=b.k2) return a.k2<b.k2 ? -1:1;
        }
        constexpr U S=sizeof(Symbol); U n=bytes/S;
        bool ae = a.pos+a.stride==n, be = b.pos+b.stride==n;
        U pa=S*a.pos,pb=S*b.pos,al=a.stride+(ae?0:W),bl=b.stride+(be?0:W);
        U la=pa<bytes?bytes-pa:0, lb=pb<bytes?bytes-pb:0;
        U inside=std::min({S*std::min(al,bl),la,lb});
        if (inside) {
            if constexpr (S==1) { int c=std::memcmp(text+pa,text+pb,inside); if (c) return c; }
            else for (U j=0;j<inside;j+=S) {
                Symbol v,w; std::memcpy(&v,text+pa+j,S); std::memcpy(&w,text+pb+j,S);
                if (v!=w) return v<w ? -1:1;
            }
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
    U carry=0,start=0,ordinal=0,base=0,size=0;
    // One sequential pass supplies the sort key from the phrase's own window.
    // The rare phrase that begins before the resident block (longer than one I/O
    // block) is read directly. Bytes past the window or past the file end are
    // zero, exactly as the page cache presents them to the exact comparator.
    auto key=[&](U pos,U al)->std::pair<U,U>{
        // A window that runs past the file end is left unkeyed: the page cache
        // presents those bytes from a reused page, so only the exact comparator
        // is authoritative there (a handful of phrases at most).
        if (al>n-pos) return {~U(0),~U(0)};
        // Two key words: 16 byte symbols, or two whole symbols at an ID level
        // (a symbol is a u64 there, never a byte of its representation).
        constexpr U KS=2*W/S;
        U take=std::min<U>({KS,al,n-pos}), off=pos-base, k1=0, k2=0;
        std::array<uint8_t,2*W> t{};
        const uint8_t* b=nullptr;
        if (pos>=base && pos+take<=base+size) {
            if constexpr (S==8) { k1=take>=1?U(buf[off]):0; k2=take>=2?U(buf[off+1]):0; return {k1,k2}; }
            b=(const uint8_t*)(buf.data()+off);
        } else {
            if (take) f.read(pos*S,t.data(),S*take);
            if constexpr (S==8) { Symbol v0{},v1{}; std::memcpy(&v0,t.data(),8); std::memcpy(&v1,t.data()+8,8);
                return {take>=1?U(v0):0, take>=2?U(v1):0}; }
            b=t.data();
        }
        // Big-endian and left-aligned: a short window's bytes keep their place
        // in the high bits and the rest are zero.
        U n1=std::min<U>(W,take), n2=take>W?take-W:0;
        for (U j=0;j<n1;++j) k1=(k1<<8)|b[j];
        for (U j=0;j<n2;++j) k2=(k2<<8)|b[W+j];
        return {k1<<(8*(W-n1)), k2<<(8*(W-n2))};
    };
    auto occ=[&](U pos,U stride,U ord,bool terminal){
        auto [k1,k2]=key(pos,stride+(terminal?0:W)); return Occ{pos,stride,ord,k1,k2};
    };
    for (U at=0;at<n;) {
        U take=std::min(chunk,n-at);
        f.read(S*at,buf.data()+carry,S*take);
        for (U j=carry;j<carry+take;++j) {
            if constexpr (S==1) ++hist[buf[j]];
            else max_id=std::max(max_id,buf[j]);
        }
        size=carry+take; base=at-carry;
        for (U j=0;j+W<=size;++j) {
            U p=base+j;
            if (p && xsa_pfp::trigger(buf.data()+j)) { hit(occ(start,p-start,ordinal++,false)); start=p; }
        }
        carry=std::min(W-1,size); std::memmove(buf.data(),buf.data()+size-carry,S*carry); at+=take;
    }
    hit(occ(start,n-start,ordinal,true));
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
    // The naming scatter is a random permutation write of the whole ID tape.
    // Size the page slab from the grant (and the tape) instead of a 16 MiB cap:
    // a cap smaller than the tape turns every few records into a pread/pwrite.
    U ip=std::min(ceildiv(8*n,PAGE),memory/(8*sizeof(IdPage)));
    U fan=std::min<U>(32,memory/(8*(FETCH*sizeof(Occ)+128)));
    // Charge even non-overlapping scan and merge buffers. memory is a sort-
    // workspace budget, not a claim about libc/OpenMP or the OS file cache.
    U fixed=bytes+BLOCK+(32<<10)+workers*(8192+2*sizeof(Page))+ip*sizeof(IdPage)+fan*(FETCH*sizeof(Occ)+256);
    check(ip && fan>=2 && memory>fixed && (memory-fixed)/sizeof(Occ)/2>=workers,"insufficient PFP naming workspace");
    U span=std::min(ceildiv(n,workers),(memory-fixed)/sizeof(Occ)/workers/2),batch=span*workers;
    std::vector<Compare<Symbol>> readers; readers.reserve(workers);
    Map mapped; mapped.open(raw,bytes);
    for (U i=0;i<workers;++i) readers.emplace_back(mapped.bytes(),bytes);
    std::vector<Occ> records; records.reserve(batch);
    std::unique_ptr<Occ[]> sorted; U sorted_cap=0;   // merge target, sized to the resident chunk
    std::array<fs::path,2> names{dir/"naming.0",dir/"naming.1"}; int src=0; File spilled;
    Scratch scratch{cap}; U disk_rows=0,m=0,passes=0,sort_calls=0; double sort_s=0,part_s=0,level_s=0,merge_s=0,scan_s=0,name_s=0;
    auto order=[&]{
        double started=omp_get_wtime(); ++sort_calls;
        // Sort this whole resident chunk in parallel, then merge pairs level by
        // level (each pair independent). One sorted run per chunk, so the naming
        // merge never re-reads the text for a comparison the key already settles.
        U count=records.size(), parts=std::min<U>(4*workers,count), step=ceildiv(count,parts);
        if (sorted_cap<count) { sorted=std::make_unique<Occ[]>(count); sorted_cap=count; }
        double mid=omp_get_wtime();
        #pragma omp parallel for schedule(static) num_threads(workers) if(parts>1)
        for (U j=0;j<parts;++j) {
            auto& cmp=readers[omp_get_thread_num()]; auto lo=records.begin()+j*step;
            std::sort(lo,lo+std::min<U>(step,records.end()-lo),[&](Occ a,Occ b){return cmp.before(a,b);});
        }
        part_s+=omp_get_wtime()-mid; mid=omp_get_wtime();
        // Level merge into the second buffer, then back: linear moves, never the
        // rotation-based in-place merge (which dominated this phase at 1 GB).
        Occ* src=records.data(); Occ* dst=sorted.get();
        for (U width=step;width<count;width*=2) {
            U pairs=ceildiv(count,2*width);
            #pragma omp parallel for schedule(static) num_threads(workers) if(pairs>1)
            for (U k=0;k<pairs;++k) {
                auto& cmp=readers[omp_get_thread_num()];
                U lo=k*2*width, mi=lo+std::min(width,count-lo), hi=mi+std::min(width,count-mi);
                std::merge(src+lo,src+mi,src+mi,src+hi,dst+lo,[&](Occ a,Occ b){return cmp.before(a,b);});
            }
            std::swap(src,dst);
        }
        if (src!=records.data()) std::memcpy(records.data(),src,count*sizeof(Occ));
        level_s+=omp_get_wtime()-mid; sort_s+=omp_get_wtime()-started;
    };
    auto spill=[&]{
        order(); if (spilled.fd<0) spilled=File(names[src],true);
        U bytes=records.size()*sizeof(Occ); scratch.take(bytes);
        spilled.write(disk_rows*sizeof(Occ),records.data(),bytes); disk_rows+=records.size(); records.clear();
    };
    std::array<U,256> hist{}; U max_id=0;
    double t0=omp_get_wtime();
    scan<Symbol>(raw,n,hist,max_id,[&](Occ x){if (records.size()==batch) spill(); records.push_back(x); ++m;});
    scan_s=omp_get_wtime()-t0;
    if constexpr (!ids_level) check(std::find(hist.begin(),hist.end(),0)!=hist.end(),"all 256 bytes are present: v10 needs an absent END code");
    U original_runs=ceildiv(m,batch),run=batch;
    if (spilled.fd<0) { order(); run=m; }
    else {
        spill(); check(disk_rows==m,"incomplete naming runs"); std::vector<Occ>().swap(records);
        t0=omp_get_wtime();
        while (ceildiv(m,run)>fan) {
            File dest(names[1-src],true); scratch.take(m*sizeof(Occ)); Writer<Occ> out{dest,{}};
            U step=run*fan;
            for (U i=0;i<m;i+=step) merge(&spilled,nullptr,i,std::min(m,i+step),run,readers[0],[&](Occ x){out.put(x);});
            out.flush(); check(out.pos==m*sizeof(Occ),"short merge pass"); scratch.retire(names[src],spilled);
            src=1-src; spilled=std::move(dest); run=step; ++passes;
        }
        merge_s+=omp_get_wtime()-t0;
    }
    scratch.take(8*m+(ids_level?sizeof(Census):sizeof(hist))); File ids(dir/"parse.u64",true),dict(dir/"phrases.ref",true);
    ids.resize(8*m);
    if constexpr (!ids_level) File(dir/"histogram",true).write(0,hist.data(),sizeof(hist));
    Scatter tape{ids,m,std::vector<IdPage>(std::min(ip,ceildiv(8*m,PAGE)))};
    Writer<Phrase> refs{dict,{}}; Occ head{}; U k=0,count=0,owned=0,coverage=0,terms=0,placed=0;
    auto finish_word=[&]{ if (count) { refs.put({head.pos,head.stride,count}); owned+=head.stride; coverage+=head.stride*count; terms+=(head.pos+head.stride==n); } };
    t0=omp_get_wtime();
    merge(spilled.fd>=0?&spilled:nullptr,records.data(),0,m,run,readers[0],[&](Occ x){
        if (!count || readers[0].word(head,x)) {
            finish_word(); head=x; scratch.take(sizeof(Phrase)); ++k; count=0;
        }
        tape.set(x.ordinal,k-1); ++count; ++placed;
    });
    finish_word(); refs.flush(); tape.finish();
    name_s=omp_get_wtime()-t0;
    check(terms==1 && placed==m && coverage==n && refs.pos==k*sizeof(Phrase),"PFP naming coverage");
    if (spilled.fd>=0) scratch.retire(names[src],spilled);
    if constexpr (ids_level) { Census c{n,m,k,owned,max_id}; File(dir/"census",true).write(0,&c,sizeof(c)); }
    std::fprintf(stderr,"%s=%" PRIu64 " trigger_scans=1 occurrences=%" PRIu64 " words=%" PRIu64
        " owned_offsets=%" PRIu64 " initial_phrase_runs=%" PRIu64 " external_merge_passes=%" PRIu64
        " sort_calls=%" PRIu64 " scan_s=%.6f sort_s=%.6f(part=%.6f level=%.6f) merge_s=%.6f name_s=%.6f"
        " scratch_peak=%" PRIu64 " scratch_final=%" PRIu64 "\n",ids_level?"PFP_ID_PARSE symbols":"PFP_PARSE bytes",n,m,k,owned,original_runs,passes,sort_calls,scan_s,sort_s,part_s,level_s,merge_s,name_s,scratch.peak,scratch.live);
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
