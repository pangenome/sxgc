// O(r), bounded-buffer adapter: certified PFP padding removal -> ri4.
// General padded-to-cyclic normalization is NOT implemented. Reject frames
// whose suffix order needs seam repair before opening either output.
// No SA/LF/text walk. .ssa/.ssa_t remain unmodified producer evidence.
#include "sxi_format.hpp"
#include <functional>
using namespace sxi;
struct Run { U c,len,h,t; };
struct Source {
 std::string prefix; U n,r,terminal;
 Source(const std::string& p,U last):prefix(p),terminal(last) {
  std::ifstream meta(p+".rlebwt.meta",std::ios::binary);check(bool(meta),"open RLE metadata");n=integer(meta);r=integer(meta);
  check(n>10&&r,"empty RLE");
  for(auto ext:{".ssa",".ssa_t"}){std::ifstream f(p+ext,std::ios::binary);check(bool(f)&&size(f)==8+8*r&&integer(f)==r,"endpoint count/size mismatch");}
 }
 void certify_seam() {
  // After deleting the first w rows, the order is ordinary suffix order of T.
  // Let z be the row for SA=0. Replacing its dollar by T[n-1] preserves the
  // cyclic LF permutation exactly iff rank_{T[n-1]}(z)==0. Otherwise the
  // suffix n-1 must move, potentially followed by Theta(n) other suffixes.
  std::ifstream b(prefix+".rlebwt",std::ios::binary),h(prefix+".ssa",std::ios::binary),t(prefix+".ssa_t",std::ios::binary);
  integer(h);integer(t);U rows=0,before=0,zeroes=0;
  for(U i=0;i<r;++i) {
   U len=0,c=0,word;
   do {word=integer(b,4);if(len)check(c==(word&255),"RLE continuation character");c=word&255;len+=(word>>8)&0x7fffff;}while(word>>31);
   U head=integer(h),tail=integer(t),rawLen=len;
   check(len&&len<=n-rows,"RLE length");
   if(rows<10&&rows+len>10) {
    // SA=0 can share the final padding dollar run. Its single remaining
    // row is known exactly, without resolving an interior SA sample.
    check(c==2&&rows+len==11&&tail==0,"unsupported PFP padding boundary");
    len=1;head=0;
   }
   if(rows+rawLen>10) {
    if(c==2) {
     check(len==1&&head==0&&tail==0,"invalid SA-zero dollar row");
     if(before)fprintf(stderr,"CYCLIC_SEAM_BLOCKED terminal=0x%02llx predecessors_before_zero=%llu\n",(unsigned long long)terminal,(unsigned long long)before);
     check(before==0,"cyclic seam repair required: terminal predecessors precede SA=0; padding removal is not cyclic-T order");
     ++zeroes;
    }
    if(c==terminal)before+=len;
   }
   rows+=rawLen;
  }
  check(rows==n&&zeroes==1&&b.peek()==EOF,"invalid padded cyclic frame");
 }
 void scan(const std::function<void(Run)>& emit) {
  std::ifstream b(prefix+".rlebwt",std::ios::binary),h(prefix+".ssa",std::ios::binary),t(prefix+".ssa_t",std::ios::binary);
  integer(h);integer(t);U rows=0,skip=10;Run pending{};bool have=false;
  for(U i=0;i<r;i++) {
   Run v{};U word;do{word=integer(b,4);if(v.len)check(v.c==(word&255),"RLE continuation character");v.c=word&255;v.len+=(word>>8)&0x7fffff;}while(word>>31);
   v.h=integer(h);v.t=integer(t);check(v.len&&v.len<=n-rows,"RLE length");rows+=v.len;
   if(skip){
    check(v.c==2||v.c==terminal,"unsupported PFP padding boundary");
    U take=std::min(skip,v.len);skip-=take;v.len-=take;
    if(!v.len)continue;
    check(v.c==2&&v.len==1&&v.t==0,"unsupported PFP padding boundary");v.h=0;
   }
   check(v.h<n-10&&v.t<n-10,"endpoint outside normalized text");
   if(v.c==2)v.c=terminal;
   check(v.c>=6&&v.c<128,"unsupported text alphabet");
   if(have&&pending.c==v.c){pending.len+=v.len;pending.t=v.t;}
   else {if(have)emit(pending);pending=v;have=true;}
  }
  check(rows==n&&!skip&&b.peek()==EOF,"RLE metadata/trailing bytes mismatch");if(have)emit(pending);
 }
};
void number(std::ostream& f,U x,unsigned bytes=8){unsigned char b[8];put(b,x,bytes);f.write((char*)b,bytes);check(bool(f),"write endpoint adapter");}
int main(int argc,char**argv){try{
 check(argc==4||argc==5,"usage: rpfbwt_endpoints PREFIX OUT.ri4 OUT.head_sa [TERMINAL_HEX]");
 U terminal=10;
 if(argc==5){std::string arg(argv[4]);size_t used=0;terminal=std::stoul(arg,&used,16);check(used==arg.size()&&terminal>=6&&terminal<128,"unsupported terminal byte");}
 Source s(argv[1],terminal);s.certify_seam();U r=0,k=0,n=0;std::array<U,256> totals{};
 s.scan([&](Run v){check(v.len<=UINT32_MAX,"run too long");r++;n+=v.len;totals[v.c]+=v.len;if(v.c==10)k+=v.len;});
 // Preserve legacy newline transport semantics for existing raw pilots.
 // Reserved-0x1E collections are ONE string, regardless of record count.
 if(terminal!=10)k=1;
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
 fprintf(stderr,"ENDPOINT_PASS raw_n=%llu raw_r=%llu n=%llu k=%llu r=%llu padding_rows=10 cyclic_seam_certified=1\n",(unsigned long long)s.n,(unsigned long long)s.r,(unsigned long long)n,(unsigned long long)k,(unsigned long long)r);return 0;
}catch(const std::exception&e){fprintf(stderr,"FATAL: %s\n",e.what());return 1;}}
