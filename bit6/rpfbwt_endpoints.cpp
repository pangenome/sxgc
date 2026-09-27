// O(r), bounded-buffer adapter: PFP padding -> existing endpoint-input ri4.
// No SA/LF/text walk. .ssa/.ssa_t remain unmodified producer evidence.
#include "sxi_format.hpp"
#include <functional>
using namespace sxi;
struct Run { U c,len,h,t; };
struct Source {
 std::string prefix; U n,r;
 Source(const std::string& p):prefix(p) {
  std::ifstream meta(p+".rlebwt.meta",std::ios::binary);check(bool(meta),"open RLE metadata");n=integer(meta);r=integer(meta);
  check(n>10&&r,"empty RLE");
  for(auto ext:{".ssa",".ssa_t"}){std::ifstream f(p+ext,std::ios::binary);check(bool(f)&&size(f)==8+8*r&&integer(f)==r,"endpoint count/size mismatch");}
 }
 void scan(const std::function<void(Run)>& emit) {
  std::ifstream b(prefix+".rlebwt",std::ios::binary),h(prefix+".ssa",std::ios::binary),t(prefix+".ssa_t",std::ios::binary);
  integer(h);integer(t);U rows=0,skip=10;Run pending{};bool have=false;
  for(U i=0;i<r;i++) {
   Run v{};U word;do{word=integer(b,4);if(v.len)check(v.c==(word&255),"RLE continuation character");v.c=word&255;v.len+=(word>>8)&0x7fffff;}while(word>>31);
   v.h=integer(h);v.t=integer(t);check(v.len&&v.len<=n-rows,"RLE length");rows+=v.len;
   if(skip){check(v.len<=skip&&(v.c==2||v.c==10),"unsupported PFP padding boundary");skip-=v.len;continue;}
   check(v.h<n-10&&v.t<n-10,"endpoint outside normalized text");
   if(v.c==2)v.c=10;
   check(v.c>=6&&v.c<128,"unsupported text alphabet");
   if(have&&pending.c==v.c){pending.len+=v.len;pending.t=v.t;}
   else {if(have)emit(pending);pending=v;have=true;}
  }
  check(rows==n&&!skip&&b.peek()==EOF,"RLE metadata/trailing bytes mismatch");if(have)emit(pending);
 }
};
void number(std::ostream& f,U x,unsigned bytes=8){unsigned char b[8];put(b,x,bytes);f.write((char*)b,bytes);check(bool(f),"write endpoint adapter");}
int main(int argc,char**argv){try{
 check(argc==4,"usage: rpfbwt_endpoints PREFIX OUT.ri4 OUT.head_sa");Source s(argv[1]);U r=0,k=0,n=0;std::array<U,256> totals{};
 s.scan([&](Run v){check(v.len<=UINT32_MAX,"run too long");r++;n+=v.len;totals[v.c]+=v.len;if(v.c==10)k+=v.len;});
 check(n==s.n-10&&k>0,"normalized length/terminators");
 std::ofstream out(argv[2],std::ios::binary),heads(argv[3],std::ios::binary);check(bool(out)&&bool(heads),"open outputs");
 number(out,0x0000000452585349ULL);number(out,n);number(out,k);number(out,r);U sum=0;for(U v:totals){number(out,sum);sum+=v;}
 s.scan([&](Run v){number(out,v.c,1);number(heads,v.h);});
 s.scan([&](Run v){number(out,v.len,4);});
 unsigned w=1;while(w<64&&(U(1)<<w)<=n-1)w++;number(out,r*w);number(out,w,1);
 __uint128_t reservoir=0;unsigned available=0;
 s.scan([&](Run v){reservoir|=__uint128_t(n-1-v.t)<<available;available+=w;if(available>=64){number(out,U(reservoir));reservoir>>=64;available-=64;}});
 if(available)number(out,U(reservoir));
 out.flush();heads.flush();check(bool(out)&&bool(heads),"flush outputs");
 fprintf(stderr,"ENDPOINT_PASS raw_n=%llu raw_r=%llu n=%llu k=%llu r=%llu padding_rows=10\n",(unsigned long long)s.n,(unsigned long long)s.r,(unsigned long long)n,(unsigned long long)k,(unsigned long long)r);return 0;
}catch(const std::exception&e){fprintf(stderr,"FATAL: %s\n",e.what());return 1;}}
