// Exact parallel token-suffix ranks from arbitrary unsigned 64-bit lexical keys.
// This is the parse only: never constructs byte suffixes, and never persists its SA.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <omp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

using U=uint64_t;using Big=__uint128_t;namespace fs=std::filesystem;
static_assert(sizeof(size_t)==8 && sizeof(off_t)==8 && std::endian::native==std::endian::little);
static void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static U number(const char*s){need(*s,"empty integer");U v=0;for(;*s;++s){need(*s>='0' && *s<='9',"decimal integer");unsigned d=*s-'0';need(v<=(UINT64_MAX-d)/10,"integer overflow");v=v*10+d;}return v;}
static const char* flag;static U deadline;
static void guard(){if(time(nullptr)>=time_t(deadline) || faccessat(AT_FDCWD,flag,F_OK,0)==0 || errno!=ENOENT){std::fprintf(stderr,"STOP parse64 deadline/flag\n");std::_Exit(75);}}
static double cpu(){timespec t{};need(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t),"process clock");return t.tv_sec+t.tv_nsec*1e-9;}
struct Map{void*p=MAP_FAILED;size_t bytes=0;int fd=-1;Map()=default;Map(const Map&)=delete;~Map(){release();}void release(){if(p!=MAP_FAILED){munmap(p,bytes);p=MAP_FAILED;}if(fd>=0){close(fd);fd=-1;}}
 void file(const char*path){fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);struct stat s{};need(fd>=0 && !fstat(fd,&s) && S_ISREG(s.st_mode) && s.st_size>0,"expected nonempty regular file");bytes=s.st_size;p=mmap(nullptr,bytes,PROT_READ,MAP_PRIVATE,fd,0);need(p!=MAP_FAILED,"file mapping");}
 void alloc(Big n){need(n>0 && n<=PTRDIFF_MAX,"address extent");bytes=size_t(n);p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);need(p!=MAP_FAILED,"workspace mapping");}};
struct Entry{U key,pos;};static_assert(sizeof(Entry)==16);
static constexpr U CONTROL=4ull<<20;  // fixed control/IO allowance; runtime stacks + OS charged by service
static constexpr U WORKER=2ull<<20;
static U bytes_for(U n,U threads){need(threads>=1 && threads<=64,"worker extent");Big x=40*Big(n)+CONTROL+WORKER*threads;need(n && x<=PTRDIFF_MAX && 8*Big(n)<=INT64_MAX,"rank extent");return U(x);}
static U cut(U n,U i,U count){return U(Big(n)*i/count);}
struct alignas(64) Histogram{std::array<U,256> cursor;};
struct Tile{U begin,end,head,active;};
static int team(U q){need(q>=1 && q<=64 && q<=U(omp_get_num_procs()),"invalid worker count");omp_set_dynamic(0);omp_set_num_threads(int(q));int actual=0;
#pragma omp parallel
 {
#pragma omp single
 actual=omp_get_num_threads(); }
 need(actual==int(q),"runtime did not supply requested team");return actual;}
// Atomic no-replace publication; partial writes never become the requested product.
struct Out{std::string tmp,dest;int fd;Out(const char*path):tmp(std::string(path)+".partial"),dest(path){need(!fs::exists(dest),"output exists");fd=open(tmp.c_str(),O_CREAT|O_EXCL|O_NOFOLLOW|O_WRONLY|O_CLOEXEC,0600);need(fd>=0,"create exclusive partial output");}
 ~Out(){if(fd>=0)close(fd);}void put(const void*p,size_t n){auto b=(const char*)p;while(n){guard();ssize_t r=write(fd,b,std::min(n,size_t(4<<20)));if(r<0 && errno==EINTR)continue;need(r>0,"output write");b+=r;n-=r;}}
 void finish(){guard();need(!fsync(fd),"fsync output");need(!close(fd),"close output");fd=-1;guard();need(!link(tmp.c_str(),dest.c_str()),"exclusive final output");need(!unlink(tmp.c_str()),"retire partial name");}};
