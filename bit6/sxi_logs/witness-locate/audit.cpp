// Read-only SXI2 v3/v4 witness-run census and backward-step simulator.
// Compile: g++ -O3 -std=c++17 model.cpp -o model
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
using U=uint64_t;
static U get(const uint8_t*p){U x;memcpy(&x,p,8);return x;}
static uint32_t get4(const uint8_t*p){uint32_t x;memcpy(&x,p,4);return x;}
static void req(bool x,const char*s){if(!x){std::cerr<<"bad SXI: "<<s<<"\n";exit(2);}}
static U packed(const uint8_t*p,U bit,unsigned width){if(!width)return 0;U x=0;for(unsigned j=0;j<width;j++)x|=U((p[(bit+j)/8]>>((bit+j)%8))&1)<<j;return x;}
static bool flag(const std::vector<uint8_t>&b,U i){return (b[i>>3]>>(i&7))&1;}
static void mark(std::vector<uint8_t>&b,U i){b[i>>3]|=uint8_t(1u<<(i&7));}
struct Mem{U off,bytes,count;};
struct BitReader{const uint8_t*p;U pos=0;int bit(){return (p[pos>>3]>>(pos++&7))&1;}};
int main(int argc,char**argv){
 req(argc>=4,"usage: model artifact label queries.tsv [--ms]");
 bool ms_mode=argc>=5&&std::string(argv[4])=="--ms";
 const char*path=argv[1];std::string label=argv[2];
 int fd=open(path,O_RDONLY);req(fd>=0,"open");struct stat st;req(!fstat(fd,&st),"stat");
 auto a=(const uint8_t*)mmap(nullptr,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);req(a!=MAP_FAILED,"mmap");
 req(!memcmp(a,"SXI2",4),"magic");U n=get(a+8),r=get(a+24),bytes=get(a+40),flags=get(a+48);req(bytes==U(st.st_size),"bytes");
 unsigned version=get4(a+4),members=get4(a+32);req(version>=3&&version<=4,"version");
 std::array<Mem,12> m{};for(unsigned i=0;i<members;i++){auto d=a+64+40*i;U id=get4(d);req(id<12,"member id");m[id]={get(d+8),get(d+16),get(d+24)};}
 std::cerr<<label<<" n="<<n<<" R="<<r<<" chi="<<m[5].count<<"\n";
 std::vector<uint8_t> chi((n+8)/8,0),mapped((n+8)/8,0),bearing((r+7)/8,0),head((r+7)/8,0),tail((r+7)/8,0);
 auto cp=a+m[5].off;U cw=get(cp),lb=get(cp+8),hb=get(cp+16);req(lb==m[5].count*cw,"chi low bits");
 const uint8_t*cl=cp+24,*ch=cl+(lb+7)/8;U chi_i=0;
 for(U bit=0;bit<hb;bit++)if((ch[bit>>3]>>(bit&7))&1){
   U v=((bit-chi_i)<<cw)|packed(cl,chi_i*cw,cw);req(v<=n,"chi coordinate");mark(chi,v);chi_i++;
 }
 req(chi_i==m[5].count,"chi count");
 auto pp=a+m[8].off;U vw=get(pp),rw=get(pp+8),uw=get(pp+16),ulb=get(pp+24),uhb=get(pp+32);
 const uint8_t*ul=pp+40,*uh=ul+(ulb+7)/8,*pv=uh+(uhb+7)/8,*pr=pv+(r*vw+7)/8;
 U hi=0,ht=0,hh=0,both=0,firstU=0,prevU=0;
 for(U i=0;i<r;i++){
   while(hi<uhb && !((uh[hi>>3]>>(hi&7))&1))hi++;
   req(hi<uhb,"phi high bits");
   U u=((hi-i)<<uw)|packed(ul,i*uw,uw),v=packed(pv,i*vw,vw),run=packed(pr,i*rw,rw);hi++;
   req(u<n&&v<n&&run<r,"phi value");req(i==0||u>prevU,"phi order");prevU=u;if(i==0)firstU=u;
   U next=(run+1==r)?0:run+1;
   U uc=u? n-u:0,vc=v?n-v:0;
   if(flag(chi,uc)){mark(tail,run);mark(bearing,run);mark(mapped,uc);ht++;}
   if(flag(chi,vc)){mark(head,next);mark(bearing,next);mark(mapped,vc);hh++;}
 }
 U br=0,hr=0,tr=0,overlap=0,matched=0;
 for(U j=0;j<bearing.size();j++){br+=__builtin_popcount((unsigned)bearing[j]);hr+=__builtin_popcount((unsigned)head[j]);tr+=__builtin_popcount((unsigned)tail[j]);overlap+=__builtin_popcount((unsigned)(head[j]&tail[j]));}
 for(uint8_t v:mapped)matched+=__builtin_popcount((unsigned)v);
 std::cout<<"{\"corpus\":\""<<label<<"\",\"n\":"<<n<<",\"r\":"<<r<<",\"chi\":"<<m[5].count
 <<",\"artifact_bytes\":"<<bytes<<",\"ef_chi_bytes\":"<<m[5].bytes<<",\"head_runs\":"<<hr
 <<",\"tail_runs\":"<<tr<<",\"bearing_runs\":"<<br<<",\"head_tail_overlap\":"<<overlap
 <<",\"head_edge_hits\":"<<hh<<",\"tail_edge_hits\":"<<ht<<",\"mapped_chi\":"<<matched<<",\"unmapped_chi\":"<<(m[5].count-matched);
 // Decode the full run table for exact rank and simulated backward search.
 auto rp=a+m[1].off;std::array<U,256>C{};for(int c=0;c<256;c++)C[c]=get(rp+8*c);
 const uint8_t*widths=rp+2048;U hbits=get(rp+2304),lbits=get(rp+2312);
 const uint8_t*hs=rp+2320,*ls=hs+(hbits+7)/8;
 struct Node{int next[2]={-1,-1};int sym=-1;};std::vector<Node> nodes(1);
 std::vector<std::pair<int,int>> order;for(int c=0;c<256;c++)if(widths[c])order.push_back({widths[c],c});std::sort(order.begin(),order.end());
 U code=0;int prev=0;for(auto[w,c]:order){code<<=w-prev;int q=0;for(int j=w-1;j>=0;j--){int b=(code>>j)&1;if(nodes[q].next[b]<0){nodes[q].next[b]=nodes.size();nodes.emplace_back();}q=nodes[q].next[b];}nodes[q].sym=c;code++;prev=w;}
 std::vector<uint8_t> chars(r);std::vector<uint32_t> lens(r);const U block=4096,blocks=(r+block-1)/block;
 std::vector<U> starts(blocks+1);std::vector<std::array<U,256>> cumulative(blocks+1);
 BitReader hd{hs},ld{ls};U position=0;std::array<U,256> totals{};
 for(U i=0;i<r;i++){
   if(i%block==0){starts[i/block]=position;cumulative[i/block]=totals;}
   int q=0;while(nodes[q].sym<0){req(hd.pos<hbits,"head bits");q=nodes[q].next[hd.bit()];req(q>=0,"Huffman");}
   int z=0;while(ld.pos<lbits && !ld.bit())z++;req(z<32,"gamma zeros");U len=1;for(int j=0;j<z;j++){req(ld.pos<lbits,"gamma bits");len=(len<<1)|ld.bit();}
   chars[i]=nodes[q].sym;lens[i]=len;position+=len;totals[chars[i]]+=len;
 }
 starts[blocks]=position;cumulative[blocks]=totals;req(position==n&&hd.pos==hbits&&ld.pos==lbits,"run decode");
 U csum=0;for(int c=0;c<256;c++){req(C[c]==csum,"C table");csum+=totals[c];}req(csum==n,"C total");
 auto run_of=[&](U p)->U{if(p==n)return r;auto it=std::upper_bound(starts.begin(),starts.end(),p);U b=std::max<U>(0,it-starts.begin()-1),j=b*block,s=starts[b];while(j+1<r&&s+lens[j]<=p){s+=lens[j++];}return j;};
 auto rank=[&](unsigned c,U p)->U{if(p==0)return 0;if(p==n)return totals[c];U j=run_of(p),b=j/block,s=starts[b],count=cumulative[b][c];for(U k=b*block;k<j;k++){if(chars[k]==c)count+=lens[k];s+=lens[k];}if(chars[j]==c)count+=p-s;return count;};
 // A rank endpoint touches its containing run and the previous c-run for
 // predecessor rank; classifying only the containing run is optimistic.
 auto touch=[&](unsigned c,U p)->std::pair<bool,bool>{
   if(p==0||p==n)return {true,true};U j=run_of(p),b=j/block,s=starts[b];
   for(U x=b*block;x<j;x++)s+=lens[x];
   bool endpoint=flag(bearing,j),inside_c=(chars[j]==c&&p>s),all=inside_c?endpoint:true;
   if(!inside_c){U k=j;bool found=false;while(k>b*block){--k;if(chars[k]==c){all=flag(bearing,k);found=true;break;}}
     if(!found&&cumulative[b][c]>0){ // predecessor may lie in earlier block
       while(b>0&&!found){--b;for(U x=std::min(r,(b+1)*block);x>b*block;){--x;if(chars[x]==c){all=flag(bearing,x);found=true;break;}}}
     }
   }
   return {endpoint,all};
 };
 std::ifstream in(argv[3]);req(bool(in),"query input");std::string line;U nq=0,full=0,partial=0,broken=0,steps=0,endpoint_ok=0,all_ok=0,nonempty=0,missing_witness=0,edge_rescue=0;
 while(std::getline(in,line)){
   if(line.empty())continue;std::string pattern=line.substr(line.find('\t')==std::string::npos?0:line.find('\t')+1);
   // Product frontend reverses the oriented query before indexed search.
   // Reverse-stored references cancel this reversal for the plus strand.
   if(!(flags&4))std::reverse(pattern.begin(),pattern.end());
   for(char&raw:pattern)raw=m[7].bytes?char(a[m[7].off+(uint8_t)raw]):raw;
   U l=0,h=n;bool any_endpoint_bad=false,any_rank_bad=false,ever_nonempty=false;
   auto step=[&](U ll,U rr,unsigned char c)->std::pair<U,U>{
     auto tl=touch(c,ll),th=touch(c,rr);bool e=tl.first&&th.first,good=tl.second&&th.second;
     steps++;endpoint_ok+=e;all_ok+=good;any_endpoint_bad|=!e;any_rank_bad|=!good;
     return {C[c]+rank(c,ll),C[c]+rank(c,rr)};
   };
   if(!ms_mode){for(unsigned char c:pattern){auto [nl,nh]=step(l,h,c);l=nl;h=nh;if(l>=h)break;}ever_nonempty=l<h;}
   else {
     U len=0;
     for(size_t i=0;i<pattern.size();i++){
       unsigned char c=pattern[i];auto [el,er]=step(l,h,c);
       if(el<er){l=el;h=er;len++;ever_nonempty=true;continue;}
       bool found=false;U j=len?len-1:0;
       for(;;){
         U cl=0,cr=n;
         for(size_t t=i-j;t<i;t++){auto [nl,nr]=step(cl,cr,(uint8_t)pattern[t]);cl=nl;cr=nr;if(cl>=cr)break;}
         if(cl<cr){auto [fl,fr]=step(cl,cr,c);if(fl<fr){l=fl;h=fr;len=j+1;found=true;ever_nonempty=true;break;}}
         if(j==0)break;--j;
       }
       if(!found){l=0;h=n;len=0;}
     }
   }
   if(l<h){
     U from=run_of(l),to=run_of(h-1);bool found=false;
     for(U j=from;j<=to;j++){
       U row=starts[j/block];for(U k=(j/block)*block;k<j;k++)row+=lens[k];
       if((flag(head,j)&&row>=l&&row<h)||(flag(tail,j)&&row+lens[j]-1>=l&&row+lens[j]-1<h)){found=true;break;}
     }
     if(!found){bool edge=false;for(U j=from;j<=to;j++){U row=starts[j/block];for(U k=(j/block)*block;k<j;k++)row+=lens[k];if((row>=l&&row<h)||(row+lens[j]-1>=l&&row+lens[j]-1<h)){edge=true;break;}}edge_rescue+=edge;missing_witness++;if(missing_witness<=10)std::cerr<<"MISSING witness query "<<nq<<" interval ["<<l<<","<<h<<") pattern "<<pattern<<"\n";}
   }
   nonempty+=ever_nonempty;full+=!any_rank_bad&&!any_endpoint_bad;partial+=!any_rank_bad&&any_endpoint_bad;broken+=any_rank_bad;nq++;
 }
 std::cout<<",\"mode\":\""<<(ms_mode?"matching-statistics":"prefix")<<"\",\"queries\":"<<nq<<",\"full_queries\":"<<full<<",\"partial_queries\":"<<partial
 <<",\"broken_queries\":"<<broken<<",\"nonempty_queries\":"<<nonempty<<",\"missing_witness\":"<<missing_witness<<",\"edge_rescue\":"<<edge_rescue<<",\"steps\":"<<steps
 <<",\"endpoint_bearing_steps\":"<<endpoint_ok<<",\"all_rank_deps_bearing_steps\":"<<all_ok<<"}\n";
 munmap((void*)a,st.st_size);close(fd);
}
