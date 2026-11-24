// Generated-only reader + counterexamples. Compile against the pinned, unedited
// LcpColumn declaration; the only supported profile here is non-spooling.
#include "overlap_finish_fold_fence.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
using rowfence::U;
static void pf_check(bool ok,const std::string& what){if(!ok)throw std::runtime_error(what);}
[[noreturn]] static void pf_fail(const std::string& what){throw std::runtime_error(what);}
static double tnow(){using namespace std::chrono;return duration<double>(steady_clock::now().time_since_epoch()).count();}
static void pf_read_full(int fd,void* data,uint64_t n,const std::string& why){auto p=static_cast<char*>(data);while(n){ssize_t z=::read(fd,p,n);pf_check(z>0,why);p+=z;n-=z;}}
namespace finish_spool { static bool enabled(){return false;} }
#include "pinned-lcp-column.inc"
using rowfence::need;
static_assert(sizeof(std::pair<U,U>)==16);
using Fields=std::array<U,5>;
template<class F> Fields fields(const F& x){return {x.loads,x.reads,x.staged,x.peak,x.max_slices};}
struct Record {U rows,b;};
struct Trial {
 std::vector<Record> records;std::vector<U> h;U B,S;
 // Independent replay: push/pull actual pending records, not arithmetic on
 // run-start indices. At a dispatch each source slice's publication may
 // precede or follow the piece. The cap must refuse no later than the first
 // input prefix that could have exposed an over-budget allocation.
 std::pair<Fields,U> reference(U limit)const{
    const U n=h.size();Fields s{};U prev=0,end=0,rows=0,ready=n+1;std::optional<Record> source,norm;
    auto normalize=[&](Record r){if(norm){if(norm->b==r.b){norm->rows+=r.rows;return;}end+=norm->rows;}norm=r;};
    auto stage=[&](U trigger){if(prev==end)return;U base=prev?prev-1:0,len=end-base,cost=len;prev=end;
        U lo=base/S,hi=(end-1)/S,reads=hi-lo+1;++s[0];s[1]+=reads;s[2]+=len;s[4]=std::max(s[4],reads);if(cost>limit)ready=std::min(ready,trigger);
        for(U j=lo;j<=hi;++j){U after=std::min(n,(j+1)*S);for(U i=j*S;i<after;++i)cost+=1+16*(h[i]>=255);
            if(cost>limit)ready=std::min(ready,std::max(trigger,after));}s[3]=std::max(s[3],cost);};
    for(U i=0;i<records.size();++i){auto r=records[i];if(source){if(source->b==r.b)source->rows+=r.rows;else{normalize(*source);source=r;}}else source=r;
        rows+=r.rows;if((i+1)%B==0 || i+1==records.size())stage(std::min(rows+1,n));}
    normalize(*source);end+=norm->rows;stage(n);need(end==n,"reference run coverage");return {s,ready};
 }
 U actual(U cap,Fields& values)const{rowfence::Fold<382> f(h.size(),B,S,cap);U pos=0;
    try{for(auto r:records)for(U j=0;j<r.rows;++j){++pos;f.add(r.b,h[pos-1]);}f.end();}
    catch(const std::runtime_error& e){need(std::string(e.what())=="fold live slice geometry","unexpected fence error");return pos;}
    values=fields(f);return h.size()+1;}
};
static void suite(){U count=0,low=0,late=0;std::mt19937_64 rng(945319);
 for(U n=1;n<=125;++n)for(U j=0;j<39;++j){Trial t{{},{},1+rng()%19,1+rng()%23};
    for(U i=0;i<n;++i){U b=(j%8==0?1:(j%8==1?(i&1):rng()%4));if(t.records.empty()||t.records.back().b!=b)t.records.push_back({0,b});++t.records.back().rows;t.h.push_back((j%9==0?i>n/2:rng()%7==0)?255:2);}
    auto [expected,never]=t.reference(UINT64_MAX);Fields v{};U peak=expected[3];need(never==n+1 && t.actual(peak,v)==n+1 && v==expected,"exact fold geometry");++count;
    for(U cap:{peak-1,rng()%peak}){auto [s,must]=t.reference(cap);U caught=t.actual(cap,v);need(s==expected && caught<=must && must<=n,"cap allowed a prefix through");late+=caught==n;++low;}}
 std::printf("PASS %llu independent two-coalescer fold schedules, %llu pre-exposure cap refusals, %llu at final row/flush\n",(unsigned long long)count,(unsigned long long)low,(unsigned long long)late);
}
constexpr U S=262144,N=7*S,CUT=6*S+13;
static U lcp(U i){return i==0?0:i%1709==0?34067:i%17==0?255:i%255;}
static U symbol(U i){if(i<6*S)return 1;if(i<CUT)return 2+(i-6*S)%2;return i<CUT+1000?1:2;}
static void partial(){
 // No user-supplied input paths. The parent supplies a fresh private cwd and
 // creates only generated transport. This class UNLINKS its canonical inputs.
 auto dir=std::filesystem::canonical(".");need(dir.parent_path()=="/tmp" && dir.filename().string().starts_with("v8-fold-geometry-"),"private fixture cwd");
 struct stat st{};need(!stat(".",&st) && st.st_uid==geteuid() && (st.st_mode&0777)==0700,"private fixture owner");
 for(auto p:{"arm","arm/bands"})need(!lstat(p,&st) && S_ISDIR(st.st_mode) && st.st_uid==geteuid(),"private generated reader input");
 LcpColumn col;col.openBands("arm/bands",N,3);rowfence::Fold<382> f(N,16,S,18*N);for(U i=0;i<N;++i)f.add(symbol(i),lcp(i));f.end();
 U max_cap=0;for(auto [base,end]:{std::pair<U,U>{0,CUT},{CUT-1,N}}){
    LcpColumn::Band band;col.load(band,base,end-base);U deep=0;for(U i=base;i<end;++i){U v=lcp(i);need(band.at(i)==v,"copied LCP value");deep+=v>=255;}
    need(band.u8.capacity()==end-base && deep==band.deep.size() && band.deep.capacity()==std::bit_ceil(deep),"unexpected fresh libstdc++ vector capacities");max_cap=std::max<U>(max_cap,16*band.deep.capacity());
    for(auto& s:col.live)need(s.fd==-1 && s.payload.size()==s.payload.capacity() && s.esc.size()==s.esc.capacity(),"source copies/closed FD");
    // All seven files have already closed/unlinked after even the FIRST
    // load. The five private inodes may be reused; deep is NOT disk credit.
    need(col.slicesRetired==7 && col.live.size()==1,"credit return/overlap");
 }
 col.finishBands();need(f.max_slices==7 && f.loads==col.stagedLoads && f.reads==col.preads && f.peak==col.maxLiveBytes && f.staged==col.stagedBytes,"source reader/fence mismatch");
 std::printf("GEN_READER rows=%llu loads=%llu intersect_reads=%llu staged=%llu max_source_slices=%llu reported_live=%llu band_deep_capacity_max=%llu canonical_unlinks=%llu\n",(unsigned long long)N,(unsigned long long)f.loads,(unsigned long long)f.reads,(unsigned long long)f.staged,(unsigned long long)f.max_slices,(unsigned long long)f.peak,(unsigned long long)max_cap,(unsigned long long)col.slicesRetired);
}
int main(int ac,char** av)try{need(ac==2,"unit|generated-consume");std::string arg=av[1];if(arg=="unit")suite();else if(arg=="generated-consume")partial();else pf_fail("generated fixture only");}catch(const std::exception& e){std::fprintf(stderr,"fold probe refused: %s\n",e.what());return 1;}
