// Differential oracle: M64/materialized DA/copied colex versus selected-width,
// derived DA and in-place dictionary access. Dump values, not allocator layout.
#include <pfp/dictionary.hpp>
#include <random>
#include <set>
#include <cstdio>
template<class T> void exercise(unsigned seed, size_t count, size_t length) {
    std::mt19937 rng(seed);
    std::set<std::vector<T>> words;
    words.insert(std::vector<T>(10,T(2)));
    while(words.size()<count) {
        std::vector<T> v(1+rng()%length);
        for(auto &x:v)x=T(6+rng()%(sizeof(T)==1?250:900));
        words.insert(v);
    }
    std::vector<T> text;
    for(auto &v:words){text.insert(text.end(),v.begin(),v.end());text.push_back(1);}
    text.push_back(0);
    std::less<T> less;
#ifdef SXI_MEMORY_DICTIONARY
    const bool da=false;
#else
    const bool da=true;
#endif
    pfpds::dictionary<T> dict(text,10,less,true,true,da,true,false,true,false);
    for(size_t i=0;i<dict.saD.size();++i) {
        uint64_t row[]={dict.saD[i],dict.isaD[i],dict.phrase_at_sa(i),dict.lcpD[i]};
        if(fwrite(row,sizeof(row),1,stdout)!=1)std::abort();
    }
    for(size_t i=0;i<dict.colex_id.size();++i) {
        uint64_t row[]={dict.colex_id[i],dict.inv_colex_id[i]};
        if(fwrite(row,sizeof(row),1,stdout)!=1)std::abort();
    }
}
int main(){
    spdlog::set_level(spdlog::level::off);
    for(unsigned s=1;s<=12;++s) {
        exercise<uint8_t>(s,100+s*17,10+s*23);
        exercise<uint32_t>(s,100+s*17,10+s*23);
    }
}
