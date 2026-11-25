#pragma once
// Parse-start navigation from the immutable RAW text and a streaming one-based inverse.u64 only.
// Linux, 64-bit file/address space. No dictionary/key column is opened; no byte SA or rank spool.
#include "view64_proof.hpp"
#include "pfp_trigger.hpp"
#include <array>
#include <bit>
#include <cerrno>
#include <climits>
#include <ctime>
#include <limits>
#include <vector>
#include <fcntl.h>
#include <omp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
namespace rawview {
using U=uint64_t;using Big=__uint128_t;constexpr U W=xsa_pfp::width,B=64;
static_assert(sizeof(size_t)==8 && sizeof(off_t)==8 && std::endian::native==std::endian::little);
[[noreturn]] inline void refuse(const char* s){std::fprintf(stderr,"raw_rank_view64 refused: %s\n",s);std::_Exit(75);}
inline void need(bool x,const char* s){if(!x)refuse(s);}
inline U divup(U n,U d){return n/d+U(n%d!=0);}
inline U cut(U n,U i,U t){return U(Big(n)*i/t);}
inline unsigned width(U max){return std::max(1u,unsigned(divup(std::bit_width(max),8)));}
inline U extent(Big n){need(n>0 && n<=PTRDIFF_MAX,"address extent");return U(n);}
inline U budget(U n,U m,U team){need(n && m && m<=n && team && team<=64 && Big(m)*8<=INT64_MAX,"invalid geometry");
 Big p=width(n),r=width(m),blocks=divup(m,B);
 return extent(n+(p+r)*m+8*blocks+std::max(8*Big(divup(n,64)),p*m+16*blocks)+(4<<20)+team*(2<<20));}
struct Stop {const char* path;U end;void check()const{if(!path)return;if(std::time(nullptr)>=time_t(end)||access(path,F_OK)==0||errno!=ENOENT)refuse("deadline/stop");}};
struct Mem {
 void* ptr=MAP_FAILED;U size=0;
 Mem()=default;Mem(const Mem&)=delete;~Mem(){drop();}
 void drop(){if(ptr!=MAP_FAILED){need(!munmap(ptr,size),"unmap");ptr=MAP_FAILED;size=0;}}
 void open(Big bytes,int fd=-1){need(ptr==MAP_FAILED,"map lifetime");size=extent(bytes);ptr=mmap(nullptr,size,fd<0?PROT_READ|PROT_WRITE:PROT_READ,MAP_PRIVATE|(fd<0?MAP_ANONYMOUS:0),fd,0);need(ptr!=MAP_FAILED,"mapping");}
 U* words(){return (U*)ptr;}const uint8_t* bytes()const{return (const uint8_t*)ptr;}
};
struct Input {
 int fd;U size;
 explicit Input(const char* path):fd(::open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)){struct stat s{};need(fd>=0 && !fstat(fd,&s) && S_ISREG(s.st_mode) && s.st_size>0,"nonempty regular input");size=U(s.st_size);}
 Input(const Input&)=delete;~Input(){close(fd);}
};
// Each slot owns exactly w bytes, so distinct slots can be filled simultaneously even when unaligned.
// Do not over-read a final slot or store a padded eight-byte word over a neighboring field.
struct Column {
 Mem mem;unsigned w=0;U limit=0;
 void open(U n,U max){w=width(max);limit=max;mem.open(Big(n)*w);}
 U get(U i)const{U v=0;std::memcpy(&v,mem.bytes()+i*w,w);return v;}
 void set(U i,U v){need(v<=limit,"column value outside universe");std::memcpy((uint8_t*)mem.ptr+i*w,&v,w);}
};
struct Reader {
 int fd;U pos,end,cursor=0,len=0;std::array<U,8192> buf;
 U get(){if(cursor==len){need(pos<end,"rank read overrun");len=std::min<U>(end-pos,buf.size());cursor=0;
   for(U done=0;done<8*len;){auto z=pread(fd,(uint8_t*)buf.data()+done,8*len-done,8*pos+done);if(z<0 && errno==EINTR)continue;need(z>0,"short rank input");done+=z;}pos+=len;}
  return buf[cursor++];}
};
using xsa_pfp::trigger;
struct Stats {
 U groups=0,owned=0,tape=0,high=0,max_h=0;Big extended=0,carried=0;
 double scan=0,scatter=0,groups_s=0,plcp=0,minima=0,build=0;
};
struct Shard {U group_count=0,owned=0,bytes=0,last_width=0,begin_key=0,carry_key=0;};
struct Word {U p,len,next;};
class View {
 const uint8_t* text;const U n,m;const int team;Stop stop;
 Column pos,psi,aligned;Mem head_mem,tree_mem;U* head=nullptr;U* tree=nullptr;const U blocks;
 struct Stripe {U count=0,ord=0,first=0,high=0,max_h=0;Big extended=0,carried=0;double plcp_s=0;};
 std::vector<Stripe> stripe;
 std::array<Shard,viewproof::shards> shards{};
 int compare(Word a,Word b)const{U common=std::min(a.len,b.len),done=0;
  while(common-done>65536){int c=std::memcmp(text+a.p+done,text+b.p+done,65536);if(c)return c;done+=65536;stop.check();}
  int c=std::memcmp(text+a.p+done,text+b.p+done,common-done);if(c)return c;
  if(a.len!=b.len){return a.len<b.len?-1:1;}
  return int(a.next==0)-int(b.next==0);}
 U extend(U p,U q,U h)const{U len=n-std::max(p,q);need(h<=len,"impossible PLCP carry");
  while(len-h>=8){U a,b;std::memcpy(&a,text+p+h,8);std::memcpy(&b,text+q+h,8);U x=a^b;if(x)return h+std::countr_zero(x)/8;h+=8;if((h&65535)<8)stop.check();}
  while(h<len && text[p+h]==text[q+h])++h;
  return h;}
 public:
 Stats stats;
 View(const uint8_t* raw,U size,U count,int workers,Stop guard):text(raw),n(size),m(count),team(workers),stop(guard),blocks(divup(m,B)),stripe(workers){}
 U slot(U rank)const{need(rank && rank<=m,"rank outside view");return rank-1;}
 U position(U r)const{return pos.get(slot(r));}U successor(U r)const{return psi.get(slot(r));}U previous_lcp(U r)const{return aligned.get(slot(r));}
 Word word(U r)const{U p=position(r),next=successor(r),q=next?position(next):n;
  need(p<q && q<=n && (!next || n-q>=W),"invalid owned span");return {p,q-p+(next?W:0),next};}
 uint16_t predecessor(U p)const{need(p<n,"not a text start");return p?text[p-1]:256;}
 bool is_head(U r)const{U i=slot(r);return (head[i/B]>>(i%B))&1;}
 U after_head(U r)const{need(r && r<=m && is_head(r),"not a group start");U w=(r-1)/B,x=head[w]&((UINT64_MAX<<((r-1)%B))<<1);
  if(x)return w*B+std::countr_zero(x)+1;
  while(++w<blocks){if(head[w])return w*B+std::countr_zero(head[w])+1;if(!(w%65536))stop.check();}
  return m+1;}
 U lce(U a,U b)const{need(std::max(a,b)<=m,"LCE outside ranks");if(!a||!b)return 0;if(a==b)return n-position(a);
  U l=std::min(a,b),r=std::max(a,b),h=UINT64_MAX;
  while(l<r && l%B)h=std::min(h,aligned.get(l++));
  while(l<r && r%B)h=std::min(h,aligned.get(--r));
  for(l=l/B+blocks,r=r/B+blocks;l<r;l/=2,r/=2){if(l%2)h=std::min(h,tree[l++]);if(r%2)h=std::min(h,tree[--r]);}return h;}
 const Shard& shard(unsigned i)const{need(i<shards.size(),"shard outside view");return shards[i];}
 U stripe_count(unsigned i)const{return stripe.at(i).count;}
 double stripe_plcp_s(unsigned i)const{return stripe.at(i).plcp_s;}
 Big stripe_extended(unsigned i)const{return stripe.at(i).extended;}
 void build(int inverse,U grant){need(budget(n,m,team)<=grant,"array grant");stop.check();double start=omp_get_wtime();
  Mem marked;U nw=divup(n,64);marked.open(8*Big(nw));U* bits=marked.words();
#pragma omp parallel num_threads(team)
  {int t=omp_get_thread_num();need(omp_get_num_threads()==team,"runtime team");U c=0;
   for(U w=cut(nw,t,team),e=cut(nw,t+1,team);w<e;++w){if(!(w%16384))stop.check();U mask=0;
    for(U p=w*64,last=std::min(n,(w+1)*64);p<last;++p){if(!p || (n-p>=W && trigger(text+p)))mask|=U(1)<<(p%64);}
    bits[w]=mask;c+=std::popcount(mask);
   }stripe[t].count=c;}
  U found=0;for(auto& s:stripe){s.ord=found;found+=s.count;}need(found==m,"cut/rank count mismatch");
  double scanned=omp_get_wtime();pos.open(m,n);psi.open(m,m);head_mem.open(8*Big(blocks));head=head_mem.words();
#pragma omp parallel num_threads(team)
  {int t=omp_get_thread_num();auto& s=stripe[t];
   if(s.count){U last=s.ord+s.count,j=s.ord;Reader input{inverse,j,last+U(last<m),0,0,{}};U rank=input.get();s.first=rank;
    for(U w=cut(nw,t,team),e=cut(nw,t+1,team);w<e;++w){if(!(w%16384))stop.check();
     for(U x=bits[w];x;x&=x-1){U p=w*64+std::countr_zero(x),next=j+1<m?input.get():0;
      need(rank && rank<=m && next<=m && bool(next)==(j+1<m),"noncanonical rank");U at=rank-1,bit=U(1)<<(at%B);
      // A corrupt permutation must not induce a data race. Claim each destination before either packed write.
      need(!(__atomic_fetch_or(head+at/B,bit,__ATOMIC_RELAXED)&bit),"duplicate rank");
      pos.set(at,p);psi.set(at,next);s.high+=p>UINT32_MAX;rank=next;++j;
     }}need(j==last,"stripe rows");}}
  marked.drop();for(const auto& s:stripe)stats.high+=s.high;double scattered=omp_get_wtime();
  // Reuse the permutation bitmap. Shard cuts are between complete bitmap words, not necessarily between heads.
#pragma omp parallel for schedule(static) num_threads(team)
  for(unsigned i=0;i<viewproof::shards;++i){auto& s=shards[i];U lo=viewproof::boundary(m,i),hi=viewproof::boundary(m,i+1);
   Word prev{};if(lo && lo<hi)prev=word(lo);
   for(U w=lo/B;w<divup(hi,B) && lo<hi;++w){if(!(w%1024))stop.check();U last=std::min(m,(w+1)*B);U expect=last%B?(U(1)<<(last%B))-1:UINT64_MAX;
    need(head[w]==expect,"missing permutation slots");U mask=0;
    for(U slot=w*B;slot<last;++slot){Word curr=word(slot+1);int c=slot?compare(prev,curr):-1;
     need(c<0 || (c==0 && prev.next<curr.next),"invalid raw-word successor recurrence");
     if(c<0){mask|=U(1)<<(slot%B);++s.group_count;s.last_width=8+curr.len;need(s.last_width<=INT64_MAX && s.bytes<=U(INT64_MAX)-s.last_width,"lexical shard extent");s.bytes+=s.last_width;s.owned+=curr.len-(curr.next?W:0);}
     prev=curr;
    }head[w]=mask;
   }}
  Big sum=0;U carry=0;for(auto& s:shards){s.begin_key=U(sum);s.carry_key=carry;sum+=s.bytes;need(sum<=INT64_MAX,"dictionary extent");if(s.group_count)carry=U(sum)-s.last_width;stats.groups+=s.group_count;stats.owned+=s.owned;}
  need(sum==Big(stats.owned)+16*Big(stats.groups)-8,"canonical dictionary coverage");stats.tape=U(sum);double grouped=omp_get_wtime();
  aligned.open(m,n);
#pragma omp parallel for schedule(static,1) num_threads(team)
  for(int t=0;t<team;++t){auto& s=stripe[t];U rank=s.first,h=0;double lane_start=omp_get_wtime();
   for(U j=0;j<s.count;++j){if(!(j%16384))stop.check();U p=position(rank),next=successor(rank);
    if(rank==1){need(!h,"carry on first rank");}else{U q=position(rank-1),prior=h;h=extend(p,q,h);s.extended+=h-prior;s.carried+=prior;
     need(p!=q && (q+h==n || (p+h<n && text[q+h]<text[p+h])),"raw adjacent suffix mismatch");}
    aligned.set(rank-1,h);s.max_h=std::max(s.max_h,h);
    if(next){U stride=position(next)-p;h=(h>=stride && h-stride>=W)?h-stride:0;}rank=next;
   }
   U a=t+1;while(a<U(team) && !stripe[a].count)++a;if(s.count)need(rank==(a==U(team)?0:stripe[a].first),"Psi crossed wrong stripe seam");s.plcp_s=omp_get_wtime()-lane_start;}
  for(const auto& s:stripe){stats.extended+=s.extended;stats.carried+=s.carried;stats.max_h=std::max(stats.max_h,s.max_h);}
  need(stats.extended<=Big(n)*(team+1)+Big(W)*m,"sparse PLCP extension bound");double plcp=omp_get_wtime();
  tree_mem.open(16*Big(blocks));tree=tree_mem.words();
#pragma omp parallel for schedule(static) num_threads(team)
  for(U b=0;b<blocks;++b){if(!(b%1024))stop.check();U h=UINT64_MAX;for(U j=b*B;j<std::min(m,(b+1)*B);++j)h=std::min(h,aligned.get(j));tree[blocks+b]=h;}
  for(U b=blocks-1;b;--b){if(!(b%65536))stop.check();tree[b]=std::min(tree[2*b],tree[2*b+1]);}stop.check();double end=omp_get_wtime();
  stats.scan=scanned-start;stats.scatter=scattered-scanned;stats.groups_s=grouped-scattered;stats.plcp=plcp-grouped;stats.minima=end-plcp;stats.build=end-start;
 }
};
} // namespace rawview
