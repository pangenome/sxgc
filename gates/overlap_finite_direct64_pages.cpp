// Test full aligned interiors, dense packed fragments and high streams that end BEFORE their reserved interior.
#include "../xsa/pfp/tools/finite_direct_route64.hpp"
#include <cassert>
#include <numeric>
#include <random>
#include <sys/wait.h>
using namespace routedirect;
int main(){char path[]="/tmp/finite-direct-pages-XXXXXX";int fd=mkstemp(path);assert(fd>=0 && !unlink(path));rawview::Stop stop{path,INT64_MAX};std::mt19937_64 rng(965421);unsigned early=0,mixed=0,empty=0;
 for(U rep=0;rep<16;++rep){unsigned t=4,nb=15;std::vector<Layout> s(t*nb);std::vector<Encoder> enc(t*nb);std::vector<std::vector<U>> truth(t*nb);std::vector<U> vs(t*nb);U sequential=0;
  for(unsigned i=0;i<s.size();++i){U c=i%13==0?0:(1000+17*i+rep*31),v=16384*(i+1)+rep;vs[i]=v;auto& a=truth[i];a.resize(c);U stride=i%3==0?1:v/(c?c:1);for(U j=0;j<c;++j)a[j]=j*stride+(stride>1?rng()%stride:0);
   s[i].count=c;s[i].fmt=shape(v,c);sequential+=8*(s[i].fmt.low+s[i].fmt.high);empty+=!c;}
  // A sizeable but highly clustered high stream: only the first pages of its upper map are populated.
  size_t z=s.size()-1;vs[z]=U(1)<<39;auto& a=truth[z];a.resize(80000+rep);std::iota(a.begin(),a.end(),0);sequential-=8*(s[z].fmt.low+s[z].fmt.high);s[z].count=a.size();s[z].fmt=shape(vs[z],a.size());sequential+=8*(s[z].fmt.low+s[z].fmt.high);
  U boundary=0,len=place(s,boundary);assert(len==sequential && !ftruncate(fd,0) && !ftruncate(fd,len));
#pragma omp parallel for num_threads(4) schedule(dynamic)
  for(unsigned i=0;i<s.size();++i){auto& b=s[i];assert(!(b.low_at%4096) && !(b.high_at%4096));for(U x:truth[i])enc[i].put(fd,b,vs[i],x,stop);enc[i].finish(fd,b);}
  Fragments f(fd,boundary,stop);U pop=0,sent=0;for(unsigned i=0;i<s.size();++i){auto& b=s[i];f.append(enc[i].low,b.fmt.low,b.low_tail);f.append(enc[i].high,b.fmt.high,b.high_tail);pop+=8*(b.fmt.low+b.used_high);sent+=8*(enc[i].low.sent+enc[i].high.sent);early+=b.used_high<b.fmt.high/BUF*BUF;mixed+=b.fmt.low>BUF && b.fmt.low%BUF;}
  sent+=f.finish(len);assert(pop<=sent && sent<=len);for(unsigned i=0;i<s.size();++i){U j=0;decode(fd,s[i],vs[i],stop,[&](U x){assert(j<truth[i].size() && truth[i][j++]==x);});assert(j==truth[i].size());}}
 assert(early && mixed && empty && !close(fd));std::printf("PASS 960 independent page/fragment streams; %u early high endings, %u mixed low columns, %u empty slices, no padding amplification in file extent\n",early,mixed,empty);}
