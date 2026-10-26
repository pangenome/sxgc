// Offline exact arbiter. No product headers, rank stamps, ISA or bank used.
// SA is a verified finite suffix array in REMAP order, with shorter suffix first.
// BWT predecessor wraps at p=0 to T[n-1]; no extra sentinel row is inserted.
// --stream reads the explicit debug 40-byte/run band dump; --container decodes
// the published m1 chars/lens. Both expand logically, comparing EVERY row.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
using U=uint64_t; using B=uint8_t;
static void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
static U le(const B* p,unsigned w=8){U v=0;for(unsigned i=0;i<w;++i)v|=U(p[i])<<(8*i);return v;}
struct Map{int fd; size_t size; const B* p;
 explicit Map(const std::string& path){fd=open(path.c_str(),O_RDONLY);struct stat s{};
 require(fd>=0 && !fstat(fd,&s) && s.st_size>0,"open/stat empty or missing file");size=s.st_size;
 p=(const B*)mmap(nullptr,size,PROT_READ,MAP_PRIVATE,fd,0);require(p!=MAP_FAILED,"mmap");}
 ~Map(){munmap((void*)p,size);close(fd);}
};
struct Bits{const B* p;U size,pos=0;unsigned bit(){require(pos<size,"truncated bits");unsigned b=(p[pos/8]>>(pos%8))&1;++pos;return b;}
 U gamma(){unsigned z=0;while(!bit()){require(++z<64,"gamma overflow");}U v=1;while(z--)v=v*2+bit();return v;}};
struct Run{U len;B c;};
struct M1{Bits heads{},lens{};U runs=0;std::array<U,64> first{},count{},base{};std::vector<B> symbols;
 M1(const B* p,U size,U r):runs(r){require(size>=2320,"short m1");
 std::vector<std::pair<unsigned,B>> codes;for(unsigned c=0;c<256;++c)if(p[2048+c]){require(p[2048+c]<=63,"Huffman width");codes.push_back({p[2048+c],B(c)});}
 require(!codes.empty(),"empty Huffman alphabet");std::sort(codes.begin(),codes.end());U code=0;unsigned prev=0;
 for(auto [w,c]:codes){code<<=(w-prev);require(code<(U(1)<<w),"oversubscribed Huffman code");if(!count[w]){first[w]=code;base[w]=symbols.size();}++count[w];symbols.push_back(c);++code;prev=w;}
 U hb=le(p+2304),lb=le(p+2312);require((hb+7)/8<=size-2320,"head size");U hs=(hb+7)/8;
 require((lb+7)/8==size-2320-hs,"length size");heads={p+2320,hb};lens={p+2320+hs,lb};}
 Run next(){require(runs>0,"too many runs");--runs;U code=0;for(unsigned w=1;w<64;++w){code=code*2+heads.bit();if(count[w] && code>=first[w] && code-first[w]<count[w])return {lens.gamma(),symbols[base[w]+code-first[w]]};}throw std::runtime_error("invalid Huffman code");}
 void finish(){require(!runs && heads.pos==heads.size && lens.pos==lens.size,"unused runs/bits");}
};
struct Compare{const B* text;const B* sa;const B* remap;U n,row=0,bad=0,runs=0;
 void add(Run v){require(v.len && row<=n && v.len<=n-row,"invalid stream length");++runs;
 for(U end=row+v.len;row<end;++row){U p=le(sa+row*8);require(p<n,"SA out of range");B want=remap[text[p?p-1:n-1]];
 if(v.c!=want){if(bad<8)fprintf(stderr,"MISMATCH row=%llu got=%u want=%u\n",(unsigned long long)row,v.c,want);++bad;}}}
 bool finish(){require(row==n,"stream does not cover n rows");printf("STREAM_VS_BWT rows=%llu runs=%llu mismatches=%llu convention=finite-SA,wrapped-predecessor\n",(unsigned long long)row,(unsigned long long)runs,(unsigned long long)bad);return !bad;}
};
// Independent scalar scan of SA-derived runs, using direct finite LCE. No
// product Sweep or column code is included. --chi-column substitutes the
// independently checked LCP column for large corpora (reported explicitly).
static U chi(const Map& t,const Map& sa,const B* remap,const std::string& colpath){
 U n=t.size;std::vector<U> escapes;std::unique_ptr<Map> col;
 if(!colpath.empty()){col=std::make_unique<Map>(colpath);require(col->size>=48 && le(col->p+8)==n,"column n");U e=le(col->p+24);require(col->size==48+n+16*e,"column size");escapes.resize(2*e);for(U i=0;i<2*e;++i)escapes[i]=le(col->p+48+n+8*i);}
 U ei=0;auto lcp=[&](U r){if(!r)return U(0);if(col){if(ei<escapes.size() && escapes[ei]==r){U v=escapes[ei+1];ei+=2;return v;}require(col->p[48+r]!=255,"missing column escape");return U(col->p[48+r]);}
 U a=le(sa.p+48+8*(r-1)),b=le(sa.p+48+8*r),k=0,cap=std::min(n-a,n-b);while(k<cap && remap[t.p[a+k]]==remap[t.p[b+k]])++k;return k;};
 std::array<int64_t,256> level;level.fill(-1);std::array<bool,256> active{};U answer=0;
 int prior=-1;U inside=UINT64_MAX,between=UINT64_MAX;
 auto advance=[&](B c,U top,U internal){if(prior>=0){U threshold=std::min({between,inside,top});
 if(c!=prior){for(unsigned cc=1;cc<256;++cc)if(int64_t(threshold)<level[cc]){answer+=active[cc];level[cc]=threshold;active[cc]=false;}
 for(unsigned cc:{unsigned(prior),unsigned(c)})if(int64_t(top)>level[cc]){level[cc]=top;active[cc]=true;}
 between=UINT64_MAX;
 }else between=threshold;}prior=c;inside=internal;};
 B c=0;U top=0,internal=UINT64_MAX;bool have=false;
 for(U r=0;r<n;++r){U p=le(sa.p+48+8*r);require(p<n,"chi SA bounds");B next=remap[t.p[p?p-1:n-1]];U l=lcp(r);
 if(have && next==c)internal=std::min(internal,l);else{if(have)advance(c,top,internal);c=next;top=l;internal=UINT64_MAX;have=true;}}
 if(have)advance(c,top,internal);
 for(unsigned cc=1;cc<256;++cc)answer+=active[cc];
 printf("CHI_INDEPENDENT value=%llu lcp_source=%s\n",(unsigned long long)answer,col?"column":"brute-finite-SA");return answer;
}
static int selftest(){B t[]={'a','b','a'},r[256];std::iota(r,r+256,0);B sa[24]{};sa[0]=2;sa[8]=0;sa[16]=1;
 Compare ok{t,sa,r,3};ok.add({1,'b'});ok.add({2,'a'});require(ok.finish(),"correct stream");Compare bad{t,sa,r,3};bad.add({3,'a'});require(!bad.finish(),"detect wrong byte");
 bool caught=false;try{Compare shorty{t,sa,r,3};shorty.add({2,'a'});shorty.finish();}catch(...){caught=true;}require(caught,"detect short stream");
 caught=false;try{Compare over{t,sa,r,3};over.add({4,'a'});}catch(...){caught=true;}require(caught,"detect long stream");
 B bits[]={0x65};Bits b{bits,7};require(b.gamma()==1 && b.gamma()==2 && b.gamma()==3,"gamma decoding");
 caught=false;try{b.bit();}catch(...){caught=true;}require(caught,"bit bounds");
 // Canonical 1-bit Huffman codes a=0,b=1; lengths 1,2 => bits 1,0,1,0.
 std::vector<B> m(2322);m[2048+'a']=m[2048+'b']=1;m[2304]=2;m[2312]=4;m[2320]=2;m[2321]=5;
 M1 decoder(m.data(),m.size(),2);auto x=decoder.next(),y=decoder.next();require(x.c=='a' && x.len==1 && y.c=='b' && y.len==2,"m1 decoding");decoder.finish();
 printf("STREAM_VS_BWT_SELFTEST_PASS corruption,truncation,extent,gamma,Huffman\n");return 0;}
