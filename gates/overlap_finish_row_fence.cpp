// Checked row43 -> verified NARROW pipe for one fixed v8 input. Not a wide
// production sink. Never forward the final packet on partial EOF/proof failure.
#include "overlap_finish_row_fence.hpp"
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <openssl/evp.h>
#include <poll.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace rowfence;
static_assert(std::endian::native==std::endian::little && sizeof(size_t)==8);
constexpr U N=100000503, RUNS=38647521, PAYLOAD=26563024, SLICE=2410736, ESC=1129147, H=34067;
constexpr const char* SHA="82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e";
struct Row {uint32_t p,h,b;}; static_assert(sizeof(Row)==12);
static U decimal(const char* a){U u=0;std::string_view s(a);auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),u);
    need(e==std::errc{} && p==s.end(),"unsigned decimal required");return u;}
struct Stop {const char* path;U until;
    void check()const{if(U(time(nullptr))>=until || !access(path,F_OK) || errno!=ENOENT){
        std::fputs("STOP_OBSERVED phase=verified-row-fence\n",stderr);std::_Exit(75);}}
};
static void pipe_fd(int fd,struct stat& st){int f=fcntl(fd,F_GETFL),size=fcntl(fd,F_GETPIPE_SZ);
    need(!fstat(fd,&st) && S_ISFIFO(st.st_mode) && size>0 && size<=512*1024 && f>=0 && !fcntl(fd,F_SETFL,f|O_NONBLOCK),"bounded pipe required");}
static size_t transfer(int fd,void* data,size_t len,bool writing,const Stop& stop){size_t at=0;
    while(at<len){stop.check();auto* p=static_cast<char*>(data)+at;
        ssize_t z=writing?write(fd,p,len-at):read(fd,p,len-at);
        if(z>0){at+=size_t(z);continue;}if(!z && !writing)break;if(z<0 && errno==EINTR)continue;
        need(z<0 && (errno==EAGAIN || errno==EWOULDBLOCK),writing?"downstream closed":"read failure");
        pollfd poller{fd,short(writing?POLLOUT:POLLIN),0};need(poll(&poller,1,50)>=0 || errno==EINTR,"pipe poll");
    }return at;}
static U final_rank(const unsigned char* raw,U n,U last,const Stop& stop,U cap,U& work){
    U rank=1; // virtual suffix; self is counted exactly once below
    for(U p=0;p<n;++p){if(!(p%65536))stop.check();if(p==last){++rank;continue;}
        need(work<cap,"final rank work");++work;int cmp=int(raw[p])-int(raw[last]);
        if(!cmp){U len=n-std::max(p,last);
            for(U off=1;off<len;){if(off>1 && off%65536==1)stop.check();U take=std::min<U>(len-off,64);
                need(take<=cap-work,"final rank work");work+=take;cmp=std::memcmp(raw+p+off,raw+last+off,take);
                if(cmp)break;
                off+=take;
            }}
        rank+=cmp<0 || (!cmp && p>=last);
    }return rank;}

