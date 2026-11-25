#pragma once
#include "finite_route64.hpp"
// Count whole lexical waves first. This policy is fit-only: never begin a partial stream
// hoping that a later oversized range or a ninth complete walk can be accommodated.
namespace finiteepoch {
using namespace route64;
inline constexpr U LIMIT=8;
struct Part {U lo=0,hi=0,bytes=0,waves=0;};
struct Plan {std::vector<Part> part;U size=0,total=0,peak=0,waves=0;};
template<class Geometry>Plan plan(U bands,U team,U slots,U cap,Geometry shape,U max_parts=LIMIT){need(bands && team && slots && cap,"empty epoch geometry");Plan p;
 for(U lo=0,hi;lo<bands;lo=hi){U used=0,bytes=0;for(hi=lo;hi<bands && hi-lo<team;++hi){auto [d,w]=shape(hi);need(d && w && d<=slots,"epoch range exceeds slots before open");if(d>slots-used)break;used+=d;bytes=narrow(Big(bytes)+w);}
  need(hi>lo && bytes<=cap,"whole epoch wave exceeds scratch before open");if(!p.size || bytes>cap-p.part[p.size-1].bytes){need(p.size<max_parts,"epoch walk bound before open");p.part.push_back({lo,lo,0,0});++p.size;}
  auto& z=p.part[p.size-1];z.hi=hi;z.bytes+=bytes;++z.waves;p.peak=std::max(p.peak,z.bytes);p.total=narrow(Big(p.total)+bytes);++p.waves;
 }return p;}
// One producer-created inode. All sequential decoders and publisher lanes have joined
// before release. Capacity is charged for the ENTIRE experiment, not reclaimed on close.
class Slot {
 const char* path;int fd=-1;struct stat id{};U cap,current=0;enum class State{idle,writing,reading};State state=State::idle;
 void match(int f)const{struct stat a{},b{};need(f>=0 && !fstat(f,&a) && !lstat(path,&b) && S_ISREG(b.st_mode) && a.st_dev==id.st_dev && a.st_ino==id.st_ino && b.st_dev==id.st_dev && b.st_ino==id.st_ino && a.st_nlink==1 && b.st_nlink==1 && a.st_size>=0 && U(a.st_size)==current,"rolling slot identity/extent");}
 public:
 Slot(const char* name,U bound):path(name),cap(bound){fd=open(name,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);need(fd>=0 && !fstat(fd,&id),"fresh rolling slot required");match(fd);}
 Slot(const Slot&)=delete;~Slot(){need(state==State::idle && fd>=0 && !close(fd),"rolling slot final close");}
 int begin(U bytes){need(state==State::idle && bytes && bytes<=cap && bytes<=INT64_MAX,"rolling capacity/lifetime");match(fd);
  // Clear any old unwritten high-stream suffix, including when the next layout has exactly the same length.
  need(!ftruncate(fd,0) && !ftruncate(fd,off_t(bytes)),"rolling slot truncate");current=bytes;state=State::writing;return fd;}
 void publish(int& reader,bool durable=true){need(state==State::writing && reader==-1,"rolling slot lifetime");
  if(durable){need(!fsync(fd),"rolling slot sync");match(fd);reader=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);match(reader);}
  else {reader=dup(fd);need(reader>=0,"open temporary posting reader");} // encode workers have joined; pread sees their writes
  state=State::reading;}
 void release(int& reader){need(state==State::reading,"missing rolling reader");match(reader);need(!close(reader),"rolling reader close");reader=-1;state=State::idle;}
};
}
