#pragma once
// Ephemeral monotone postings for DISTINCT word offsets. Disk offsets are bytes, bit order LE.
#include "raw_rank_view64.hpp"
#include <algorithm>
#include <utility>
namespace route64 {
using rawview::U; using rawview::Big; using rawview::need; using rawview::divup;
constexpr U PAGE=32, ANCHOR=512, CELL=256*257, BUF=512;
inline U narrow(Big x){need(x<=UINT64_MAX,"routing integer overflow");return U(x);}
inline U mask(unsigned b){return b==64?UINT64_MAX:(U(1)<<b)-1;}
inline U words(Big bits){return narrow((bits+63)/64);}
struct Shape { unsigned l=0; U low=0,high=0; }; // u64 words
inline Shape shape(U universe,U count){
 need(count<=universe,"posting count exceeds stripe universe");if(!count)return {};
 unsigned best=0;Big z=Big(count)+universe-1;
 for(unsigned l=1;l<64;++l){Big b=Big(count)*(l+1)+((universe-1)>>l);if(b<z){z=b;best=l;}}
 return {best,words(Big(count)*best),words(Big(count)+((universe-1)>>best))};
}
inline void transfer(int fd,bool wr,U at,void* data,U n){
 need(Big(at)+n<=INT64_MAX,"file offset overflow");
 for(U done=0;done<n;){auto p=(char*)data+done;ssize_t s=wr?pwrite(fd,p,n-done,at+done):pread(fd,p,n-done,at+done);
  if(s<0 && errno==EINTR)continue;
  need(s>0,wr?"short posting write":"short posting read");done+=U(s);}
}
struct Layout {
 U count=0, rows=0, low_at=0,high_at=0,used_high=0; Shape fmt;
 U end()const{return narrow(Big(high_at)+8*Big(fmt.high));}
 void place(U at,U universe){fmt=shape(universe,count);low_at=at;high_at=narrow(Big(at)+8*Big(fmt.low));}
};
struct OutBuf {
 std::array<U,BUF> buf; U used=0,sent=0;
 void flush(int fd,U at){if(used){transfer(fd,true,at+8*sent,buf.data(),8*used);sent+=used;used=0;}}
 void put(int fd,U at,U cap,U x){need(sent+used<cap,"EF slice overflow");buf[used++]=x;if(used==BUF)flush(fd,at);}
};
struct Encoder {
 OutBuf low,high;U seen=0,prev=0,low_word=0,high_word=0,hi_index=0;unsigned filled=0;
 void put(int fd,const Layout& s,U universe,U x,const rawview::Stop& stop){
  need(x<universe && (!seen || prev<x) && seen<s.count,"posting order/extent");prev=x;
  unsigned l=s.fmt.l;if(l){U v=x&mask(l);unsigned room=64-filled;low_word|=v<<filled;
   if(l>=room){low.put(fd,s.low_at,s.fmt.low,low_word);low_word=l==room?0:v>>room;filled=l-room;}else filled+=l;}
  U bit=narrow(Big(x>>l)+seen),w=bit/64;
  while(hi_index<w){high.put(fd,s.high_at,s.fmt.high,high_word);high_word=0;++hi_index;if(!(hi_index%65536))stop.check();}
  high_word|=U(1)<<(bit%64);++seen;
 }
 void finish(int fd,Layout& s){need(seen==s.count,"missing postings");if(filled)low.put(fd,s.low_at,s.fmt.low,low_word);
  if(seen)high.put(fd,s.high_at,s.fmt.high,high_word);
  low.flush(fd,s.low_at);high.flush(fd,s.high_at);s.used_high=high.sent;need(low.sent==s.fmt.low && high.sent<=s.fmt.high,"EF extents");}
};
struct InBuf {
 int fd;U pos,end;std::array<U,BUF> buf;U index=0,size=0;
 InBuf(int file,U at,U length):fd(file),pos(at),end(at+8*length){}
 U get(){if(index==size){need(pos<end,"truncated posting bits");size=std::min<U>(BUF,(end-pos)/8);transfer(fd,false,pos,buf.data(),8*size);pos+=8*size;index=0;}return buf[index++];}
 bool empty()const{return pos==end && index==size;}
};
template<class Visit> void decode(int fd,const Layout& s,U universe,const rawview::Stop& stop,Visit visit){
 InBuf low(fd,s.low_at,s.fmt.low),high(fd,s.high_at,s.used_high);U bits=0,seen=0,prev=0;unsigned have=0,l=s.fmt.l;
 for(U w=0;w<s.used_high;++w){if(!(w%65536))stop.check();for(U m=high.get();m;m&=m-1){
   if(!(seen%65536))stop.check();
   U select=narrow(Big(w)*64+std::countr_zero(m));need(seen<s.count && select>=seen,"excess high bits");
   U v=bits;if(have<l){unsigned want=l-have;U next=low.get();v|=next<<have;bits=next>>want;have=64-want;}
   else {bits>>=l;have-=l;}
   U x=narrow((Big(select-seen)<<l)|(v&mask(l)));need(x<universe && (!seen || prev<x),"malformed EF position");visit(x);prev=x;++seen;
  }}need(seen==s.count && high.empty() && low.empty() && !bits,"posting count/low padding");
}
// Virtual layout: one 32-slot base page PER RANK (even non-heads); an owned stride s
// at a head gets floor((s-1)/32) EXTRA pages. Thus pos -> (rank,offset) without a
// dictionary start vector or a cursor per owned byte. 1=extra, 0=rank's base page.
struct Pages {
 struct Anchor {U extra,base;};std::vector<U> flags;std::vector<Anchor> dir;U count=0;
 static Big bytes(U p){return Big(8)*divup(p,64)+Big(sizeof(Anchor))*divup(p,ANCHOR);}
 void allocate(U p){need(p && Big(p)*PAGE<=UINT64_MAX,"virtual page universe");count=p;flags.assign(divup(p,64),0);dir.resize(divup(p,ANCHOR));}
 void extras(U start,U len,const rawview::Stop& stop){need(start<=count && len<=count-start,"extra page bounds");
  for(U end=start+len;start<end;){if(len>65536 && !(start%65536))stop.check();unsigned b=start%64,num=std::min<U>(64-b,end-start);U v=mask(num)<<b;
   need(!(__atomic_fetch_or(&flags[start/64],v,__ATOMIC_RELAXED)&v),"duplicate extra page");start+=num;}}
 void finish(U m,const rawview::Stop& stop){U extras=0,base=0;
  for(U w=0;w<flags.size();++w){if(!(w%65536))stop.check();if(w%8==0)dir[w/8]={extras,base};
   U valid=mask(std::min<U>(64,count-64*w)),x=flags[w];need(!(x&~valid),"extra padding");extras+=std::popcount(x);
   if(U bases=(~x)&valid)base=64*w+std::bit_width(bases)-1;}
  need(!(flags[0]&1) && count-extras==m,"rank base coverage");}
 static std::pair<U,U> in_anchor(U token,const U* flags,Anchor anchor){U page=token/PAGE,w=page/64,first=w-w%8;
  U x=flags[w-first],part=mask(unsigned(page%64+1)),extra=anchor.extra,base=anchor.base;
  for(U j=first;j<w;++j){U f=flags[j-first];extra+=std::popcount(f);if(~f)base=64*j+std::bit_width(~f)-1;}
  extra+=std::popcount(x&part);if(U f=(~x)&part)base=64*w+std::bit_width(f)-1;
  need(base<=page && extra<=page,"invalid page anchor");return {page+1-extra,PAGE*(page-base)+(token%PAGE)};}
 std::pair<U,U> locate(U token)const{U page=token/PAGE;need(page<count,"token outside page index");U z=page/ANCHOR;return in_anchor(token,&flags[8*z],dir[z]);}
};
}
