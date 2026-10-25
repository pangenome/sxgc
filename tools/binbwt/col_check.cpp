// col_check - verify an adjacent-rank LCP column against brute force.
//   col_check --corpus C --remap R --sa SA --col COL
// For every rank r>0: col[r] must equal LCE(SA[r-1], SA[r]) in the remapped
// alphabet (escapes carry values >= 255).
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t;
static void die(const char* m){ std::fprintf(stderr,"col_check: %s\n",m); std::exit(2); }
static const u8 *T=nullptr, *RMP=nullptr; static u64 N=0;
static inline int sym(u64 p){ u8 c=T[p]; return RMP?RMP[c]:c; }
static inline u64 lce(u64 a,u64 b){
    const u64 cap=std::min(N-a,N-b); u64 i=0;
    while(i<cap && sym(a+i)==sym(b+i)) ++i;
    return i;
}
int main(int argc,char**argv){
    std::string corpus,sap,colp,remap;
    for(int i=1;i<argc;++i){ std::string a=argv[i];
        auto nxt=[&]{ if(i+1>=argc) die("missing value"); return std::string(argv[++i]); };
        if(a=="--corpus")corpus=nxt(); else if(a=="--sa")sap=nxt();
        else if(a=="--col")colp=nxt(); else if(a=="--remap")remap=nxt();
        else die("unknown arg"); }
    int fd=::open(corpus.c_str(),O_RDONLY); struct stat st{};
    if(fd<0||::fstat(fd,&st)) die("corpus"); N=(u64)st.st_size;
    T=(const u8*)::mmap(nullptr,N,PROT_READ,MAP_PRIVATE,fd,0); if(T==MAP_FAILED) die("mmap corpus");
    std::vector<u8> rb;
    if(!remap.empty()){ int r=::open(remap.c_str(),O_RDONLY); rb.resize(256);
        if(r<0||::read(r,rb.data(),256)!=256) die("remap"); ::close(r); RMP=rb.data(); }
    int sf=::open(sap.c_str(),O_RDONLY); struct stat ss{};
    if(sf<0||::fstat(sf,&ss)) die("sa"); u64 sb=(u64)ss.st_size;
    const u64* SAm=(const u64*)::mmap(nullptr,sb,PROT_READ,MAP_PRIVATE,sf,0);
    if(SAm==(void*)-1) die("mmap sa");
    const u64* SA=SAm+6;                       // 48-byte header
    int cf=::open(colp.c_str(),O_RDONLY); struct stat cs{};
    if(cf<0||::fstat(cf,&cs)) die("col"); u64 cb=(u64)cs.st_size;
    const u8* C=(const u8*)::mmap(nullptr,cb,PROT_READ,MAP_PRIVATE,cf,0);
    if(C==(void*)-1) die("mmap col");
    u64 cn; std::memcpy(&cn,C+8,8);
    u64 nesc; std::memcpy(&nesc,C+24,8);
    std::fprintf(stderr,"COL_CHECK col_n=%" PRIu64 " escapes=%" PRIu64 " corpus_n=%" PRIu64 "\n",cn,nesc,N);
    std::unordered_map<u64,u64> esc;
    for(u64 i=0;i<nesc;++i){ u64 p[2]; std::memcpy(p,C+48+cn+i*16,16); esc[p[0]]=p[1]; }
    const u8* body=C+48;
    u64 bad=0, checked=0;
    for(u64 r=1;r<cn;++r){
        const u64 want=lce(SA[r-1],SA[r]);
        u64 got=body[r]; auto it=esc.find(r); if(it!=esc.end()) got=it->second;
        if(got!=want){ if(++bad<6) std::fprintf(stderr,"MISMATCH rank=%" PRIu64 " got=%" PRIu64 " want=%" PRIu64 "\n",r,got,want); }
        ++checked;
    }
    std::printf("COL_CHECK_%s checked=%" PRIu64 " bad=%" PRIu64 "\n", bad?"FAIL":"PASS", checked, bad);
    return bad?3:0;
}
