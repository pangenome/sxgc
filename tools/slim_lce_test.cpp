// Standalone primitive checks; compiles with the dumper's pinned dependencies.
#define main slim_dumper_main
#include "../bit6/chi_rspace_dump.cpp"
#undef main
#include <random>
int main() {
    std::mt19937_64 rng(20260926);
    uint64_t checks=0;
    for(uint64_t len: {1,2,7,31,256,1025}) {
        for(int mode=0;mode<3;++mode) {
            std::vector<uint32_t> seq(len);
            for(uint64_t i=0;i<len;++i)seq[i]=mode==0?7:mode==1?i%7:rng()%23;
            for(uint64_t tau: {1,2,3,7,32,2048}) {
                SlimFingerprint<std::vector<uint32_t>> fp(seq,tau);
                for(int q=0;q<2000;++q) {
                    uint64_t i=rng()%len,j=rng()%len,cap=rng()%(len+1);
                    uint64_t want=0;
                    while(want<cap && i+want<len && j+want<len && seq[i+want]==seq[j+want])++want;
                    if(fp.lce(i,j,cap)!=want)slim_fail("unit differential LCE mismatch");
                    ++checks;
                }
            }
        }
    }
    fprintf(stderr,"SLIM_UNIT PASS %llu exact capped LCE checks (random/periodic/uniform; tau 1..2048)\n",(unsigned long long)checks);
}
