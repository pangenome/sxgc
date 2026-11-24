// Exercise the actual pack/unpack and bitmap searches without forging a multi-GB raw input.
#define main front64_cli_entry
#include "../xsa/pfp/tools/front64.cpp"
#undef main
#include <random>
int main(){
 std::mt19937_64 rng(306221);
 for(U n:std::array<U,14>{1,2,63,64,65,511,512,513,U(1)<<31,(U(1)<<32)-1,U(1)<<32,(U(1)<<32)+1,100000000000ull,U(INT64_MAX)}){
  posfront::Project layout(n);
  for(U it=0;it<1024;++it){
   U p=(it%3==0)?n-1:rng()%n,q=rng()%n,a=rng(),b=(a&(~U(0)<<52))|(rng()>>12);
   U x=layout.entry(p,a),y=layout.entry(q,b);ck(layout.pos(x)==p && layout.pos(y)==q,"full position roundtrip");
   if((x^y)&~layout.mask)ck((x<y)==(a<b),"projected-prefix order");
  }
 }
 for(U n:std::array<U,8>{1,63,64,65,511,512,513,65537}){
  posfront::Bits idx(n);std::vector<U> starts;
  for(U p=0;p<n;++p){if(p==0||p==n-1||(p>=60 && p<70)||p%4921==0){starts.push_back(p);idx.bits[p/64]|=U(1)<<(p%64);}}
  for(U b=0;b<=idx.blocks;++b)idx.before[b]=std::lower_bound(starts.begin(),starts.end(),b*512)-starts.begin();
  for(U j=0;j<starts.size();++j){ck(idx.ordinal(starts[j])==j && idx.following(starts[j])==(j+1<starts.size()?starts[j+1]:n),"sparse successor and ordinal");}
 }
 std::puts("PASS real projection at u32+1, 100 GB and off_t ceiling; bitmap exclusive rank and successor across empty stripes");
}