int main(int argc,char** argv){try{
 std::string corpus,sap,remapp,stream,container,col;bool wantchi=false;U expect=UINT64_MAX;
 for(int i=1;i<argc;++i){std::string a=argv[i];auto value=[&]{require(i+1<argc,"missing argument");return std::string(argv[++i]);};
 if(a=="--selftest")return selftest();else if(a=="--corpus")corpus=value();else if(a=="--sa")sap=value();else if(a=="--remap")remapp=value();else if(a=="--stream")stream=value();else if(a=="--container")container=value();else if(a=="--chi")wantchi=true;else if(a=="--chi-column"){col=value();wantchi=true;}else if(a=="--chi-expect"){expect=std::stoull(value());wantchi=true;}else throw std::runtime_error("unknown argument");}
 require(!corpus.empty() && !sap.empty() && !remapp.empty(),"need --corpus --sa --remap");require(!stream.empty() || !container.empty(),"need --stream or --container");
 Map t(corpus),sa(sap),remap(remapp);require(remap.size==256 && sa.size==48+8*t.size,"input sizes");
 require(le(sa.p+8)==t.size && le(sa.p+24)==t.size,"SA header n/rows");bool pass=true;
 if(!stream.empty()){Map dump(stream);require(dump.size%40==0,"truncated stream record");Compare check{t.p,sa.p+48,remap.p,t.size};U endpointBad=0;for(U off=0;off<dump.size;off+=40){require(le(dump.p+off)==off/40,"stream record index");U start=check.row;
 check.add({le(dump.p+off+32,4),dump.p[off+36]});
 endpointBad += le(dump.p+off+16)!=le(sa.p+48+8*start);
 endpointBad += le(dump.p+off+24)!=le(sa.p+48+8*(check.row-1));}
 pass&=check.finish();pass&=endpointBad==0;
 printf("STREAM_ENDPOINTS runs=%llu mismatches=%llu\n",(unsigned long long)check.runs,(unsigned long long)endpointBad);}
 if(!container.empty()){Map product(container);require(product.size>=64 && !memcmp(product.p,"SXI2",4) && le(product.p+8)==t.size,"container header");U count=le(product.p+32,4);require(count<100 && product.size>=64+40*count,"member directory");bool found=false;
 for(U i=0;i<count;++i){const B* d=product.p+64+40*i;if(le(d,4)!=1)continue;require(!found && le(d+4,4)==101,"m1 codec");found=true;U off=le(d+8),size=le(d+16),r=le(d+24);require(off<=product.size && size<=product.size-off,"m1 bounds");M1 decoder(product.p+off,size,r);Compare check{t.p,sa.p+48,remap.p,t.size};while(decoder.runs)check.add(decoder.next());decoder.finish();pass&=check.finish();}require(found,"missing m1");}
 if(wantchi){U got=chi(t,sa,remap.p,col);if(expect!=UINT64_MAX)require(got==expect,"independent chi mismatch");}
 return pass?0:3;
 }catch(const std::exception& e){fprintf(stderr,"stream_vs_bwt: %s\n",e.what());return 2;}}
