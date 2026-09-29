#include "byte_remap.hpp"
#include <cassert>
#include <random>
#include <vector>
#include <algorithm>
using vcfbwt::pfp::ByteRemap;
int main(){
 std::mt19937 rng(42);
 for(int trial=0;trial<1000;++trial){
  std::vector<unsigned char> symbols;for(int c=0;c<256;++c)if(c!=30)symbols.push_back(c);
  std::shuffle(symbols.begin(),symbols.end(),rng);symbols.resize(249);symbols.push_back(30);
  ByteRemap m;std::array<int,256> assigned;assigned.fill(-1);std::array<bool,256> used{};
  for(auto c:symbols){auto v=m.map(c);assert(v>=6&&!used[v]);used[v]=true;assigned[c]=v;
   for(auto prev:symbols)if(assigned[prev]>=0)assert(m.map(prev)==assigned[prev]);}
  bool failed=false;for(int c=0;c<256;++c)if(assigned[c]<0){try{m.map(c);}catch(const std::runtime_error&){failed=true;}break;}
  assert(failed&&m.map(30)==30);
 }
 ByteRemap identity;for(int c=6;c<256;++c)assert(identity.map(c)==c);
}
