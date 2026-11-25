#pragma once
#include "finite_route64.hpp"
#include <condition_variable>
#include <mutex>
#include <poll.h>
#include <thread>
namespace finiteemit {
using namespace route64;
// Sparse in time, bounded in space: each nonempty word has a presence bit in its parent.
// Drain in increasing one-based successor rank and clear ONLY touched words; no O(m) reset per tie.
class Marks {
 std::vector<std::vector<U>> lev;U m=0;
 public:
 static Big bytes(U m){Big total=0;do{m=divup(m,64);total+=8*Big(m);}while(m>1);return total;}
 explicit Marks(U ranks):m(ranks){need(m,"empty successor universe");U size=m;do{size=divup(size,64);lev.emplace_back(size,0);}while(size>1);}
 void add(U rank){need(rank && rank<=m,"invalid tie continuation");U i=rank-1;for(auto& level:lev){U w=i/64,mask=U(1)<<(i%64),old=level[w];need(!(old&mask),"duplicate successor in tie");level[w]=old|mask;if(old)break;i=w;}}
 template<class F>void drain(F f){std::array<U,11> word{},bits{};unsigned top=unsigned(lev.size()-1),l=top;need(top<bits.size(),"rank bitmap levels");bits[l]=lev[l][0];lev[l][0]=0;
  for(;;){if(!bits[l]){if(l==top)break;++l;continue;}U i=64*word[l]+std::countr_zero(bits[l]);bits[l]&=bits[l]-1;
   if(!l){f(i+1);continue;}--l;word[l]=i;bits[l]=lev[l][i];need(bits[l],"missing marked child");lev[l][i]=0;}}
};
struct Row {uint32_t p,h,tag;};static_assert(sizeof(Row)==12);
inline Row row(U p,U h,unsigned b){need(p<(U(1)<<43) && h<(U(1)<<43) && b<=256,"row43 wire extent");return {uint32_t(p),uint32_t(h),0x80000000u|(uint32_t(h>>32)<<20)|(uint32_t(p>>32)<<9)|b};}
struct Sink {U writes=0,again=0,bytes=0;double wait=0,send_s=0;};
inline void send(int fd,const Row* rows,U count,const rawview::Stop& stop,Sink& s){double start=omp_get_wtime();const char* p=(const char*)rows;U len=sizeof(Row)*count;
 while(len){stop.check();ssize_t z=write(fd,p,len);if(z>0){p+=z;len-=U(z);++s.writes;s.bytes+=U(z);continue;}if(z<0 && errno==EINTR)continue;
  need(z<0 && (errno==EAGAIN || errno==EWOULDBLOCK),"row sink closed");++s.again;pollfd out{fd,POLLOUT,0};int rc=poll(&out,1,100);need(rc>=0 || errno==EINTR,"row sink poll");}s.send_s+=omp_get_wtime()-start;}
// One producer and one ordered consumer. Stalled later bands can hold at most Q packets each.
// A packet is filled in place, not copied through a heap of rows.
class Pipe {
 static constexpr U Q=3,K=2048;std::array<std::array<Row,K>,Q> slots;std::array<U,Q> sizes{};
 std::mutex mu;std::condition_variable cv;U made=0,used=0,fill=0;bool done=false;rawview::Stop stop;
 void publish(){if(fill){std::lock_guard<std::mutex> lock(mu);sizes[made%Q]=fill;++made;fill=0;cv.notify_all();}}
 public:
 double blocked=0;
 explicit Pipe(rawview::Stop guard):stop(guard){}
 void put(Row r){if(!fill){std::unique_lock<std::mutex> lock(mu);if(made-used==Q){double t=omp_get_wtime();do{stop.check();cv.wait_for(lock,std::chrono::milliseconds(50));}while(made-used==Q);blocked+=omp_get_wtime()-t;}}
  slots[made%Q][fill++]=r;if(fill==K)publish();}
 void finish(){publish();std::lock_guard<std::mutex> lock(mu);done=true;cv.notify_all();}
 U cat(int fd,U expected,Sink& s){U rows=0;for(;;){std::unique_lock<std::mutex> lock(mu);if(used==made && !done){double t=omp_get_wtime();do{stop.check();cv.wait_for(lock,std::chrono::milliseconds(50));}while(used==made && !done);s.wait+=omp_get_wtime()-t;}
   if(used==made){need(done && rows==expected,"incomplete ordered band");return made;}
   U i=used%Q,n=sizes[i];need(n && rows<=expected && n<=expected-rows,"excess band rows");lock.unlock();send(fd,slots[i].data(),n,stop,s);rows+=n;
   lock.lock();++used;cv.notify_all();}}
};
}
