// Separate oracle, intentionally allowed to cache the source. Not producer IO.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
using U=uint32_t;struct Row{U p,h,b;};int main(int ac,char**av){assert(ac==4);U hi=std::stoul(av[3]),n=std::filesystem::file_size(av[1]);int fd=open(av[1],O_RDONLY);assert(fd>=0);auto t=(const unsigned char*)mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);assert(t!=MAP_FAILED);U expected=1;for(U i=0;i<n;i++)expected+=((2+t[i])*258u+(i+1<n?2+t[i+1]:1u)<hi);std::ifstream f(av[2],std::ios::binary);Row r;U rows=0,prev=n;while(f.read((char*)&r,12)){assert(r.p<=n&&(rows?r.p<n:r.p==n)&&r.b==(r.p?t[r.p-1]:256));U h=0;if(rows){assert((2+t[r.p])*258u+(r.p+1<n?2+t[r.p+1]:1u)<hi);while(prev+h<n&&r.p+h<n&&t[prev+h]==t[r.p+h])++h;assert(prev+h==n||(r.p+h<n&&t[prev+h]<t[r.p+h]));assert(prev!=r.p);}assert(r.h==h);prev=r.p;++rows;}assert(rows==expected&&std::filesystem::file_size(av[2])==12ull*rows);printf("ORACLE verified_positions_lcps_predecessors=%u exhaustive_first_window=1\n",rows);}
