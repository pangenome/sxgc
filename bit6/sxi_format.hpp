#pragma once
// SXI1 v1: see SXI_FORMAT.md. All integers on disk are little endian.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace sxi {
using U=uint64_t;
inline void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
inline U get(const unsigned char* p,unsigned bytes=8){U v=0;for(unsigned i=0;i<bytes;i++)v|=U(p[i])<<(8*i);return v;}
inline void put(unsigned char* p,U v,unsigned bytes=8){for(unsigned i=0;i<bytes;i++)p[i]=v>>(8*i);}
inline uint32_t crc(uint32_t v,const unsigned char* p,size_t n){
    static const auto table=[] {std::array<uint32_t,256> t{};for(unsigned i=0;i<256;i++){uint32_t x=i;for(int k=0;k<8;k++)x=(x>>1)^(0xedb88320U&-(x&1));t[i]=x;}return t;}();
    for(size_t i=0;i<n;i++)v=table[(v^p[i])&255]^(v>>8);
    return v;
}
inline void read(std::istream& f,void* p,size_t n){check(bool(f.read((char*)p,n)),"SXI: truncated input");}
inline U integer(std::istream& f,unsigned bytes=8){unsigned char b[8];read(f,b,bytes);return get(b,bytes);}
inline U size(std::ifstream& f){f.seekg(0,std::ios::end);auto n=f.tellg();check(n>=0,"SXI: stat input");f.seekg(0);return U(n);}
struct Member {uint32_t id=0,codec=0;U offset=0,bytes=0,count=0;uint32_t checksum=0;};
struct Container {
    U n=0,k=0,r=0,flags=0;std::vector<Member> members;
    const Member& member(unsigned id)const{for(auto& m:members)if(m.id==id)return m;throw std::runtime_error("SXI: missing member");}
    explicit Container(const std::string& path,bool verify=true){
        std::ifstream f(path,std::ios::binary);check(bool(f),"SXI: open");U length=size(f);
        unsigned char b[64];read(f,b,64);check(!memcmp(b,"SXI1",4)&&get(b+4,4)==1,"SXI: bad magic/version");
        n=get(b+8);k=get(b+16);r=get(b+24);U count=get(b+32,4),hs=get(b+36,4);flags=get(b+48);
        check(n&&n<UINT64_MAX&&r&&r<=n&&k<=n&&r<=UINT32_MAX,"SXI: invalid n/k/r");
        check(count>=5&&count<=6&&hs==64+40*count&&get(b+40)==length&&flags<=1&&!get(b+60,4),"SXI: invalid header");
        uint32_t wanted=get(b+56,4);put(b+56,0,4);uint32_t crcval=crc(~0U,b,64);U end=hs;
        for(U i=0;i<count;i++){
            unsigned char d[40];read(f,d,40);crcval=crc(crcval,d,40);
            Member m{uint32_t(get(d,4)),uint32_t(get(d+4,4)),get(d+8),get(d+16),get(d+24),uint32_t(get(d+32,4))};
            check(m.id==i+1&&m.codec==m.id&&!get(d+36,4),"SXI: unsupported/duplicate member");
            check(end<=UINT64_MAX-7&&m.offset==(end+7)/8*8&&m.offset<=length&&m.bytes<=length-m.offset,"SXI: overlapping/out-of-bounds member");
            end=m.offset+m.bytes;members.push_back(m);
        }
        check(~crcval==wanted&&end==length,"SXI: header CRC or trailing bytes");
        check(member(1).count==r&&member(1).bytes==2048+5*r&&member(2).count==r&&member(3).count==r&&member(3).bytes==8*r,"SXI: run/sample sizes");
        check(member(4).count<=k&&member(4).count<=(UINT64_MAX-12)/16&&member(4).bytes==12+16*member(4).count,"SXI: anchor size");
        check(member(5).count<=n+1&&member(5).count<=UINT64_MAX/10&&member(5).bytes>=member(5).count&&member(5).bytes<=10*member(5).count,"SXI: chi size");
        check(flags==1||(member(5).count==0&&member(5).bytes==0),"SXI: unfinished chi");
        if(members.size()==6)check(member(6).count==member(6).bytes,"SXI: names size");
        if(verify){std::vector<unsigned char> buf(1<<20);for(auto& m:members){f.seekg(m.offset);U left=m.bytes;uint32_t c=~0U;while(left){size_t z=std::min<U>(left,buf.size());read(f,buf.data(),z);c=crc(c,buf.data(),z);left-=z;}check(~c==m.checksum,"SXI: member CRC mismatch");}}
        f.seekg(member(2).offset);U bits=integer(f);unsigned w=integer(f,1);
        check(w&&w<=64&&bits==r*w&&member(2).bytes==9+((bits+63)/64)*8,"SXI: packed tail size");
        f.seekg(member(4).offset);check(integer(f,4)==0x434e4158&&integer(f)==member(4).count,"SXI: anchor header");
    }
};
inline bool is_sxi(const std::string& path){std::ifstream f(path,std::ios::binary);char b[4]{};f.read(b,4);return !memcmp(b,"SXI1",4);}
}
