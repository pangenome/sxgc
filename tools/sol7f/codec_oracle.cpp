#include <random>
#define main cross_merge_program_main
#include "../../bit6/cross_lcp_merge.cpp"
#undef main
int main() {
    std::mt19937_64 rng(7); U cases=0,packed=0,fallback=0;
    for(unsigned width=1;width<=8;++width) for(unsigned trial=0;trial<64;++trial) {
        U count=trial%2 ? 4096 : 1+trial;
        std::vector<char> raw(count*25);
        for(U i=0;i<count;++i) {
            char* b=raw.data()+25*i;b[0]=char(rng());
            U length=trial%4==0 ? UINT64_MAX : (trial%4==1 ? 1 : 1+rng()%100000);
            U mask=width==8 ? UINT64_MAX : (U(1)<<(width*8))-1;
            U head=rng()&mask,tail=length==1 ? head : rng()&mask;
            put_le64(b+1,length);put_le64(b+9,head);put_le64(b+17,tail);
        }
        auto block=spoolEncode(raw,width);
        require(block.size()<=8+raw.size() && block.size()<=spoolSlot,"codec expansion bound");
        bool encoded=uint8_t(block[7])&128;
        packed+=encoded;fallback+=!encoded;
        std::vector<char> restored;spoolDecode(block,count,width,restored);
        require(restored==raw,"codec exact roundtrip");++cases;
    }
    require(packed && fallback,"codec modes exercised");
    std::printf("SOL7F_SEGMENT_CODEC_PASS cases=%llu widths=1..8 records=1..4096 u64_lengths=1 singleton_tail_elision=1 exact_roundtrip=1 packed=%llu raw_fallback=%llu\n",(unsigned long long)cases,(unsigned long long)packed,(unsigned long long)fallback);
}
