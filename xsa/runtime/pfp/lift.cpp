// P (integer symbols), Q = PFP(P), inverse(Q) -> inverse(P).
// No RAW argument, SA(P) sort, or LCP required. Only dictionary offsets enter
// the finite-key sort. Occurrences are already ordered within each Q word.
#include "pages.hpp"
#include "../rolling/pfp_trigger.hpp"
#include <omp.h>

namespace pfp {
namespace {
constexpr U W=xsa_pfp::width;
struct Word { U start, stride, begin; }; // begin: first zero-based row of Q for this ID
struct Aligned { U start, next; };       // indexed by rank(Q)-1; next=0 is END
struct Offset { U word, off; };
struct Item { Offset at; U first; };    // first ID; reused as a list cursor *after* sorting
struct Cell { U count=0, mass=0, range=0; };
struct Range { U start, count=0, mass=0; };
struct Batch { std::array<Offset,64> data; U used=0, sent=0; };
static_assert(sizeof(Offset)==16 && sizeof(Item)==24);

void fits(U required,U available,const char* what) {
    if (required>available) {
        std::fprintf(stderr,"pfp lift: %s required=%" PRIu64 " available=%" PRIu64 "\n",what,required,available);
        fail("insufficient ID lift budget");
    }
}
template<class T> struct Sequential {
    const File& f; U size; std::array<T,256> buf;
    T get(U i) {
        if (i%buf.size()==0) f.read(i*sizeof(T),buf.data(),std::min<U>(size-i,buf.size())*sizeof(T));
        return buf[i%buf.size()];
    }
};
struct View {
    const Census c;
    std::vector<Word> words;
    std::vector<Aligned> rows;
    View(Census census,const File& refs,const File& q,const File& inv):
        c(census),words(c.k+1),rows(c.m,{c.n,0}) {
        Sequential<Phrase> dict{refs,c.k,{}}; U total=0,owned=0,ends=0;
        for (U i=0;i<c.k;++i) {
            auto [p,s,f]=dict.get(i);
            check(p<c.n && s && s<=c.n-p && f && f<=c.m-total && s<=c.n-owned,"ID dictionary extent");
            if (s==c.n-p) { ++ends; check(f==1,"repeated terminal word"); }
            else check(W<=c.n-p-s,"short ID overlap");
            words[i]={p,s,total}; total+=f; owned+=s;
        }
        check(ends==1 && total==c.m && owned==c.owned,"ID dictionary census");
        words.back()={c.n,0,total};
        // Lexical names + exact frequencies give contiguous intervals. Scatter
        // the occurrence starts into the supplied ranks; do NOT sort them.
        Sequential<U> tape{q,c.m,{}}, rank{inv,c.m,{}}; U p=0,prev=0;
        for (U i=0;i<c.m;++i) {
            U id=tape.get(i),r=rank.get(i); check(id<c.k,"upper ID outside dictionary");
            auto w=words[id];
            check(r>w.begin && r<=words[id+1].begin && rows[r-1].start==c.n,"upper rank outside phrase interval or repeated");
            check(w.stride<=c.n-p && ((w.start+w.stride==c.n)==(i+1==c.m)),"upper tape coverage/END");
            rows[r-1].start=p; if (prev) rows[prev-1].next=r; prev=r; p+=w.stride;
        }
        check(p==c.n,"short upper tape");
    }
    template<class Visit> void offsets(Cache& ids,Visit visit) const {
        for (U id=0;id<c.k;++id) {
            auto w=words[id]; U mass=words[id+1].begin-w.begin;
            for (U off=0;off<w.stride;++off) visit(Offset{id,off},ids.token(w.start+off),mass);
        }
    }
    int compare(const Item& a,const Item& b,Cache& ids) const {
        if (a.first!=b.first) return a.first<b.first?-1:1;
        auto [i,x]=a.at; auto [j,y]=b.at; if (i==j && x==y) return 0;
        auto u=words[i],v=words[j]; bool ue=u.start+u.stride==c.n,ve=v.start+v.stride==c.n;
        U lu=u.stride-x+(ue?0:W),lv=v.stride-y+(ve?0:W);
        // Compare finite references in NUMERIC symbol order. Never read a
        // lower-level continuation here. A strict prefix can end at END.
        for (U h=1,common=std::min(lu,lv);h<common;++h) {
            U left=ids.token(u.start+x+h),right=ids.token(v.start+y+h);
            if (left!=right) return left<right?-1:1;
        }
        if (lu!=lv) return lu<lv?-1:1;
        return int(ue)-int(ve);
    }
    void expand(Item* a,Item* end,Cache& ids,Scatter& out,U& rank,U& shared) const {
        while (a!=end) {
            Item* b=a+1;
            while (b!=end && !compare(*a,*b,ids)) ++b;
            if (b==a+1) {
                // One offset, possibly MANY occurrences, all already in order.
                auto [id,off]=a->at;
                for (U r=words[id].begin;r<words[id+1].begin;++r) out.set(rows[r].start+off,++rank);
            } else {
                // Same representative in different words. Destructively merge
                // just their sorted list heads, not a sort of their occurrences.
                ++shared;
                for (auto t=a;t!=b;++t) t->first=words[t->at.word].begin;
                auto later=[&](const Item& x,const Item& y){return rows[x.first].next>rows[y.first].next;};
                std::make_heap(a,b,later); U last=0;
                for (auto hi=b;hi!=a;) {
                    std::pop_heap(a,hi,later); auto& t=hi[-1]; auto row=rows[t.first];
                    check(row.next>last,"unordered or duplicate continuation rank"); last=row.next;
                    out.set(row.start+t.at.off,++rank);
                    if (++t.first==words[t.at.word+1].begin) --hi;
                    else std::push_heap(a,hi,later);
                }
            }
            a=b;
        }
    }
};

void lift(const fs::path& input,const fs::path& child,const fs::path& dest,U workers,U memory,U cap) {
    File lower(input),q(child/"parse.u64"),refs(child/"phrases.ref"),inv(child/"inverse"),meta(child/"census");
    Census c{}; check(meta.size()==sizeof(c),"bad ID census wire"); meta.read(0,&c,sizeof(c));
    check(c.n && c.n<(U(1)<<43) && c.m && c.m<=c.n && c.k && c.k<=c.m && c.k<=c.owned && c.owned<=c.n,"bad ID census");
    check(lower.size()==8*c.n && q.size()==8*c.m && inv.size()==8*c.m && refs.size()==sizeof(Phrase)*c.k,"ID column lengths");
    check(workers && workers<=U(omp_get_num_procs()) && workers<=64,"invalid lift workers");
    // All live ancestors/child columns have ALREADY been deducted from cap.
    U wire=c.owned*sizeof(Offset); fits(wire+8*c.n,cap,"postings + lower inverse bytes");
    unsigned bits=std::bit_width(c.max_id), cellbits=std::min(10u,bits);
    while (cellbits && (U(1)<<cellbits)*sizeof(Batch)>memory/32) --cellbits;
    U ncells=U(1)<<cellbits;
    auto bin=[&](U id){check(id<=c.max_id,"invalid ID census maximum");return ncells==1?0:id>>(bits-cellbits);};
    // Charge upper navigation, routing/IO controls (incl. maximum #ranges),
    // per-lane buffers, caches and all sort slots BEFORE allocating. This is
    // the array dial, not an OS cache or OpenMP stack limit.
    U base=(64<<10)+workers*(16<<10)+ncells*(sizeof(Cell)+sizeof(Range)+sizeof(Batch))
          +(c.k+1)*sizeof(Word)+c.m*sizeof(Aligned);
    fits(base,memory,"upper navigation + controls"); U room=memory-base, pages=ceildiv(8*c.n,PAGE);
    U cp=std::min(pages,std::max<U>(1,room/(3*workers*sizeof(Page))));
    U ip=std::min(pages,std::max<U>(1,room/(3*sizeof(IdPage))));
    U fixed=base+workers*cp*sizeof(Page)+ip*sizeof(IdPage);
    fits(fixed+sizeof(Item),memory,"page caches + one slot");
    U slots=std::min(c.owned,(memory-fixed)/sizeof(Item)),target=ceildiv(slots,2*workers);
    View view(c,refs,q,inv);
    std::vector<Cache> caches; caches.reserve(workers);
    for (U j=0;j<workers;++j) caches.push_back({lower,8*c.n,std::vector<Page>(cp)});
    std::vector<Cell> cells(ncells); U counted=0,routed=0,loaded=0;
    view.offsets(caches[0],[&](Offset,U id,U f){auto& cell=cells[bin(id)];++cell.count;cell.mass+=f;++counted;});
    std::vector<Range> ranges; ranges.reserve(ncells); U extent=0,mass=0;
    for (auto& cell:cells) if (cell.count) {
        fits(cell.count,slots,"largest ID cell (descriptor slots)");
        if (ranges.empty() || ranges.back().count+cell.count>target) ranges.push_back({extent});
        cell.range=ranges.size()-1; ranges.back().count+=cell.count; ranges.back().mass+=cell.mass;
        extent+=cell.count; mass+=cell.mass;
    }
    check(extent==c.owned && counted==extent && mass==c.n,"ID range census");
    File post(child/"lift.offsets",true);
    {
        std::vector<Batch> batches(ranges.size());
        auto flush=[&](U i){auto& b=batches[i];auto r=ranges[i];check(b.sent+b.used<=r.count,"overfilled ID range");
            post.write((r.start+b.sent)*sizeof(Offset),b.data.data(),b.used*sizeof(Offset));b.sent+=b.used;b.used=0;};
        view.offsets(caches[0],[&](Offset x,U id,U){U i=cells[bin(id)].range;auto& b=batches[i];
            b.data[b.used++]=x;if (b.used==b.data.size()) flush(i);++routed;});
        for (U i=0;i<batches.size();++i) {flush(i);check(batches[i].sent==ranges[i].count,"short ID range");}
    }
    check(routed==extent && post.size()==wire,"ID posting extent");
    File inverse(dest,true); inverse.resize(8*c.n); Scatter out{inverse,c.n,std::vector<IdPage>(ip)};
    U rank=0,shared=0; std::vector<Item> arena(slots); omp_set_dynamic(0);
    for (U lo=0,hi;lo<ranges.size();lo=hi) {
        std::array<U,65> at{};hi=lo;
        while (hi<ranges.size() && hi-lo<workers && ranges[hi].count<=slots-at[hi-lo]) {
            at[hi-lo+1]=at[hi-lo]+ranges[hi].count;++hi;
        }
        int lanes=int(hi-lo);check(lanes>0,"empty ID range wave");
        #pragma omp parallel for num_threads(lanes) schedule(static,1) if(lanes>1)
        for (int j=0;j<lanes;++j) {
            auto r=ranges[lo+j];auto& cache=caches[j];auto* work=arena.data()+at[j];std::array<Offset,256> buf;
            for (U i=0;i<r.count;) {
                U take=std::min<U>(buf.size(),r.count-i);post.read((r.start+i)*sizeof(Offset),buf.data(),take*sizeof(Offset));
                for (U z=0;z<take;++z) {auto x=buf[z];work[i++]={x,cache.token(view.words[x.word].start+x.off)};}
            }
            std::sort(work,work+r.count,[&](const Item& a,const Item& b){return view.compare(a,b,cache)<0;});
        }
        loaded+=at[lanes];
        // Single owner of scatter pages. Adjacent ranges never split a first
        // ID, hence never split an equality group. No cross-range merge.
        for (int j=0;j<lanes;++j) {U before=rank;view.expand(arena.data()+at[j],arena.data()+at[j+1],caches[j],out,rank,shared);
            check(rank-before==ranges[lo+j].mass,"wrong induced ID range mass");}
    }
    out.finish();check(loaded==extent && rank==c.n,"ID lift coverage");
    post.close();check(fs::remove(child/"lift.offsets"),"retire ID postings");
    std::fprintf(stderr,"PFP_LIFT lower=%" PRIu64 " upper=%" PRIu64 " words=%" PRIu64 " ranges=%zu slots=%" PRIu64
        " counted=%" PRIu64 " routed=%" PRIu64 " ordered=%" PRIu64 " shared_groups=%" PRIu64 " ranks=%" PRIu64
        " posting_bytes=%" PRIu64 " array_bound=%" PRIu64 "\n",c.n,c.m,c.k,ranges.size(),slots,counted,routed,loaded,shared,rank,wire,fixed+slots*sizeof(Item));
}
}
}
int main(int argc,char** argv) try {
    using namespace pfp;
    check(argc==7,"usage: pfp_lift LOWER.u64 UPPER_COLUMNS NEW_INVERSE WORKERS SORT_BYTES FREE_SCRATCH_BYTES");
    lift(argv[1],argv[2],argv[3],number(argv[4]),number(argv[5]),number(argv[6]));return 0;
} catch (const std::exception& e) { pfp::fail(e.what()); }
