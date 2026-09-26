// G1 primitive diagnostic ONLY. Positions come from the baseline CRA1, so
// this does NOT validate position resolution or constitute an end-to-end gate.
#define main slim_dumper_main
#include "../bit6/chi_rspace_dump.cpp"
#undef main
int main(int argc,char** argv) {
    if(argc!=8){fprintf(stderr,"usage: probe PREFIX BASELINE_AGG N TAU1 TAU2 SAMPLE_COUNT DICT_STREAM_0_OR_1\n");return 1;}
    G_T0=(int64_t)tnow();
    std::array<std::ifstream,4> f;
    for(auto& s:f)s.open(argv[2],std::ios::binary);
    uint32_t magic;uint64_t r;f[0].read((char*)&magic,4);f[0].read((char*)&r,8);
    if(!f[0]||magic!=0x31415243)slim_fail("probe CRA1 input");
    uint64_t n=strtoull(argv[3],nullptr,10),t1=strtoull(argv[4],nullptr,10),t2=strtoull(argv[5],nullptr,10),samples=strtoull(argv[6],nullptr,10);
    SlimLCE lce(argv[1],r,t1,t2,atoi(argv[7]));
    auto get=[&](int field,uint64_t i){uint64_t v;auto& s=f[field];s.seekg(12+8*(field*r+i));s.read((char*)&v,8);if(!s)slim_fail("probe read");return v;};
    uint64_t checked=0;double start=tnow();
    for(uint64_t i=0;i<r;i+=std::max<uint64_t>(1,r/std::max<uint64_t>(1,samples))){
        uint64_t a=get(1,i),b=get(2,i),want=get(0,i),interior=get(3,i);
        uint64_t prev=i?get(2,i-1):0;
        uint64_t top=i?std::min(lce(a,prev),n-std::max(a,prev)):0;
        uint64_t mid=interior==INF?INF:std::min(lce(a,b),n-std::max(a,b));
        if(top!=want||mid!=interior){fprintf(stderr,"PROBE mismatch run=%llu top=%llu want=%llu interior=%llu want=%llu\n",(unsigned long long)i,(unsigned long long)top,(unsigned long long)want,(unsigned long long)mid,(unsigned long long)interior);return 2;}
        ++checked;
    }
    fprintf(stderr,"SLIM_PRIMITIVE_PROBE PASS runs=%llu query_wall=%.3f (baseline positions; NOT end-to-end)\n",(unsigned long long)checked,tnow()-start);
    lce.ph->report("parse");lce.dh->report("dict");
}
