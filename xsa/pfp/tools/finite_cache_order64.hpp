#pragma once
#include "finite_emit_refine.hpp"
// In-place finite-key order. High slot bits initially contain the producer's monotone,
// zero-padded prefix. In a small collision run they temporarily index a fixed raw-span cache.
namespace finitecache {
using namespace finiteemit;
constexpr U K=512;constexpr unsigned REFILLS=2;
struct Tail {U code,p,remaining,tag;};
struct Stats {U prefix_sorts=0,cache_runs=0,cache_desc=0,reprobes=0,whole_equal=0,slow_runs=0,slow_desc=0,slow_comps=0;
 void add(const Stats& x){prefix_sorts+=x.prefix_sorts;cache_runs+=x.cache_runs;cache_desc+=x.cache_desc;reprobes+=x.reprobes;whole_equal+=x.whole_equal;slow_runs+=x.slow_runs;slow_desc+=x.slow_desc;slow_comps+=x.slow_comps;}};
// 16 KiB of bounded raw metadata plus conservative call/telemetry slack per simultaneous sort lane;
// no auxiliary permutation or second descriptor arena. No allocation inside the sorter.
constexpr U PER_LANE=K*sizeof(Tail)+(8<<10);
inline U prefix(const uint8_t* p,U len){U x=0;if(len>=8)std::memcpy(&x,p,8);else if(len)std::memcpy(&x,p,len);return __builtin_bswap64(x);}
template<class Access,class Guard>class Sorter {
 const uint8_t* raw;const unsigned payload,step;const U low,capacity;Access access;Guard guard;Stats& s;U compares=0;
 std::array<Tail,K> cache;
 void check(){if(!(++compares%32768))guard();}
 Tail tail(Span key,U depth)const{U z=std::min(key.len,depth),len=key.len-z,bytes=std::min<U>(len,8);return {prefix(raw+key.p+z,len),key.p+z+bytes,len-bytes,2*key.len+key.terminal};}
 bool less(const Tail& a,const Tail& b)const{if(a.code!=b.code)return a.code<b.code;U n=std::min(a.remaining,b.remaining),i=0;
  while(n-i>65536){int c=std::memcmp(raw+a.p+i,raw+b.p+i,65536);if(c)return c<0;i+=65536;guard();}
  int c=n>i?std::memcmp(raw+a.p+i,raw+b.p+i,n-i):0;return c?c<0:a.tag<b.tag;}
 void leaf(U* a,U n,U known,U code){++s.cache_runs;s.cache_desc+=n;
  if(n==2){auto x=tail(access(a[0]&low),known),y=tail(access(a[1]&low),known);if(less(y,x))std::swap(a[0],a[1]);return;}
  // Equal projected bits carry no distinguishing information inside this run. Save an ordinal
  // there while reordering the original slots, then restore the uniform projection for expansion.
  for(U i=0;i<n;++i){U x=a[i]&low;cache[i]=tail(access(x),known);a[i]=x|(i<<payload);}
  std::sort(a,a+n,[&](U x,U y){return less(cache[x>>payload],cache[y>>payload]);});for(U i=0;i<n;++i)a[i]=code|(a[i]&low);}
 void slow(U* a,U n,U known){++s.slow_runs;s.slow_desc+=n;std::sort(a,a+n,[&](U x,U y){check();++s.slow_comps;return less(tail(access(x&low),known),tail(access(y&low),known));});}
 void level(U* a,U n,U depth,unsigned round,bool distribute){guard();if(distribute){++s.prefix_sorts;std::sort(a,a+n,[&](U x,U y){check();return (x&~low)<(y&~low);});}
  const U known=depth+step;
  for(U i=0,j,next_guard=0;i<n;i=j){if(i>=next_guard){guard();next_guard=i+8192;}U code=a[i]&~low;j=i+1;while(j<n && (a[j]&~low)==code){++j;if(!(j%65536))guard();}U size=j-i;if(size<2)continue;
   if(size<=capacity){leaf(a+i,size,known,code);continue;}
   if(!step || round==REFILLS){slow(a+i,size,known);continue;}
   // Keep only proven whole bytes in 'known'. A partial final projected byte is read again.
   // Zero-padding never implies equal lengths, so use real extent+terminal at every exact leaf.
   U first=0,tag=0;bool uniform=true,same_tag=true;s.reprobes+=size;
   for(U k=i;k<j;++k){if(!(k%8192))guard();U x=a[k]&low;Span key=access(x);U z=std::min(known,key.len),v=prefix(raw+key.p+z,key.len-z)&~low,t=2*key.len+key.terminal;
    if(k==i){first=v;tag=t;}else{uniform&=v==first;same_tag&=t==tag;}a[k]=x|v;}
   if(uniform && same_tag && (tag>>1)<=known+step){s.whole_equal+=size;continue;}level(a+i,size,known,round+1,!uniform);
  }}
 public:
 Sorter(const uint8_t* t,unsigned p,U mask,Access get,Guard stop,Stats& stats):raw(t),payload(p),step((64-p)/8),low(mask),capacity(p<=64u-unsigned(std::bit_width(K-1))?K:U(1)<<(64-p)),access(get),guard(stop),s(stats){}
 void run(U* a,U n,U known){need(n && payload<=64,"cache order geometry");if(n>1)level(a,n,known,0,payload<64);}
};
}