static void ranks(const char*input,const char*output,U workers,U grant){int nt=team(workers);Map src;src.file(input);need(!(src.bytes%8),"partial 64-bit key");U n=src.bytes/8,peak=bytes_for(n,workers);need(peak<=grant,"rank array grant exceeded");guard();auto keys=(const U*)src.p;
 double start=omp_get_wtime(),c0=cpu();Map a,b;a.alloc(16*Big(n));b.alloc(16*Big(n));auto v=(Entry*)a.p,w=(Entry*)b.p;U high=0,varied=0;U first=keys[0];
#pragma omp parallel for schedule(static) reduction(+:high) reduction(|:varied)
 for(U i=0;i<n;++i){U key=keys[i];v[i]={key,i};high+=key>UINT32_MAX;varied|=key^first;}
 std::vector<Histogram> hist(nt);unsigned digits=0;
 for(unsigned shift=0;shift<64;shift+=8){if(!(255&(varied>>shift)))continue;guard();++digits;
#pragma omp parallel
  {int t=omp_get_thread_num();auto&h=hist[t].cursor;h.fill(0);for(U j=cut(n,t,nt),e=cut(n,t+1,nt);j<e;++j)++h[uint8_t(v[j].key>>shift)];}
  U s=0;for(unsigned i=0;i<256;++i)for(auto&h:hist){U k=h.cursor[i];h.cursor[i]=s;s+=k;}need(s==n,"histogram extent");
#pragma omp parallel
  {int t=omp_get_thread_num();auto&h=hist[t].cursor;for(U j=cut(n,t,nt),e=cut(n,t+1,nt);j<e;++j){auto x=v[j];w[h[uint8_t(x.key>>shift)]++]=x;}}
  std::swap(v,w);
 }
 if(v==a.p)b.release();else a.release();w=nullptr;double initial=omp_get_wtime()-start;
 Map rmem,links;rmem.alloc(8*Big(n));links.alloc(8*Big(n));auto r=(U*)rmem.p,link=(U*)links.p;
 // Cut only BETWEEN first symbols. All subsequent groups refine these disjoint intervals.
 std::vector<Tile> tiles;U prev=0,goal=workers*32;for(U t=1;t<=goal;++t){U e=cut(n,t,goal);if(e<=prev)continue;if(e<n && v[e-1].key==v[e].key)e=std::upper_bound(v+e,v+n,v[e].key,[](U x,Entry y){return x<y.key;})-v;if(e>prev){tiles.push_back({prev,e,e,0});prev=e;}}need(prev==n,"tile coverage");
 U active=0,maxhead=0,groups=0;double t0=omp_get_wtime();
#pragma omp parallel for schedule(dynamic,1) reduction(+:active,groups) reduction(max:maxhead)
 for(size_t s=0;s<tiles.size();++s){auto&t=tiles[s];U last=t.end;for(U i=t.begin;i<t.end;){U e=i+1;while(e<t.end && v[e].key==v[i].key)++e;for(U j=i;j<e;++j)r[v[j].pos]=i+1;++groups;maxhead=std::max(maxhead,e-i);
   if(e-i>=2){if(last==t.end)t.head=i;else link[last+1]=i;last=i;link[i]=e;t.active+=e-i;}i=e;}if(last!=t.end)link[last+1]=t.end;active+=t.active;}
 std::printf("HEAD rows=%llu tiles=%zu distinct=%llu max_group=%llu unresolved=%llu radix_passes=%u keys_above_u32=%llu requested=%d team=%d\n",(unsigned long long)n,tiles.size(),(unsigned long long)groups,(unsigned long long)maxhead,(unsigned long long)active,digits,(unsigned long long)high,int(workers),nt);std::fflush(stdout);
 U rounds=0,depth=1;while(active){guard();need(depth<n,"unresolved after full parse");U left=0,maxgroup=0;
 // Gather ALL second ranks before overwriting ANY old rank. No shared histogram by max key.
#pragma omp parallel for schedule(dynamic,1)
  for(size_t s=0;s<tiles.size();++s){auto&t=tiles[s];for(U i=t.head;i<t.end;i=link[i+1])for(U j=i;j<link[i];++j){U p=v[j].pos;v[j].key=p<n-depth?r[p+depth]:0;}}
#pragma omp parallel for schedule(dynamic,1) reduction(+:left) reduction(max:maxgroup)
  for(size_t s=0;s<tiles.size();++s){auto&t=tiles[s];U old=t.head,last=t.end;t.head=t.end;t.active=0;
   while(old<t.end){U e=link[old],following=link[old+1];if(e-old>1048576){U cmp=0;std::sort(v+old,v+e,[&cmp](Entry a,Entry b){if(!(++cmp%1048576))guard();return a.key<b.key;});}else std::sort(v+old,v+e,[](Entry a,Entry b){return a.key<b.key;});
    for(U i=old;i<e;){U z=i+1;while(z<e && v[z].key==v[i].key)++z;for(U j=i;j<z;++j)r[v[j].pos]=i+1;
     if(z-i>=2){if(last==t.end)t.head=i;else link[last+1]=i;last=i;link[i]=z;t.active+=z-i;maxgroup=std::max(maxgroup,z-i);}i=z;}old=following;}
   if(last!=t.end){link[last+1]=t.end;}
   left+=t.active;}
  active=left;depth*=2;++rounds;std::printf("REFINE depth=%llu unresolved=%llu max_group=%llu\n",(unsigned long long)depth,(unsigned long long)active,(unsigned long long)maxgroup);std::fflush(stdout);
 }
 double refine=omp_get_wtime()-t0;links.release();guard();double tc=omp_get_wtime();int bad=0;
#pragma omp parallel for schedule(static) reduction(|:bad)
 for(U j=0;j<n;++j){U p=v[j].pos;if(p>=n || r[p]!=j+1){bad=1;continue;}if(j){U q=v[j-1].pos;if(q>=n){bad=1;continue;}U x=keys[q],y=keys[p];if(x>y || (x==y && (q+1<n?r[q+1]:0)>=(p+1<n?r[p+1]:0)))bad=1;}}
 need(!bad,"rank permutation or suffix recurrence");double cert=omp_get_wtime()-tc;a.release();b.release();src.release();guard();double tw=omp_get_wtime();Out out(output);out.put(r,8*n);out.finish();std::printf("PARSE64 rows=%llu key_bits=64 rank_bits=64 array_bound=%llu rounds=%llu initial_s=%.6f refine_s=%.6f certify_s=%.6f write_fsync_s=%.6f prepared_s=%.6f cpu_s=%.6f permutation_recurrence=1\n",(unsigned long long)n,(unsigned long long)peak,(unsigned long long)rounds,initial,refine,cert,omp_get_wtime()-tw,omp_get_wtime()-start,cpu()-c0);}
