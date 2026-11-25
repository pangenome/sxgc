// Fixed 100 MB partial-rank study and a small independent generated selector test.
#include "overlap_prefix_window_select.hpp"
#include <bit>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace prefixwindow;
using prefixproof::Counts;
using prefixproof::Verified;
namespace fs=std::filesystem;
static time_t deadline;
static void guard(){require(time(nullptr)<deadline && !fs::exists("stop.requested"),"window study STOP/deadline");}
static std::string line(const fs::path& p){std::ifstream f(p);std::string v;require(bool(std::getline(f,v)),"read grant");return v;}
static std::string grant(){
    auto path=line("/proc/self/cgroup");require(path.starts_with("0::/"),"service cgroup");std::string cg="/sys/fs/cgroup"+path.substr(3);
    cpu_set_t cpu;CPU_ZERO(&cpu);require(!sched_getaffinity(0,sizeof(cpu),&cpu) && CPU_COUNT(&cpu)==1,"one pinned CPU");
    require(line(cg+"/memory.max")=="268435456" && line(cg+"/cpu.max")=="100000 100000" && line(cg+"/memory.swap.max")=="0","window worker grant");return cg;
}
static void fixtures(){
    std::mt19937 gen(296530u);std::vector<std::uint8_t> t(6000);
    for(auto& c:t)c=std::uint8_t(gen());
    std::copy_n(t.begin(),1400,t.begin()+2500);t.back()=0;
    std::vector<I> sa(t.size()+1);std::iota(sa.begin(),sa.end(),0);
    std::sort(sa.begin(),sa.end(),[&](I a,I b){return std::lexicographical_compare(t.begin()+a,t.end(),t.begin()+b,t.end());});
    std::vector<Window> w={{1,38},{411,247},{2103,120},{5161,840}};
    auto cover=select(t,w,2500,guard);Work s(32ull<<20,guard),h(8ull<<20,guard);I edges=0;
    for(auto& v:cover){order(v,t,s);for(I i=0;i<v.extent;++i)require(v.pos[i]==sa[v.base+i],"fixture full lexical cover vs independent SA");
        for(I j=v.wanted.first;j<v.wanted.first+v.wanted.count;++j){I a=sa[j-1],b=sa[j],l=0;
            while(a+l<t.size() && b+l<t.size() && t[a+l]==t[b+l])++l;
            require(l==direct(t,a,b,h),"fixture independent exact LCP");++edges;}}
    require(edges==1245,"missing test edges");
    std::vector<std::uint8_t> unary(100,0);auto rejection=[&](auto plan,I cap,const char* why){try{(void)select(unary,plan,cap,guard);}catch(const std::runtime_error& e){require(std::string(e.what())==why,"unexpected selector refusal");return;}require(false,"missing pre-allocation refusal");};
    rejection(std::vector<Window>{{4,4}},50,"complete prefix cover exceeds position cap before allocation");
    rejection(std::vector<Window>{{4,4},{11,3}},120,"overlapping lexical covers");
    rejection(std::vector<Window>{{99,3}},100,"rank extent");
    bool refused=false;try{Work tiny(2,guard);(void)direct(unary,5,4,tiny);}catch(const std::runtime_error& e){refused=std::string(e.what())=="raw sort/LCP work cap";}require(refused,"missing raw work refusal");
    std::cout<<"FIXTURE_PASS full_oracle_rows="<<sa.size()<<" observed_edges="<<edges<<" END_NUL=1 length_ties=1 short_cap=1 overlap=1 invalid_rank=1 raw_work=1\n";
}
static int pred(Raw t,I p){return p?t[p-1]:256;}
static U anchors(unsigned id,Raw raw,const Cover& v,const std::vector<U>& starts){
    // `starts` contains ranks only, never any expected position. Positions are
    // compared to the pinned m11 after exit, not supplied to the sorting code.
    I first=v.wanted.first,past=first+v.wanted.count;
    auto at=[&](I r){return v.pos[r-v.base];};auto it=std::lower_bound(starts.begin(),starts.end(),first);
    U reports=0;I changes=0;bool previous=false;
    for(I r=first;r<past;++r){
        bool cut=pred(raw,at(r-1))!=pred(raw,at(r));changes+=cut;
        if(it==starts.end() || *it!=r)continue;
        require(cut && (!previous || changes==1024),"m12 run block disagrees with independently ordered raw predecessors");changes=0;previous=true;
        I end=r+1;while(end<past && pred(raw,at(end))==pred(raw,at(r)))++end;
        std::cout<<"ANCHOR window="<<id<<" block="<<(it-starts.begin())<<" rank="<<r<<" tail="<<(end-1)<<" pos="<<at(end-1)<<" complete="<<(end<past)<<'\n';
        ++it;++reports;
    }
    return reports;
}
static void counts(const char* name,const Counts& b,const Counts& a,U edges,U bytes){
    require(a.requests-b.requests==edges && a.asked-b.asked==bytes && (a.compared-b.compared)+(a.skipped-b.skipped)==bytes,"positive prefix proof accounting");
    std::cout<<' '<<name<<"_compared="<<a.compared-b.compared<<' '<<name<<"_skipped="<<a.skipped-b.skipped<<' '<<name<<"_lookups="<<a.lookups-b.lookups<<' '<<name<<"_hits="<<a.full_hits-b.full_hits<<' '<<name<<"_evicted="<<a.evictions-b.evictions;
}
static void arm(){
    constexpr I n=100000503;constexpr I cap=4*1048576;
    static_assert(std::endian::native==std::endian::little);
    int fd=open("text",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st{};
    require(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && (st.st_mode&0777)==0400 && st.st_uid==geteuid() && st.st_nlink==1 && st.st_size==n,"private input");
    void* p=mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);require(p!=MAP_FAILED,"raw window mapping");Raw t{static_cast<const std::uint8_t*>(p),n};
    std::ifstream f("starts.u64",std::ios::binary);std::vector<U> starts(37742);require(f.read(reinterpret_cast<char*>(starts.data()),starts.size()*8) && f.peek()==EOF && starts[0]==0 && starts.back()<n+1,"m12 table");
    for(unsigned i=1;i<starts.size();++i)require(starts[i]>=starts[i-1]+1024,"m12 rank progression");
    auto bins=select(t,ranks(n),cap,guard);Work s(32ull<<30,guard),hwork(2ull<<30,guard);U checked=0,total=0,sampled=0;
    for(unsigned id=0;id<bins.size();++id){auto& v=bins[id];order(v,t,s);total+=v.extent;
        std::cout<<"COVER window="<<id<<" first="<<v.wanted.first<<" count="<<v.wanted.count<<" key_lo="<<v.lo<<" key_hi="<<v.hi<<" base="<<v.base<<" extent="<<v.extent<<'\n';
        checked+=anchors(id,t,v,starts);
        auto narrow=std::make_unique<Verified<256,4,1024>>(t);auto large=std::make_unique<Verified<>>(t);
        for(unsigned half=0;half<2;++half){
            Counts nb=narrow->counts(),lb=large->counts();I first=v.wanted.first+32768*half;
            U bytes=0,esc=0,deep=0,dbytes=0,far=0,above4k=0,high=0;std::vector<bool> pages((n+4095u)/4096u,false);
            for(I r=first;r<first+32768;++r){if(!(r%2048))guard();I a=v.pos[r-1-v.base],b=v.pos[r-v.base];I h=direct(t,a,b,hwork);
                require(h<=34067 && narrow->equal(a,b,h,guard) && large->equal(a,b,h,guard),"wrong prefix/archived LCP limit");
                bytes+=h;esc+=h>=255;deep+=h>=1024;dbytes+=h>=1024?h:0;above4k+=h>=4096;
                high=std::max<U>(high,h);far+=std::max(a,b)-std::min(a,b)>=(1<<20);if(a<n)pages[a/4096]=true;if(b<n)pages[b/4096]=true;
            }
            std::cout<<"PHASE window="<<id<<" half="<<half<<" edges=32768 prefix="<<bytes<<" escapes="<<esc<<" ge1024="<<deep<<" ge4096="<<above4k<<" eligible_bytes="<<dbytes<<" max_h="<<high<<" far_pairs="<<far<<" start_pages="<<std::count(pages.begin(),pages.end(),true);
            counts("small",nb,narrow->counts(),32768,bytes);counts("large",lb,large->counts(),32768,bytes);std::cout<<'\n';sampled+=32768;
        }
    }
    require(sampled==1048576 && total<=cap && checked>=64 && !munmap(p,n) && !close(fd),"incomplete rank window study");
    std::cout<<"WINDOWS_PASS sampled_edges="<<sampled<<" sorted_cover_positions="<<total<<" reported_anchor_starts="<<checked<<" sort_memcmp_request="<<s.bytes<<" lcp_memcmp_request="<<hwork.bytes<<" histogram_bytes="<<4ull*Keys<<" large_table="<<Verified<>::table_bytes<<" small_table="<<Verified<256,4,1024>::table_bytes<<'\n';
}
int main(int argc,char** argv) try {
    require(argc==3,"fixed mode and short deadline only");deadline=std::stoll(argv[2]);require(deadline<=time(nullptr)+180,"deadline too long");guard();auto cg=grant();auto here=fs::current_path();struct stat d{};require(!stat(".",&d) && (d.st_mode&0777)==0700 && d.st_uid==geteuid(),"owned private directory");
    std::string mode=argv[1],name=here.filename().string();
    if(mode=="fixture"){require(here.parent_path()=="/tmp" && name.starts_with("prefix-window-tools-"),"fixture path");fixtures();}
    else{require(mode=="observe" && here.parent_path()=="/mnt/nvme2n1/erikg/TAPEREADER/sandbox" && name.starts_with("raw-prefix-windows-") && name.size()==25,"real window profile only");arm();}
    guard();rusage ru{};require(!getrusage(RUSAGE_SELF,&ru) && 512ull*ru.ru_inblock<=(128ull<<20) && 512ull*ru.ru_oublock<=(1ull<<20),"worker attributed IO limit");
    std::cout<<"RESOURCE peak="<<line(cg+"/memory.peak")<<" read="<<ru.ru_inblock*512ull<<" write_before_report="<<ru.ru_oublock*512ull<<" user_us="<<1000000ull*ru.ru_utime.tv_sec+ru.ru_utime.tv_usec<<" sys_us="<<1000000ull*ru.ru_stime.tv_sec+ru.ru_stime.tv_usec<<'\n';return 0;
} catch(const std::exception& e){std::cerr<<"window study refused: "<<e.what()<<'\n';return 1;}
