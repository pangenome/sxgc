// Compile with -O2 -std=c++17 -pthread. Exercise the actual standalone planner.
#define main scatter_sort_main
#ifndef SCATTER_SOURCE
#define SCATTER_SOURCE "scatter_sort.cpp"
#endif
#include SCATTER_SOURCE
#undef main
#include <cassert>

static void check(const std::string& text, u64 target, u64 stride,
                  bool want_deep, bool want_analytic, bool want_cap) {
    T=reinterpret_cast<const u8*>(text.data()); FILE_N=text.size(); N=FILE_N+1;
    KEY_N=FILE_N>=24 ? FILE_N-23 : 0; REMAP=nullptr; build_pairmap();
    u64 trailing=FILE_N;
    while (trailing && T[trailing-1]==T[FILE_N-1]) --trailing;
    std::vector<u64> sample;
    for(u64 p=0;p<KEY_N;p+=stride) sample.push_back(p);
    auto plan=plan_tape(sample,stride,target,0,trailing);
    bool deep=false, analytic=false, cap=false;
    std::vector<std::vector<u64>> bands(plan.size());
    for(u64 p=0;p<N;++p) {
        size_t b=0;
        while (b+1<plan.size()) {
            auto f=plan[b+1].first;
            int c=prefix_cmp(p,f.pos,f.depth);
            if(c<0 || (c==0 && f.after)) break;
            ++b;
        }
        bands[b].push_back(p);
    }
    std::vector<u64> actual;
    for(size_t b=0;b<plan.size();++b) {
        auto leaf=plan[b];
        deep |= leaf.first.depth>24; analytic |= leaf.analytic;
        cap |= leaf.first.depth==192 && leaf.samples*stride>target*4/5;
        if(leaf.analytic) {
            for(u64 p:bands[b]) assert(p>=trailing);
            std::reverse(bands[b].begin(),bands[b].end());
        } else std::sort(bands[b].begin(),bands[b].end(),[](u64 a,u64 b){return suf_cmp_from(a,b,0)<0;});
        actual.insert(actual.end(),bands[b].begin(),bands[b].end());
    }
    std::vector<u64> expected;
    for(u64 p=0;p<N;++p) expected.push_back(p);
    std::sort(expected.begin(),expected.end(),[](u64 a,u64 b){return suf_cmp_from(a,b,0)<0;});
    assert(actual==expected);
    assert(deep==want_deep && analytic==want_analytic && cap==want_cap);
    std::printf("PASS rows=%llu stride=%llu deep=%d analytic=%d cap=%d\n",
        (unsigned long long)N,(unsigned long long)stride,deep,analytic,cap);
}
int main() {
    // Shared bytes do not prove terminality. Descend through bytes 25..72.
    std::string text;
    for(unsigned i=0;i<80;++i) text+=std::string(60,'a')+char(33+i)+"z";
    check(text,700,1,true,false,false);
    check(text,700,3,true,false,false);
    // Corpus-proved single symbol to EOF, position DESCENDING, finite tails.
    check(std::string(1800,'a'),2000,1,false,true,false);
    // Internal runs must never be mistaken for the trailing run.
    check(std::string(1000,'a')+"z"+std::string(1000,'a'),1200,1,true,false,true);
    check(std::string(400,'a')+"z"+std::string(400,'a')+"b",300,1,true,false,true);
    check("azaa",100,1,false,false,false);
}
