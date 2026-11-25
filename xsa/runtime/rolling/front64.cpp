// Raw bytes -> lexical 64-bit records and their text-order u64 addresses.
// Linux/LE only. Uses no ordinal spools, dense-ID format, or byte suffix array.
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#include <fcntl.h>
#include <omp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
using U=uint64_t;using Big=__uint128_t;namespace fs=std::filesystem;
static_assert(sizeof(off_t)==8 && sizeof(size_t)==8 && std::endian::native==std::endian::little);
[[noreturn]] static void fail(const char*why){std::fprintf(stderr,"front64 refused: %s\n",why);std::_Exit(75);}
static void ck(bool ok,const char*why){if(!ok)fail(why);}
static U num(const char*s){U v=0;ck(*s,"empty integer");for(;*s;++s){ck(*s>='0' && *s<='9',"nondecimal integer");U digit=*s-'0';ck(v<=(UINT64_MAX-digit)/10,"integer overflow");v=v*10+digit;}return v;}
static const char*flag;static U deadline;
static void guard(){if(!flag)return;if(std::time(nullptr)>=time_t(deadline) || !access(flag,F_OK) || errno!=ENOENT)fail("deadline/stop");}
static constexpr U BINS=4096,W=8,BLOCK=1<<20;
struct Entry{U prefix,ord;};static_assert(sizeof(Entry)==16);
static U budget(U n,U m,U t){Big x=n+24*Big(m)+8+(4ull<<20)+t*(2ull<<20);ck(n && m && m<=n && t>=1 && t<=64 && x<=PTRDIFF_MAX,"address extent");return U(x);}
static U cut(U n,U i,U q){return U(Big(n)*i/q);}
static void* anon(U bytes){void*p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);ck(p!=MAP_FAILED,"array mapping");return p;}
static bool trigger(const uint8_t*w){U h=1469598103934665603ull;for(U i=0;i<W;++i)h=(h^w[i])*1099511628211ull;return h%31==0;}
static void writeat(int fd,const uint8_t*data,U len,U at){while(len){guard();ssize_t z=pwrite(fd,data,std::min<U>(len,4<<20),at);if(z<0 && errno==EINTR)continue;ck(z>0,"short file write");at+=z;len-=z;data+=z;}}
struct Buffer{int fd;U at,used=0;std::array<uint8_t,65536>b;
 void flush(){if(used)writeat(fd,b.data(),used,at);at+=used;used=0;}
 void put(const void*p,U n){auto*s=(const uint8_t*)p;while(n){U k=std::min<U>(n,b.size()-used);std::memcpy(b.data()+used,s,k);used+=k;s+=k;n-=k;if(used==b.size())flush();}}};
static int file(const fs::path&p,U size){ck(size<=INT64_MAX,"file extent");int f=open(p.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|O_CLOEXEC,0600);ck(f>=0 && !ftruncate(f,size),"size new column");return f;}
static void finish(int fd){ck(!fsync(fd) && !close(fd),"column fsync/close");}
static void syncdir(const fs::path&p){int fd=open(p.c_str(),O_DIRECTORY|O_RDONLY|O_CLOEXEC);ck(fd>=0,"directory open");finish(fd);}
// One writer per bin per phase. No atomics, clocks or shared telemetry in row loops.
struct BinMeter {
 double sort=0, label=0, publish=0;
 int order_lane=-1, publish_lane=-1;
};
static void report_bins(const std::array<U,BINS+1>& offsets, const std::vector<BinMeter>& meters,
                        int team, bool publishing, double wall) {
 struct Lane { U bins=0, rows=0; double active=0; };
 std::vector<Lane> lanes(team);
 const char* phase=publishing?"publish":"order";
 double longest=-1,sort=0,label=0,active=0;
 U longest_bin=0;
 for(U b=0;b<BINS;++b){
  U rows=offsets[b+1]-offsets[b]; if(!rows)continue;
  const auto& m=meters[b];int tid=publishing?m.publish_lane:m.order_lane;
  ck(tid>=0 && tid<team,"missing bin worker");
  double dt=publishing?m.publish:m.sort+m.label;auto& lane=lanes[tid];
  ++lane.bins;lane.rows+=rows;lane.active+=dt;active+=dt;
  if(dt>longest){longest=dt;longest_bin=b;}
  if(!publishing){sort+=m.sort;label+=m.label;}
 }
 for(int tid=0;tid<team;++tid){const auto& a=lanes[tid];
  std::printf("FRONT64_LANE phase=%s lane=%d bins=%llu rows=%llu active_s=%.9f\n",phase,tid,(unsigned long long)a.bins,(unsigned long long)a.rows,a.active);
 }
 std::printf("FRONT64_BINS phase=%s wall_s=%.9f active_s=%.9f sort_s=%.9f label_s=%.9f longest_bin=%llu longest_rows=%llu longest_s=%.9f\n",phase,wall,active,sort,label,(unsigned long long)longest_bin,(unsigned long long)(offsets[longest_bin+1]-offsets[longest_bin]),longest);
}
static void front(const char*input,const fs::path&out,U threads,U grant,U scratch){
 ck(threads>=1 && threads<=64 && threads<=U(omp_get_num_procs()),"worker extent");omp_set_dynamic(0);omp_set_num_threads(int(threads));int actual=0;
#pragma omp parallel
 {
#pragma omp single
  actual=omp_get_num_threads();
 }
 ck(U(actual)==threads,"wrong OpenMP team");int fd=open(input,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);struct stat st{};
 ck(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>0,"nonempty immutable regular input required");U n=st.st_size;
 ck(budget(n,1,threads)<=grant && !fs::exists(out),"input grant or existing destination");auto raw=(const uint8_t*)mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);ck(raw!=MAP_FAILED,"raw mapping");double t0=omp_get_wtime();
 // Every possible trigger start belongs to exactly one lane; the eight bytes may cross a lane boundary.
 U windows=n>W?n-W:0;std::vector<U> count(threads),base(threads);U m=1;
 auto scan=[&](U lane,auto hit){U lo=1+cut(windows,lane,threads),hi=1+cut(windows,lane+1,threads);for(U a=lo;a<hi;){guard();U e=a+std::min(BLOCK,hi-a);for(U i=a;i<e;++i)if(trigger(raw+i))hit(i);a=e;}};
