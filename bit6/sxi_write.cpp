// g++ -O3 -std=c++17 -Wall -Wextra bit6/sxi_write.cpp -o sxi_write
#include "sxi_format.hpp"
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <map>
using namespace sxi;
struct Output {
    std::string temp,path;std::fstream f;std::vector<Member> members;bool done=false;
    Output(const std::string& p,unsigned count):temp(p+".partial"),path(p){
        check(access(path.c_str(),F_OK)!=0,"output exists");int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0666);check(fd>=0,"create partial");close(fd);
        f.open(temp,std::ios::in|std::ios::out|std::ios::binary);check(bool(f),"open partial");std::vector<char> zero(64+40*count);f.write(zero.data(),zero.size());
    }
    ~Output(){if(!done)std::remove(temp.c_str());}
    void begin(unsigned id,U count){while(U(f.tellp())%8)f.put(0);members.push_back({id,id,U(f.tellp()),0,count,~0U});}
    void write(const void* p,size_t n){auto& m=members.back();f.write((const char*)p,n);check(bool(f),"write output");m.bytes+=n;m.checksum=crc(m.checksum,(const unsigned char*)p,n);}
    void number(U v,unsigned n=8){unsigned char b[8];put(b,v,n);write(b,n);}
    void copy(const std::string& path,U offset,U bytes){std::ifstream in(path,std::ios::binary);check(bool(in),"open member");in.seekg(offset);std::vector<unsigned char>b(1<<20);while(bytes){size_t z=std::min<U>(bytes,b.size());read(in,b.data(),z);write(b.data(),z);bytes-=z;}}
    void finish(U n,U k,U r,bool complete,U metadata){
        U total=f.tellp();std::vector<unsigned char>h(64+40*members.size());memcpy(h.data(),"SXI1",4);put(h.data()+4,1,4);put(h.data()+8,n);put(h.data()+16,k);put(h.data()+24,r);put(h.data()+32,members.size(),4);put(h.data()+36,h.size(),4);put(h.data()+40,total);put(h.data()+48,U(complete)|metadata);
        for(size_t i=0;i<members.size();i++){auto&m=members[i];m.checksum=~m.checksum;auto*d=h.data()+64+40*i;put(d,m.id,4);put(d+4,m.codec,4);put(d+8,m.offset);put(d+16,m.bytes);put(d+24,m.count);put(d+32,m.checksum,4);fprintf(stderr,"SXI_MEMBER id=%u count=%llu bytes=%llu crc32=%08x\n",m.id,(unsigned long long)m.count,(unsigned long long)m.bytes,m.checksum);}
        put(h.data()+56,~crc(~0U,h.data(),h.size()),4);f.seekp(0);f.write((char*)h.data(),h.size());f.flush();check(bool(f),"flush output");f.close();
        // Atomic no-clobber publication, including a competing writer.
        check(link(temp.c_str(),path.c_str())==0,"publish output (exists?)");check(unlink(temp.c_str())==0,"unlink partial");done=true;
        auto&m=members[4];fprintf(stderr,"SXI_PASS n=%llu runs=%llu bytes=%llu chi_complete=%d chi_delta_ratio=%.9f\n",(unsigned long long)n,(unsigned long long)r,(unsigned long long)total,complete,m.count?double(m.bytes)/(8.0*m.count):0);
    }
};
#ifndef SXI_WRITE_TEST
int main(int argc,char**argv){try{
    std::map<std::string,std::string>a;for(int i=1;i<argc;i+=2){check(i+1<argc,"usage: sxi_write --ri4 IN --heads RAW --output OUT [--chi RAW] [--anchors XANC] [--names TSV]");std::string key=argv[i];check(key=="--ri4"||key=="--heads"||key=="--output"||key=="--chi"||key=="--anchors"||key=="--names"||key=="--mode"||key=="--orientation"||key=="--remap","unknown option");check(a.emplace(key,argv[i+1]).second,"duplicate option");}
    check(a.count("--ri4")&&a.count("--heads")&&a.count("--output"),"need --ri4 --heads --output");
    std::ifstream f(a["--ri4"],std::ios::binary);check(bool(f),"open ri4");U filebytes=size(f);check(integer(f)==0x0000000452585349ULL,"need ri4 v4");U n=integer(f),k=integer(f),r=integer(f);check(n&&n<UINT64_MAX&&r&&r<=n&&k<=n&&r<=(UINT64_MAX-2089)/8,"invalid n/k/r");
    U tailoff=2080+5*r;check(tailoff+9<=filebytes,"truncated runs");f.seekg(tailoff);U bits=integer(f);unsigned w=integer(f,1);check(w&&w<=64&&r<=UINT64_MAX/w&&bits==r*w,"bad samples");check(bits<=UINT64_MAX-63,"sample bits overflow");U tailbytes=9+((bits+63)/64)*8;check(filebytes==tailoff+tailbytes,"truncated/trailing ri4");
    std::array<U,256> supplied{},totals{};f.seekg(32);for(auto&v:supplied)v=integer(f);
    // Two sequential streams avoid O(r) run-table duplication.
    std::ifstream chars(a["--ri4"],std::ios::binary);chars.seekg(2080);f.seekg(2080+r);
    U sum=0;for(U i=0;i<r;i++){unsigned c=integer(chars,1);U len=integer(f,4);check(len&&len<=n-sum,"bad run length");sum+=len;totals[c]+=len;}check(sum==n,"bad run sum");sum=0;for(unsigned c=0;c<256;c++){check(supplied[c]==sum,"bad C table");sum+=totals[c];}
    std::ifstream heads(a["--heads"],std::ios::binary);check(bool(heads)&&size(heads)==8*r,"bad head size");
    // Decode packed tails sequentially with a two-word window; no text walk.
    f.seekg(tailoff+9);__uint128_t reservoir=0;unsigned available=0;
    std::ifstream lengths(a["--ri4"],std::ios::binary);lengths.seekg(2080+r);
    for(U i=0;i<r;i++){if(available<w){reservoir|=__uint128_t(integer(f))<<available;available+=64;}U v=U(reservoir&((__uint128_t(1)<<w)-1));check(v<n,"tail out of range (INF forbidden)");U h=integer(heads),len=integer(lengths,4);check(h<n,"head out of range");check(len!=1||h==n-1-v,"singleton head/tail mismatch");reservoir>>=w;available-=w;}

    U anchors=0,anchorbytes=12;if(a.count("--anchors")){std::ifstream q(a["--anchors"],std::ios::binary);check(bool(q),"open anchors");anchorbytes=size(q);check(integer(q,4)==0x434e4158,"bad anchor magic");anchors=integer(q);check(anchors<=k&&anchorbytes==12+16*anchors,"bad anchor size");U last=0;for(U i=0;i<anchors;i++){U row=integer(q),v=integer(q);check(row<n&&v<n&&(!i||row>last),"bad anchor");last=row;}}
    std::vector<U> chi;if(a.count("--chi")){std::ifstream q(a["--chi"],std::ios::binary);check(bool(q),"open chi");U bytes=size(q);check(bytes%8==0&&bytes/8<=n+1&&bytes<140000000000ULL,"chi size/budget");chi.resize(bytes/8);for(auto&v:chi){v=integer(q);check(v<=n,"chi out of range");}std::sort(chi.begin(),chi.end());check(std::adjacent_find(chi.begin(),chi.end())==chi.end(),"chi contains duplicates");}
    U metadata=8;
    if(a.count("--mode")){check(a["--mode"]=="dna"||a["--mode"]=="text","invalid mode");if(a["--mode"]=="dna")metadata|=2;}
    if(a.count("--orientation")){check(a["--orientation"]=="forward"||a["--orientation"]=="reversed","invalid orientation");if(a["--orientation"]=="reversed")metadata|=4;}
    if(a.count("--names")){std::ifstream q(a["--names"],std::ios::binary);check(bool(q),"open names");U bytes=size(q);k=named_records(q,bytes,n);}
    Remap sigma=a.count("--remap")?load_remap(a["--remap"]):identity_remap();
    bool remapped=sigma!=identity_remap();if(remapped)metadata|=16;
    Output out(a["--output"],5+a.count("--names")+remapped);out.begin(1,r);out.copy(a["--ri4"],32,2048+5*r);out.begin(2,r);out.copy(a["--ri4"],tailoff,tailbytes);out.begin(3,r);out.copy(a["--heads"],0,8*r);out.begin(4,anchors);if(a.count("--anchors"))out.copy(a["--anchors"],0,anchorbytes);else{out.number(0x434e4158,4);out.number(0);}
    out.begin(5,chi.size());U prev=0;for(U v:chi){U d=v-prev;do{unsigned char b=d&127;d>>=7;if(d)b|=128;out.write(&b,1);}while(d);prev=v;}
    if(a.count("--names")){std::ifstream q(a["--names"],std::ios::binary);check(bool(q),"open names");U bytes=size(q);out.begin(6,bytes);out.copy(a["--names"],0,bytes);}
    if(remapped){out.begin(7,256);out.write(sigma.data(),sigma.size());}
    out.finish(n,k,r,a.count("--chi"),metadata);return 0;
}catch(const std::exception&e){fprintf(stderr,"FATAL: %s\n",e.what());return 1;}}
#endif