int main(int ac,char** av) try {
    bool exact=true; Limits limits{RUNS,SLICE,PAYLOAD};const char* want=SHA;
#ifdef ROW_FENCE_FIXTURE
    exact=false;need(ac==9 && std::string_view(av[1])=="--fixture","test profile only");
    limits={decimal(av[5]),decimal(av[6]),decimal(av[7])};want=av[8];
#else
    need(ac==5 && std::string_view(av[1])=="--v8-100m","--v8-100m PRIVATE_RAW STOP DEADLINE");
#endif
    Stop stop{av[3],decimal(av[4])};stop.check();signal(SIGPIPE,SIG_IGN);
    struct stat in{},out{},st{};pipe_fd(0,in);pipe_fd(1,out);need(in.st_ino!=out.st_ino || in.st_dev!=out.st_dev,"pipe loop");
    int fd=open(av[2],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    need(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>0,"private raw file");
    U n=U(st.st_size);need(exact?n==N:n<=524288,"wrong raw rung");
    auto raw=static_cast<const unsigned char*>(mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0));need(raw!=MAP_FAILED,"map raw");
    auto* sha=EVP_MD_CTX_new();need(sha && EVP_DigestInit_ex(sha,EVP_sha256(),nullptr)==1,"hash init");
    Fence fence(limits);std::array<Row,8192> buf;U prev=n,final_work=0,rank=0;char digest[65]{};
    // Same-stream upper on equality bytes: nonescapes <=254, escapes <=H.
    // Check before comparing; erroneous claims cannot buy unlimited work.
    U prefix_cap=exact?(n+1-ESC)*254+ESC*H:(n+1)*n;
    while(fence.rows<=n){size_t count=std::min<U>(buf.size(),n+1-fence.rows),bytes=count*sizeof(Row);
        need(transfer(0,buf.data(),bytes,false,stop)==bytes,"short stream");
        for(size_t j=0;j<count;++j){if(j%512==0)stop.check();auto& row=buf[j];U p=row.p,h=row.h,b=row.b&511;
            // Before narrowing: BOTH high fields zero, tag present. No covert
            // source-position tie break: strict raw suffix order proves uniqueness.
            need((row.b & ~511u)==0x80000000 && b<=256,"not narrow row43");
            need((fence.rows?p<n:p==n) && h<=n-std::max(p,prev) && h<=prefix_cap-fence.sum_h && (!exact || h<=H),"row extent/work");
            need(b==(p?raw[p-1]:256u) && (!exact || b!=0),"raw predecessor/remap");
            for(U off=0;off<h;off+=65536){if(off)stop.check();need(!std::memcmp(raw+p+off,raw+prev+off,std::min<U>(h-off,65536)),"claimed prefix");}
            if(fence.rows)need(p!=prev && (prev+h==n || (p+h<n && raw[prev+h]<raw[p+h])),"order/LCP");
            fence.add(b,h);prev=p;row.b=uint32_t(b);
        }
        need(EVP_DigestUpdate(sha,buf.data(),bytes)==1,"hash rows");
        if(fence.rows==n+1){char extra;need(!transfer(0,&extra,1,false,stop),"extra rows");fence.end();
            unsigned char d[EVP_MAX_MD_SIZE];unsigned k=0;need(EVP_DigestFinal_ex(sha,d,&k)==1 && k==32,"hash final");
            for(unsigned i=0;i<k;++i)snprintf(digest+2*i,3,"%02x",d[i]);
            need(!strcmp(digest,want),"narrow row SHA");
            need(!exact || (fence.runs==RUNS && fence.lcp_payload==PAYLOAD && fence.slice_peak==SLICE && fence.total_esc==ESC && fence.max_h==H),"v8 geometry mismatch");
            // For sorted distinct suffixes LCP(SA[i],last)<=LCP(SA[i],SA[i+1]).
            // Each final-rank probe can overshoot its first difference by <64 bytes.
            rank=final_rank(raw,n,prev,stop,fence.sum_h+65*n,final_work);need(rank==n+1,"final rank");
        }
        transfer(1,buf.data(),bytes,true,stop);
    }
    stop.check();EVP_MD_CTX_free(sha);need(!munmap(const_cast<unsigned char*>(raw),n) && !close(fd),"release raw");
    std::fprintf(stderr,"FENCED rows=%llu final_rank=%llu runs=%llu sxls_peak=%llu sxls_escapes=%llu m14_blocks=%llu m14_payload=%llu prefix_bytes=%llu final_compare_bytes=%llu narrow_sha256=%s\n",
        (unsigned long long)fence.rows,(unsigned long long)rank,(unsigned long long)fence.runs,(unsigned long long)fence.slice_peak,
        (unsigned long long)fence.total_esc,(unsigned long long)fence.m14_blocks,(unsigned long long)fence.lcp_payload,
        (unsigned long long)fence.sum_h,(unsigned long long)final_work,digest);
    return 0;
} catch(const std::exception& e){std::fprintf(stderr,"row fence refused: %s\n",e.what());return 70;}
