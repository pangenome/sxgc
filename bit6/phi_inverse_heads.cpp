// Mine the immutable TeraLCP Phi table; no suffix/LF walk, PLCP, or text scan.
// Build: g++ -O3 -std=c++17 -fopenmp bit6/phi_inverse_heads.cpp -o phi_inverse_heads
// Usage: phi_inverse_heads INDEX RI4 OUTPUT [threads=32]
// Raw little-endian u64 output: head[j] = inverse_phi(tail[(j+R-1)%R]).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <omp.h>
using U=uint64_t;
static void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
struct Map {
    int fd; size_t bytes; const uint8_t* p;
    explicit Map(const char* name){fd=open(name,O_RDONLY);struct stat st{};require(fd>=0&&!fstat(fd,&st),"open/stat input");bytes=st.st_size;require(bytes>0,"empty input");p=(const uint8_t*)mmap(nullptr,bytes,PROT_READ,MAP_PRIVATE,fd,0);require(p!=MAP_FAILED,"mmap input");}
    ~Map(){munmap((void*)p,bytes);close(fd);}
    void range(U o,U n)const{require(o<=bytes&&n<=bytes-o,"truncated input");}
    U u64(U o)const{range(o,8);U x;memcpy(&x,p+o,8);return x;}
    uint8_t u8(U o)const{range(o,1);return p[o];}
};
static U packed(const Map& m,U base,U bit,unsigned w){
    unsigned shift=bit%64;U x=m.u64(base+(bit/64)*8)>>shift;
    if(shift+w>64)x|=m.u64(base+(bit/64+1)*8)<<(64-shift);
    return w==64?x:x&((U(1)<<w)-1);
}
static void skip_iv(const Map& m,U& off){U bits=m.u64(off);unsigned w=m.u8(off+8);require(w>=1&&w<=64,"bad int_vector width");off+=9;U bytes=(bits/64+(bits%64!=0))*8;m.range(off,bytes);off+=bytes;}
struct Triple {
    const Map& m;U base,count;unsigned a,b,c,w;
    Triple(const Map& mm,U& off):m(mm){a=m.u8(off);b=m.u8(off+1);c=m.u8(off+2);w=m.u8(off+3);U bits=m.u64(off+4);require(a&&b&&c&&a<=64&&b<=64&&c<=64&&w==a+b+c&&bits%w==0,"bad packed triple");base=off+12;count=bits/w;U bytes=(bits/64+(bits%64!=0))*8;m.range(base,bytes);off=base+bytes;}
    U get(U i,unsigned field)const{require(i<count,"Phi interval index out of range");return packed(m,base,i*w+(field==0?0:field==1?a:a+b),field==0?a:field==1?b:c);}
};
struct Image {U dest,start,len;};
// Bounded-depth, in-place parallel partitioning; std::sort at leaves preserves
// worst-case O(r log r), without a second full-size sorting allocation.
static void sort_images(Image* b,Image* e,int depth){
    if(e-b<(1<<20)||depth==0){std::sort(b,e,[](const Image& x,const Image& y){return x.dest<y.dest;});return;}
    U v[3]={b->dest,b[(e-b)/2].dest,(e-1)->dest};std::sort(v,v+3);U pivot=v[1];
    Image* mid=std::partition(b,e,[&](const Image& x){return x.dest<pivot;});
    Image* hi=std::partition(mid,e,[&](const Image& x){return x.dest==pivot;});
    #pragma omp task
    sort_images(b,mid,depth-1);
    sort_images(hi,e,depth-1);
    #pragma omp taskwait
}
static auto epoch=std::chrono::steady_clock::now();
static void phase(const char* name){struct rusage ru{};getrusage(RUSAGE_SELF,&ru);fprintf(stderr,"PHI_PHASE %s elapsed=%.6f peak_kib=%ld\n",name,std::chrono::duration<double>(std::chrono::steady_clock::now()-epoch).count(),ru.ru_maxrss);}
int main(int argc,char** argv){try{
    require(argc==4||argc==5,"usage: phi_inverse_heads INDEX RI4 OUTPUT [threads]");
    uint16_t endian=1;require(*(uint8_t*)&endian==1,"little-endian host required");
    int threads=argc==5?std::stoi(argv[4]):32;require(threads>0&&threads<=64,"threads must be 1..64");omp_set_num_threads(threads);
    require(access(argv[3],F_OK)!=0,"output exists; refusing overwrite");
    U n=0;std::vector<Image> images;
    {
        Map idx(argv[1]);n=idx.u64(0);U off=8;skip_iv(idx,off);Triple psi(idx,off);skip_iv(idx,off);Triple phi(idx,off);
        require(phi.count>=2,"empty Phi");U r=phi.count-1;
        require(phi.get(0,2)==0&&phi.get(r,2)==n,"Phi domain must tile [0,n)");
        // Upper bound includes every Phi page plus the entire image array.
        require(r<=SIZE_MAX/sizeof(Image)&&r*sizeof(Image)+(phi.count*phi.w/8)+ (1ULL<<30)<150000000000ULL,"extractor memory budget exceeds 150 GB");
        fprintf(stderr,"PHI_META n=%llu intervals=%llu offset=%llu bits_per_interval=%u image_bytes=%llu threads=%d\n",(unsigned long long)n,(unsigned long long)r,(unsigned long long)phi.base,phi.w,(unsigned long long)(r*sizeof(Image)),threads);
        images.resize(r);
        U invalid=0;
        #pragma omp parallel for reduction(+:invalid) schedule(static)
        for(U i=0;i<r;++i){U start=phi.get(i,2),end=phi.get(i+1,2),target=phi.get(i,0);if(target>=r){++invalid;continue;}U dest=phi.get(target,2),delta=phi.get(i,1);if(start>=end||dest>n||delta>n-dest){++invalid;continue;}dest+=delta;if(end-start>n-dest){++invalid;continue;}images[i]={dest,start,end-start};}
        require(invalid==0,"invalid Phi source/image or target interval");
        phase("decode-images");
    }
    #pragma omp parallel
    {
        #pragma omp single
        sort_images(images.data(),images.data()+images.size(),16);
    }
    phase("sort-images");
    U cursor=0;for(const auto& v:images){require(v.dest==cursor,"Phi images overlap or leave a gap");cursor+=v.len;}require(cursor==n,"Phi images do not cover domain");phase("validate-permutation");
    Map ri(argv[2]);require(ri.u64(0)==0x0000000452585349ULL,"bad ri4 magic/version");require(ri.u64(8)==n,"ri4/index length mismatch");U R=ri.u64(24);require(R>0&&R<=(UINT64_MAX-2080)/5,"invalid run count");U off=2080+5*R,bits=ri.u64(off);unsigned w=ri.u8(off+8);require(w>0&&w<=64&&R<=UINT64_MAX/w&&bits==R*w,"invalid ri4 samples");U base=off+9;ri.range(base,(bits/64+(bits%64!=0))*8);
    fprintf(stderr,"RI4_META n=%llu runs=%llu sample_width=%u\n",(unsigned long long)n,(unsigned long long)R,w);
    std::string tmp=std::string(argv[3])+".partial";int fd=open(tmp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0666);require(fd>=0,"create output partial");
    constexpr U chunk=1<<20;std::vector<U> out(chunk);U bad=0;
    for(U begin=0;begin<R;begin+=chunk){U count=std::min(chunk,R-begin);
        #pragma omp parallel for reduction(+:bad) schedule(static)
        for(U k=0;k<count;++k){U j=begin+k,prev=j?j-1:R-1,sample=packed(ri,base,prev*w,w);if(sample>=n){++bad;continue;}U x=n-1-sample,lo=0,hi=images.size();while(lo+1<hi){U mid=lo+(hi-lo)/2;if(images[mid].dest<=x)lo=mid;else hi=mid;}const auto& v=images[lo];if(x-v.dest>=v.len){++bad;continue;}out[k]=v.start+(x-v.dest);}
        require(bad==0,"invalid tail sample or inverse result");U bytes=count*8;const char* p=(const char*)out.data();while(bytes){ssize_t z=write(fd,p,bytes);require(z>0,"write heads");p+=z;bytes-=z;}
        if(begin%(chunk*64)==0){fprintf(stderr,"PHI_PROGRESS heads=%llu/%llu\n",(unsigned long long)(begin+count),(unsigned long long)R);phase("query");}
    }
    require(close(fd)==0,"close output");require(rename(tmp.c_str(),argv[3])==0,"publish output");phase("complete");fprintf(stderr,"PHI_PASS all_values_in_range=1 permutation_validated=1 lf_steps=0 heads=%llu\n",(unsigned long long)R);return 0;
}catch(const std::exception& e){fprintf(stderr,"FATAL: %s\n",e.what());return 1;}}
