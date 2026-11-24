// Check the physical (parse, dictionary) representation, NOT suffix sorting.
// decode takes only the column directory. oracle takes only the original text.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <openssl/evp.h>
#include <sys/resource.h>
namespace fs=std::filesystem;
using U=uint32_t; using Q=uint64_t; using Byte=unsigned char;
static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__);
class Digest {
    EVP_MD_CTX* c=EVP_MD_CTX_new();
  public:
    Digest(){assert(c && EVP_DigestInit_ex(c,EVP_sha256(),nullptr)==1);}
    Digest(const Digest&)=delete; ~Digest(){EVP_MD_CTX_free(c);}
    void add(const void* p,size_t n){assert(EVP_DigestUpdate(c,p,n)==1);}
    std::string hex(){Byte h[32];unsigned len=0; assert(EVP_DigestFinal_ex(c,h,&len)==1 && len==32);
        char out[65];for(unsigned i=0;i<len;i++)std::snprintf(out+2*i,3,"%02x",h[i]); return out;}
};
std::vector<Byte> sequential(const fs::path& path,size_t cap){
    const auto bytes=fs::file_size(path);assert(bytes && bytes<=cap);std::vector<Byte> data(bytes);
    std::ifstream f(path,std::ios::binary);assert(f.read((char*)data.data(),bytes));assert(f.peek()==EOF);return data;
}
U tag(const Byte* p){U x;std::memcpy(&x,p,4);return x;}
struct Check {
    Digest text,boundaries; U m=0,n=0;
    void start(U i,U pred){U pair[]={i,pred};boundaries.add(pair,sizeof pair);++m;}
    void finish(U last){U end[]={n,last};boundaries.add(end,sizeof end);
        printf("CHECK n=%u phrases=%u end_pred=%u text_sha=%s boundary_sha=%s\n",n,m,last,text.hex().c_str(),boundaries.hex().c_str());}
};
void stop(const fs::path& flag){if(fs::exists(flag)){fprintf(stderr,"STOP_OBSERVED phase=packed-check\n");exit(75);}}
void oracle(const fs::path& corpus,const fs::path& flag){
    auto t=sequential(corpus,1'010'000'000);Check c;c.n=t.size();c.text.add(t.data(),t.size());c.start(0,256);
    for(U i=1;i+8<=t.size();i++){if(!(i%1048576))stop(flag);Q h=1469598103934665603ull;
        for(unsigned z=0;z<8;z++)h=(h^t[i+z])*1099511628211ull;
        if(h%31==0)c.start(i,t[i-1]);}
    c.finish(t.back());
}
void decode(const fs::path& dir,const fs::path& flag){
    auto tape=sequential(dir/"dictionary.packed",1'500'000'000);std::ifstream f(dir/"parse.u32",std::ios::binary);
    const Q pbytes=fs::file_size(dir/"parse.u32");assert(f && pbytes && pbytes%4==0 && pbytes<=128'000'000);
    const U m=pbytes/4;std::vector<U> start;start.reserve(std::min(m,26'000'000u));int terminals=0;
    for(size_t i=0;i<tape.size();){assert(tape.size()-i>=4 && start.size()<26'000'000);start.push_back(i);
        U t=tag(tape.data()+i),size=t&0x7fff'ffffu; terminals+=t>>31;
        assert(size>(t>>31?0u:8u) && size<=tape.size()-i-4);i+=4+size;}
    assert(terminals==1); Check c;std::array<U,65536> ids;const Byte* overlap=nullptr;U pred=256;
    while(c.m<m){stop(flag);U take=std::min<U>(m-c.m,ids.size());assert(f.read((char*)ids.data(),4*take));
        for(U j=0;j<take;j++){
            const Byte* ptr=tape.data()+start.at(ids[j]); U t=tag(ptr),len=t&0x7fff'ffffu;const Byte* word=ptr+4;
            bool eof=t>>31;assert(eof==(c.m+1==m));U stride=eof?len:len-8;
            if(overlap)assert(len>=8 && std::memcmp(overlap,word,8)==0);
            c.start(c.n,pred); c.text.add(word,stride); assert(Q(c.n)+stride<=1'010'000'000);c.n+=stride;
            // Not word[len-1]: the last eight bytes of a nonfinal word
            // are owned by the following phrase. No source text lookup.
            pred=word[stride-1];overlap=eof?nullptr:word+stride;
        }}
    assert(f.peek()==EOF);c.finish(pred);
    printf("DECODE lexical_words=%zu packed_bytes=%zu parse_bytes=%llu corpus_refs_opened=0 overlap_edges=%u decoded_output_files=0\n",start.size(),tape.size(),(unsigned long long)pbytes,m-1);
}
int main(int ac,char**av){assert(ac==4);std::string mode=av[1];stop(av[3]);
    if(mode=="decode")decode(av[2],av[3]);else{assert(mode=="oracle");oracle(av[2],av[3]);}
    std::ifstream f("/proc/self/io");std::string k;Q v;while(f>>k>>v)if(k=="read_bytes:"||k=="write_bytes:")printf("IO %s %llu\n",k.c_str(),(unsigned long long)v);
}
