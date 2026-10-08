// Padded PFP RLE -> cyclic ri4 and run heads. Certified frames stream;
// other frames use bounded seam classes. Refusals precede output creation.
// Producer .ssa/.ssa_t samples are read-only. See seam_repair.hpp.
#include "sxi_format.hpp"
#include <cstring>
#include <functional>
#include <chrono>
#include <atomic>
#include <memory>
#include <limits>
static constexpr uint64_t INF=UINT64_MAX;
static double tnow(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double G_T0=tnow();
#define SLIM_LCE_CORE_ONLY
#include "slim_lce.hpp"
using namespace sxi;
struct Run { U c,len,h,t; };
struct Source {
 std::string prefix; U n,r,terminal,w1;
 void padding_check(bool ok,const char* site,U row,U c,U len,U head,U tail) const {
  if(ok)return;
  fprintf(stderr,"PFP_PADDING_REFUSED site=%s row=%llu char=0x%02llx len=%llu head=%llu tail=%llu terminal=0x%02llx\n",site,(unsigned long long)row,(unsigned long long)c,(unsigned long long)len,(unsigned long long)head,(unsigned long long)tail,(unsigned long long)terminal);
  throw std::runtime_error("unsupported PFP padding boundary");
 }
 Source(const std::string& p,U last,U window):prefix(p),terminal(last),w1(window) {
  std::ifstream meta(p+".rlebwt.meta",std::ios::binary);check(bool(meta),"open RLE metadata");n=integer(meta);r=integer(meta);
  check(n>w1&&r,"empty RLE");
  for(auto ext:{".ssa",".ssa_t"}){std::ifstream f(p+ext,std::ios::binary);check(bool(f)&&size(f)==8+8*r&&integer(f)==r,"endpoint count/size mismatch");}
  // Row zero is SA=n-w1 (the padding suffix), so its BWT byte is
  // T.back(). Infer it for legacy callers instead of assuming newline.
  std::ifstream b(p+".rlebwt",std::ios::binary),h(p+".ssa",std::ios::binary),t(p+".ssa_t",std::ios::binary);
  U word=integer(b,4);integer(h);integer(t);U head=integer(h),tail=integer(t);
  if(terminal==INF)terminal=word&255;
  padding_check(word==(terminal|U(1)<<8)&&head==n-w1&&tail==n-w1&&terminal>=6&&terminal<256,
                "terminal-row",0,word&255,(word>>8)&0x7fffff,head,tail);
  fprintf(stderr,"PFP_TERMINAL byte=0x%02llx source=%s raw_n=%llu raw_r=%llu\n",(unsigned long long)terminal,last==INF?"padding-row":"argument",(unsigned long long)n,(unsigned long long)r);
 }
 Run trim_padding(Run v,U row) const {
  if(row>=w1)return v;
  padding_check(row==0 ? (v.c==terminal&&v.len==1&&v.h==n-w1&&v.t==n-w1) : v.c==2,
                "prefix",row,v.c,v.len,v.h,v.t);
  U take=std::min(w1-row,v.len);v.len-=take;
  if(v.len) {
   // SA=0 may share the dollar run ending at row w1+1. A dollar run
   // ending exactly at row w1 is equally valid and leaves no row.
   padding_check(v.c==2&&v.len==1&&v.t==0,"straddle",row,v.c,v.len+take,v.h,v.t);
   v.h=0;
  }
  return v;
 }
 bool certify_seam() {
  // After deleting the first w rows, the order is ordinary suffix order of T.
  // Let z be the row for SA=0. Replacing its dollar by T[n-1] preserves the
  // cyclic LF permutation exactly iff rank_{T[n-1]}(z)==0. Otherwise the
  // suffix n-1 must move, potentially followed by Theta(n) other suffixes.
  std::ifstream b(prefix+".rlebwt",std::ios::binary),h(prefix+".ssa",std::ios::binary),t(prefix+".ssa_t",std::ios::binary);
  integer(h);integer(t);U rows=0,before=0,zeroes=0;bool certified=true;
  for(U i=0;i<r;++i) {
   U len=0,c=0,word;
   do {word=integer(b,4);if(len)check(c==(word&255),"RLE continuation character");c=word&255;len+=(word>>8)&0x7fffff;}while(word>>31);
   U head=integer(h),tail=integer(t),rawLen=len;
   check(len&&len<=n-rows,"RLE length");
   Run v=trim_padding({c,len,head,tail},rows);len=v.len;head=v.h;
   if(len) {
    if(c==2) {
     check(len==1&&head==0&&tail==0,"invalid SA-zero dollar row");
     if(before)fprintf(stderr,"CYCLIC_SEAM_REPAIR terminal=0x%02llx predecessors_before_zero=%llu\n",(unsigned long long)terminal,(unsigned long long)before);
     certified=before==0;
     ++zeroes;
    }
    if(c==terminal)before+=len;
   }
   rows+=rawLen;
  }
  check(rows==n&&zeroes==1&&b.peek()==EOF,"invalid padded cyclic frame");
  return certified;
 }
 void scan(const std::function<void(Run)>& emit) {
  std::ifstream b(prefix+".rlebwt",std::ios::binary),h(prefix+".ssa",std::ios::binary),t(prefix+".ssa_t",std::ios::binary);
  integer(h);integer(t);U rows=0;Run pending{};bool have=false;
  for(U i=0;i<r;i++) {
   Run v{};U word;do{word=integer(b,4);if(v.len)check(v.c==(word&255),"RLE continuation character");v.c=word&255;v.len+=(word>>8)&0x7fffff;}while(word>>31);
   v.h=integer(h);v.t=integer(t);check(v.len&&v.len<=n-rows,"RLE length");
   U rawLen=v.len;v=trim_padding(v,rows);rows+=rawLen;if(!v.len)continue;
   check(v.h<n-w1&&v.t<n-w1,"endpoint outside normalized text");
   if(v.c==2)v.c=terminal;
   check(v.c>=6&&v.c<256,"unsupported text alphabet");
   if(have&&pending.c==v.c){pending.len+=v.len;pending.t=v.t;}
   else {if(have)emit(pending);pending=v;have=true;}
  }
  check(rows==n&&b.peek()==EOF,"RLE metadata/trailing bytes mismatch");if(have)emit(pending);
 }
};
#include "seam_repair.hpp"
void number(std::ostream& f,U x,unsigned bytes=8){unsigned char b[8];put(b,x,bytes);f.write((char*)b,bytes);check(bool(f),"write endpoint adapter");}
int main(int argc,char**argv){try{
 std::string pfText,pfCkpt;
 {int j=4;while(j<argc){if(!strcmp(argv[j],"--pf-text")&&j+1<argc){pfText=argv[++j];}else if(!strcmp(argv[j],"--pf-checkpoints")&&j+1<argc){pfCkpt=argv[++j];}else{++j;}}}
 check(argc>=4&&argc<=11,"usage: rpfbwt_endpoints PREFIX OUT.ri4 OUT.head_sa [TERMINAL_HEX] [--w1 N] [--pf-text T.pftext --pf-checkpoints T.pfck (ADOPT the merge-time sidecars for the repair LCE; skips the padded walk)]");
 U terminal=INF;
 U w1=10;int i=4;
 if(i<argc&&std::string(argv[i])!="--w1"&&std::string(argv[i]).rfind("--pf-",0)!=0){std::string arg(argv[i++]);size_t used=0;terminal=std::stoul(arg,&used,16);check(used==arg.size()&&terminal>=6&&terminal<256,"unsupported terminal byte");}
 while(i<argc&&std::string(argv[i]).rfind("--pf-",0)==0)i+=2;
 if(i<argc){check(i+2==argc&&std::string(argv[i])=="--w1","invalid endpoint arguments");w1=std::stoull(argv[i+1]);}
 check(w1>=3&&w1<=512,"--w1 must be 3..512");
 Source s(argv[1],terminal,w1);terminal=s.terminal;bool certified=s.certify_seam();
 check(pfText.empty()==pfCkpt.empty(),"--pf-text and --pf-checkpoints go together");
 SeamRepair repair(s,certified,argv[2],pfText,pfCkpt);
 auto scan=[&](const std::function<void(Run)>& emit){repair.scan(emit);};
 U r=0,k=0,n=0;std::array<U,256> totals{};
 scan([&](Run v){check(v.len<=UINT32_MAX,"run too long");r++;n+=v.len;totals[v.c]+=v.len;if(v.c==10)k+=v.len;});
 // Preserve legacy newline transport semantics for existing raw pilots.
 // Reserved-0x1E collections are ONE string, regardless of record count.
 if(terminal!=10)k=1;
 check(n==s.n-w1&&k>0,"normalized length/terminators");
 std::ofstream out(argv[2],std::ios::binary),heads(argv[3],std::ios::binary);check(bool(out)&&bool(heads),"open outputs");
 number(out,0x0000000452585349ULL);number(out,n);number(out,k);number(out,r);U sum=0;for(U v:totals){number(out,sum);sum+=v;}
 scan([&](Run v){number(out,v.c,1);number(heads,v.h);});
 scan([&](Run v){number(out,v.len,4);});
 unsigned w=1;while(w<64&&(U(1)<<w)<=n-1)w++;number(out,r*w);number(out,w,1);
 __uint128_t reservoir=0;unsigned available=0;
 scan([&](Run v){reservoir|=__uint128_t(n-1-v.t)<<available;available+=w;if(available>=64){number(out,U(reservoir));reservoir>>=64;available-=64;}});
 if(available)number(out,U(reservoir));
 out.flush();heads.flush();check(bool(out)&&bool(heads),"flush outputs");
 fprintf(stderr,"ENDPOINT_PASS raw_n=%llu raw_r=%llu n=%llu k=%llu r=%llu padding_rows=%llu cyclic_seam_certified=%d\n",(unsigned long long)s.n,(unsigned long long)s.r,(unsigned long long)n,(unsigned long long)k,(unsigned long long)r,(unsigned long long)w1,int(certified));return 0;
}catch(const std::exception&e){fprintf(stderr,"FATAL: %s\n",e.what());return 1;}}
