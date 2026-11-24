// Post-clock lexical-tape witness. No occurrence parse, corpus, sorter or row stream.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
namespace fs=std::filesystem; using U=uint32_t;using Q=uint64_t;
static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__ && sizeof(off_t)==8);
constexpr fs::path::value_type source[]="/mnt/nvme2n1/erikg/TAPEREADER/sandbox/packed-front-10g-YebiCY/columns/dictionary.packed";
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Halt{};
struct Guard{fs::path flag;time_t end;void poll()const{if(time(nullptr)>=end||fs::exists(flag))throw Halt{};}};
struct Hash{
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(),EVP_MD_CTX_free};
    Hash(){need(ctx && EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr),"SHA init");}
    void update(const void* p,size_t n){need(EVP_DigestUpdate(ctx.get(),p,n),"SHA update");}
    std::string finish(){unsigned char v[EVP_MAX_MD_SIZE];unsigned n=0;need(EVP_DigestFinal_ex(ctx.get(),v,&n)&&n==32,"SHA final");char hex[65];for(unsigned i=0;i<32;++i)std::snprintf(hex+2*i,3,"%02x",unsigned(v[i]));return hex;}
};
struct Reader{
    int fd;const Guard &guard;Hash hash;Q left,unread;std::vector<unsigned char> buf=std::vector<unsigned char>(1<<20);size_t cur=0,lim=0;
    Reader(int f,Q bytes,const Guard& g):fd(f),guard(g),left(bytes),unread(bytes){}
    void take(void* out,size_t n){need(n<=left,"truncated record");left-=n;auto p=static_cast<unsigned char*>(out);
        while(n){if(cur==lim){guard.poll();size_t want=std::min<Q>(unread,buf.size());need(want,"unexpected end");ssize_t r=::read(fd,buf.data(),want);if(r<0&&errno==EINTR)continue;need(r>0,"short packed tape");cur=0;lim=size_t(r);unread-=lim;hash.update(buf.data(),lim);}
            auto cnt=std::min(n,lim-cur);std::memcpy(p,buf.data()+cur,cnt);p+=cnt;cur+=cnt;n-=cnt;}}
};
std::string text(const fs::path& p){std::ifstream s(p);std::string v;need(bool(std::getline(s,v)),"missing cgroup setting");return v;}
void verify(const fs::path& file,const Guard& g,bool exact){
    g.poll();if(exact){need(file==source,"reference pathname");const auto group=text("/proc/self/cgroup");need(group.rfind("0::/",0)==0,"expected cgroup v2");const auto cg=fs::path("/sys/fs/cgroup")/group.substr(4);need(text(cg/"cpu.max")=="100000 100000"&&text(cg/"memory.max")=="201326592"&&text(cg/"memory.swap.max")=="0","dedicated verifier grant");}
    int fd=::open(file.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);need(fd>=0,"open packed tape");struct stat st{};need(::fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0,"not a packed file");
    Q size=Q(st.st_size);need(exact?size==11942962780ULL:size<=16u<<20,"wrong tape extent");Reader in(fd,size,g);
    std::array<Q,256*257> cells{};Q k=0,descriptors=0,eofs=0;U longest=0;std::vector<unsigned char> now,prev;bool prev_end=false;
    while(in.left){if(!(k%65536))g.poll();U tag;in.take(&tag,4);const bool end=tag>>31;const U len=tag&0x7fffffffu;need(len>(end?0u:8u)&&len<=1u<<20,"invalid word length");
        now.resize(len);in.take(now.data(),len);if(k)need(prev<now||(prev==now&&!prev_end&&end),"word order/density");
        const U owned=end?len:len-8;for(U j=0;j<owned;++j){const unsigned code=(j+1==len)?0u:unsigned(now[j+1])+1; ++cells[now[j]*257u+code];}
        descriptors+=owned;++k;eofs+=end;longest=std::max(longest,len);prev.swap(now);prev_end=end;
    }
    struct stat after{};need(::fstat(fd,&after)==0&&after.st_size==st.st_size&&after.st_ctim.tv_sec==st.st_ctim.tv_sec&&after.st_ctim.tv_nsec==st.st_ctim.tv_nsec,"tape changed");need(!::close(fd),"close");g.poll();
    need(!in.unread && k && eofs==1,"missing or repeated EOF word");const Q D=descriptors+9*k-7;need(D+3*k-1==size,"packed extent mismatch");
    Q sum=0,terminal=0,peak=0;size_t nonempty=0;for(size_t i=0;i<cells.size();++i){sum+=cells[i];terminal+=(i%257==0)?cells[i]:0;nonempty+=cells[i]>0;peak=std::max(peak,cells[i]);}need(sum==descriptors&&terminal==1,"descriptor coverage");
    if(exact)need(k==234472723&&descriptors==9129290112ULL&&D==11239544612ULL&&longest==88468,"banked census mismatch");
    Hash h;h.update(cells.data(),cells.size()*sizeof(Q));std::printf("VOCAB_REFERENCE k=%llu descriptors=%llu D=%llu tape=%llu max_phrase=%u nonempty=%zu largest=%llu tape_sha256=%s descriptor_cells_sha256=%s\n",(unsigned long long)k,(unsigned long long)descriptors,(unsigned long long)D,(unsigned long long)size,longest,nonempty,(unsigned long long)peak,in.hash.finish().c_str(),h.finish().c_str());
}
int main(int ac,char** av)try{
    need(ac==5,"usage: reference --ten-reference TAPE STOP DEADLINE");bool exact=std::string(av[1])=="--ten-reference";
#ifdef VOCAB_REFERENCE_TINY
    need(!exact&&std::string(av[1])=="--fixture","fixture mode only");
#else
    need(exact,"10 GB reference mode only");
#endif
    size_t used;std::string ts(av[4]);long long deadline=std::stoll(ts,&used);need(used==ts.size()&&deadline>0,"invalid deadline");verify(av[2],Guard{av[3],deadline},exact);return 0;
}catch(const Halt&){std::fprintf(stderr,"STOP_OBSERVED vocabulary-reference\n");return 75;}catch(const std::exception& e){std::fprintf(stderr,"VOCAB_REFERENCE_REFUSED %s\n",e.what());return 70;}
