// Offline immutable packed columns. No raw input, writable tape or suffix arrays.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/mman.h>
#include <unistd.h>
namespace fs=std::filesystem;
using U=uint32_t; using Q=uint64_t;
static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__ && sizeof(size_t)==8);
void need(bool ok,const char* why){if(!ok) throw std::runtime_error(why);}
Q number(std::string_view s){Q n;auto r=std::from_chars(s.begin(),s.end(),n);need(r.ec==std::errc() && r.ptr==s.end(),"bad unsigned argument");return n;}
struct Word {std::string_view raw;bool end;};
struct Tape {
    const char* data; Q size;
    Word at(Q pos) const {
        need(pos<=size && size-pos>=4,"short tag"); U tag; memcpy(&tag,data+pos,4);
        U n=tag&0x7fffffff;bool end=tag>>31;
        need(n && n<=1u<<20 && n<=size-pos-4 && (end || n>8),"invalid word length");
        return {{data+pos+4,n},end};
    }
};
// A canonical word cannot skip a trigger strictly beyond its initial window.
void boundaries(Word w){bool cut=false;
    for(size_t i=8;i<w.raw.size();i++){
        Q h=1469598103934665603ull;
        for(size_t j=i-7;j<=i;j++)h=(h^uint8_t(w.raw[j]))*1099511628211ull;
        if(h%31==0){need(i+1==w.raw.size(),"skipped trigger");cut=true;}
    }
    need(cut!=w.end,"word boundary/END mismatch");
}
int main(int argc,char** argv){try{
    need(argc==5,"usage: verify COLUMNS EXPECTED_N RAW_SHA256 METADATA_LIMIT_BYTES");
    Q n=number(argv[2]),budget=number(argv[4]);need((n && n<=10000000)||n==1000000665||n==10000000000,"unqualified rung");
    fs::path dir=argv[1],p=dir/"dictionary.packed",ids=dir/"parse.u32";
    Q bytes=fs::file_size(ids),m=bytes/4,t=fs::file_size(p);
    need(bytes%4==0 && m && m<=UINT32_MAX && t && t<=2*n && bytes<=2*n-t,"invalid column extent");
    int fd=open(p.c_str(),O_RDONLY);need(fd>=0,"open dictionary");
    auto map=mmap(nullptr,t,PROT_READ,MAP_PRIVATE,fd,0);close(fd);need(map!=MAP_FAILED,"map dictionary");
    Tape tape{static_cast<const char*>(map),t}; Q k=0,ends=0,descriptors=0,longest=0;
    std::pair<std::string_view,bool> prev{};
    for(Q pos=0;pos<t;){auto w=tape.at(pos); auto key=std::make_pair(w.raw,w.end);
        need(!k || prev<key,"not strictly lexical");prev=key;boundaries(w);ends+=w.end;
        descriptors+=w.raw.size()-(w.end?0:8);longest=std::max<Q>(longest,w.raw.size());
        need(++k<=m,"unused words"); pos+=4+w.raw.size();}
    need(ends==1,"END count");Q metadata=8*k+(k+7)/8;need(metadata<=budget,"metadata grant too small");
    std::vector<Q> offsets(k);std::vector<unsigned char> seen((k+7)/8);
    for(Q i=0,pos=0;i<k;i++){offsets[i]=pos;pos+=4+tape.at(pos).raw.size();}
    std::ifstream in(ids,std::ios::binary);std::array<U,16384> block;
    auto hash=std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    need(hash && EVP_DigestInit_ex(hash.get(),EVP_sha256(),nullptr)==1,"SHA init");
    Q decoded=0,reached=0;std::string_view tail;
    for(Q j=0;j<m;){Q count=std::min<Q>(block.size(),m-j);need(bool(in.read((char*)block.data(),4*count)),"short IDs");
        for(Q i=0;i<count;i++,j++){U id=block[i];need(id<k,"ID out of range");auto w=tape.at(offsets[id]);
            need(w.end==(j+1==m),"END not uniquely last");need(!j || w.raw.substr(0,8)==tail,"overlap");
            auto owned=w.raw.size()-(w.end?0:8);need(owned<=n-decoded,"decoded length overflow");decoded+=owned;
            need(EVP_DigestUpdate(hash.get(),w.raw.data(),owned)==1,"SHA update");
            if(!w.end)tail=w.raw.substr(owned);
            U mask=1u<<(id%8);if(!(seen[id/8]&mask)){seen[id/8]|=mask;++reached;}
        }}
    need(in.peek()==EOF && reached==k && decoded==n,"unused ID or incomplete decode");
    unsigned char digest[EVP_MAX_MD_SIZE];unsigned len;need(EVP_DigestFinal_ex(hash.get(),digest,&len)==1 && len==32,"SHA final");
    std::ostringstream out;for(unsigned i=0;i<len;i++)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(digest[i]);
    need(out.str()==argv[3],"decoded SHA mismatch");std::cout<<"VERIFIED n="<<n<<" k="<<k<<" m="<<m<<" descriptors="<<descriptors<<" D="<<(descriptors+9*k-7)<<" max_phrase="<<longest<<" metadata_bytes="<<metadata<<" sha256="<<out.str()<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 70;}}
