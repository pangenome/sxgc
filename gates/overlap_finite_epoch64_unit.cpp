#include "../xsa/pfp/tools/finite_epoch64.hpp"
#include <string_view>
using namespace finiteepoch;
int main(int ac,char** av){need(ac==3,"DIR_FILE MODE");std::string_view mode=av[2];U base=(U(1)<<36)+12345;
 auto p=plan(18,4,4*base,8*(base+8),[&](U){return std::pair(base,base+8);});need(p.waves==5 && p.size==3 && p.total==18*(base+8) && p.peak==8*(base+8),"wide epoch sum/greedy plan");
 if(mode=="nine"){plan(9,1,1,base,[&](U){return std::pair(U(1),base);});return 80;}
 Slot slot(av[1],16384);std::array<uint8_t,16384> data,stored;struct stat id{};int r=-1;
 for(U i=0;i<5;++i){U size=i<2?16384:i==2?64:i==3?8192:1000;int w=slot.begin(size);if(mode=="double")slot.begin(1);if(mode=="rename"){need(!unlink(av[1]),"unit unlink");slot.publish(r);return 80;}
  data.fill(i?0:0xa3);if(!i)transfer(w,true,0,data.data(),size);else{data[size-1]=uint8_t(i);transfer(w,true,size-1,&data[size-1],1);}slot.publish(r);
  if(mode=="reuse")slot.begin(1);
  struct stat cur{};need(!lstat(av[1],&cur) && U(cur.st_size)==size && (!i || (cur.st_dev==id.st_dev && cur.st_ino==id.st_ino)),"slot replaced");id=cur;stored.fill(0x82);transfer(r,false,0,stored.data(),size);need(!std::memcmp(data.data(),stored.data(),size),"old bytes leaked");slot.release(r);need(r==-1,"reader still open");}
 need(mode=="ok","unknown mode");std::puts("PASS >u32 wave/offset plan and same named inode at unchanged, shrinking and regrown extents with zero stale data");}
