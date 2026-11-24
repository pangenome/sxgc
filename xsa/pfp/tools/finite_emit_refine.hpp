#pragma once
#include "finite_emit_rows.hpp"
namespace finiteemit {
struct Span {U p=0,len=0;bool terminal=false;};
struct Job {U band=0,d=0,rows=0;Span anchor{};U depth=0;unsigned lo=0,hi=256;bool filter=false,exact=false;};
// Split ONLY the deficient coarse posting stream. A child is a full shared byte prefix
// plus a closed next-code interval. Code 0 = finite exhaustion (not a raw NUL).
struct Planner {
 static constexpr U LIMIT=16384,WORK=16;
 const Route& ix;std::vector<Job> jobs;U scans=0,exact=0,decodes=0,max_stack=0;double seconds=0;
 explicit Planner(const Route& i):ix(i){}
 Span span(U r,U off)const{auto w=ix.v.word(r);need(off<w.len-(w.next?8:0),"finite offset exceeds owned stride");return {w.p+off,w.len-off,!w.next};}
 unsigned code(Span s,U h)const{need(h<=s.len,"split depth exceeds key");return h==s.len?0:1u+ix.text[s.p+h];}
 U prefix(Span a,Span b,U limit)const{U h=0,n=std::min({a.len,b.len,limit});if(a.p==b.p)return n;while(n-h>=8){U x,y;std::memcpy(&x,ix.text+a.p+h,8);std::memcpy(&y,ix.text+b.p+h,8);
   if(x!=y)return h+std::countr_zero(x^y)/8;
   h+=8;if(!(h%65536))ix.stop.check();}while(h<n && ix.text[a.p+h]==ix.text[b.p+h])++h;return h;}
 template<class F>void visit(const Job& j,F action)const{U got=0;ix.visit(j.band,[&](U r,U off,U token){auto s=span(r,off);
   if(j.filter){if(s.len<j.depth || prefix(s,j.anchor,j.depth)!=j.depth)return;unsigned c=code(s,j.depth);if(c<j.lo || c>j.hi)return;}++got;action(r,off,token,s);});need(got==j.d,"local descriptor interval coverage");}
 void charge(U d){need(Big(decodes)+d<=Big(WORK)*ix.v.stats.owned,"local replay work bound");decodes=narrow(Big(decodes)+d);}
 void build(U slots){double t=omp_get_wtime();need(slots && ix.bands.size()<=LIMIT,"routing leaf count");jobs.reserve(LIMIT);std::vector<Job> pending;pending.reserve(LIMIT);
  for(U b=0;b<ix.bands.size();++b){const auto& coarse=ix.bands[b];pending.push_back({b,coarse.load.d,coarse.load.rows});
   while(!pending.empty()){ix.stop.check();Job j=pending.back();pending.pop_back();need(jobs.size()+pending.size()<LIMIT,"too many local leaves");
    if(j.d<=slots){charge(coarse.load.d);jobs.push_back(j);continue;}
    ++scans;charge(coarse.load.d);std::array<Load,257> hist{};Span witness{};U h=0,seen=0,mass=0;
    visit(j,[&](U r,U,U,Span s){if(!seen){witness=s;h=s.len;}else{U z=prefix(witness,s,h);if(z<h){h=z;hist.fill({});hist[code(witness,h)]={seen,mass};}}
     U freq=ix.v.after_head(r)-r;auto& c=hist[code(s,h)];++c.d;c.rows+=freq;++seen;mass+=freq;});need(mass==j.rows && (!j.filter || h>=j.depth),"refinement prefix/mass");
    if(hist[0].d==j.d){j.exact=true;
     // Preserve the input predicate: witness of equality may differ from the ancestor anchor
     // but shares its entire prefix, so substituting it leaves membership unchanged.
     j.anchor=witness;charge(coarse.load.d);jobs.push_back(j);++exact;continue;}
    std::array<Job,257> children;unsigned count=0;for(unsigned i=0;i<hist.size();++i){auto c=hist[i];if(!c.d)continue;
     if(!count || children[count-1].d>slots || c.d>slots-children[count-1].d){children[count++]={b,0,0,witness,h,i,i,true,false};}
     auto& z=children[count-1];z.hi=i;z.d+=c.d;z.rows+=c.rows;}
    need(count>=2 && jobs.size()+pending.size()+count<=LIMIT,"nonprogress or excessive local split");
    while(count){auto z=children[--count];need(z.d<j.d,"nonshrinking cell");pending.push_back(z);}max_stack=std::max<U>(max_stack,pending.size());
   }}U d=0,rows=0;for(const auto& j:jobs){d+=j.d;rows+=j.rows;}need(d==ix.v.stats.owned && rows==ix.n,"refined coverage");seconds=omp_get_wtime()-t;}
};
}
