// General finite byte strings: exhaustion, embedded zeros and terminal tags, including geometries much wider than small fixtures.
#include "../xsa/pfp/tools/finite_cache_order64.hpp"
#include <random>
using namespace finitecache;
struct Case {std::vector<uint8_t> raw{0};std::vector<Span> keys;
 void add(std::vector<uint8_t> s,bool term){keys.push_back({raw.size(),s.size(),term});raw.insert(raw.end(),s.begin(),s.end());}
 int cmp(U a,U b)const{auto x=keys[a],y=keys[b];U n=std::min(x.len,y.len);int c=std::memcmp(raw.data()+x.p,raw.data()+y.p,n);if(c)return c;return x.len!=y.len?(x.len<y.len?-1:1):int(x.terminal)-int(y.terminal);}
 void run(unsigned p,U skip,std::mt19937_64& rng,Stats& total)const{need(p>=unsigned(std::bit_width(keys.size()-1)) && p<=64,"unit id geometry");U mask=p==64?UINT64_MAX:(U(1)<<p)-1;std::vector<U> a;std::vector<bool> used(keys.size());
  for(U i=0;i<keys.size();++i){auto s=keys[i];U proj=0;for(U j=0;j<8;++j){proj=(proj<<8)+(skip+j<s.len?raw[s.p+skip+j]:0);}a.push_back(i|(proj&~mask));}
  std::shuffle(a.begin(),a.end(),rng);U calls=0,guards=0;Stats z;Sorter order(raw.data(),p,mask,[&](U id){++calls;return keys.at(id);},[&](){++guards;},z);order.run(a.data(),a.size(),skip);
  for(U i=0;i<a.size();++i){U id=a[i]&mask;need(id<keys.size() && !used[id],"lost/spurious id");used[id]=true;if(i){int c=cmp(a[i-1]&mask,id);need(c<=0 && (c || !((a[i]^a[i-1])&~mask)),"wrong order or nonuniform equality filter");}}
  need(guards && calls==z.cache_desc+z.reprobes+2*z.slow_comps && z.reprobes<=2*a.size() && z.cache_desc+z.whole_equal+z.slow_desc<=a.size(),"span probe accounting");total.add(z);}
};
int main(){std::mt19937_64 rng(5291881);Case common,general,shorts,identical,deep;for(U i=0;i<1800;++i){std::vector<uint8_t> s(i%41);for(auto& x:s)x=uint8_t(rng());general.add(s,(i%7)==0);if(i%4)s.insert(s.begin(),10,0);else s.clear();s.insert(s.begin(),5,253);common.add(s,i%3==0);shorts.add(std::vector<uint8_t>(i%13),i%5==0);identical.add(std::vector<uint8_t>{0,0,0,0,0,0},false);}
 std::vector<uint8_t> longword(65851,127);for(U j=0;j<12;++j){auto x=longword;if(j%2)x.back()=j;x.resize(x.size()-j%3);deep.add(std::move(x),j%2);}
 for(U j=12;j<600;++j)deep.keys.push_back(deep.keys[j%12]);
 Stats all;U n=0;for(unsigned bits:{11u,19u,28u,37u,43u,48u,50u,55u,56u,57u,61u,63u,64u}){for(auto [c,k]:std::array<std::pair<Case*,U>,5>{{{&general,0},{&common,5},{&shorts,0},{&identical,0},{&deep,0}}}){c->run(bits,k,rng,all);++n;}}
 need(all.slow_runs && all.whole_equal && all.reprobes && all.cache_runs,"missing branches");std::printf("PASS %llu complete finite orders incl 11..64 payload bits, partial last bytes, exhaustion/terminal/zero extension, huge common prefixes, bounded refills, equality filters and exact load counters\n",(unsigned long long)n);}
