// Offline exact arbiter. No product headers, rank stamps, ISA or bank used.
// THE CONVENTION: the text is T' = corpus . 0xF5 (exactly one appended
// end-of-text byte, product symbol 0, never written to disc), n = |T'| = the
// corpus size + 1, and SA is the verified suffix array of T' in REMAP order. The
// BWT row of position p carries the predecessor T'[p-1], which for p == 0 is the
// sentinel - the unique symbol-0 row.
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
 U fileN=0;   // corpus bytes on disc; index n-1 == fileN is the sentinel
 U sampleFirst=0,sampleCount=0,checked=0;
 // The product symbol at text index i (the corpus byte through the remap, or the
 // appended sentinel past the end of the file).
 B sym(U i)const{if(i>=fileN)return remap[0xF5];return remap[text[i]];}
 void add(Run v){require(v.len && row<=n && v.len<=n-row,"invalid stream length");++runs;
 U end=row+v.len;
 U lo=sampleCount?std::max(row,sampleFirst):row;
 U hi=sampleCount?std::min(end,sampleFirst+sampleCount):end;
 for(U rank=lo;rank<hi;++rank){U p=le(sa+(sampleCount?rank-sampleFirst:rank)*8);require(p<n,"SA out of range");B want=sym(p?p-1:n-1);
 if(v.c!=want){if(bad<8)fprintf(stderr,"MISMATCH row=%llu got=%u want=%u\n",(unsigned long long)rank,v.c,want);++bad;}++checked;}row=end;}
 bool finish(){require(row==n,"stream does not cover n rows");require(checked==(sampleCount?sampleCount:n),"comparison coverage");if(sampleCount)printf("STREAM_VS_BWT_SAMPLE first_rank=%llu sampled_rows=%llu total_rows=%llu mismatches=%llu\n",(unsigned long long)sampleFirst,(unsigned long long)checked,(unsigned long long)n,(unsigned long long)bad);printf("STREAM_VS_BWT rows=%llu runs=%llu mismatches=%llu convention=sentinel-text(0xf5),finite-SA,predecessor-of-0-is-sentinel\n",(unsigned long long)row,(unsigned long long)runs,(unsigned long long)bad);return !bad;}
};
// Independent scalar scan of SA-derived runs, using direct finite LCE. No
// product Sweep or column code is included. --chi-column substitutes the
// independently checked LCP column for large corpora (reported explicitly).
static U chi(const Map& t,const Map& sa,const B* remap,const std::string& colpath){
 // n includes the appended sentinel; the corpus bytes on disc are t.size-1.
 const U n=t.size+1;const U fileN=t.size;   // n includes the appended sentinel
 auto sym=[&](U i)->B{if(i>=fileN)return remap[0xF5];return remap[t.p[i]];};
 std::vector<U> escapes;std::unique_ptr<Map> col;
 if(!colpath.empty()){col=std::make_unique<Map>(colpath);require(col->size>=48 && le(col->p+8)==n,"column n");U e=le(col->p+24);require(col->size==48+n+16*e,"column size");escapes.resize(2*e);for(U i=0;i<2*e;++i)escapes[i]=le(col->p+48+n+8*i);}
 U ei=0;auto lcp=[&](U r){if(!r)return U(0);if(col){if(ei<escapes.size() && escapes[ei]==r){U v=escapes[ei+1];ei+=2;return v;}require(col->p[48+r]!=255,"missing column escape");return U(col->p[48+r]);}
 U a=le(sa.p+48+8*(r-1)),b=le(sa.p+48+8*r),k=0,cap=std::min(n-a,n-b);while(k<cap && sym(a+k)==sym(b+k))++k;return k;};
 std::array<int64_t,256> level;level.fill(-1);std::array<bool,256> active{};U answer=0;
 int prior=-1;U inside=UINT64_MAX,between=UINT64_MAX;
 auto advance=[&](B c,U top,U internal){if(prior>=0){U threshold=std::min({between,inside,top});
 if(c!=prior){for(unsigned cc=1;cc<256;++cc)if(int64_t(threshold)<level[cc]){answer+=active[cc];level[cc]=threshold;active[cc]=false;}
 for(unsigned cc:{unsigned(prior),unsigned(c)})if(int64_t(top)>level[cc]){level[cc]=top;active[cc]=true;}
 between=UINT64_MAX;
 }else between=threshold;}prior=c;inside=internal;};
 B c=0;U top=0,internal=UINT64_MAX;bool have=false;
 for(U r=0;r<n;++r){U p=le(sa.p+48+8*r);require(p<n,"chi SA bounds");B next=sym(p?p-1:n-1);U l=lcp(r);
 if(have && next==c)internal=std::min(internal,l);else{if(have)advance(c,top,internal);c=next;top=l;internal=UINT64_MAX;have=true;}}
 if(have)advance(c,top,internal);
 for(unsigned cc=1;cc<256;++cc)answer+=active[cc];
 printf("CHI_INDEPENDENT value=%llu lcp_source=%s\n",(unsigned long long)answer,col?"column":"brute-finite-SA");return answer;
}
// A TINY BRUTE-FORCE ORACLE IN THE SENTINEL CONVENTION. The text is
// T' = corpus . 0xF5; SA is built by direct comparison over that virtual byte,
// and the BWT row of position p is the predecessor symbol. Adversarial cases are
// included on purpose: every byte equal, periodic two-byte text, text whose bytes
// include the remapped NUL/0x05 symbols, and lengths 1..7.
static bool brute_selftest_case(const std::vector<B>& corpus, const B* remap){
 const U fileN=corpus.size(), n=fileN+1;
 auto cand=[&](U i)->B{return i>=fileN?remap[0xF5]:remap[corpus[i]];};
 std::vector<U> sa(n);for(U i=0;i<n;++i)sa[i]=i;
 std::sort(sa.begin(),sa.end(),[&](U a,U b){
  for(U i=0;;++i){const U x=a+i,y=b+i;
   if(x>=n||y>=n)return y>=n && x<n ? false : (x>=n && y<n ? true : a<b);
   if(cand(x)!=cand(y))return cand(x)<cand(y);}
  return a<b;});
 Compare check{corpus.data(),(const B*)sa.data(),remap,n};check.fileN=fileN;
 for(U r=0;r<n;++r)check.add({1,cand(sa[r]?sa[r]-1:n-1)});
 if(!check.finish())return false;
 Compare bad{corpus.data(),(const B*)sa.data(),remap,n};bad.fileN=fileN;
 for(U r=0;r<n;++r){B c=cand(sa[r]?sa[r]-1:n-1);if(r==n/2)c=(B)(c+1);bad.add({1,c});}
 return !bad.finish();
}
static int selftest(){
 B r[256];std::iota(r,r+256,0);
 bool okAll=true;
 const std::vector<std::vector<B>> cases={
  {'a'}, {'a','a'}, {'a','b','a'}, {'a','b','a','b'}, {'b','b','b','b'},
  {5,0,5,0,5}, {1,2,3,4,5,6,7}, {9,9,1,9,9}, {'a','b','a','b','a','b','a'}};
 for(const auto& c:cases){
  B map[256];unsigned next=1;std::array<int,256> val{};val.fill(-1);
  for(unsigned b=0;b<256;++b)if(b!=0xF5 && std::find(c.begin(),c.end(),(B)b)!=c.end())val[b]=int(next++);
  unsigned fill=next;
  for(unsigned b=0;b<256;++b)map[b]= b==0xF5 ? 0 : (val[b]>=0 ? B(val[b]) : B(fill++));
  if(!brute_selftest_case(c,map)){okAll=false;break;}
 }
 require(okAll,"sentinel brute oracle (SA order, BWT rows, corruption)");
 // corruption/truncation/extent refusals at the Compare level
 B t[]={'a','b','a'};B sa[32]{};sa[0]=3;sa[8]=2;sa[16]=0;sa[24]=1;
 Compare shorty{t,sa,r,4};shorty.fileN=3;shorty.add({3,'a'});bool caught=false;
 try{shorty.finish();}catch(...){caught=true;}require(caught,"detect short stream");
 caught=false;try{Compare over{t,sa,r,4};over.fileN=3;over.add({5,'a'});}catch(...){caught=true;}require(caught,"detect long stream");
 B bits[]={0x65};Bits b{bits,7};require(b.gamma()==1 && b.gamma()==2 && b.gamma()==3,"gamma decoding");
 caught=false;try{b.bit();}catch(...){caught=true;}require(caught,"bit bounds");
 // the canonical T'={a,b,a}.0xF5 stream: SA=[3,2,0,1], BWT rows a b $ a
 {
  Compare good{t,sa,r,4};good.fileN=3;
  good.add({1,'a'});good.add({1,'b'});good.add({1,0xF5});good.add({1,'a'});
  require(good.finish(),"correct stream in the sentinel convention");
  Compare sample{t,sa+8,r,4};sample.fileN=3;sample.sampleFirst=1;sample.sampleCount=2;
  sample.add({1,'a'});sample.add({1,'b'});sample.add({1,0xF5});sample.add({1,'a'});
  require(sample.finish() && sample.checked==2,"sample offset/coverage");
 }
 // Canonical 1-bit Huffman codes a=0,b=1; lengths 1,2 => bits 1,0,1,0.
 std::vector<B> m(2322);m[2048+'a']=m[2048+'b']=1;m[2304]=2;m[2312]=4;m[2320]=2;m[2321]=5;
 M1 decoder(m.data(),m.size(),2);auto x=decoder.next(),y=decoder.next();require(x.c=='a' && x.len==1 && y.c=='b' && y.len==2,"m1 decoding");decoder.finish();
 caught=false;try{M1 truncated(m.data(),2321,2);}catch(...){caught=true;}require(caught,"m1 extent");
 printf("STREAM_VS_BWT_SELFTEST_PASS tiny_sentinel_bruteforce=9 corruption,truncation,extent,sample,gamma,Huffman\n");return 0;}
