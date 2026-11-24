// Fixture-entry-only raw -> packed census. No large-input authorization here.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

namespace fs = std::filesystem;
using U = uint32_t;
using Q = uint64_t;
constexpr U W=8, END=1u<<31, MAX_WORD=1u<<20, FAN=16, MAX_RUNS=65536;
struct Header { U tag,j; U len() const { return tag & ~END; } };
static_assert(sizeof(Header)==8 && sizeof(off_t)==8 && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__);
using Key=std::pair<std::string_view,bool>;
void check(bool ok,const char* msg){ if(!ok) throw std::runtime_error(msg); }
struct Stop : std::runtime_error {using runtime_error::runtime_error;};
struct Ledger {
    fs::path dir,flag;
    Q cap,live=0,peak=0;
    [[noreturn]] void stop(const char* why,Q attempted=0) const {
        std::cerr<<"SCRATCH live="<<live<<" peak="<<peak<<" cap="<<cap<<" attempted="<<attempted<<'\n';
        throw Stop(why);
    }
    void poll() const { if(fs::exists(flag)) stop("requested"); }
    void charge(Q b){ if(b>cap-live) stop("2N-scratch",b); live+=b; peak=std::max(peak,live); }
    void retire(const fs::path& p){ Q b=fs::file_size(p); check(b<=live && fs::remove(p),"retire segment"); live-=b; }
};
void limits(){
    cpu_set_t allowed; check(!sched_getaffinity(0,sizeof allowed,&allowed),"get affinity");
    int cpu=0; while(cpu<CPU_SETSIZE && !CPU_ISSET(cpu,&allowed)) ++cpu;
    check(cpu<CPU_SETSIZE,"no CPU"); CPU_ZERO(&allowed); CPU_SET(cpu,&allowed);
    check(!sched_setaffinity(0,sizeof allowed,&allowed),"pin CPU");
    rlimit ram{192u<<20,192u<<20}; check(!setrlimit(RLIMIT_AS,&ram),"address space cap");
    check(!mlockall(MCL_CURRENT|MCL_FUTURE),"no-swap memory lock refused");
}
// Keep the final trigger's eight bytes for the following (possibly END-only) word.
template<class F> U scan(std::ifstream& in,Q n,Ledger& disk,F consume){
    std::vector<char> word; word.reserve(MAX_WORD); std::array<char,65536> buf;
    Q pos=0,m=0;
    auto emit=[&](bool end){
        if(m==UINT32_MAX) disk.stop("occurrence-width");
        consume(Header{U(word.size())|(end?END:0u),U(m++)},word.data(),pos-word.size());
    };
    in.clear(); in.seekg(0);
    while(pos<n){
        disk.poll(); U want=std::min<Q>(buf.size(),n-pos); check(bool(in.read(buf.data(),want)),"short corpus");
        for(U i=0;i<want;i++){
            if(word.size()==MAX_WORD) disk.stop("phrase-cap");
            word.push_back(buf[i]); ++pos;
            if(pos<=W) continue;
            Q h=1469598103934665603ull; const char* tail=word.data()+word.size()-W;
            for(U a=0;a<W;a++) h=(h^uint8_t(tail[a]))*1099511628211ull;
            if(h%31==0){emit(false); std::memmove(word.data(),tail,W); word.resize(W);}
        }
    }
    check(in.peek()==EOF,"corpus grew"); emit(true); return U(m);
}
struct Run { U id,parts=0; };
fs::path segment(const Ledger& d,Run r,U p){return d.dir/("r"+std::to_string(r.id)+"."+std::to_string(p));}
struct Writer {
    Ledger& d; Run r; U limit,bytes=0; std::ofstream out;
    Writer(Ledger& d,U id,U limit):d(d),r{id},limit(limit){out.exceptions(std::ios::badbit|std::ios::failbit);}
    void put(Header h,const char* raw){
        U inc=sizeof h+h.len();
        if(!bytes || bytes+inc>limit){
            d.poll(); if(bytes)out.close(); out.open(segment(d,r,r.parts++),std::ios::binary); bytes=0;
        }
        d.charge(inc); out.write((char*)&h,sizeof h).write(raw,h.len()); bytes+=inc;
    }
    Run finish(){out.close();return r;}
};
struct Reader {
    Ledger& d; Run r; U part=0; Q left=0; std::ifstream in;
    Header h{}; std::string raw;
    Reader(Ledger& d,Run r):d(d),r(r){raw.reserve(MAX_WORD);}
    Key key() const {return {raw,bool(h.tag&END)};}
    bool advance(){
        if(!left){
            d.poll(); if(part){in.close(); d.retire(segment(d,r,part-1));}
            if(part==r.parts) return false;
            auto p=segment(d,r,part++); left=fs::file_size(p); in.open(p,std::ios::binary);
        }
        check(left>=sizeof h && bool(in.read((char*)&h,sizeof h)),"short segment");
        check(h.len() && h.len()<=MAX_WORD && h.len()<=left-sizeof h,"bad segment length");
        raw.resize(h.len()); check(bool(in.read(raw.data(),raw.size())),"short segment payload");
        left-=sizeof h+raw.size(); return true;
    }
};
template<class F> void merge(Ledger& disk,const Run* runs,size_t count,F emit){
    auto later=[](Reader* a,Reader* b){return a->key()>b->key();};
    std::priority_queue<Reader*,std::vector<Reader*>,decltype(later)> heap(later);
    std::vector<std::unique_ptr<Reader>> readers;
    for(size_t i=0;i<count;i++){
        readers.emplace_back(std::make_unique<Reader>(disk,runs[i]));
        auto* p=readers.back().get(); if(p->advance()) heap.push(p);
    }
    while(!heap.empty()){auto* p=heap.top(); heap.pop(); emit(p->h,p->raw.data()); if(p->advance())heap.push(p);}
}
// Stable text order needs no random parse writes. Route fixed-width pairs by
// occurrence interval (not lexical mass), then invert a bounded leaf in RAM.
struct Assignment {U j,id;};
static_assert(sizeof(Assignment)==8);
struct Router {
    Ledger& disk; Q base,count,span; U leaf;
    std::vector<U> ids;
    std::vector<std::ofstream> children;
    Router(Ledger& d,Q base,Q count,U leaf):disk(d),base(base),count(count),span((count+63)/64),leaf(leaf){
        if(count<=leaf){ids.assign(count,UINT32_MAX);return;}
        children.resize((count+span-1)/span);
        for(size_t i=0;i<children.size();i++){
            children[i].exceptions(std::ios::failbit|std::ios::badbit);
            children[i].open(path(i),std::ios::binary);
        }
    }
    fs::path path(size_t i) const {return disk.dir/("a"+std::to_string(base)+"."+std::to_string(count)+"."+std::to_string(i));}
    void put(Assignment a){
        check(a.j>=base && a.j-base<count,"bad occurrence interval");
        Q j=a.j-base;
        if(!ids.empty()){check(ids[j]==UINT32_MAX,"duplicate occurrence");ids[j]=a.id;}
        else {disk.charge(sizeof a);children[j/span].write((char*)&a,sizeof a);}
    }
    void emit(std::ofstream& parse,U k){
        disk.poll();
        if(!ids.empty()){
            for(U id:ids) check(id<k,"missing occurrence");
            disk.charge(4*count);parse.write((char*)ids.data(),4*count);return;
        }
        for(auto& f:children)f.close();
        for(size_t i=0;i<children.size();i++){
            disk.poll(); auto p=path(i); Q rows=std::min(span,count-i*span);
            check(fs::file_size(p)==8*rows,"assignment size");
            Router child(disk,base+i*span,rows,leaf);std::ifstream in(p,std::ios::binary);
            for(Q j=0;j<rows;j++){
                if(!(j%65536))disk.poll();
                Assignment a{};
                check(bool(in.read((char*)&a,sizeof a)),"short assignment");child.put(a);
            }
            in.close();disk.retire(p);child.emit(parse,k);
        }
    }
};
void census(const fs::path& src,const fs::path& dir,Q n,bool small){
    check(fs::create_directory(dir),"not a new output directory");
    Ledger disk{dir,dir.parent_path()/"stop.requested",2*n}; disk.poll();
    std::ifstream in(src,std::ios::binary); check(bool(in),"open corpus");
    U longest=0; Q high=0,owned=0; std::array<bool,256> alphabet{};
    U m=scan(in,n,disk,[&](Header h,const char* raw,Q start){
        longest=std::max(longest,h.len()); high+=start>UINT32_MAX;
        U stride=h.len()-((h.tag&END)?0:W);owned+=stride;
        for(U i=0;i<stride;i++)alphabet[uint8_t(raw[i])]=true;
    });check(owned==n,"scan ownership");
    const U chunk=small?1024:16u<<20, slice=small?512:1u<<20, rows=small?64:1u<<20;
    std::vector<Run> runs; U serial=0;
    std::vector<char> arena; arena.reserve(std::max(chunk,MAX_WORD+8));
    std::vector<U> offsets; offsets.reserve(rows);
    auto header=[&](U off){Header h;std::memcpy(&h,arena.data()+off,sizeof h);return h;};
    auto key=[&](U off)->Key{auto h=header(off);return {{arena.data()+off+8,h.len()},bool(h.tag&END)};};
    auto sort_chunk=[&]{
        disk.poll();std::sort(offsets.begin(),offsets.end(),[&](U a,U b){return key(a)<key(b);});
    };
    auto flush=[&]{
        disk.poll(); if(runs.size()==MAX_RUNS)disk.stop("run-directory-cap");
        sort_chunk();
        Writer w(disk,serial++,slice); for(U off:offsets)w.put(header(off),arena.data()+off+8);
        runs.push_back(w.finish());arena.clear();offsets.clear();
    };
    check(m==scan(in,n,disk,[&](Header h,const char* raw,Q){
        if(!offsets.empty() && (offsets.size()==rows || arena.size()+8+h.len()>chunk))flush();
        offsets.push_back(U(arena.size())); auto* p=(const char*)&h;
        arena.insert(arena.end(),p,p+8); arena.insert(arena.end(),raw,raw+h.len());
    }),"scan disagreement");
    if(runs.empty())sort_chunk();
    else {flush();std::vector<char>().swap(arena);std::vector<U>().swap(offsets);}
    in.close(); Q initial=runs.size(),passes=0;
    while(runs.size()>FAN){std::vector<Run> next;
        for(size_t i=0;i<runs.size();i+=FAN){Writer w(disk,serial++,slice);
            merge(disk,&runs[i],std::min<size_t>(FAN,runs.size()-i),[&](Header h,const char* raw){w.put(h,raw);});
            next.push_back(w.finish());
        }runs.swap(next);++passes;
    }
    Router order(disk,0,m,small?64:1u<<20);
    auto pp=dir/"parse.u32";
    std::ofstream dict; dict.exceptions(std::ios::badbit|std::ios::failbit); dict.open(dir/"dictionary.packed",std::ios::binary);
    std::string last; bool end=false; U k=0,terminals=0; Q descriptors=0,seen=0;
    auto assign=[&](Header h,const char* raw){
        Key v{{raw,h.len()},bool(h.tag&END)},prev{last,end};
        if(!k || v!=prev){check(!k || prev<v,"merge order"); check(k<m,"dictionary width");++k;
            disk.charge(4ull+h.len()); dict.write((char*)&h.tag,4).write(raw,h.len());
            last.assign(v.first);end=v.second;terminals+=end;descriptors+=h.len()-(end?0:W);
        }
        if(!(seen%65536))disk.poll();
        order.put({h.j,k-1}); ++seen;
    };
    if(runs.empty()){for(U off:offsets)assign(header(off),arena.data()+off+8);}
    else merge(disk,runs.data(),runs.size(),assign);
    dict.close();check(terminals==1 && seen==m,"final counts");
    std::ofstream parse;parse.exceptions(std::ios::failbit|std::ios::badbit);parse.open(pp,std::ios::binary);
    order.emit(parse,k);parse.close();disk.poll();
    Q D=descriptors+9ull*k-7,tape=fs::file_size(dir/"dictionary.packed");
    check(D+3ull*k-1==tape && disk.live==tape+4ull*m && fs::file_size(pp)==4ull*m,"packed size");
    std::cout<<"LEX100 n="<<n<<" m="<<m<<" k="<<k<<" D="<<D<<" descriptors="<<descriptors<<" dictionary_bytes="<<tape
             <<" parse_extent="<<m+6ull*k<<" parse_bytes="<<4ull*m<<" max_phrase="<<longest<<" starts_above_u32="<<high<<" alphabet="<<std::count(alphabet.begin(),alphabet.end(),true)
             <<" initial_runs="<<initial<<" intermediate_passes="<<passes<<" peak_counted="<<disk.peak<<'\n';
}
int main(int argc,char** argv){try{
    check(argc==4,"usage: lexparse100 CORPUS NEW_DIR --fixture[-small]");std::string_view mode(argv[3]);
    check(mode=="--fixture" || mode=="--fixture-small","fixture entry only");
    check(fs::is_regular_file(argv[1]),"not a regular input"); Q n=fs::file_size(argv[1]);
    check(n && n<=10'000'000,"wrong fixture size"); limits(); census(argv[1],argv[2],n,mode=="--fixture-small");return 0;
}catch(const Stop& e){std::cerr<<"STOP_OBSERVED lexparse100="<<e.what()<<'\n';return 75;}
 catch(const std::exception& e){std::cerr<<"lexparse100: "<<e.what()<<'\n';return 70;}}