#pragma omp parallel
 {U lane=omp_get_thread_num(),c=0;scan(lane,[&](U){++c;});count[lane]=c;}
 for(U t=0;t<threads;++t){base[t]=m;m+=count[t];}U arrays=budget(n,m,threads);ck(arrays<=grant,"front array grant exceeded");auto starts=(U*)anon(8*(m+1));starts[0]=0;starts[m]=n;
#pragma omp parallel
 {U lane=omp_get_thread_num(),j=base[lane];scan(lane,[&](U pos){ck(j<base[lane]+count[lane],"inconsistent trigger scans");starts[j++]=pos;});ck(j==base[lane]+count[lane],"missing starts");}
 auto len=[&](U j){return starts[j+1]-starts[j]+(j+1<m?W:0);};
 auto key=[&](U j){U p=0;std::memcpy(&p,raw+starts[j],8);return __builtin_bswap64(p);};
 // Short EOF records must not read through the mapping; the only short record is the entire n<8 input.
 auto prefix=[&](U j){if(n-starts[j]>=8)return key(j);U p=0;std::memcpy(&p,raw+starts[j],n-starts[j]);return __builtin_bswap64(p);};
 auto less=[&](Entry x,Entry y){if(x.prefix!=y.prefix)return x.prefix<y.prefix;U a=len(x.ord),b=len(y.ord),l=std::min(a,b);if(l>8){int c=std::memcmp(raw+starts[x.ord]+8,raw+starts[y.ord]+8,l-8);if(c)return c<0;}return a!=b?a<b:x.ord+1!=m && y.ord+1==m;};
 double ts=omp_get_wtime();auto v=(Entry*)anon(16*m);std::vector<std::array<U,BINS>> hist(threads);std::array<U,BINS+1> bins{},col{};U above=0,longest=0;
#pragma omp parallel reduction(+:above) reduction(max:longest)
 {U lane=omp_get_thread_num();auto&h=hist[lane];for(U j=cut(m,lane,threads);j<cut(m,lane+1,threads);++j){if(!(j%65536))guard();++h[prefix(j)>>52];above+=starts[j]>UINT32_MAX;longest=std::max(longest,len(j));}}
 U maxbin=0,present=0;for(U b=0;b<BINS;++b){U s=bins[b];for(auto&h:hist){U c=h[b];h[b]=s;s+=c;}bins[b+1]=s;maxbin=std::max(maxbin,s-bins[b]);present+=s>bins[b];}ck(bins.back()==m,"histogram total");
#pragma omp parallel
 {U lane=omp_get_thread_num();auto&h=hist[lane];for(U j=cut(m,lane,threads);j<cut(m,lane+1,threads);++j){if(!(j%65536))guard();U p=prefix(j);v[h[p>>52]++]={p,j};}}
 std::vector<BinMeter> meters(BINS);
 double tp=omp_get_wtime();U k=0,owned=0,term=0;
 // Equal full records necessarily share a bin. Only this local sort compares raw trailing bytes.
#pragma omp parallel for schedule(dynamic,1) reduction(+:k,owned,term)
 for(U b=0;b<BINS;++b){guard();U lo=bins[b],hi=bins[b+1],calls=0;double begin=omp_get_wtime();std::sort(v+lo,v+hi,[&](Entry x,Entry y){if(!(++calls%BLOCK))guard();return less(x,y);});
  double sorted=omp_get_wtime();Entry last{};U bytes=0,loc=0;for(U j=lo;j<hi;++j){if(!(j%65536))guard();Entry e=v[j];if(j==lo || less(last,e)){U l=len(e.ord);loc=bytes;bytes+=8+l;++k;owned+=starts[e.ord+1]-starts[e.ord];term+=e.ord+1==m;}last=e;v[j].prefix=loc;}col[b+1]=bytes;
  meters[b].order_lane=omp_get_thread_num();meters[b].sort=sorted-begin;meters[b].label=omp_get_wtime()-sorted;}
 double ordered=omp_get_wtime();
 // Lengths are bounded by n, only one terminal. Sum in a wide domain BEFORE converting to file offsets.
 Big total=0;for(U b=1;b<=BINS;++b){total+=col[b];ck(total<=INT64_MAX,"dictionary exceeds off_t");col[b]=U(total);}U tape=U(total);Big combined=total+8*Big(m);ck(term==1 && total==Big(owned)+16*Big(k)-8 && combined<=scratch && combined<=2*Big(n),"column scratch/geometry");
 guard();auto partial=out;partial+=".partial";ck(fs::create_directory(partial),"fresh partial directory");int dict=file(partial/"dictionary.lex64",tape);double td=omp_get_wtime();
