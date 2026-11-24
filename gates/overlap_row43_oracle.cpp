// Independent full raw-text verifier for the 12-byte tagged wide stream.
// No producer library or parse. Final suffix rank is brute-forced as at 3 GB.
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
namespace fs=std::filesystem; using U=uint32_t;using Q=uint64_t;
static_assert(sizeof(size_t)==8 && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__);
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Row {Q p,h;U b;};
constexpr Row decode(U p,U h,U t){return {p+(Q((t>>9)&2047)<<32),h+(Q((t>>20)&2047)<<32),t&511};}
static_assert(decode(0x12345678,0x98765432,0x80200300).p==0x112345678ull && decode(0x12345678,0x98765432,0x80200300).h==0x298765432ull && decode(0,0,0x80200300).b==256);
void stop(const fs::path& flag){if(fs::exists(flag)){std::fprintf(stderr,"STOP_OBSERVED phase=row43-oracle\n");std::exit(75);}}
#include "overlap_v10_product_pipe.hpp"
void run(const fs::path& corpus,const fs::path& flag,bool fixture,bool feed){
    ProductPipe pipe{flag}; Q n=fs::file_size(corpus);check(feed?n==ProductPipe::n:fixture?(0<n && n<=101'000'000):n==10'000'000'000ull,"wrong corpus rung");stop(flag);if(feed)pipe.open();
    int fd=open(corpus.c_str(),O_RDONLY);check(fd>=0,"open corpus");auto t=static_cast<const unsigned char*>(mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0));check(t!=MAP_FAILED,"map corpus");
    std::array<U,3*16384> buf;Q rows=0,prev=n,wide_positions=0,max_h=0;
    size_t tail=0;
    for(;;){stop(flag);size_t bytes=feed?pipe.move(0,buf.data(),std::min<Q>(sizeof buf,12*(n+1-rows))):std::fread(buf.data(),1,sizeof buf,stdin);if(!bytes)break;check(bytes%12==0,"partial row");
        for(size_t i=0;i<bytes/4;i+=3){U tag=buf[i+2];Row r=decode(buf[i],buf[i+1],tag);Q j=r.p,h=r.h;
            check((tag>>31) && r.b<=256,"invalid tag/predecessor");check(rows<=n && (rows?j<n:j==n) && h<=n-std::max(prev,j),"row extent or END");
            if(feed)pipe.count(r);
            check(r.b==(j?t[j-1]:256),"raw predecessor");check(!h || !std::memcmp(t+prev,t+j,h),"claimed common prefix");
            if(rows)check(prev!=j && (prev+h==n || (j+h<n && t[prev+h]<t[j+h])),"adjacent order or LCP");
            prev=j;++rows;wide_positions+=(j>UINT32_MAX);max_h=std::max(max_h,h);
        }
        if(feed){if(rows==n+1){char extra;check(!pipe.move(0,&extra,1),"extra rows");tail=bytes;break;}pipe.move(1,buf.data(),bytes);}
    }
    check((feed?tail!=0:std::feof(stdin)&&!std::ferror(stdin))&&rows==n+1,"incomplete stream");check(wide_positions==(n>UINT32_MAX?n-UINT32_MAX:0),"wide position count");
    Q rank=1;for(Q i=0;i<n;i++){
        if(!(i%(1u<<22)))stop(flag);
        if(t[i]!=t[prev])rank+=t[i]<t[prev];else{Q z=std::min(n-i,n-prev);int c=std::memcmp(t+i,t+prev,z);rank+=c<0||(c==0&&n-i<=n-prev);}
    }check(rank==rows,"independent final rank");if(feed){pipe.move(1,buf.data(),tail);std::fprintf(stderr,"PRODUCT_FENCE runs=%" PRIu64 " escapes=0\n",pipe.runs);}std::fprintf(feed?stderr:stdout,"ORACLE rows=%" PRIu64 " independent_final_rank=%" PRIu64 " all_adjacent_sa_lcp_bwt_verified=1 above_u32=%" PRIu64 " max_lcp=%" PRIu64 "\n",rows,rank,wide_positions,max_h);
    check(munmap((void*)t,n)==0 && close(fd)==0,"release corpus");
}
int main(int ac,char** av){try{bool feed=ac==4&&std::string_view(av[3])=="--generated-v10-pipe";check(ac==3||feed||(ac==4&&std::string_view(av[3])=="--fixture"),"row43-oracle CORPUS STOP [--fixture|--generated-v10-pipe]");run(av[1],av[2],ac==4,feed);return 0;}catch(const std::exception& e){std::fprintf(stderr,"row43-oracle: %s\n",e.what());return 70;}}
