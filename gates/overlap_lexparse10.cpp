// Exact packed front end for the 10,000,000,000-byte width gate. Keep the
// measured 1/3 GB executables reproducible. Corpus offsets are 64 bit; no
// corpus offset need be serialized: the bucket already contains the payload.
// Bucket record: {tag:u32, occurrence:u32}, then raw bytes. EOF is in tag.
#include <algorithm>
#include <array>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace fs=std::filesystem;
using U=uint32_t; using Q=uint64_t;
using Key=std::pair<std::string_view,bool>;
constexpr U W=8, EOFBIT=1u<<31, NB=1024, SHARD=1u<<20;
constexpr Q N10=10'000'000'000ull;
struct Rec { U tag, j; U len() const {return tag & ~EOFBIT;} bool eof() const {return tag & EOFBIT;} };
struct Assignment {U j,id;};
static_assert(sizeof(Rec)==8 && sizeof(Assignment)==8 && sizeof(size_t)==8 && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__);
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
[[noreturn]] void stopped(const char* s){std::fprintf(stderr,"STOP_OBSERVED lexparse10=%s\n",s);std::exit(75);}
Q traffic(const char* key){std::ifstream f("/proc/self/io");std::string k;Q v;while(f>>k>>v)if(k==key)return v;throw std::runtime_error("no proc IO counter");}
template<typename Visit> U scan(const char* t,Q n,const fs::path& flag,Visit visit){
    Q s=0,m=0;
    auto phrase=[&](Q len,bool eof){require(len && len<EOFBIT && m<UINT32_MAX,"word length or occurrence ID overflow");visit(s,Rec{U(len)|(eof?EOFBIT:0u),U(m++)});};
    for(Q i=1;i+W<=n;i++){
        if(!(i%(1u<<22)) && fs::exists(flag))stopped("requested");
        Q h=1469598103934665603ull;for(U a=0;a<W;a++)h=(h^(uint8_t)t[i+a])*1099511628211ull;
        if(h%31==0){phrase(i+W-s,false);s=i;}
    }
    phrase(n-s,true);return U(m);
}
void run(fs::path corpus,fs::path d,bool fixture){
    const Q n=fs::file_size(corpus);require(fixture?(n&&n<=101'000'000):n==N10,"wrong input rung");
    require(fs::create_directory(d),"not a fresh column directory");fs::path flag=d.parent_path()/"stop.requested";
    auto guard=[&] {if(fs::exists(flag))stopped("requested");if(traffic("read_bytes:")>6*n+(8<<20))stopped("physical-read-guard");};guard();
    int fd=open(corpus.c_str(),O_RDONLY);require(fd>=0,"open corpus");
    auto* t=static_cast<char*>(mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0));require(t!=MAP_FAILED && madvise(t,n,MADV_SEQUENTIAL)==0,"map corpus");
    Q live=0,peak=0;
    auto add=[&](Q bytes){if(bytes>2*n-live && !fixture)stopped("2N-scratch");live+=bytes;peak=std::max(peak,live);};
    auto retire=[&](const fs::path& p){Q sz=fs::file_size(p);require(sz<=live && fs::remove(p),"retire scratch");live-=sz;};
    auto path=[&](char type,U i){return d/(std::string(1,type)+std::to_string(i));};
    auto output=[](const fs::path& p){std::ofstream f;f.exceptions(std::ios::failbit|std::ios::badbit);f.open(p,std::ios::binary);return f;};
    std::vector<std::pair<std::string,bool>> sample; Q next=0,sbytes=0,high_words=0,owned=0;U longest=0;std::array<bool,256> present{};
    U m=scan(t,n,flag,[&](Q p,Rec r){
        longest=std::max(longest,r.len()); if(p>UINT32_MAX)++high_words;
        U stride=r.len()-(r.eof()?0u:W);owned+=stride;
        for(U i=0;i<stride;i++)present[(uint8_t)t[p+i]]=true;
        if(p>=next){sbytes+=r.len();require(sbytes<=32<<20,"splitter sample byte cap");sample.emplace_back(std::string(t+p,r.len()),r.eof());next=p+std::max<Q>(1,n/32768);}
    });
    require(owned==n,"ownership scan");if(!fixture && n+20ull*m>2*n)stopped("worst-case-live-buckets-and-assignments");
    std::sort(sample.begin(),sample.end()); std::vector<Key> piv;
    for(U b=1;b<NB;b++){auto& s=sample[sample.size()*b/NB];piv.emplace_back(s.first,s.second);}
    std::vector<std::ofstream> buckets; std::array<U,NB> size{},count{};
    for(U b=0;b<NB;b++)buckets.push_back(output(path('b',b)));
    require(scan(t,n,flag,[&](Q p,Rec r){
        Key k{{t+p,r.len()},r.eof()};U b=std::lower_bound(piv.begin(),piv.end(),k)-piv.begin();Q inc=sizeof r+r.len();
        if(inc+size[b]>64u<<20 || count[b]>=2'000'000)stopped("lexical-bucket-cap");
        add(inc);size[b]+=U(inc);++count[b];buckets[b].write((const char*)&r,sizeof r).write(k.first.data(),r.len());
    })==m,"scan disagreement");for(auto& f:buckets)f.close();buckets.clear();piv.clear();sample.clear();
    require(munmap(t,n)==0 && close(fd)==0,"release source corpus");
    std::vector<std::ofstream> ass;for(U i=0;i<(Q(m)+SHARD-1)/SHARD;i++)ass.push_back(output(path('a',i)));
    auto dict=output(d/"dictionary.packed");U k=0,terminals=0;Q desc=0;std::string prev_raw;bool prev_eof=false;
    for(U b=0;b<NB;b++){
        guard();std::vector<char> data(size[b]);std::ifstream f(path('b',b),std::ios::binary);require(bool(f.read(data.data(),data.size())),"read bucket");f.close();retire(path('b',b));
        auto rec=[&](U off){Rec r;std::memcpy(&r,data.data()+off,sizeof r);return r;};
        auto key=[&](U off)->Key {auto r=rec(off);return {{data.data()+off+sizeof r,r.len()},r.eof()};};
        std::vector<U> offsets;offsets.reserve(count[b]);for(U off=0;off<data.size();){require(data.size()-off>=sizeof(Rec),"short header");auto r=rec(off);require(r.len()<=data.size()-off-sizeof r,"short phrase");offsets.push_back(off);off+=sizeof r+r.len();}
        require(offsets.size()==count[b],"bucket occurrence count");std::sort(offsets.begin(),offsets.end(),[&](U x,U y){return key(x)<key(y);});
        Key last{prev_raw,prev_eof};for(U off:offsets){Rec r=rec(off);Key v=key(off);
            if(k==0||v!=last){require(!k || last<v,"nonlexical bucket boundary");require(k<UINT32_MAX,"word ID overflow");++k;terminals+=v.second;desc+=r.len()-(v.second?0u:W);
                add(4+v.first.size());dict.write((char*)&r.tag,4).write(v.first.data(),v.first.size());}
            last=v;Assignment a{r.j,k-1};add(sizeof a);ass.at(r.j/SHARD).write((char*)&a,sizeof a);
        }if(!offsets.empty()){prev_raw=last.first;prev_eof=last.second;}
    }
    dict.close();for(auto& f:ass)f.close();require(terminals==1,"EOF word count");auto parse=output(d/"parse.u32");
    for(U i=0;i<ass.size();i++){
        guard();U rows=std::min<Q>(SHARD,Q(m)-Q(i)*SHARD);std::vector<U> ids(rows,UINT32_MAX);std::ifstream f(path('a',i),std::ios::binary);
        for(U z=0;z<rows;z++){Assignment a;require(bool(f.read((char*)&a,sizeof a)) && a.j/SHARD==i && a.j%SHARD<rows && a.id<k && ids[a.j%SHARD]==UINT32_MAX,"invalid assignment");ids[a.j%SHARD]=a.id;}
        require(f.peek()==EOF,"extra assignment");f.close();retire(path('a',i));add(4ull*rows);parse.write((char*)ids.data(),4ull*rows);
    }parse.close();guard();Q tape=fs::file_size(d/"dictionary.packed"),D=desc+9ull*k-7;
    require(tape==D+3ull*k-1 && live==tape+4ull*m && fs::file_size(d/"parse.u32")==4ull*m,"final column size mismatch");
    std::printf("LEX10 n=%" PRIu64 " m=%u k=%u D=%" PRIu64 " descriptors=%" PRIu64 " dictionary_bytes=%" PRIu64 " max_phrase=%u max_bucket=%u starts_above_u32=%" PRIu64 " alphabet=%zu peak_counted=%" PRIu64 " read_bytes=%" PRIu64 " write_bytes=%" PRIu64 "\n",n,m,k,D,desc,tape,longest,*std::max_element(size.begin(),size.end()),high_words,size_t(std::count(present.begin(),present.end(),true)),peak,traffic("read_bytes:"),traffic("write_bytes:"));
}
int main(int ac,char** av){try{require(ac==3||(ac==4 && std::string_view(av[3])=="--fixture"),"lexparse10 CORPUS NEW_COLS [--fixture]");run(av[1],av[2],ac==4);return 0;}catch(const std::exception& e){std::fprintf(stderr,"lexparse10: %s\n",e.what());return 70;}}
