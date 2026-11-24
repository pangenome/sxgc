// Stream both complete lexical tables (including tags) and both text-order columns.
// The legacy reader is an oracle for supported historical rungs, not the front's format.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <openssl/evp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
using U=uint64_t;static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__);
static void ck(bool ok,const char*s){if(!ok)throw std::runtime_error(s);}
struct Input{int fd;U size,loaded=0,at=0,valid=0;std::array<unsigned char,65536>b;EVP_MD_CTX*h;explicit Input(const char*p){fd=open(p,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat st{};ck(fd>=0 && !fstat(fd,&st) && st.st_size>0 && S_ISREG(st.st_mode),"invalid input");size=st.st_size;h=EVP_MD_CTX_new();ck(h && EVP_DigestInit_ex(h,EVP_sha256(),nullptr),"digest init");}
 ~Input(){EVP_MD_CTX_free(h);close(fd);}U pos()const{return loaded-valid+at;}
 void fill(){if(valid==at){ssize_t n=read(fd,b.data(),std::min<U>(size-loaded,b.size()));ck(n>0,"truncated data");ck(EVP_DigestUpdate(h,b.data(),n),"digest update");at=0;loaded+=n;valid=n;}}
 template<class V>V word(){V v;size_t done=0;while(done<sizeof v){fill();auto n=std::min<U>(sizeof(v)-done,valid-at);std::memcpy((char*)&v+done,b.data()+at,n);at+=n;done+=n;}return v;}
 void equal(Input&other,U n){while(n){fill();other.fill();U z=std::min({n,valid-at,other.valid-other.at});ck(!std::memcmp(b.data()+at,other.b.data()+other.at,z),"unequal raw word");at+=z;other.at+=z;n-=z;}}
 std::string digest(){ck(pos()==size,"trailing bytes");unsigned char hash[32];unsigned len=0;ck(EVP_DigestFinal_ex(h,hash,&len) && len==32,"digest final");char hex[65];for(int i=0;i<32;++i)std::sprintf(hex+2*i,"%02x",hash[i]);return hex;}};
int main(int ac,char**av)try{ck(ac==9,"verify NEW_DICT NEW_KEYS OLD_DICT OLD_IDS K ARRAY_BYTES STOP DEADLINE");U k=std::stoull(av[5]),end=std::stoull(av[8]);auto guard=[&]{ck(std::time(nullptr)<time_t(end) && !std::filesystem::exists(av[7]),"verify deadline/stop");};guard();Input dict(av[1]),keys(av[2]),old(av[3]),ids(av[4]);U m=ids.size/4;ck(ids.size%4==0 && keys.size%8==0 && keys.size/8==m && k && k<=m && k<=UINT32_MAX && 8*__uint128_t(k)+(1<<20)<=std::stoull(av[6]),"verify extents");std::vector<U> offsets(k);U eof=k,u=0;
 for(U i=0;i<k;++i){if(i%65536==0)guard();offsets[i]=dict.pos();U tag=dict.word<U>();U h=old.word<uint32_t>(),len=h&0x7fffffff;bool final=h>>31;ck((tag>>1)==len && (tag&1)==final && len>(final?0u:8u),"tag/length mismatch");if(final){ck(eof==k,"duplicate EOF");eof=i;}u+=len-(final?0:8);dict.equal(old,len);}
 ck(eof<k,"missing EOF");U high=0;for(U j=0;j<m;++j){if(j%65536==0)guard();U id=ids.word<uint32_t>(),key=keys.word<U>();ck(id<k && key==offsets[id] && ((id==eof)==(j+1==m)),"incorrect text-order key");high+=key>UINT32_MAX;}
 guard();auto d=dict.digest(),s=keys.digest(),o=old.digest(),p=ids.digest();std::printf("FRONT64_VERIFIED m=%llu k=%llu descriptors=%llu keys_above_u32=%llu lex64_sha256=%s keys_sha256=%s legacy_tape_sha256=%s legacy_ids_sha256=%s\n",(unsigned long long)m,(unsigned long long)k,(unsigned long long)u,(unsigned long long)high,d.c_str(),s.c_str(),o.c_str(),p.c_str());return 0;}catch(const std::exception&e){std::fprintf(stderr,"front64 verify refused: %s\n",e.what());return 75;}
