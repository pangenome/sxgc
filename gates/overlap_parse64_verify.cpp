// Independent one-CPU proof from original text-order tokens and a published inverse rank file.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
using U=uint64_t;
static void ck(bool b,const char*m){if(!b)throw std::runtime_error(m);}
struct In{int fd;size_t n;void*p;In(const char*s){struct stat st{};fd=open(s,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);ck(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>0,"verify input");n=st.st_size;p=mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);ck(p!=MAP_FAILED,"verify mmap");}~In(){munmap(p,n);close(fd);}};
int main(int ac,char**av)try{static_assert(sizeof(size_t)==8 && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__);ck(ac==7,"verify HEADS LE_WIDTH RANKS GRANT STOP DEADLINE");std::string width=av[2];ck(width=="8" || width=="4","token wire width");U w=width=="8"?8:4;In h(av[1]),inv(av[3]);U m=h.n/w;ck(h.n%w==0 && inv.n%8==0 && inv.n/8==m && m && (__uint128_t)(16+w)*m+(1<<20)<=std::stoull(av[4]),"verify shape/grant");U end=std::stoull(av[6]);auto guard=[&]{ck(std::time(nullptr)<static_cast<time_t>(end) && !std::filesystem::exists(av[5]),"verify stop");};guard();auto r=(const U*)inv.p;std::vector<U> pos(m,UINT64_MAX);
 for(U i=0;i<m;++i){if(i%1048576==0)guard();U x=r[i];ck(x>0 && x<=m && pos[x-1]==UINT64_MAX,"nonpermutation");pos[x-1]=i;}
 auto head=[&](U i)->U{return w==8?((const U*)h.p)[i]:((const uint32_t*)h.p)[i];};U a=0,b=0;
 for(U j=0;j<m;++j){if(j%1048576==0)guard();U p=pos[j],x=head(p),y=p+1==m?0:r[p+1];ck(!j || a<x || (a==x && b<y),"successor recurrence failed");a=x;b=y;}
 guard();std::printf("VERIFIED_PARSE64 rows=%llu source_token_width=%llu ranks=uint64 permutation=1 strict_recurrence=1 virtual_end_rank=0\n",(unsigned long long)m,(unsigned long long)w);return 0;}catch(const std::exception&e){std::fprintf(stderr,"verify refused: %s\n",e.what());return 75;}
