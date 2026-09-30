// Publish-only SXI1 -> SXI2 transcode. Never visits text or BWT rows.
// g++ -O3 -std=c++17 bit6/sxi2_write.cpp -o sxi2_write
#include "sxi_format.hpp"
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <queue>
#include <numeric>
#include <map>
#include <cmath>
#include <tuple>
#include <sys/wait.h>
using namespace sxi;
struct Bits {
    std::ofstream f; unsigned char byte=0; unsigned fill=0; U count=0;
    explicit Bits(const std::string& p):f(p,std::ios::binary|std::ios::trunc){check(bool(f),"bit temp");}
    void bit(unsigned b){byte|=(b&1)<<fill++;++count;if(fill==8){f.put(byte);byte=fill=0;}}
    void msb(U v,unsigned w){for(unsigned i=w;i--;)bit((v>>i)&1);}
    void lsb(U v,unsigned w){for(unsigned i=0;i<w;i++)bit((v>>i)&1);}
    void gamma(U v){check(v>0,"zero gamma");unsigned w=64-__builtin_clzll(v);for(unsigned i=1;i<w;i++)bit(0);msb(v,w);}
    void finish(){if(fill)f.put(byte);f.close();check(bool(f),"bit flush");}
};
struct Out {
    std::string path,temp,chi_check;std::fstream f;std::vector<Member> ms;bool done=false;
    Out(const std::string& p,unsigned count):path(p),temp(p+".partial"){
        check(access(path.c_str(),F_OK)!=0,"output exists");
        int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0666);check(fd>=0,"partial exists");close(fd);
        f.open(temp,std::ios::binary|std::ios::in|std::ios::out);
        std::vector<char> z(64+40*count);f.write(z.data(),z.size());
    }
    ~Out(){if(!chi_check.empty())unlink(chi_check.c_str());if(!done)unlink(temp.c_str());}
    void begin(unsigned id,unsigned codec,U count){while(U(f.tellp())%8)f.put(0);ms.push_back({id,codec,U(f.tellp()),0,count,~0U});}
    void write(const void* p,size_t z){f.write((const char*)p,z);check(bool(f),"write member");auto& m=ms.back();m.bytes+=z;m.checksum=crc(m.checksum,(const unsigned char*)p,z);}
    void num(U v,unsigned w=8){check(w<=8,"number width");unsigned char b[8]{};put(b,v,w);write(b,w);}
    void copy(const std::string& p,U off,U n){std::ifstream in(p,std::ios::binary);check(bool(in),"open copy");in.seekg(off);std::vector<unsigned char>b(1<<20);while(n){size_t z=std::min<U>(n,b.size());read(in,b.data(),z);write(b.data(),z);n-=z;}}
    void finish(U n,U k,U r,U flags,const std::string& validator,const std::string& source,const Member& source_chi){
        U length=f.tellp();std::vector<unsigned char>h(64+40*ms.size());
        memcpy(h.data(),"SXI2",4);put(h.data()+4,2,4);put(h.data()+8,n);put(h.data()+16,k);put(h.data()+24,r);
        put(h.data()+32,ms.size(),4);put(h.data()+36,h.size(),4);put(h.data()+40,length);put(h.data()+48,flags);
        for(size_t i=0;i<ms.size();i++){auto&m=ms[i];auto*d=h.data()+64+40*i;
            put(d,m.id,4);put(d+4,m.codec,4);put(d+8,m.offset);put(d+16,m.bytes);put(d+24,m.count);
            put(d+32,~m.checksum,4);
            fprintf(stderr,"SXI2_MEMBER id=%u codec=%u count=%llu bytes=%llu\n",m.id,m.codec,(unsigned long long)m.count,(unsigned long long)m.bytes);
        }
        put(h.data()+56,~crc(~0U,h.data(),h.size()),4);f.seekp(0);f.write((char*)h.data(),h.size());f.flush();check(bool(f),"flush");
        int fd=open(temp.c_str(),O_RDONLY);check(fd>=0&&fsync(fd)==0,"fsync");close(fd);f.close();
        chi_check=temp+".chi-check";
        check(access(chi_check.c_str(),F_OK)!=0,"chi check exists");
        pid_t child=fork();check(child>=0,"fork validator");
        if(child==0){execl(validator.c_str(),validator.c_str(),"sxi-info",temp.c_str(),"--chi-out",chi_check.c_str(),(char*)nullptr);_exit(127);}
        int status=0;check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"SXI2 validator rejected partial");
        std::ifstream original(source,std::ios::binary),decoded(chi_check,std::ios::binary);
        check(bool(original)&&bool(decoded)&&size(decoded)==8*source_chi.count,"chi validation size");
        original.seekg(source_chi.offset);U previous=0,used=0;
        for(U i=0;i<source_chi.count;i++){U delta=0;unsigned shift=0;
            for(;;){check(used++<source_chi.bytes,"source chi truncated");unsigned b=integer(original,1);
                delta|=U(b&127)<<shift;if(!(b&128))break;shift+=7;check(shift<64,"source chi overflow");}
            previous+=delta;check(integer(decoded)==previous,"SXI2 EF chi byte mismatch");
        }
        check(used==source_chi.bytes,"source chi trailing");
        check(unlink(chi_check.c_str())==0,"unlink chi check");chi_check.clear();
        check(link(temp.c_str(),path.c_str())==0,"publish exists");check(unlink(temp.c_str())==0,"unlink partial");done=true;
        fprintf(stderr,"SXI2_PASS bytes=%llu\n",(unsigned long long)length);
    }
};
struct Temp {
    std::vector<std::string> paths;
    ~Temp(){for(const auto& p:paths)unlink(p.c_str());}
    std::string add(const std::string& base,const char* suffix){auto p=base+suffix;check(access(p.c_str(),F_OK)!=0,"bit temp exists");paths.push_back(p);return p;}
};
struct Edge {U u,v;uint32_t run,pad=0;bool operator<(const Edge& x)const{return u<x.u;}};
int main(int argc,char**argv){try{
    std::string src,dst,escape,validator;U max_bytes=0;
    for(int i=1;i<argc;i+=2){check(i+1<argc,"usage: sxi2_write --sxi SXI1 --output SXI2 [--escape SXESC3]");
        std::string key=argv[i];check(key=="--sxi"||key=="--output"||key=="--escape"||key=="--validator"||key=="--max-bytes","unknown option");
        if(key=="--max-bytes"){check(!max_bytes,"duplicate max bytes");max_bytes=std::stoull(argv[i+1]);continue;}
        auto& value=key=="--sxi"?src:key=="--output"?dst:key=="--escape"?escape:validator;
        check(value.empty(),"duplicate option");value=argv[i+1];}
    check(!src.empty()&&!dst.empty()&&!validator.empty()&&src!=dst,"need distinct --sxi --output and --validator");
    Container c(src);check(c.flags&1,"SXI2 needs completed chi");U n=c.n,r=c.r;
    check(!max_bytes||24*r<=max_bytes,"SXI2 phi member alone exceeds --max-bytes");
    std::ifstream in(src,std::ios::binary);
    auto&m1=c.member(1);in.seekg(m1.offset);std::array<U,256>C{};for(auto&v:C)v=integer(in);
    std::array<U,256> freq{};for(U i=0;i<r;i++){unsigned ch=integer(in,1);++freq[ch];}
    struct Node{U weight;int left,right,sym;};
    std::vector<Node> tree;using Q=std::pair<U,int>;std::priority_queue<Q,std::vector<Q>,std::greater<Q>> q;
    for(int i=0;i<256;i++)if(freq[i]){q.push({freq[i],int(tree.size())});tree.push_back({freq[i],-1,-1,i});}
    check(!q.empty(),"empty alphabet");if(q.size()==1){auto one=q.top();q.pop();tree.push_back({one.first,one.second,-1,-1});q.push({one.first,int(tree.size()-1)});}
    while(q.size()>1){auto a=q.top();q.pop();auto b=q.top();q.pop();U w=a.first+b.first;tree.push_back({w,a.second,b.second,-1});q.push({w,int(tree.size()-1)});}
    std::array<unsigned char,256> lens{};auto visit=[&](auto&& self,int i,unsigned depth)->void{
        if(tree[i].sym>=0){check(depth&&depth<=63,"Huffman depth");lens[tree[i].sym]=depth;return;}
        self(self,tree[i].left,depth+1);if(tree[i].right>=0)self(self,tree[i].right,depth+1);
    };visit(visit,q.top().second,0);
    std::array<U,256> codes{};std::vector<std::pair<unsigned,unsigned>> order;
    for(unsigned i=0;i<256;i++)if(lens[i])order.push_back({lens[i],i});
    std::sort(order.begin(),order.end());U code=0;unsigned prev=0;
    for(auto [len,sym]:order){code<<=len-prev;codes[sym]=code++;prev=len;}
    Temp tmp;auto hp=tmp.add(dst,".headbits"),lp=tmp.add(dst,".lenbits"),lowp=tmp.add(dst,".lowbits"),highp=tmp.add(dst,".highbits");
    Bits heads(hp),lengths(lp);
    std::ifstream cs(src,std::ios::binary),ls(src,std::ios::binary);cs.seekg(m1.offset+2048);ls.seekg(m1.offset+2048+r);
    for(U i=0;i<r;i++){unsigned ch=integer(cs,1);U len=integer(ls,4);heads.msb(codes[ch],lens[ch]);lengths.gamma(len);}
    heads.finish();lengths.finish();
    auto& mt=c.member(2);std::ifstream tails(src,std::ios::binary);tails.seekg(mt.offset);
    U bits=integer(tails);unsigned width=integer(tails,1);(void)bits;
    std::vector<Edge> edges;edges.reserve(r);std::ifstream hs(src,std::ios::binary);hs.seekg(c.member(3).offset);
    U first_head=integer(hs);__uint128_t reservoir=0;unsigned available=0;
    for(U i=0;i<r;i++){
        if(available<width){reservoir|=__uint128_t(integer(tails))<<available;available+=64;}
        U mirror=U(reservoir&((__uint128_t(1)<<width)-1));reservoir>>=width;available-=width;
        U v=i+1<r?integer(hs):first_head;edges.push_back({n-1-mirror,v,uint32_t(i)});
    }
    std::sort(edges.begin(),edges.end());for(U i=1;i<r;i++)check(edges[i-1].u<edges[i].u,"duplicate phi domain");
    U gcd=0;std::array<U,256> counts{};
    // Frequency gcd comes from C differences; the final bucket ends at n.
    for(unsigned i=0;i<256;i++)counts[i]=(i==255?n:C[i+1])-C[i];
    for(U v:counts)if(v)gcd=std::gcd(gcd,v);
    std::vector<std::tuple<U,U,U>> failures;
    for(U i=0;i<r;i++){U next=edges[(i+1)%r].u;U domain=next>edges[i].u?next-edges[i].u:n-edges[i].u+next;
        if(gcd!=1&&domain!=1)failures.emplace_back(edges[i].run,edges[i].u,domain);}
    std::sort(failures.begin(),failures.end());U failed=failures.size();
    std::vector<unsigned char> esc;
    if(!escape.empty()){std::ifstream e(escape,std::ios::binary);check(bool(e),"open escape");U z=size(e);check(z<=n*8+40+32*r,"escape size");esc.resize(z);read(e,esc.data(),z);}
    check(!failed||!esc.empty(),"periodic nonsingleton phi domains require exact --escape");
    if(esc.empty())check(!failed,"missing escape");
    else {
        check(esc.size()>=40+32*failed&&!memcmp(esc.data(),"SXESC3\0\0",8)
            &&get(esc.data()+8)==n&&get(esc.data()+16)==r&&get(esc.data()+24)==failed,
            "escape header/count");
        unsigned ew=std::max(1,64-__builtin_clzll(n-1?n-1:1));
        check(esc[32]==ew&&std::all_of(esc.begin()+33,esc.begin()+40,[](unsigned char x){return x==0;}),"escape width");
        U endbit=0;
        for(U i=0;i<failed;i++){const auto* p=esc.data()+40+32*i;auto[run,u,len]=failures[i];
            check(get(p)==run&&get(p+8)==u&&get(p+16)==len&&get(p+24)==endbit,"escape domain");
            check(len<=(UINT64_MAX-endbit)/ew,"escape bits overflow");endbit+=len*ew;}
        check(esc.size()==40+32*failed+(endbit+7)/8,"escape payload size");
        if(endbit%8)check(!(esc.back()>>(endbit%8)),"escape padding");
        const unsigned char* payload=esc.data()+40+32*failed;
        for(U bit=0;bit<endbit;bit+=ew){U value=0;for(unsigned j=0;j<ew;j++)value|=U((payload[(bit+j)/8]>>((bit+j)%8))&1)<<j;
            check(value<n,"escape successor range");}
    }
    Out out(dst,c.members.size()+2);
    out.begin(1,101,r);for(U v:C)out.num(v);out.write(lens.data(),lens.size());out.num(heads.count);out.num(lengths.count);
    out.copy(hp,0,(heads.count+7)/8);out.copy(lp,0,(lengths.count+7)/8);
    for(unsigned id=2;id<=4;id++){auto&m=c.member(id);out.begin(id,id,m.count);out.copy(src,m.offset,m.bytes);}
    auto& mc=c.member(5);U chi=mc.count;unsigned l=chi?std::max(0,int(std::log2(double(n+1)/double(chi)))):0;
    if(l>63)l=63;
    U lowbits=chi*l,highbits=chi?((n>>l)+chi+1):0;
    Bits low(lowp),high(highp);std::ifstream ch(src,std::ios::binary);ch.seekg(mc.offset);
    U previous_chi=0,used=0,highpos=0;
    for(U i=0;i<chi;i++){U delta=0;unsigned shift=0;for(;;){check(used++<mc.bytes,"chi truncated");unsigned b=integer(ch,1);delta|=U(b&127)<<shift;if(!(b&128))break;shift+=7;check(shift<64,"chi overflow");}
        U v=previous_chi+delta;check(v<=n&&(i==0||v>previous_chi),"chi order");previous_chi=v;low.lsb(v,l);U mark=(v>>l)+i;while(highpos<mark){high.bit(0);++highpos;}high.bit(1);++highpos;
    }
    check(used==mc.bytes,"chi trailing");while(highpos<highbits){high.bit(0);++highpos;}
    low.finish();high.finish();check(low.count==lowbits&&high.count==highbits,"EF bits");
    out.begin(5,105,chi);out.num(l);out.num(lowbits);out.num(highbits);out.copy(lowp,0,(lowbits+7)/8);out.copy(highp,0,(highbits+7)/8);
    for(unsigned id=6;id<=7;id++)for(const auto&m:c.members)if(m.id==id){out.begin(id,id,m.count);out.copy(src,m.offset,m.bytes);}
    out.begin(8,108,r);for(const auto&e:edges){out.num(e.u);out.num(e.v);out.num(e.run,4);out.num(0,4);}
    out.begin(9,109,failed);if(!esc.empty())out.write(esc.data(),esc.size());
    out.finish(n,c.k,r,c.flags,validator,src,c.member(5));
    return 0;
}catch(const std::exception&e){fprintf(stderr,"SXI2 FATAL: %s\n",e.what());return 1;}}
