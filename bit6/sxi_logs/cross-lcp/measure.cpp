// Standalone measurement, NOT a merge implementation. All input handles are read-only.
// c++ -O3 -std=c++17 measure.cpp -o /tmp/cross-lcp-measure
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
using U=uint64_t;
using Clock=std::chrono::steady_clock;
static void need(bool v,const char* s){if(!v)throw std::runtime_error(s);}
struct H {U a=0,b=0; bool operator==(H x)const{return a==x.a&&b==x.b;}};
static H add(H x,H y){return {x.a+y.a,x.b+y.b};}
static H sub(H x,H y){return {x.a-y.a,x.b-y.b};}
static H mul(H x,H y){return {x.a*y.a,x.b*y.b};}
static constexpr H base{0x9e3779b185ebca87ULL,0xc2b2ae3d27d4eb4fULL};
struct Chunk {U offset=0,n=0,runs=0;std::vector<uint32_t> heads,ends;};
struct Counts {U queries=0,quick=0,probes=0,verified=0,unverified=0,max_lce=0;};
struct Engine {
 std::vector<uint8_t> t; std::vector<H> h,pow; Counts cnt;
 U direct_cap=4096, query_probe_cap=128,total_budget=10000000000ULL, used=0;
 void charge(U x){need(x<=total_budget-used,"total work budget refused; no fallback");used+=x;}
 void setup(U n,U maxcap){t.resize(n);h.resize(n+1);pow.resize(maxcap+1);pow[0]={1,1};for(U i=1;i<=maxcap;++i)pow[i]=mul(pow[i-1],base);}
 void append(U i,uint8_t c){t[i]=c;h[i+1]=add(mul(h[i],base),{U(c)+1,U(c)+1});}
 H range(U i,U n)const{return sub(h[i+n],mul(h[i],pow[n]));}
 uint8_t symbol(const Chunk& c,U p)const{return t[c.offset+(p<c.n?p:p%c.n)];}
 H cyclic(const Chunk& c,U p,U n)const{
   // Concatenate the first partial cycle, repeated full cycles by doubling,
   // and a tail. This takes O(log cap) even for very unequal chunk lengths.
   if(p>=c.n)p%=c.n;
   U first=std::min(n,c.n-p);H out=range(c.offset+p,first);n-=first;if(!n)return out;
   U repeats=n/c.n,tail=n%c.n,width=c.n;H whole=range(c.offset,c.n);
   while(repeats){if(repeats&1)out=add(mul(out,pow[width]),whole);repeats>>=1;if(repeats){whole=add(mul(whole,pow[width]),whole);width*=2;}}
   if(tail)out=add(mul(out,pow[tail]),range(c.offset,tail));
   return out;
 }
 U lce(const Chunk& a,U x,const Chunk& b,U y){
   ++cnt.queries;U cap=a.n+b.n-std::gcd(a.n,b.n),lo=0,qp=0;
   U quick=std::min<U>(16,cap);
   while(lo<quick){charge(1);++cnt.quick;if(symbol(a,x+lo)!=symbol(b,y+lo))return lo;++lo;}
   auto eq=[&](U k){need(++qp<=query_probe_cap,"query probe budget refused");charge(1);++cnt.probes;return cyclic(a,x,k)==cyclic(b,y,k);};
   U hi=cap,step=16;
   while(lo<cap){U next=lo+std::min(step,cap-lo);if(!eq(next)){hi=next;break;}lo=next;step=std::min(cap,step*2);}
   while(hi-lo>1){U mid=lo+(hi-lo)/2;if(eq(mid))lo=mid;else hi=mid;}
   U want=lo+(lo<cap),check=std::min(want,direct_cap);
   charge(check);cnt.verified+=check;
   for(U k=0;k<check;++k)need((symbol(a,x+k)==symbol(b,y+k))==(k<lo),"fingerprint direct verification mismatch");
   if(want>check){++cnt.unverified;if(lo<cap){charge(1);++cnt.verified;need(symbol(a,x+lo)!=symbol(b,y+lo),"fingerprint mismatch boundary failed");}}
   cnt.max_lce=std::max(cnt.max_lce,lo);return lo;
 }
 int compare(const Chunk&a,U x,const Chunk&b,U y,U l)const{
   U cap=a.n+b.n-std::gcd(a.n,b.n);if(l==cap)return 0;
   return symbol(a,x+l)<symbol(b,y+l)?-1:1;
 }
};
static U word(std::istream& f,unsigned n){U v=0;for(unsigned i=0;i<n;++i){int c=f.get();need(c!=EOF,"short chunk word");v|=U(uint8_t(c))<<(8*i);}return v;}
static Chunk read_chunk(const std::string& path){
 std::ifstream f(path,std::ios::binary);need(bool(f),"open chunk");need(word(f,4)==0x52435853&&word(f,4)==1,"chunk magic/version");
 Chunk c;c.offset=word(f,8);c.n=word(f,8);c.runs=word(f,8);need(c.n&&c.n<=UINT32_MAX&&c.runs<=c.n,"chunk size");
 c.heads.reserve(c.runs);c.ends.reserve(std::min<U>(c.n,2*c.runs));U sum=0;int prev=-1;
 for(U i=0;i<c.runs;++i){U ch=word(f,1),len=word(f,4),h=word(f,8),t=word(f,8);need(len&&h<c.n&&t<c.n&&int(ch)!=prev,"invalid run");prev=ch;sum+=len;c.heads.push_back(h);c.ends.push_back(h);if(len>1)c.ends.push_back(t);}
 need(sum==c.n&&f.peek()==EOF,"chunk coverage/trailer");return c;
}
static const std::array<U,12> edges{{0,1,4,16,64,100,1000,10000,100000,1000000,10000000,UINT64_MAX}};
struct Hist {
 U count=0,sum=0,max=0,gt100=0,gt10k=0,gt1m=0,censored=0;std::array<U,12> bins{};
 void put(U l,bool equal=false){++count;sum+=l;max=std::max(max,l);gt100+=l>100;gt10k+=l>10000;gt1m+=l>1000000;censored+=equal;++bins[std::lower_bound(edges.begin(),edges.end(),l)-edges.begin()];}
 void json()const{std::cout<<"{\"count\":"<<count<<",\"sum\":"<<sum<<",\"max\":"<<max<<",\"gt100\":"<<gt100<<",\"gt10000\":"<<gt10k<<",\"gt1000000\":"<<gt1m<<",\"equal_infinite\":"<<censored<<",\"bins\":[";for(size_t i=0;i<bins.size();++i){if(i)std::cout<<',';std::cout<<bins[i];}std::cout<<"]}";}
};
struct Parse {
 U w=10;std::vector<U> len{0},starts;std::vector<uint32_t> ids;std::vector<uint16_t> membership;std::vector<int> owner;
 void load(const std::string& prefix,const std::vector<Chunk>& chunks,U n){
   std::ifstream f(prefix+".dict",std::ios::binary);need(bool(f),"dict open");U length=0,dollars=0;bool initial=true,terminated=false;char c;
   while(f.get(c)){uint8_t b=c;if(initial&&b==2)++dollars;else initial=false;
     if(b==0){need(length==0&&f.peek()==EOF,"dict terminal");terminated=true;break;}
     if(b==1){need(length>=w||len.size()==1,"short phrase");len.push_back(length);length=0;}else ++length;
   }
   need(terminated&&dollars&&dollars<=w&&len.size()>1,"dict format");len[1]+=w-dollars;
   membership.resize(len.size());std::ifstream p(prefix+".parse",std::ios::binary);need(bool(p),"parse open");U pos=0;size_t chunk=0;
   while(p.peek()!=EOF){U id=word(p,4);need(id&&id<len.size()&&len[id]>w,"parse id/length");starts.push_back(pos);ids.push_back(id);
     int owned=-1;
     if(pos>=w){U s=pos-w;while(chunk+1<chunks.size()&&s>=chunks[chunk+1].offset)++chunk;
       if(s>=chunks[chunk].offset&&s+len[id]<=chunks[chunk].offset+chunks[chunk].n){owned=chunk;membership[id]|=uint16_t(1U<<chunk);}}
     owner.push_back(owned);pos+=len[id]-w;
   }
   need(pos==n+w,"parse extent != text + window");starts.push_back(pos);
   for(size_t b=1;b<chunks.size();++b){U da=0,db=0,shared=0,oa=0,ob=0,sa=0,sb=0,ba=0,bb=0,sba=0,sbb=0;
     for(size_t id=1;id<len.size();++id){bool a=membership[id]&(1U<<(b-1)),z=membership[id]&(1U<<b);da+=a;db+=z;shared+=a&&z;}
     for(size_t j=0;j<ids.size();++j){auto mask=membership[ids[j]];bool sh=(mask&(1U<<(b-1)))&&(mask&(1U<<b));U bytes=len[ids[j]]-w;
       if(owner[j]==int(b-1)){++oa;ba+=bytes;if(sh){++sa;sba+=bytes;}}
       if(owner[j]==int(b)){++ob;bb+=bytes;if(sh){++sb;sbb+=bytes;}}
     }
     std::cout<<"{\"type\":\"dictionary\",\"boundary\":"<<b-1<<",\"distinct_a\":"<<da<<",\"distinct_b\":"<<db<<",\"intersection\":"<<shared<<",\"occurrences_a\":"<<oa<<",\"occurrences_b\":"<<ob<<",\"shared_occurrences_a\":"<<sa<<",\"shared_occurrences_b\":"<<sb<<",\"covered_bytes_a\":"<<ba<<",\"covered_bytes_b\":"<<bb<<",\"shared_bytes_a\":"<<sba<<",\"shared_bytes_b\":"<<sbb<<"}"<<std::endl;
   }
   std::cerr<<"PARSE phrases="<<len.size()-1<<" occurrences="<<ids.size()<<" excluded_seam_occurrences="<<std::count(owner.begin(),owner.end(),-1)<<"\n";
 }
 std::pair<U,U> at(U p)const{p+=w;U j=std::upper_bound(starts.begin(),starts.end(),p)-starts.begin()-1;need(j<ids.size(),"phrase lookup");return {j,p-starts[j]};}
};
static void selftest(){
 std::mt19937_64 rng(71);U checks=0;
 for(unsigned trial=0;trial<100;++trial){U n=8+rng()%24,m=8+rng()%24;Engine e;e.setup(n+m,2*std::max(n,m));Chunk a,b;a.n=n;b.n=m;b.offset=n;
   for(U i=0;i<n+m;++i)e.append(i,trial%4==0?'a':('a'+rng()%3));
   for(U x=0;x<n;++x)for(U y=0;y<m;++y){U cap=n+m-std::gcd(n,m),l=0;while(l<cap&&e.symbol(a,x+l)==e.symbol(b,y+l))++l;need(e.lce(a,x,b,y)==l,"selftest LCE");++checks;}
   auto exact=[&](const Chunk& c,U x,const Chunk& d,U y){U cap=c.n+d.n-std::gcd(c.n,d.n),l=0;while(l<cap&&e.symbol(c,x+l)==e.symbol(d,y+l))++l;return e.compare(c,x,d,y,l);};
   std::vector<U> candidates;for(U x=0;x<n;x+=2)candidates.push_back(x);
   std::stable_sort(candidates.begin(),candidates.end(),[&](U x,U y){return exact(a,x,a,y)<0;});
   for(U y=0;y<m;++y){U expected=0;while(expected<candidates.size()&&exact(a,candidates[expected],b,y)<0)++expected;
     U lo=0,hi=candidates.size();while(lo<hi){U mid=lo+(hi-lo)/2,l=e.lce(a,candidates[mid],b,y);if(e.compare(a,candidates[mid],b,y,l)<0)lo=mid+1;else hi=mid;}
     need(lo==expected,"selftest nearest endpoint lower_bound");}
 }
 Engine e;e.setup(64,64);for(U i=0;i<64;++i)e.append(i,'a');Chunk a,b;a.n=b.n=32;b.offset=32;e.total_budget=1;bool refused=false;try{e.lce(a,0,b,0);}catch(const std::exception&){refused=true;}need(refused,"selftest budget refusal");
 std::cout<<"SELFTEST_PASS comparisons="<<checks<<" periodic_equal=1 budget_refusal=1\n";
}
int main(int argc,char**argv){try{
 if(argc==2&&std::string(argv[1])=="--self-test"){selftest();return 0;}
 need(argc==8||argc==9,"usage: measure SOURCE CHUNK_DIR COUNT REMAP PARSE_PREFIX|- SAMPLES SEED [DIRECT_CAP]");
 unsigned k=std::stoul(argv[3]);need(k>=2&&k<=16,"2..16 chunks required");U samples=std::stoull(argv[6]),seed=std::stoull(argv[7]);need(samples&&samples<=1000000,"sample limit 1M per boundary");
 std::vector<Chunk> chunks;U total=0,maxn=0;auto started=Clock::now();
 for(unsigned i=0;i<k;++i){auto c=read_chunk(std::string(argv[2])+"/chunk-"+std::to_string(i)+".crle");need(c.offset==total,"chunk contiguous coverage");total+=c.n;maxn=std::max(maxn,c.n);std::cerr<<"CHUNK_READ index="<<i<<" runs="<<c.runs<<" endpoints="<<c.ends.size()<<"\n";chunks.push_back(std::move(c));}
 std::array<uint8_t,256> remap;std::ifstream rm(argv[4],std::ios::binary);need(bool(rm.read((char*)remap.data(),256))&&rm.peek()==EOF,"remap read");auto sorted=remap;std::sort(sorted.begin(),sorted.end());for(unsigned i=0;i<256;++i)need(sorted[i]==i,"remap permutation");need(remap[30]==30,"remap separator");
 need(std::filesystem::file_size(argv[1])==total,"source size mismatch");Engine e;
 if(argc==9)e.direct_cap=std::stoull(argv[8]);
 need(e.direct_cap>=16&&e.direct_cap<=131072,"direct verification cap outside 16..131072");
 e.setup(total,2*maxn);
 std::ifstream source(argv[1],std::ios::binary);need(bool(source),"source open");std::array<char,1<<20> block;U read=0;
 while(source){source.read(block.data(),block.size());for(std::streamsize j=0;j<source.gcount();++j){need(read<total,"source grew");e.append(read++,remap[uint8_t(block[j])]);}}
 need(read==total,"source short read");for(const auto&c:chunks)need(e.t[c.offset+c.n-1]==30,"chunk end separator");
 std::cout.precision(12);std::cout<<"{\"type\":\"configuration\",\"source_bytes\":"<<total<<",\"source_passes\":1,\"samples_per_boundary\":"<<samples<<",\"seed\":"<<seed<<",\"direct_cap\":"<<e.direct_cap<<",\"query_probe_cap\":"<<e.query_probe_cap<<",\"total_work_budget\":"<<e.total_budget<<",\"histogram_upper_edges\":[0,1,4,16,64,100,1000,10000,100000,1000000,10000000,18446744073709551615],\"prefix_hash_bytes\":"<<e.h.size()*sizeof(H)<<",\"power_bytes\":"<<e.pow.size()*sizeof(H)<<"}"<<std::endl;
 Parse p;bool hasparse=std::string(argv[5])!="-";if(hasparse)p.load(argv[5],chunks,total);
 std::cerr<<"PREPROCESS_DONE seconds="<<std::chrono::duration<double>(Clock::now()-started).count()<<"\n";
 std::mt19937_64 rng(seed);
 for(unsigned i=0;i+1<k;++i){const auto&a=chunks[i];const auto&b=chunks[i+1];std::uniform_int_distribution<U> random_run(0,b.heads.size()-1);Hist pred,succ,best,both,search;
   Counts before=e.cnt;U work0=e.used,aligned=0,anchor_bytes=0,reached=0,reached_bytes=0,shared_query=0,seam_pairs=0,witness_max=0;double pair_cost_sq=0;auto begin=Clock::now();
   for(U q=0;q<samples;++q){U y=b.heads[random_run(rng)],lo=0,hi=a.ends.size();
     while(lo<hi){U mid=lo+(hi-lo)/2,l=e.lce(a,a.ends[mid],b,y);search.put(l);if(e.compare(a,a.ends[mid],b,y,l)<0)lo=mid+1;else hi=mid;}
     U mx=0,paircost=0;
     if(hasparse){auto [j,off]=p.at(b.offset+y);shared_query+=(p.membership[p.ids[j]]&(1U<<i))!=0;}
     for(int side=0;side<2;++side){if((side==0&&lo==0)||(side==1&&lo==a.ends.size()))continue;U x=a.ends[side==0?lo-1:lo],l=e.lce(a,x,b,y);bool equal=l==a.n+b.n-std::gcd(a.n,b.n);
       if(l>witness_max){witness_max=l;std::cout<<"{\"type\":\"witness\",\"boundary\":"<<i<<",\"sample_index\":"<<q<<",\"a_local_pos\":"<<x<<",\"b_local_pos\":"<<y<<",\"side\":"<<side<<",\"lce\":"<<l<<"}"<<std::endl;}
       (side==0?pred:succ).put(l,equal);both.put(l,equal);mx=std::max(mx,l);paircost+=l;seam_pairs+=l>=std::min(a.n-x,b.n-y);
       if(hasparse){auto [ja,oa]=p.at(a.offset+x);auto [jb,ob]=p.at(b.offset+y);
         if(p.ids[ja]==p.ids[jb]&&oa==ob&&p.owner[ja]==int(i)&&p.owner[jb]==int(i+1)){++aligned;anchor_bytes+=std::min(l,p.len[p.ids[ja]]-oa);}
         U da=p.starts[ja+1]-(a.offset+x+p.w),db=p.starts[jb+1]-(b.offset+y+p.w);
         if(da==db&&da<=l&&ja+1<p.ids.size()&&jb+1<p.ids.size()&&p.owner[ja+1]==int(i)&&p.owner[jb+1]==int(i+1)&&p.ids[ja+1]==p.ids[jb+1]){++reached;reached_bytes+=std::min(l-da,p.len[p.ids[ja+1]]);}
       }
     }
     best.put(mx);pair_cost_sq+=double(paircost)*double(paircost);
     if((q+1)%10000==0)std::cerr<<"PROGRESS boundary="<<i<<" samples="<<q+1<<" work="<<e.used<<" elapsed="<<std::chrono::duration<double>(Clock::now()-begin).count()<<"\n";
   }
   auto d=[&](U Counts::*member){return e.cnt.*member-before.*member;};
   std::cout<<"{\"type\":\"boundary\",\"boundary\":"<<i<<",\"a_runs\":"<<a.runs<<",\"b_runs\":"<<b.runs<<",\"a_endpoints\":"<<a.ends.size()<<",\"samples\":"<<samples<<",\"seconds\":"<<std::chrono::duration<double>(Clock::now()-begin).count()<<",\"work\":"<<e.used-work0<<",\"lce_queries\":"<<d(&Counts::queries)<<",\"quick_symbols\":"<<d(&Counts::quick)<<",\"hash_probes\":"<<d(&Counts::probes)<<",\"verified_symbols\":"<<d(&Counts::verified)<<",\"not_fully_verified_queries\":"<<d(&Counts::unverified)<<",\"seam_candidate_pairs\":"<<seam_pairs<<",\"aligned_phrase_pairs\":"<<aligned<<",\"aligned_phrase_bytes\":"<<anchor_bytes<<",\"reached_next_phrase_pairs\":"<<reached<<",\"reached_next_phrase_bytes\":"<<reached_bytes<<",\"b_heads_in_a_dictionary\":"<<shared_query<<",\"pair_cost_sum_squares\":"<<pair_cost_sq<<",\"predecessor\":";pred.json();std::cout<<",\"successor\":";succ.json();std::cout<<",\"best\":";best.json();std::cout<<",\"both\":";both.json();std::cout<<",\"search\":";search.json();std::cout<<"}"<<std::endl;
 }
 std::cout<<"{\"type\":\"complete\",\"work\":"<<e.used<<",\"max_lce\":"<<e.cnt.max_lce<<",\"seconds\":"<<std::chrono::duration<double>(Clock::now()-started).count()<<"}"<<std::endl;
 return 0;
 }catch(const std::exception& e){std::cerr<<"CROSS_LCP_FATAL "<<e.what()<<"\n";return 2;}}
