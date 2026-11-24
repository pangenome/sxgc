#pragma once
#include "finite_route_bits.hpp"
namespace route64 {
struct Load {U d=0,rows=0;};
struct Band {unsigned lo,hi; Load load;};
struct Stripe {U lo=0,hi=0,base=0,pages=0;}; // rank-1 slices; virtual coordinates concatenated in same order
struct Route {
 const rawview::View& v;const uint8_t* text;U n,m;int team;rawview::Stop stop;
 std::vector<Stripe> stripe;std::vector<unsigned> band_for;std::vector<Band> bands;std::vector<Layout> slices;Pages pages;
 int fd=-1;U wire=0,transferred=0,array_bound=0;double census_s=0,plan_s=0,encode_s=0,fsync_s=0;
 Route(const rawview::View& view,const uint8_t* src,U len,U ranks,int workers,rawview::Stop guard):v(view),text(src),n(len),m(ranks),team(workers),stop(guard){}
 Route(const Route&)=delete;~Route(){if(fd!=-1)close(fd);}
 static unsigned cell_at(const uint8_t* raw,U len,U off){return raw[off]*257u+(off+1<len?1u+raw[off+1]:0u);}
 template<class F>void heads(const Stripe& s,F visit)const{
  for(U r=s.lo+1;r<=s.hi;++r){if(!(r%8192))stop.check();if(v.is_head(r)){auto w=v.word(r);U owned=w.len-(w.next?8:0);need(owned,"empty representative");visit(r,w,owned);}}}
 template<class F>void offsets(rawview::Word w,U owned,F visit)const{
  const auto* p=text+w.p;
  for(U base=0;base<owned;){U last=base+std::min<U>(owned-base,65536);if(base)stop.check();
   for(U o=base;o<last;++o)visit(o,cell_at(p,w.len,o));
   base=last;}}
 void build(const char* newfile,U target,U arrays,U other_files){
  stop.check();need(target && other_files>=8*m && Big(other_files)<=2*Big(n),"route target/other scratch");
  Big base=Big(rawview::budget(n,m,team))+Big(team)*CELL*sizeof(Load)+Big(CELL)*(sizeof(unsigned)+sizeof(Band))+U(team)*sizeof(Stripe)+(1<<20);
  need(base<=arrays,"routing census array grant");double t=omp_get_wtime();
  std::vector<Load> counts(CELL*team);stripe.resize(team);band_for.resize(CELL);bands.reserve(CELL);
#pragma omp parallel num_threads(team)
  {int lane=omp_get_thread_num();need(omp_get_num_threads()==team,"route team");auto& s=stripe[lane];s.lo=rawview::cut(m,lane,team);s.hi=rawview::cut(m,lane+1,team);s.pages=s.hi-s.lo;
   auto c=counts.data()+CELL*lane;
   heads(s,[&](U r,auto w,U owned){U freq=v.after_head(r)-r;s.pages+=(owned-1)/PAGE;
    offsets(w,owned,[&](U,unsigned key){++c[key].d;c[key].rows+=freq;});});}
  double tc=omp_get_wtime();U page_count=0;for(auto& s:stripe){s.base=page_count;page_count=narrow(Big(page_count)+s.pages);}
  need(Big(page_count)*PAGE<=UINT64_MAX,"route token extent");
  Band band{0,0,{}};U all=0,mass=0;
  for(unsigned i=0;i<CELL;++i){Load cell;for(int lane=0;lane<team;++lane){auto c=counts[CELL*lane+i];cell.d+=c.d;cell.rows+=c.rows;}
   // Never cut a cell. A single over-target cell is a real oversize band, not proof of fit.
   if(band.load.rows && cell.rows && (band.load.rows>target || cell.rows>target-band.load.rows)){
    band.hi=i;bands.push_back(band);band={i,0,{}};}
   band_for[i]=unsigned(bands.size());band.load.d+=cell.d;band.load.rows+=cell.rows;all+=cell.d;mass+=cell.rows;}
  band.hi=CELL;bands.push_back(band);need(all==v.stats.owned && mass==n && band.load.d,"route census coverage");
  U nb=bands.size(),ns=nb*team;array_bound=narrow(base+Pages::bytes(page_count)+Big(ns)*(sizeof(Layout)+sizeof(Encoder))+Big(team)*sizeof(InBuf)*2);
  need(array_bound<=arrays,"routing index array grant");slices.resize(ns);pages.allocate(page_count);
  for(int lane=0;lane<team;++lane){for(unsigned i=0;i<CELL;++i){auto& b=slices[band_for[i]*team+lane];auto c=counts[CELL*lane+i];b.count+=c.d;b.rows+=c.rows;}}
  std::vector<Load>().swap(counts);
  for(U b=0;b<nb;++b){if(!(b%1024))stop.check();for(int lane=0;lane<team;++lane){auto& s=slices[b*team+lane];s.place(wire,stripe[lane].pages*PAGE);wire=s.end();}}
  need(wire<=INT64_MAX && Big(other_files)+wire<=Big(n)*2,"descriptor posting scratch exceeds 2N");stop.check();
  // Named, fresh scratch, NOT a suffix-row spool. Never open an old output for update.
  int writer=open(newfile,O_CLOEXEC|O_EXCL|O_NOFOLLOW|O_RDWR|O_CREAT,0600);need(writer>=0,"create new posting payload");
  need(ftruncate(writer,off_t(wire))==0,"posting file extent");double tp=omp_get_wtime();
#pragma omp parallel num_threads(team)
  {int lane=omp_get_thread_num();const auto& s=stripe[lane];std::vector<Encoder> enc(nb);U extra=0;
   heads(s,[&](U r,auto w,U owned){U page=s.base+(r-1-s.lo)+extra,more=(owned-1)/PAGE;pages.extras(page+1,more,stop);extra+=more;
    U rel=PAGE*(page-s.base);offsets(w,owned,[&](U off,unsigned key){U b=band_for[key];enc[b].put(writer,slices[b*team+lane],s.pages*PAGE,rel+off,stop);});});
   need(s.hi-s.lo+extra==s.pages,"stripe extra-page total");for(U b=0;b<nb;++b)enc[b].finish(writer,slices[b*team+lane]);}
  pages.finish(m,stop);for(const auto& s:slices)transferred+=8*(s.fmt.low+s.used_high);
  double te=omp_get_wtime();stop.check();need(!fsync(writer),"sync posting payload");
  struct stat a{},b{};need(!fstat(writer,&a) && U(a.st_size)==wire && a.st_nlink==1,"posting output shape");fd=open(newfile,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
  need(fd>=0 && !fstat(fd,&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_size==b.st_size && b.st_nlink==1 && !close(writer),"posting handoff");
  stop.check();census_s=tc-t;plan_s=tp-tc;encode_s=te-tp;fsync_s=omp_get_wtime()-te;
 }
 template<class Visit>void visit(U b,Visit action)const{need(b<bands.size() && fd>=0,"route band bounds");stop.check();
  for(int lane=0;lane<team;++lane){auto& s=slices[b*team+lane];auto& span=stripe[lane];
   decode(fd,s,span.pages*PAGE,stop,[&](U rel){U token=span.base*PAGE+rel;auto [rank,off]=pages.locate(token);
    need(rank>span.lo && rank<=span.hi && v.is_head(rank),"posting does not name a head");action(rank,off,token);});}}
};
}
