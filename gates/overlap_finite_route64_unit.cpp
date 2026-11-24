// Standalone EF bit/geometry regression, including realistic wide coordinates and malformed streams.
#include "../xsa/pfp/tools/finite_route_bits.hpp"
#include <cassert>
#include <random>
#include <set>
#include <sys/wait.h>
using namespace route64;
int main(){FILE* file=tmpfile();assert(file);int fd=fileno(file);rawview::Stop g{"/dev/null/not-a-file",INT64_MAX};
 // Stop must be a path whose lookup fails with ENOENT (not ENOTDIR).
 char absent[]="/tmp/route64-unit-XXXXXX";int temp=mkstemp(absent);assert(temp>=0 && !close(temp) && !unlink(absent));g.path=absent;
 std::mt19937_64 rng(20261124);U cases=0;
 auto round=[&](U universe,const std::vector<U>& xs){Layout s;s.count=xs.size();s.place(0,universe);assert(s.end()<=INT64_MAX && !ftruncate(fd,s.end()));Encoder enc;
  for(auto x:xs)enc.put(fd,s,universe,x,g);
  enc.finish(fd,s);std::vector<U> actual;decode(fd,s,universe,g,[&](U x){actual.push_back(x);});assert(actual==xs);++cases;};
 for(U u=1;u<=12;++u){for(U subset=0;subset<(U(1)<<u);++subset){std::vector<U> a;for(U i=0;i<u;++i)if(subset&(U(1)<<i))a.push_back(i);round(u,a);}}
 for(U u:{U(127),U(65536),U(UINT32_MAX)+1,U(201802703584),UINT64_MAX}){
  for(unsigned z=0;z<24;++z){std::set<U> xs{0,u-1};for(unsigned j=0;j<z*713;++j)xs.insert(rng()%u);round(u,{xs.begin(),xs.end()});}}
 // In particular cross many high/low refill boundaries, zero-bit low fields, and gaps across l=63.
 round(UINT64_MAX,{UINT64_MAX-1});std::vector<U> dense(50000);for(U i=0;i<dense.size();++i)dense[i]=i;round(dense.size(),dense);
 Pages p;std::vector<std::pair<U,U>> want;std::vector<U> extra{0,62,0,64,1,509,1800,63,0,511,5,0};U size=0;
 for(U x:extra)size+=x+1;
 p.allocate(size);U at=0;for(U i=0;i<extra.size();++i){p.extras(at+1,extra[i],g);for(U j=0;j<=extra[i];++j)want.push_back({i+1,j});at+=extra[i]+1;}p.finish(extra.size(),g);
 for(U i=0;i<size;++i){for(U x:{U(0),U(13),U(31)}){auto got=p.locate(i*PAGE+x);assert(got.first==want[i].first && got.second==PAGE*want[i].second+x);}}
 // A single decoded anchor at a true >u32 rank; another lies deep inside a >u32 stride.
 std::array<U,8> f{};U start=U(1)<<34,token=(start+509)*PAGE+7;auto a=Pages::in_anchor(token,f.data(),{0,start-1});assert(a.first==start+510 && a.second==7);
 f.fill(UINT64_MAX);U distance=(U(1)<<29)+1;auto b=Pages::in_anchor(token,f.data(),{distance-1,start-distance});assert(b.first==start-distance+1 && b.second==(distance+509)*PAGE+7);
 Layout s;s.count=3;s.place(0,211);assert(!ftruncate(fd,s.end()));Encoder e;for(U x:{U(1),U(2),U(159)})e.put(fd,s,211,x,g);e.finish(fd,s);U zero=0;transfer(fd,true,s.high_at,&zero,8);
 auto pid=fork();assert(pid>=0);if(!pid){decode(fd,s,211,g,[](U){});_exit(0);}int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==75);
 assert(!fclose(file));std::printf("PASS %llu EF posting permutations, malformed bitmap refusal and %llu paged rank/offset seams\n",(unsigned long long)cases,(unsigned long long)size);
}
