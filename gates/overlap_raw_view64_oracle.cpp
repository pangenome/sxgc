// Independently produce the expected aligned fields. Shares ONLY the attestation encoding.
// No producer bitmap, Psi traversal, PLCP carry, head reconstruction or min tree.
#include "../xsa/pfp/tools/view64_proof.hpp"
#include <cerrno>
#include <charconv>
#include <cinttypes>
#include <ctime>
#include <fcntl.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
using U=uint64_t;using Big=__uint128_t;
static const char* flag;static U deadline;
static void check(bool x,const char* why){if(!x){std::fprintf(stderr,"raw view oracle refused: %s\n",why);std::_Exit(75);}}
static void live(){check(std::time(nullptr)<time_t(deadline) && access(flag,F_OK)!=0 && errno==ENOENT,"stop/time");}
static U integer(const char* p){U x=0;std::string_view s(p);auto [e,ec]=std::from_chars(p,p+s.size(),x);check(e==p+s.size() && ec==std::errc{},"unsigned argument");return x;}
struct File{int fd;U len;void* ptr=MAP_FAILED;
 explicit File(const char* p){fd=open(p,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat st{};check(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>0,"regular file");len=st.st_size;}
 void map(){ptr=mmap(nullptr,len,PROT_READ,MAP_PRIVATE,fd,0);check(ptr!=MAP_FAILED,"map input");}
 void drop(){if(ptr!=MAP_FAILED){check(!munmap(ptr,len),"unmap input");ptr=MAP_FAILED;}}~File(){drop();close(fd);}};
static U common(const unsigned char* t,U n,U p,U q){U h=0,end=n-std::max(p,q);while(end-h>=64 && !std::memcmp(t+p+h,t+q+h,64)){h+=64;if(!(h%65536))live();}
 while(h<end && t[p+h]==t[q+h])++h;
 return h;}
static std::string decimal(Big v){std::string s;do{s.push_back(char('0'+v%10));v/=10;}while(v);std::reverse(s.begin(),s.end());return s;}
int main(int ac,char**av){check(ac==7,"oracle RAW CERTIFIED_KEYS.u64 INVERSE.u64 ARRAY_BYTES STOP DEADLINE");flag=av[5];deadline=integer(av[6]);check(deadline<=INT64_MAX,"deadline extent");live();File raw(av[1]),keys(av[2]),inv(av[3]);U n=raw.len,m=inv.len/8;
 check(inv.len%8==0 && keys.len==inv.len && m && m<=n && std::max(Big(n)+8*Big(m),24*Big(m))+(2<<20)<=integer(av[4]),"oracle array grant/extent");
 raw.map();auto text=(const unsigned char*)raw.ptr;std::vector<U> order(m,UINT64_MAX);
 FILE* ranks=fdopen(dup(inv.fd),"rb");check(ranks,"inverse stream");U j=0;
 for(U p=0;p<n;++p){if(!(p%(1<<20)))live();bool hit=!p;
  if(p && n-p>=8){U hash=1469598103934665603ull;for(U k=p;k<p+8;++k){hash^=text[k];hash*=1099511628211ull;}hit=(hash%31==0);}
  if(hit){U r=0;check(std::fread(&r,sizeof r,1,ranks)==1 && r && r<=m && order[r-1]==UINT64_MAX,"invalid file permutation");order[r-1]=p;++j;}
 }
 check(j==m && fgetc(ranks)==EOF && !ferror(ranks) && fclose(ranks)==0,"raw trigger/row count");
 std::array<viewproof::Digest,viewproof::shards> a{},b{},q{};U max_h=0,high_pos=0;Big sum_h=0;
 for(unsigned i=0;i<viewproof::shards;++i){viewproof::Sha ha,hq;
  for(U r=viewproof::boundary(m,i),last=viewproof::boundary(m,i+1);r<last;++r){if(!(r%16384))live();U p=order[r],h=0;
   if(r){U prev=order[r-1];h=common(text,n,p,prev);check(prev+h==n || (p+h<n && text[prev+h]<text[p+h]),"not raw suffix order");}
   ha.raw(p,h,p?text[p-1]:256);max_h=std::max(max_h,h);sum_h+=h;high_pos+=p>UINT32_MAX;
   check(sum_h<=384*Big(n),"raw byte comparison guard");}
  viewproof::queries(m,i,[&](U x,U y){U h=!x||!y?0:x==y?n-order[x-1]:common(text,n,order[x-1],order[y-1]);hq.query(x,y,h);});a[i]=ha.finish();q[i]=hq.finish();
 }
 raw.drop();keys.map();inv.map();auto v=(const U*)inv.ptr,k=(const U*)keys.ptr;
 for(U p=0;p<m;++p){if(!(p%65536))live();U r=v[p];check(r && r<=m,"rank changed");order[r-1]=p;}
 U prevkey=0,prevnext=0,distinct=0,high_keys=0;
 for(unsigned i=0;i<viewproof::shards;++i){viewproof::Sha hb;for(U r=viewproof::boundary(m,i),last=viewproof::boundary(m,i+1);r<last;++r){if(!(r%16384))live();U p=order[r],key=k[p],next=p+1<m?v[p+1]:0;
   check(!r || key>prevkey || (key==prevkey && next>prevnext),"original key/rank recurrence");hb.navigation(key,next);distinct+=!r || key!=prevkey;prevkey=key;prevnext=next;high_keys+=key>UINT32_MAX;
  }b[i]=hb.finish();}
 live();std::printf("RAW_VIEW_ORACLE n=%" PRIu64 " m=%" PRIu64 " k=%" PRIu64 " starts_above_u32=%" PRIu64 " keys_above_u32=%" PRIu64 " max_lcp=%" PRIu64 " sum_lcp=%s raw_root=%s nav_root=%s query_root=%s\n",n,m,distinct,high_pos,high_keys,max_h,decimal(sum_h).c_str(),viewproof::root(a).c_str(),viewproof::root(b).c_str(),viewproof::root(q).c_str());
}