#pragma omp parallel for schedule(dynamic,1)
 for(U b=0;b<BINS;++b){double begin=omp_get_wtime();Buffer w{dict,col[b],0,{}};U prev=UINT64_MAX;for(U j=bins[b];j<bins[b+1];++j){if(!(j%65536))guard();auto&e=v[j];U pos=e.prefix;if(pos!=prev){ck(w.at+w.used==col[b]+pos,"range offset");U l=len(e.ord),tag=(l<<1)|U(e.ord+1==m);w.put(&tag,8);w.put(raw+starts[e.ord],l);}prev=pos;e.prefix+=col[b];}w.flush();ck(w.at==col[b+1],"range end");meters[b].publish_lane=omp_get_thread_num();meters[b].publish=omp_get_wtime()-begin;}
 double published=omp_get_wtime();finish(dict);munmap(const_cast<uint8_t*>(raw),n);close(fd);double tw=omp_get_wtime();U high_keys=0;
 // All raw lengths are dead. Reuse starts, indexed by the original occurrence ordinal, for the key column.
#pragma omp parallel for schedule(static) reduction(+:high_keys)
 for(U j=0;j<m;++j){starts[v[j].ord]=v[j].prefix;high_keys+=v[j].prefix>UINT32_MAX;}
 munmap(v,16*m);int keys=file(partial/"keys.u64",8*m);writeat(keys,(const uint8_t*)starts,8*m,0);finish(keys);munmap(starts,8*(m+1));syncdir(partial);guard();ck(syscall(SYS_renameat2,AT_FDCWD,partial.c_str(),AT_FDCWD,out.c_str(),RENAME_NOREPLACE)==0,"publish columns");syncdir(out.has_parent_path()?out.parent_path():fs::path("."));
 double end=omp_get_wtime();
 report_bins(bins,meters,actual,false,ordered-tp);
 report_bins(bins,meters,actual,true,published-td);
 std::printf("FRONT64 n=%llu m=%llu k=%llu descriptors=%llu max_phrase=%llu starts_above_u32=%llu keys_above_u32=%llu dictionary_bytes=%llu key_bytes=%llu arrays=%llu team=%d nonempty_bins=%llu max_bin=%llu scan_s=%.6f partition_s=%.6f sort_dedup_s=%.6f publish_dict_s=%.6f publish_keys_s=%.6f raw_s=%.6f\n",(unsigned long long)n,(unsigned long long)m,(unsigned long long)k,(unsigned long long)owned,(unsigned long long)longest,(unsigned long long)above,(unsigned long long)high_keys,(unsigned long long)tape,(unsigned long long)(8*m),(unsigned long long)arrays,actual,(unsigned long long)present,(unsigned long long)maxbin,ts-t0,tp-ts,td-tp,tw-td,end-tw,end-t0);
}
#include "front64_positions.hpp"
int main(int ac,char**av)try{
 if(ac==8 && std::string(av[1])=="keys"){ // RAW KEYS HISTOGRAM THREADS MEMORY SCRATCH
  posfront::run(av[2],av[3],num(av[5]),num(av[6]),num(av[7]),false,av[4]);return 0;
 }
 if(ac==5 && std::string(av[1])=="plan-pos"){std::printf("front64_position_bound=%llu\n",(unsigned long long)posfront::scheduled(num(av[2]),num(av[3]),num(av[4])));return 0;}
 if(ac==9 && (std::string(av[1])=="pos" || std::string(av[1])=="pos-manifest")){flag=av[7];deadline=num(av[8]);ck(deadline<=INT64_MAX,"deadline extent");guard();posfront::run(av[2],av[3],num(av[4]),num(av[5]),num(av[6]),std::string(av[1])=="pos-manifest");return 0;}
 if(ac==5 && std::string(av[1])=="plan"){ std::printf("front64_array_bound=%llu\n",(unsigned long long)budget(num(av[2]),num(av[3]),num(av[4])));return 0;}ck(ac==8,"front64 [pos] RAW NEW_DIR THREADS ARRAY_BYTES SCRATCH_BYTES STOP DEADLINE or plan|plan-pos N M THREADS");flag=av[6];deadline=num(av[7]);ck(deadline<=INT64_MAX,"deadline extent");guard();front(av[1],av[2],num(av[3]),num(av[4]),num(av[5]));return 0;}catch(const std::exception&e){fail(e.what());}