// Compatibility only: read the banked u32 ID wire and emit the REAL packed record address of each word.
static void lift(const char*tape,const char*parse,const char*out,U k,U workers,U grant){team(workers);Map dict,ids,dir;dict.file(tape);ids.file(parse);need(!(ids.bytes%4) && k && k<=ids.bytes/4 && k<=UINT32_MAX,"legacy lexical ID extent");need(Big(dict.bytes)+ids.bytes+8*Big(k)+(16<<20)+workers*WORKER<=grant,"adapter grant");dir.alloc(8*Big(k));auto starts=(U*)dir.p;auto raw=(const char*)dict.p;U at=0,eof=k;std::string_view previous;bool prev_end=false;
 for(U i=0;i<k;++i){if(!(i%65536))guard();need(dict.bytes-at>=4,"short packed header");uint32_t h;std::memcpy(&h,raw+at,4);bool tagged=h>>31;U len=h&0x7fffffff;starts[i]=at;at+=4;need(len>(tagged?0u:8u) && len<=dict.bytes-at,"bad packed length");std::string_view word(raw+at,len);if(i)need(previous<word || (previous==word && !prev_end && tagged),"lexical record order");if(tagged){need(eof==k,"duplicate EOF");eof=i;}previous=word;prev_end=tagged;at+=len;}
 need(at==dict.bytes && eof<k,"dictionary extent/EOF");auto id=(const uint32_t*)ids.p;U n=ids.bytes/4;Out dest(out);std::vector<U> buf(1<<17);U high=0,maximum=0;
 for(U i=0;i<n;){guard();U take=std::min<U>(buf.size(),n-i);int bad=0;
#pragma omp parallel for schedule(static) reduction(|:bad) reduction(+:high) reduction(max:maximum)
  for(U j=0;j<take;++j){U x=id[i+j];if(x>=k || ((x==eof)!=(i+j+1==n))){bad=1;continue;}U p=starts[x];buf[j]=p;high+=p>UINT32_MAX;maximum=std::max(maximum,p);}need(!bad,"invalid legacy occurrence");dest.put(buf.data(),8*take);i+=take;}
 dest.finish();std::printf("KEYS64 rows=%llu k=%llu max_key=%llu keys_above_u32=%llu packed_bytes=%zu\n",(unsigned long long)n,(unsigned long long)k,(unsigned long long)maximum,(unsigned long long)high,dict.bytes);}
int main(int ac,char**av)try{std::string mode=ac>1?av[1]:"";if(ac==4 && mode=="plan"){std::printf("parse64_array_bound=%llu\n",(unsigned long long)bytes_for(number(av[2]),number(av[3])));return 0;}
 need((ac==8 && mode=="rank")||(ac==10 && mode=="lift"),"rank KEYS.u64 NEW_INVERSE.u64 THREADS ARRAY_BYTES STOP DEADLINE | lift DICT.packed PARSE.u32 NEW_KEYS.u64 K THREADS ARRAY_BYTES STOP DEADLINE | plan M THREADS");flag=av[ac-2];deadline=number(av[ac-1]);need(deadline<=INT64_MAX,"deadline extent");guard();if(mode=="rank")ranks(av[2],av[3],number(av[4]),number(av[5]));else lift(av[2],av[3],av[4],number(av[5]),number(av[6]),number(av[7]));return 0;}catch(const std::exception&e){std::fprintf(stderr,"parse64 refused: %s\n",e.what());return 75;}
