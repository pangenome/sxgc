// Forward-from-zero polynomial for each block, then chain to its successor.
// No finish code, suffix rows, segment correction pass or second checkpoint table.
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

using U=uint64_t;
constexpr U T=128, BASE=0x9e3779b185ebca87ULL, N=100000503, K=8192;
static_assert(std::endian::native==std::endian::little);
static void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Input {
    int fd;struct stat id{};
    explicit Input(const char* p):fd(open(p,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK)){
        need(fd>=0 && !fstat(fd,&id) && S_ISREG(id.st_mode) && id.st_size>=0,"regular input");}
    ~Input(){close(fd);}
    void read(void* dst,U n,U pos)const {
        need(pos<=U(id.st_size) && n<=U(id.st_size)-pos,"read extent");
        while(n){auto k=pread(fd,dst,n,off_t(pos));if(k<0 && errno==EINTR)continue;
            need(k>0,"short pread");n-=U(k);pos+=U(k);dst=static_cast<char*>(dst)+k;}
    }
    void stable()const {struct stat s{};need(!fstat(fd,&s) && s.st_size==id.st_size && s.st_ctim.tv_sec==id.st_ctim.tv_sec && s.st_ctim.tv_nsec==id.st_ctim.tv_nsec,"input changed");}
};
static std::array<uint8_t,T*K> buf;
int main(int ac,char** av) try {
    need(ac==7,"--v8-100m|--fixture RAW REMAP PFCK STOP UNTIL");bool real=std::string_view(av[1])=="--v8-100m";
    need(real || std::string_view(av[1])=="--fixture","bad mode");auto arg=std::string_view(av[6]);U until=0;
    auto [end,err]=std::from_chars(arg.data(),arg.data()+arg.size(),until);need(err==std::errc{} && end==arg.data()+arg.size(),"deadline");
    auto guard=[&](){if(U(time(nullptr))>=until || access(av[5],F_OK)==0 || errno!=ENOENT){std::fputs("STOP_OBSERVED phase=pfck-proof\n",stderr);std::_Exit(75);}};
    guard();Input raw(av[2]),map(av[3]),ck(av[4]);U size=U(raw.id.st_size);need(real?size==N:(size && size<=2*T*K),"wrong rung");
    U n=size+1,count=n/T+1;need(map.id.st_size==256 && U(ck.id.st_size)==40+8*count,"sidecar extent");
    std::array<uint8_t,256> remap{},seen{};map.read(remap.data(),256,0);U header[5];ck.read(header,40,0);
    need(header[0]==0x000000014b434650ULL && header[1]==n && header[2]==T && header[3]==count && !header[4],"PFCK header");
    // H(i) for remapped raw followed by exactly one virtual END. Independently
    // prove H(kT) = block_sum + B^len * H((k+1)T) modulo 2^64 for EVERY k.
    // The last short block uses zero beyond n. When n%T==0 there is a real
    // final empty checkpoint too. Read one lookahead checkpoint across IO seams.
    std::array<U,K+1> ckpts{};std::array<U,T+1> powers{};powers[0]=1;
    for(U j=0;j<T;++j)powers[j+1]=powers[j]*BASE;
    U values=0,text_bytes=0;
    for(U first=0;first<count;first+=K){guard();U num=std::min(K,count-first),more=(first+num<count);
        ck.read(ckpts.data(),8*(num+more),40+8*first);U off=first*T,len=std::min(num*T,n-off),bytes=std::min(len,size-std::min(off,size));
        if(bytes)raw.read(buf.data(),bytes,off);
        text_bytes+=bytes;
        for(U j=0;j<num;++j){U h=0,l=std::min(T,n-(first+j)*T);for(U k=0;k<l;++k){U at=j*T+k,code=0;
                if(at<bytes){uint8_t b=buf[at];seen[b]=1;code=remap[b];}else need(off+at==size,"virtual END extent");
                h+=(code+1)*powers[k];}
            U successor=first+j+1<count?ckpts[j+1]:0;
            need(ckpts[j]==h+powers[l]*successor,"checkpoint recurrence mismatch");++values;}
    }
    unsigned prev=0,alphabet=0;for(size_t i=0;i<256;++i)if(seen[i]){need(remap[i]>prev,"non-increasing remap or nonunique END");prev=remap[i];++alphabet;}
    guard();raw.stable();map.stable();ck.stable();need(values==count && text_bytes==size,"incomplete proof");
    std::printf("PFCK_PROVED raw_bytes=%llu n=%llu tau=128 checkpoints=%llu file_bytes=%llu used_symbols=%u virtual_end=1 exact_mod_2pow64=1\n",
        (unsigned long long)size,(unsigned long long)n,(unsigned long long)values,(unsigned long long)ck.id.st_size,alphabet);
    return 0;
} catch(const std::exception& e){std::fprintf(stderr,"pfck proof refused: %s\n",e.what());return 70;}
