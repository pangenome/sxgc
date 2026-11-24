#pragma once
#include "finite_emit_refine.hpp"
#include <memory>
namespace finiteemit {
struct Counts {U rows=0,groups=0,multi=0,bitmaps=0,max_tie=0,max_mass=0;double sort=0,gather=0,produce=0;};
struct Emit {
 const Route& ix;Planner plan;const rawview::View& v;const uint8_t* text;U n,m;int team;rawview::Stop stop;
 unsigned ob=0,payload=0;bool pair=false;U value_mask=0,arrays=0,packets=0,waves=0;double sort_wall=0,stream_wall=0,queue_blocked=0;Counts totals;Sink sink_stats;
 explicit Emit(const Route& index):ix(index),plan(index),v(index.v),text(index.text),n(index.n),m(index.m),team(index.team),stop(index.stop){
  U longest=0;for(auto& s:ix.stripe)longest=std::max(longest,s.longest);need(longest,"no owned words");ob=std::bit_width(longest-1);unsigned rb=std::bit_width(m-1);
  pair=rb+ob<=64;payload=pair?rb+ob:std::bit_width(ix.pages.count*PAGE-1);value_mask=mask(payload);
 }
 std::pair<U,U> unpack(U slot)const{U x=slot&value_mask;if(!pair)return ix.pages.locate(x);return {1+(x>>ob),x&mask(ob)};}
 Span span(U r,U off)const{return plan.span(r,off);}
 Span span(U slot)const{auto [r,off]=unpack(slot);return span(r,off);}
 U pack(U r,U off,U tok,U skip,Span key)const{U code=pair?((r-1)<<ob)|off:tok;need(!(code&~value_mask) && skip<=key.len,"slot packing");
  if(payload==64)return code;
  U high=0;unsigned bytes=unsigned(std::min<U>(divup(64-payload,8),key.len-skip));std::memcpy(&high,text+key.p+skip,bytes);return code|(__builtin_bswap64(high)&~value_mask);}
 int cmp(Span a,Span b)const{U len=std::min(a.len,b.len),h=0;while(len-h>65536){int z=std::memcmp(text+a.p+h,text+b.p+h,65536);if(z)return z;h+=65536;stop.check();}
  int z=std::memcmp(text+a.p+h,text+b.p+h,len-h);if(z)return z;if(a.len!=b.len)return a.len<b.len?-1:1;return int(a.terminal)-int(b.terminal);}
 U seam(Span a,Span b)const{if(!a.len)return 0;U h=0,len=std::min(a.len,b.len);while(len-h>=8){U x,y;std::memcpy(&x,text+a.p+h,8);std::memcpy(&y,text+b.p+h,8);if(x!=y){h+=std::countr_zero(x^y)/8;break;}h+=8;if(!(h%65536))stop.check();}
  while(h<len && text[a.p+h]==text[b.p+h])++h;
  // Proper-prefix exhaustion between DIFFERENT finite keys can only be the EOF suffix.
  need((h==a.len && a.terminal && h<b.len) || (h<len && text[a.p+h]<text[b.p+h]),"unordered/split finite key seam");return h;}
 unsigned skip(U b)const{const auto& z=ix.bands[b];return z.hi==z.lo+1 && z.lo%257!=0?2:(z.lo/257==(z.hi-1)/257?1:0);}
 void order(const Job& j,U* a,Counts& c)const{if(j.exact)return;double ts=omp_get_wtime();const U d=j.d;U i=0,known=std::max<U>(skip(j.band),j.depth);
  plan.visit(j,[&](U r,U off,U tok,Span s){need(i<d,"sort arena overrun");a[i++]=pack(r,off,tok,known,s);});need(i==d,"sort arena underflow");
  c.gather=omp_get_wtime()-ts;U compares=0;std::sort(a,a+d,[&](U x,U y){if(!(++compares%32768))stop.check();U hx=x&~value_mask,hy=y&~value_mask;return hx!=hy?hx<hy:cmp(span(x),span(y))<0;});c.sort=omp_get_wtime()-ts;}
 template<class Feed>void union_group(Span key,U h0,Marks& marks,Pipe& out,Counts& c,Feed feed)const{
  need(!key.terminal && key.len>8,"shared EOF/short ordinary key");++c.multi;std::array<U,16> small;U num=0,start=c.rows;
  feed([&](U head){U end=v.after_head(head);for(U r=head;r<end;++r){if(!(num%16384))stop.check();U s=v.successor(r);
    if(num<small.size())small[num]=s;else{if(num==small.size())for(U t:small)marks.add(t);marks.add(s);}++num;}});
  U left=key.len-8,previous=0;auto emit=[&](U s){if(!(c.rows%16384))stop.check();need(s>previous && s<=m && v.position(s)>=left,"tie successor order");U p=v.position(s)-left,h=h0;
   if(previous){U aligned=s==previous+1?v.previous_lcp(s):v.lce(previous,s);need(aligned>=8,"tie continuations miss trigger");h=left+aligned;}out.put(row(p,h,v.predecessor(p)));previous=s;++c.rows;};
  if(num<=small.size()){std::sort(small.begin(),small.begin()+num);for(U i=0;i<num;++i)emit(small[i]);}else{++c.bitmaps;marks.drain(emit);}need(c.rows-start==num,"lost tie occurrences");}
 void expand(const Job& job,const U* a,Span prev,Marks& marks,Pipe& out,Counts& c)const{double start=omp_get_wtime();U d=job.d;
  if(job.exact){auto key=job.anchor;union_group(key,seam(prev,key),marks,out,c,[&](auto add){plan.visit(job,[&](U r,U,U,Span s){need(!cmp(s,key),"nonuniform bypass class");add(r);});});
   c.groups=1;c.max_tie=d;c.max_mass=c.rows;d=0;}
  for(U i=0,j=0;i<d;i=j){auto key=span(a[i]);U h0=seam(prev,key);prev=key;j=i+1;
   while(j<d && !((a[j]^a[i])&~value_mask) && !cmp(key,span(a[j]))){if(!(j%16384))stop.check();++j;}
   U mass0=c.rows;++c.groups;c.max_tie=std::max(c.max_tie,j-i);
   if(j==i+1){auto [r,off]=unpack(a[i]);U hi=v.after_head(r);
    for(U z=r;z<hi;++z){if(!(c.rows%16384))stop.check();U p=v.position(z)+off,h=h0;if(z>r){U aligned=v.previous_lcp(z);need(aligned>=off,"unmatched singleton prefix");h=aligned-off;}
     out.put(row(p,h,v.predecessor(p)));++c.rows;}}
   else union_group(key,h0,marks,out,c,[&](auto add){for(U k=i;k<j;++k)add(unpack(a[k]).first);});
   c.max_mass=std::max(c.max_mass,c.rows-mass0);
  }
  need(c.rows==job.rows,"band row coverage");out.finish();c.produce=omp_get_wtime()-start;
 }
 static Big emit_arrays(U n,U m,U team,U nb,U pg,U slots){Big live=Big(rawview::budget(n,m,team))+Pages::bytes(pg)+Big(team)*nb*sizeof(Layout)+CELL*(sizeof(Band)+sizeof(unsigned))+team*sizeof(Stripe);
  return live+8*Big(slots)+team*(Marks::bytes(m)+sizeof(Pipe)+(256<<10))+2*Big(Planner::LIMIT)*sizeof(Job)+(2<<20);}
 void run(int sink,U max_slots,U grant){need(n<(U(1)<<43) && max_slots,"row ABI or empty arena");stop.check();U slots=std::min(max_slots,v.stats.owned);
  need(ix.bands.capacity()==CELL,"unexpected vector reservation");arrays=narrow(std::max<Big>(ix.array_bound,emit_arrays(n,m,team,ix.bands.size(),ix.pages.count,slots)));need(arrays<=grant,"ordered stream array grant");plan.build(slots);const auto& jobs=plan.jobs;
  std::vector<U> arena(slots);std::vector<Marks> marks;marks.reserve(team);for(int i=0;i<team;++i)marks.emplace_back(m);
  int flags=fcntl(sink,F_GETFL);need(flags>=0 && !fcntl(sink,F_SETFL,flags|O_NONBLOCK),"nonblocking row sink");Span prev{};Row first=row(n,0,text[n-1]);send(sink,&first,1,stop,sink_stats);
  for(U lo=0,hi;lo<jobs.size();lo=hi){std::array<U,65> at{};hi=lo;while(hi<jobs.size() && hi-lo<U(team)){U i=hi-lo,d=jobs[hi].exact?0:jobs[hi].d;if(d>slots-at[i])break;at[i+1]=at[i]+d;++hi;}
   int lanes=int(hi-lo);need(lanes>0,"empty wave");++waves;std::vector<Counts> stats(lanes);double ts=omp_get_wtime();
#pragma omp parallel for num_threads(lanes) schedule(static,1)
   for(int j=0;j<lanes;++j)order(jobs[lo+j],arena.data()+at[j],stats[j]);
   sort_wall+=omp_get_wtime()-ts;ts=omp_get_wtime();std::array<Span,64> edges;std::vector<std::unique_ptr<Pipe>> pipes;
   for(int j=0;j<lanes;++j){edges[j]=prev;prev=jobs[lo+j].exact?jobs[lo+j].anchor:span(arena[at[j+1]-1]);pipes.emplace_back(std::make_unique<Pipe>(stop));}
   // All lanes are live (never a pool queued behind a blocked later range). Cat does no suffix comparisons.
   std::thread cat([&](){for(int j=0;j<lanes;++j)packets+=pipes[j]->cat(sink,jobs[lo+j].rows,sink_stats);});
#pragma omp parallel num_threads(lanes)
   {int j=omp_get_thread_num();need(omp_get_num_threads()==lanes,"missing emission lane");expand(jobs[lo+j],arena.data()+at[j],edges[j],marks[j],*pipes[j],stats[j]);}
   cat.join();stream_wall+=omp_get_wtime()-ts;for(auto& p:pipes)queue_blocked+=p->blocked;
   for(auto& c:stats){totals.rows+=c.rows;totals.groups+=c.groups;totals.multi+=c.multi;totals.bitmaps+=c.bitmaps;totals.sort+=c.sort;totals.gather+=c.gather;totals.produce+=c.produce;totals.max_tie=std::max(totals.max_tie,c.max_tie);totals.max_mass=std::max(totals.max_mass,c.max_mass);}}
  stop.check();need(totals.rows==n && sink_stats.bytes==12*(n+1),"incomplete byte rows");}
};
}
