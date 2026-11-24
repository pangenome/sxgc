#pragma once
#include "finite_emit_rows.hpp"
namespace finiteemit {
// One ordered stream of tiles per lane, bounded independently of the number/size of tiles.
// Packet storage is exactly the same 3 x 2048 row capacity as the full-band Pipe.
class TilePipe {
 static constexpr U Q=3,K=2048;struct Header{U tile,size;bool last;};std::array<std::array<Row,K>,Q> data;std::array<Header,Q> headers;
 std::mutex mu;std::condition_variable cv;U made=0,used=0,fill=0,id=0;bool active=false,done=false;rawview::Stop stop;
 void publish(bool last){need(fill && active,"empty tile packet");std::lock_guard<std::mutex> lock(mu);headers[made%Q]={id,fill,last};++made;fill=0;cv.notify_all();}
 public:
 double blocked=0;explicit TilePipe(rawview::Stop s):stop(s){}
 void begin(U i){need(!active && !fill && !done,"overlapping tile writer");active=true;id=i;}
 void put(Row r){need(active,"row outside tile");if(fill==K)publish(false);if(!fill){std::unique_lock<std::mutex> lock(mu);
   if(made-used==Q){double t=omp_get_wtime();do{stop.check();cv.wait_for(lock,std::chrono::milliseconds(50));}while(made-used==Q);blocked+=omp_get_wtime()-t;}}
  data[made%Q][fill++]=r;}
 void finish(){publish(true);active=false;}
 void close(){std::lock_guard<std::mutex> lock(mu);need(!active && !fill,"incomplete tile");done=true;cv.notify_all();}
 U cat(int fd,U wanted,U remaining,Sink& sink,U& packets){U rows=0;for(;;){std::unique_lock<std::mutex> lock(mu);
   if(used==made && !done){double t=omp_get_wtime();do{stop.check();cv.wait_for(lock,std::chrono::milliseconds(50));}while(used==made && !done);sink.wait+=omp_get_wtime()-t;}
   need(used<made,"tile stream ended early");U q=used%Q;Header h=headers[q];need(h.tile==wanted && h.size && h.size<=K && rows<=remaining && h.size<=remaining-rows,"wrong tile/row count");lock.unlock();
   send(fd,data[q].data(),h.size,stop,sink);++packets;rows+=h.size;lock.lock();++used;cv.notify_all();if(h.last)return rows;}}
 void drained()const{need(done && !active && !fill && used==made,"undrained tiles");}
};
}
