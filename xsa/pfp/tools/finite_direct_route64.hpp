#pragma once
#include "finite_route64.hpp"
#include <span>
// Direct monotone postings to learned finite-key ranges. Sampling sets boundaries, NOT counts or equality.
namespace routedirect {
using namespace route64;
constexpr U OVER=64,MAX_BANDS=16384;
inline Big upper(U v,U d,U bands,U team){need(v>=d && d && bands && team,"direct dimensions");Big best=Big(d)+Big(bands)*(v-1);
 for(unsigned l=1;l<64;++l)best=std::min(best,Big(d)*(l+1)+Big(bands)*((v-1)>>l));
 return (best+7)/8+Big(16)*bands*team;}
inline U allowed(U v,U d,U target,U team,U available){need(target && upper(v,d,1,team)<=available,"direct posting scratch exceeds 2N");
 U lo=1,hi=std::min({MAX_BANDS,d,divup(d,target)});while(lo<hi){U mid=lo+(hi-lo+1)/2;if(upper(v,d,mid,team)<=available)lo=mid;else hi=mid-1;}return lo;}
inline U mix(U x){x+=0x9e3779b97f4a7c15ull;x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;x=(x^(x>>27))*0x94d049bb133111ebull;return x^(x>>31);}
// The exact same total number of words as contiguous EF. Aligned whole-page interiors,
// then one packed region of tails. No per-range 4 KiB disk padding, ever.
inline U place(std::span<Layout> streams,U& boundary){Big at=0;for(auto& s:streams){s.low_at=narrow(at);at+=8*Big(s.fmt.low/BUF*BUF);s.high_at=narrow(at);at+=8*Big(s.fmt.high/BUF*BUF);}boundary=narrow(at);
 for(auto& s:streams){s.low_tail=narrow(at);at+=8*(s.fmt.low%BUF);s.high_tail=narrow(at);at+=8*(s.fmt.high%BUF);}return narrow(at);}
class Fragments {
 int fd;U begin,at,n=0;std::array<U,32768> buf;rawview::Stop stop;
 void flush(){if(n){stop.check();transfer(fd,true,at,buf.data(),8*n);at+=8*n;n=0;}}
 public:
 Fragments(int output,U offset,rawview::Stop g):fd(output),begin(offset),at(offset),stop(g){}
 void append(const OutBuf& out,U cap,U offset){U len=cap%BUF;need(at+8*n==offset && out.used<=len && (!out.used || out.sent==cap-len),"fragment bounds");
  // At most 511 words, possibly across a concatenation buffer seam. A truncated high
  // stream's unused reserved tail is deliberately zero-filled and charged as a write.
  for(U start=0;start<len;){U take=std::min<U>(buf.size()-n,len-start),valid=std::min<U>(take,out.used>start?out.used-start:0);
   if(valid)std::memcpy(buf.data()+n,out.buf.data()+start,8*valid);
   std::fill_n(buf.data()+n+valid,take-valid,0);n+=take;start+=take;if(n==buf.size())flush();}}
 U finish(U end){flush();need(at==end,"incomplete fragment region");return at-begin;}
};
struct Key {U p,len,projection;unsigned cell;bool terminal;};
struct Bounds {unsigned lo=0,hi=0;};
struct Direct {
 Route& ix;const rawview::View& view;const uint8_t* raw;const U n,m,d;int team;rawview::Stop stop;
 std::vector<Key> pivots;std::vector<Bounds> cells;std::vector<U> skip;
 U target_bands=0,samples=0,file_upper=0,explicit_write=0,fragment_bytes=0,max_desc=0,max_rows=0;double sample_s=0;
 explicit Direct(Route& r):ix(r),view(r.v),raw(r.text),n(r.n),m(r.m),d(r.v.stats.owned),team(r.team),stop(r.stop){}
 U projection(U p,U len)const{U v=0;U skip=std::min<U>(2,len),bytes=std::min<U>(8,len-skip);if(bytes==8)std::memcpy(&v,raw+p+skip,8);else if(bytes)std::memcpy(&v,raw+p+skip,bytes);return __builtin_bswap64(v);}
 Key key(rawview::Word w,U o,unsigned cell)const{return {w.p+o,w.len-o,projection(w.p+o,w.len-o),cell,!w.next};}
 int cmp(const Key& a,const Key& b)const{if(a.cell!=b.cell)return a.cell<b.cell?-1:1;if(a.projection!=b.projection)return a.projection<b.projection?-1:1;
  // Equal projections may hide exhausted keys or zero bytes. Lexical identity is never hash/projection identity.
  U len=std::min(a.len,b.len),i=std::min<U>(10,len);while(len-i>65536){int c=std::memcmp(raw+a.p+i,raw+b.p+i,65536);if(c)return c;i+=65536;stop.check();}
  int c=std::memcmp(raw+a.p+i,raw+b.p+i,len-i);if(c)return c;if(a.len!=b.len)return a.len<b.len?-1:1;return int(a.terminal)-int(b.terminal);}
 U lcp(const Key& a,const Key& b)const{U i=0,len=std::min(a.len,b.len);while(len-i>=8){U x,y;std::memcpy(&x,raw+a.p+i,8);std::memcpy(&y,raw+b.p+i,8);if(x!=y)return i+std::countr_zero(x^y)/8;i+=8;if(!(i%65536))stop.check();}while(i<len && raw[a.p+i]==raw[b.p+i])++i;return i;}
 unsigned classify(rawview::Word w,U o,unsigned c)const{auto [lo,hi]=cells[c];if(lo<hi){auto k=key(w,o,c);while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(cmp(k,pivots[mid])<0)hi=mid;else lo=mid+1;}}return lo;}
 void learn(){samples=std::min(d,target_bands*OVER);std::vector<Key> sample(samples);std::vector<U> ord(samples);U t0=0;
  for(U j=0;j<samples;++j){U t1=rawview::cut(d,j+1,samples);ord[j]=t0+mix(j)%(t1-t0);t0=t1;}
  std::array<U,viewproof::shards+1> owned{};for(unsigned j=0;j<viewproof::shards;++j)owned[j+1]=owned[j]+view.shard(j).owned;need(owned.back()==d,"sample head extents");
#pragma omp parallel for num_threads(team) schedule(static)
  for(unsigned j=0;j<viewproof::shards;++j){Stripe part;part.lo=viewproof::boundary(m,j);part.hi=viewproof::boundary(m,j+1);U pos=owned[j],i=std::lower_bound(ord.begin(),ord.end(),pos)-ord.begin();
   ix.heads(part,[&](U,auto w,U stride){U end=pos+stride;while(i<samples && ord[i]<end){U off=ord[i]-pos;sample[i]=key(w,off,Route::cell_at(raw+w.p,w.len,off));++i;}pos=end;});need(pos==owned[j+1],"sample stripe drift");}
  U compares=0;std::sort(sample.begin(),sample.end(),[&](auto& a,auto& b){if(!(++compares%32768))stop.check();return cmp(a,b)<0;});pivots.reserve(target_bands);
  for(U i=1;i<target_bands;++i){auto& k=sample[rawview::cut(samples,i,target_bands)];auto& last=pivots.empty()?sample.front():pivots.back();if(cmp(last,k)<0)pivots.push_back(k);}
  cells.resize(CELL);unsigned after=0;for(unsigned c=0;c<CELL;++c){unsigned first=after;while(after<pivots.size() && pivots[after].cell==c)++after;cells[c]={first,after};}need(after==pivots.size(),"unsorted sample cells");
  skip.resize(pivots.size()+1);for(U i=1;i<pivots.size();++i)skip[i]=lcp(pivots[i-1],pivots[i]);
 }
 static Big work_arrays(U n,U m,U w,U nb,U pg,U sample){return Route::build_arrays(n,m,w,nb,pg)+Big(nb)*(sizeof(Key)+8)+Big(sample)*(sizeof(Key)+8)+Big(CELL)*sizeof(Bounds)+Big(nb)*w*sizeof(Load)+(1<<20);}
 Big retained()const{return Big(pivots.capacity())*sizeof(Key)+Big(cells.capacity())*sizeof(Bounds);}
 void prepare(U target,U arrays,U other,bool epochs=false,Big extra=0){double t=omp_get_wtime();stop.check();need(target && other>=8*m && Big(other)<Big(2)*n && d>=view.stats.groups,"direct scratch inputs");
  U pg=narrow(Big(m)+(d-view.stats.groups)/PAGE),vmax=narrow(Big(pg)*PAGE);target_bands=epochs?std::min(MAX_BANDS,divup(d,target)):allowed(vmax,d,target,team,narrow(2*Big(n)-other));file_upper=narrow(upper(vmax,d,target_bands,team));
  ix.array_bound=narrow(work_arrays(n,m,team,target_bands,pg,std::min(d,OVER*target_bands))+extra);need(ix.array_bound<=arrays,"direct splitter/EF array grant");learn();double ts=omp_get_wtime();sample_s=ts-t;U nb=pivots.size()+1;
  ix.stripe.resize(team);ix.bands.reserve(CELL);ix.bands.resize(nb);std::vector<Load> counts(team*nb);
#pragma omp parallel num_threads(team)
  {int lane=omp_get_thread_num();need(omp_get_num_threads()==team,"direct census team");auto& s=ix.stripe[lane];s.lo=rawview::cut(m,lane,team);s.hi=rawview::cut(m,lane+1,team);s.pages=s.hi-s.lo;auto* local=counts.data()+lane*nb;
   ix.heads(s,[&](U r,auto w,U stride){s.longest=std::max(s.longest,stride);s.pages+=(stride-1)/PAGE;U freq=view.after_head(r)-r;
    ix.offsets(w,stride,[&](U o,unsigned c){auto& z=local[classify(w,o,c)];++z.d;z.rows+=freq;});});}
  double tc=omp_get_wtime();U pages=0;for(auto& s:ix.stripe){s.base=pages;pages+=s.pages;}need(pages<=pg,"actual page geometry");ix.pages.allocate(pages);ix.slices.resize(nb*team);U found=0,mass=0;
  for(U b=0;b<nb;++b){auto& z=ix.bands[b];z.lo=0;z.hi=CELL;for(int lane=0;lane<team;++lane){auto c=counts[lane*nb+b];z.load.d+=c.d;z.load.rows+=c.rows;auto& s=ix.slices[b*team+lane];s.count=c.d;s.rows=c.rows;s.fmt=shape(ix.stripe[lane].pages*PAGE,c.d);}need(z.load.d,"empty learned range");found+=z.load.d;mass+=z.load.rows;max_desc=std::max(max_desc,z.load.d);max_rows=std::max(max_rows,z.load.rows);}
  need(found==d && mass==n,"learned range coverage");ix.census_s=tc-ts;ix.plan_s=omp_get_wtime()-tc;}
 U band_wire(U b)const{Big z=0;for(int lane=0;lane<team;++lane){auto& s=ix.slices.at(b*team+lane);z+=8*(Big(s.fmt.low)+s.fmt.high);}return narrow(z);}
 U layout(U lo,U hi,U& boundary){need(lo<hi && hi<=ix.bands.size() && ix.fd==-1,"direct layout/lifetime");ix.active_lo=lo;ix.active_hi=hi;return place(std::span(ix.slices).subspan(lo*team,(hi-lo)*team),boundary);}
 template<bool Rolling>void encode(int fd,U lo,U hi,U bytes,U boundary){stop.check();U nb=hi-lo;double start=omp_get_wtime();std::vector<Encoder> enc(nb*team);double tp=omp_get_wtime();ix.plan_s+=tp-start;
#pragma omp parallel num_threads(team)
  {int lane=omp_get_thread_num();auto& s=ix.stripe[lane];U extra=0;ix.heads(s,[&](U r,auto w,U stride){U page=s.base+(r-1-s.lo)+extra,more=(stride-1)/PAGE;if(!Rolling || !lo)ix.pages.extras(page+1,more,stop);extra+=more;U rel=PAGE*(page-s.base);
    ix.offsets(w,stride,[&](U o,unsigned c){if constexpr(Rolling){if(cells[c].lo>=hi || cells[c].hi<lo)return;}unsigned b=classify(w,o,c);if constexpr(Rolling){if(b<lo || b>=hi)return;}enc[lane*nb+b-lo].put(fd,ix.slices[b*team+lane],s.pages*PAGE,rel+o,stop);});});need(s.hi-s.lo+extra==s.pages,"direct stripe extent");
   for(U b=lo;b<hi;++b)enc[lane*nb+b-lo].finish(fd,ix.slices[b*team+lane]);}
  Fragments tails(fd,boundary,stop);for(U b=lo;b<hi;++b){for(int lane=0;lane<team;++lane){auto& s=ix.slices[b*team+lane];auto& e=enc[lane*nb+b-lo];tails.append(e.low,s.fmt.low,s.low_tail);tails.append(e.high,s.fmt.high,s.high_tail);explicit_write+=8*(e.low.sent+e.high.sent);ix.transferred+=8*(s.fmt.low+s.used_high);}}
  U tails_written=tails.finish(bytes);fragment_bytes+=tails_written;explicit_write+=tails_written;ix.wire=narrow(Big(ix.wire)+bytes);need(ix.transferred<=explicit_write && explicit_write<=ix.wire && ix.wire<=file_upper,"direct logical write bounds");if(!lo)ix.pages.finish(m,stop);ix.encode_s+=omp_get_wtime()-tp;}
 void drop_splitters(){std::vector<Key>().swap(pivots);std::vector<Bounds>().swap(cells);}
 void build(const char* file,U target,U arrays,U other){prepare(target,arrays,other);double t=omp_get_wtime();U boundary=0,nb=ix.bands.size(),bytes=layout(0,nb,boundary);need(bytes<=file_upper && Big(other)+bytes<=Big(2)*n && bytes<=INT64_MAX,"direct payload bounds");stop.check();
  int fd=open(file,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);need(fd>=0 && !ftruncate(fd,off_t(bytes)),"new direct payload");ix.plan_s+=omp_get_wtime()-t;encode<false>(fd,0,nb,bytes,boundary);double te=omp_get_wtime();drop_splitters();struct stat a{},b{};need(!fsync(fd) && !fstat(fd,&a) && U(a.st_size)==ix.wire && a.st_nlink==1,"direct sync/shape");ix.fd=open(file,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);need(ix.fd>=0 && !fstat(ix.fd,&b) && a.st_ino==b.st_ino && a.st_dev==b.st_dev && a.st_size==b.st_size && b.st_nlink==1 && !close(fd),"direct handoff");stop.check();ix.fsync_s=omp_get_wtime()-te;
 }
};
}
