// Generated-only local suffix fixtures. Test arrays are for a small independent
// oracle, never the constructor or a target-size verifier. No corpus arguments.
#include "overlap_raw_prefix_intervals.hpp"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <random>
#include <sched.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
using prefixproof::require;using prefixproof::U;using prefixproof::Verified;
using Bytes=std::vector<std::uint8_t>;namespace fs=std::filesystem;
static time_t until;
static void guard(){if(time(nullptr)>=until || fs::exists("stop.requested"))throw std::runtime_error("STOP/deadline prefix proof");}
static double wall(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static std::string line(const fs::path& p){std::string s;std::ifstream f(p);require(bool(std::getline(f,s)),"read service grant");return s;}
static fs::path grant(){std::string cg=line("/proc/self/cgroup");require(cg.starts_with("0::/"),"not scoped");fs::path p("/sys/fs/cgroup"+cg.substr(3));cpu_set_t cores;CPU_ZERO(&cores);require(!sched_getaffinity(0,sizeof(cores),&cores) && CPU_COUNT(&cores)==1,"one pinned core required");require(line(p/"memory.max")=="134217728" && line(p/"memory.swap.max")=="0" && line(p/"cpu.max")=="100000 100000","test grant");return p;}
static constexpr auto go=[]{};
static void negatives(){
    Bytes x(100,7);x[60]=3;Verified<1,4,1> c(x);
    require(c.equal(0,48,12,go) && c.equal(61,13,12,go),"setup disjoint equalities");auto k=c.counts().compared;
    require(!c.equal(0,48,25,go) && c.counts().compared==k+1,"unproved one-byte hole was skipped");
    require(!c.equal(12,60,1,go),"failed request installed");k=c.counts().compared;require(c.equal(51,3,8,go) && c.counts().compared==k,"reverse/subrange hit");
    for(auto h:{UINT64_MAX,U(101)}){bool refused=false;try{c.equal(0,1,h,go);}catch(const std::runtime_error&){refused=true;}require(refused,"overflow/malformed extent");}
    bool self=false;try{c.equal(4,4,1,go);}catch(const std::runtime_error&){self=true;}require(self && c.equal(100,100,0,go),"self pair/empty END");
    Bytes zeros(350000,0);Verified<1,2,1> v(zeros);unsigned calls=0;bool stopped=false;
    try{v.equal(0,175000,160000,[&]{if(++calls==2)throw std::runtime_error("injected stop");});}catch(const std::runtime_error&){stopped=true;}
    k=v.counts().compared;require(stopped && v.equal(0,175000,65536,go) && v.counts().compared-k==65536,"partial stopped request installed");
    Verified<1,2,1> ev(zeros);for(int i=0;i<4;++i)for(U delta:{U(100),U(101),U(102)})require(ev.equal(0,delta,20,go),"collision fallback");
    require(ev.counts().full_hits==0 && ev.counts().compared==ev.counts().asked && ev.counts().evictions==10,"thrashing set must recompare");
    U cases=0;for(unsigned bits=0;bits<256;++bits){Bytes raw(8);for(unsigned j=0;j<8;++j)raw[j]=(bits>>j)&1;Verified<2,2,1> tiny(raw);
        for(U h=0;h<=8;++h)for(U a=0;a+h<=8;++a)for(U b=a+1;b+h<=8;++b){bool expected=!h || !std::memcmp(raw.data()+a,raw.data()+b,h);
            require(tiny.equal(a,b,h,go)==expected && tiny.equal(b,a,h,go)==expected,"exhaustive differential");cases+=2;}}
    std::printf("NEGATIVE_PASS directed_raw_claims=%llu mismatching_gap=1 cancelled_partial=1 extent=1 symmetric=1 collision_recompare=1\n",(unsigned long long)cases);
}
struct Order {std::vector<int> sa,h;};
static Order truth(const Bytes& raw){int n=int(raw.size());require(n>0 && n<=180000,"generated corpus cap");Order z{std::vector<int>(n+1),std::vector<int>(n+1)};std::vector<int> rank(n+1),next(n+1);std::iota(z.sa.begin(),z.sa.end(),0);for(int i=0;i<n;++i)rank[i]=int(raw[i])+1;
    // Independent doubling sort; compute test annotations with Kasai. Neither
    // ranks nor annotations are carried into the proof table as equalities.
    for(int step=1;step<=n;step*=2){guard();auto key=[&](int i){return std::pair{rank[i],i+step<=n?rank[i+step]:-1};};std::sort(z.sa.begin(),z.sa.end(),[&](int a,int b){return key(a)<key(b);});next[z.sa[0]]=0;for(int j=1;j<=n;++j)next[z.sa[j]]=next[z.sa[j-1]]+int(key(z.sa[j-1])!=key(z.sa[j]));rank.swap(next);if(rank[z.sa.back()]==n)break;}
    require(z.sa[0]==n && rank[z.sa.back()]==n,"fixture order");int h=0;for(int i=0;i<n;++i){if(!(i%8192))guard();int r=rank[i],p=z.sa[r-1];while(i+h<n && p+h<n && raw[i+h]==raw[p+h])++h;z.h[r]=h;if(h)--h;}return z;
}
static void bench(const char* name,const Bytes& raw){double t=wall();auto z=truth(raw);double oracle=wall()-t;U bytes=0,high=0;const unsigned n=raw.size();
    t=wall();for(unsigned j=1;j<=n;++j){if(!(j%1024))guard();unsigned a=z.sa[j-1],b=z.sa[j],h=z.h[j];require(h<=n-std::max(a,b) && a!=b && (a+h==n || (b+h<n && raw[a+h]<raw[b+h])),"invalid ordered fixture");if(h)require(!std::memcmp(raw.data()+a,raw.data()+b,h),"invalid fixture prefix");bytes+=h;high=std::max<U>(high,h);}double base=wall()-t;
    auto proof=std::make_unique<Verified<>>(raw);t=wall();for(unsigned j=1;j<=n;++j){if(!(j%1024))guard();unsigned a=z.sa[j-1],b=z.sa[j],h=z.h[j];require(h<=n-std::max(a,b) && a!=b && (a+h==n || (b+h<n && raw[a+h]<raw[b+h])),"changed order check");require(proof->equal(a,b,h,guard),"proof disagrees with direct equality");}double elapsed=wall()-t;auto c=proof->counts();require(c.requests==n && bytes==c.asked && bytes==c.compared+c.skipped,"successful byte accounting");std::string_view mode=name;
    if(mode=="random")require(!c.lookups && !c.skipped && c.compared==bytes,"unexpected chance long match");
    if(mode=="twins")require(high>=65536 && c.skipped>bytes*95/100 && bytes>(U(2)<<30),"long-repeat mechanism below 95% byte reuse");
    std::printf("PREFIX_CASE name=%s raw=%u rows=%u max_lcp=%llu bytes_claimed=%llu bytes_compared=%llu proved_reuse=%llu lookups=%llu full_hits=%llu evictions=%llu table_bytes=%zu oracle_s=%.6f direct_s=%.6f proof_s=%.6f warm_generated=1\n",name,n,n+1,(unsigned long long)high,(unsigned long long)bytes,(unsigned long long)c.compared,(unsigned long long)c.skipped,(unsigned long long)c.lookups,(unsigned long long)c.full_hits,(unsigned long long)c.evictions,std::size_t(Verified<>::table_bytes),oracle,base,elapsed);
}
int main(int argc,char** argv) try {
    require(argc==2,"generated fixture deadline only");until=std::stoll(argv[1]);auto path=fs::current_path();struct stat st{};require(path.parent_path()=="/tmp" && path.filename().string().starts_with("raw-prefix-proof-") && !stat(".",&st) && (st.st_mode&0777)==0700 && st.st_uid==geteuid() && until<=time(nullptr)+120,"fresh generated fixture required");guard();auto cg=grant();negatives();std::mt19937_64 rng(926131);Bytes random(131074);for(auto& b:random)b=2+rng()%254;bench("random",random);Bytes twins(random.begin(),random.begin()+65536);twins.push_back(0);twins.insert(twins.end(),random.begin(),random.begin()+65536);twins.push_back(1);bench("twins",twins);
    Bytes mosaic;while(mosaic.size()<160000){unsigned at=2048*(rng()%40),len=1500+rng()%549;mosaic.insert(mosaic.end(),random.begin()+at,random.begin()+at+len);mosaic.push_back(rng()%2);if(rng()%2)mosaic[mosaic.size()-1-rng()%len]^=31;}bench("mosaic",mosaic);guard();rusage ru{};require(!getrusage(RUSAGE_SELF,&ru) && ru.ru_inblock<=16384 && ru.ru_oublock<=4096,"generated IO cap");std::printf("FIXTURE_PASS peak_cgroup=%s cpu_s=%.6f attributed_read=%ld attributed_write=%ld corpus_bytes=0 no_product=1\n",line(cg/"memory.peak").c_str(),double(clock())/CLOCKS_PER_SEC,512L*ru.ru_inblock,512L*ru.ru_oublock);return 0;
} catch(const std::exception& e){std::fprintf(stderr,"prefix fixture refused: %s\n",e.what());return 1;}