int main(int argc,char** argv){try{
 std::string corpus,sap,remapp,stream,container,col;bool wantchi=false,decodeCheck=false;U expect=UINT64_MAX,sampleFirst=UINT64_MAX;
 for(int i=1;i<argc;++i){std::string a=argv[i];auto value=[&]{require(i+1<argc,"missing argument");return std::string(argv[++i]);};
 if(a=="--selftest")return selftest();else if(a=="--corpus")corpus=value();else if(a=="--sa")sap=value();else if(a=="--sample-first-rank")sampleFirst=std::stoull(value());else if(a=="--remap")remapp=value();else if(a=="--stream")stream=value();else if(a=="--container")container=value();else if(a=="--decode-check")decodeCheck=true;else if(a=="--chi")wantchi=true;else if(a=="--chi-column"){col=value();wantchi=true;}else if(a=="--chi-expect"){expect=std::stoull(value());wantchi=true;}else throw std::runtime_error("unknown argument");}
 require(!corpus.empty() && !sap.empty() && !remapp.empty(),"need --corpus --sa --remap");require(!stream.empty() || !container.empty(),"need --stream or --container");
 Map t(corpus),sa(sap),remap(remapp);require(remap.size==256 && sa.size>=48,"input sizes");
 require(remap.p[0xF5]==0,"remap must map the end-of-text byte 0xf5 to symbol 0");
 const U textN=t.size+1;   // T' = corpus . 0xF5
 U rows=le(sa.p+24);bool sampled=sampleFirst!=UINT64_MAX;
 require(le(sa.p+8)==textN && sa.size==48+8*rows,"SA header n/rows");
 if(sampled){require(rows>0 && sampleFirst<=textN && rows<=textN-sampleFirst,"sample bounds");require(stream.empty() && !wantchi,"sample mode supports container BWT only");}
 else require(rows==textN,"full SA required without explicit sample mode");bool pass=true;
 if(!stream.empty()){Map dump(stream);require(dump.size%40==0,"truncated stream record");Compare check{t.p,sa.p+48,remap.p,textN};check.fileN=t.size;U endpointBad=0;for(U off=0;off<dump.size;off+=40){require(le(dump.p+off)==off/40,"stream record index");U start=check.row;
 check.add({le(dump.p+off+32,4),dump.p[off+36]});
 endpointBad += le(dump.p+off+16)!=le(sa.p+48+8*start);
 endpointBad += le(dump.p+off+24)!=le(sa.p+48+8*(check.row-1));}
 pass&=check.finish();pass&=endpointBad==0;
 printf("STREAM_ENDPOINTS runs=%llu mismatches=%llu\n",(unsigned long long)check.runs,(unsigned long long)endpointBad);}
 if(!container.empty()){Map product(container);require(product.size>=64 && !memcmp(product.p,"SXI2",4) && le(product.p+8)==textN,"container header");U count=le(product.p+32,4);require(count<100 && product.size>=64+40*count,"member directory");bool found=false;
 for(U i=0;i<count;++i){const B* d=product.p+64+40*i;if(le(d,4)!=1)continue;require(!found && le(d+4,4)==101,"m1 codec");found=true;U off=le(d+8),size=le(d+16),r=le(d+24);require(off<=product.size && size<=product.size-off,"m1 bounds");M1 decoder(product.p+off,size,r);Compare check{t.p,sa.p+48,remap.p,textN};check.fileN=t.size;if(sampled){check.sampleFirst=sampleFirst;check.sampleCount=rows;}while(decoder.runs)check.add(decoder.next());decoder.finish();pass&=check.finish();}require(found,"missing m1");}
 // ---- G1: DECODE ROUND-TRIP. Reconstruct the text from the container's own BWT
 // rows and the SA (recon[SA[r] ? SA[r]-1 : n-1] = BWT[r]), invert the remap, and
 // compare against the corpus bytes on disc. The appended sentinel must land on
 // the last position and must not appear anywhere else.
 if(decodeCheck){
  require(!container.empty(),"--decode-check needs --container");
  Map product(container);U count=le(product.p+32,4);bool found=false;U reconBad=0,sentinelAt=0;
  std::vector<B> recon(t.size+1,0);std::vector<char> seen(t.size+1,0);
  B inv[256];for(unsigned i=0;i<256;++i)inv[remap.p[i]]=B(i);
  for(U i=0;i<count;++i){const B* d=product.p+64+40*i;if(le(d,4)!=1)continue;found=true;
   U off=le(d+8),size=le(d+16),r=le(d+24);require(off<=product.size && size<=product.size-off,"m1 bounds");
   M1 decoder(product.p+off,size,r);U rank=0;
   while(decoder.runs){Run run=decoder.next();
    for(U k=0;k<run.len;++k,++rank){const U p=le(sa.p+48+8*rank);require(p<textN,"decode SA bounds");
     const U at=p?p-1:textN-1;require(!seen[at],"decode position written twice");seen[at]=1;
     recon[at]=inv[run.c];if(at==textN-1)++sentinelAt;}}
   decoder.finish();rank=0;}
  require(found,"missing m1 for --decode-check");
  for(U i=0;i<t.size;++i)if(recon[i]!=t.p[i])++reconBad;
  const U corpusF5=sentinelAt;const U tailOk=(recon[textN-1]==0xF5)?1u:0u;
  printf("DECODE_CHECK corpus_bytes=%llu reconstructed_bytes=%llu mismatches=%llu sentinel_rows_at_last_position=%llu sentinel_byte_last=%llu\n",
         (unsigned long long)t.size,(unsigned long long)(textN-1),(unsigned long long)reconBad,
         (unsigned long long)corpusF5,(unsigned long long)tailOk);
  require(reconBad==0 && sentinelAt==1 && tailOk==1,"decode round-trip mismatch");
 }
 if(wantchi){U got=chi(t,sa,remap.p,col);if(expect!=UINT64_MAX)require(got==expect,"independent chi mismatch");}
 return pass?0:3;
 }catch(const std::exception& e){fprintf(stderr,"stream_vs_bwt: %s\n",e.what());return 2;}}
