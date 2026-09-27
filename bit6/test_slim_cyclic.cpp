// Test-only direct text oracle for cyclic LCE, including both seam crossings.
// Build with tools/build_slim_dump.sh OUT bit6/test_slim_cyclic.cpp.
#define main slim_program_main
#include "chi_rspace_dump.cpp"
#undef main
int main(int argc,char** argv) {
 if(argc!=3)return 2;
 std::ifstream f(argv[2],std::ios::binary);std::string text((std::istreambuf_iterator<char>(f)),{});
 SlimLCE lce(argv[1],100,0,0,true,false,true);
 if(lce.n!=text.size()+10||lce.stringEnds.size()!=1)return 3;
 uint64_t checked=0,n=text.size();
 for(uint64_t i=0;i<n;++i)for(uint64_t j=0;j<n;++j) {
  if(i%31&&j%29&&i+32<n&&j+32<n)continue;
  uint64_t want=0;while(want<n&&text[(i+want)%n]==text[(j+want)%n])++want;
  uint64_t got=lce.collection_lce(i,j);
  if(got!=want){fprintf(stderr,"FAIL i=%llu j=%llu got=%llu want=%llu\n",(unsigned long long)i,(unsigned long long)j,(unsigned long long)got,(unsigned long long)want);return 1;}
  ++checked;
 }
 fprintf(stderr,"PASS cyclic_lce checked=%llu n=%llu\n",(unsigned long long)checked,(unsigned long long)n);
}
