// Independent verifier for a prefix streamed on stdin. The last suffix's
// brute-force rank proves completeness in addition to adjacent row checks.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
using U=uint32_t;using Q=uint64_t;int main(int ac,char**av){assert(ac==2);U n=std::filesystem::file_size(av[1]);int fd=open(av[1],O_RDONLY);assert(fd>=0);auto t=(const unsigned char*)mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);assert(t!=MAP_FAILED);U p=n,rows=0;U x[3];while(fread(x,1,12,stdin)==12){U j=x[0],h=x[1];assert((rows?j<n:j==n)&&x[2]==(j?t[j-1]:256)&&h<=n-std::max(p,j));assert(!h||!memcmp(t+p,t+j,h));if(rows)assert(j!=p&&(p+h==n||(j+h<n&&t[p+h]<t[j+h])));p=j;++rows;}assert(feof(stdin)&&!ferror(stdin)&&rows);Q rank=1;for(U i=0;i<n;i++){if(t[i]!=t[p])rank+=t[i]<t[p];else{U z=std::min(n-i,n-p);int c=memcmp(t+i,t+p,z);rank+=c<0||(c==0&&n-i<=n-p);}}assert(rank==rows);printf("ORACLE rows=%u independent_final_rank=%llu all_adjacent_sa_lcp_bwt_verified=1\n",rows,(unsigned long long)rank);}
