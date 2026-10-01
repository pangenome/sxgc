// One sequential pass over SXI2 member 8; all later comparisons use RAM.
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cmath>
using U=uint64_t;
static U num(const unsigned char*p){U v=0;for(int j=7;j>=0;--j)v=(v<<8)|p[j];return v;}
static U packed(const unsigned char*p,U i,unsigned w){if(!w)return 0;U bit=i*w;unsigned shift=bit&7;unsigned z=(shift+w+7)/8;__uint128_t v=0;for(unsigned j=0;j<z;++j)v|=(__uint128_t)p[(bit>>3)+j]<<(8*j);return U((v>>shift)&(((__uint128_t)1<<w)-1));}
int main(int argc,char**argv){
 if(argc!=2)return 2;int fd=open(argv[1],O_RDONLY);if(fd<0){perror("open");return 2;}struct stat st;if(fstat(fd,&st)){perror("stat");return 2;}
 auto base=(const unsigned char*)mmap(nullptr,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);if(base==MAP_FAILED){perror("mmap");return 2;}
 U n=num(base+8),r=num(base+24),members=*(const uint32_t*)(base+32),off=0,bytes=0;
 for(U i=0;i<members;i++){const unsigned char*p=base+64+40*i;if(*(const uint32_t*)p==8){off=num(p+8);bytes=num(p+16);}}
 if(!off||!bytes){fprintf(stderr,"member 8 absent\n");return 2;}
 const unsigned char*p=base+off;unsigned vw=num(p),rw=num(p+8),ul=num(p+16);U lb=num(p+24),hb=num(p+32);
 auto low=p+40,high=low+(lb+7)/8,vv=high+(hb+7)/8,rr=vv+(r*vw+7)/8;
 std::vector<U> u_by_run(r),v_by_run(r);U highpos=0;
 U prev_run=0,prev_v=0;U near_run[6]{},near_v[6]{},near_vu[6]{},absdelta_log[64]{};U first_run=0,first_v=0;
 const U thresholds[]={0,1,16,256,65536,n/1024};
 for(U i=0;i<r;i++){
  while(!(high[highpos>>3]&(1u<<(highpos&7))))++highpos;
  U u=((highpos-i)<<ul)|packed(low,i,ul);++highpos;
  U v=packed(vv,i,vw),run=packed(rr,i,rw);u_by_run[run]=u;v_by_run[run]=v;
  if(i){U d=run>prev_run?run-prev_run:prev_run-run;U q=v>prev_v?v-prev_v:prev_v-v;for(int j=0;j<6;j++){near_run[j]+=d<=thresholds[j];near_v[j]+=q<=thresholds[j];}}
  else{first_run=run;first_v=v;}
  prev_run=run;prev_v=v;
 }
 U eq=0,cyclic_near[6]{},nonzero=0;
 for(U run=0;run<r;run++){
  U v=v_by_run[run],u=u_by_run[(run+1)%r];U d=v>u?v-u:u-v;d=std::min(d,n-d);
  eq+=d==0;nonzero+=d!=0;for(int j=0;j<6;j++)cyclic_near[j]+=d<=thresholds[j];
  if(d)absdelta_log[63-__builtin_clzll(d)]++;
 }
 printf("{\"path\":\"%s\",\"n\":%llu,\"r\":%llu,\"widths\":[%u,%u,%u],\"thresholds\":[",argv[1],(unsigned long long)n,(unsigned long long)r,vw,rw,ul);
 for(int j=0;j<6;j++)printf("%s%llu",j?",":"",(unsigned long long)thresholds[j]);
 printf("],\"adjacent_run_near\":[");for(int j=0;j<6;j++)printf("%s%llu",j?",":"",(unsigned long long)near_run[j]);
 printf("],\"adjacent_v_near\":[");for(int j=0;j<6;j++)printf("%s%llu",j?",":"",(unsigned long long)near_v[j]);
 printf("],\"v_equals_next_run_u\":%llu,\"v_next_u_near\":[",(unsigned long long)eq);
 for(int j=0;j<6;j++)printf("%s%llu",j?",":"",(unsigned long long)cyclic_near[j]);
 printf("],\"v_next_u_log2_abs_delta\":[");for(int j=0;j<64;j++)printf("%s%llu",j?",":"",(unsigned long long)absdelta_log[j]);puts("]}");
 munmap((void*)base,st.st_size);close(fd);
}
