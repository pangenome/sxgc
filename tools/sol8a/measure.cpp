// Keys-only measurement: no suffix positions are stored or written.
#define main retired_merge_main
#include "../../bit6/cross_lcp_merge.cpp"
#undef main
#include <cmath>
int main(int argc,char**argv) {
    require(argc==3,"measure CORPUS REMAP");
    TextSeg s;s.path=argv[1];s.len=fs::file_size(s.path);
    std::ifstream map(argv[2],std::ios::binary);map.read((char*)s.remap.data(),256);
    require(bool(map)&&s.remap[0x1e]==0x1e,"production remap");
    CorpusText text;text.open({s},true);U n=text.n;
    std::vector<U> a(n),b(n);
    U key=0;for(U j=0;j<8;++j)key=(key<<8)|(j<n?text.at(j):0);
    for(U p=0;p<n;++p){a[p]=key;key=(key<<8)|(p+8<n?text.at(p+8):0);}
    // Stable LSD byte radix. Key values are never persisted.
    for(unsigned byte=0;byte<8;++byte){
        std::array<U,256> count{},off{};
        for(U k:a)++count[(k>>(byte*8))&255];
        for(U j=1;j<256;++j)off[j]=off[j-1]+count[j-1];
        for(U k:a)b[off[(k>>(byte*8))&255]++]=k;
        a.swap(b);std::fprintf(stderr,"RADIX_MEASURE_PASS byte=%u\n",byte);
    }
    b.clear();b.shrink_to_fit();
    for(unsigned N:{3,4,5,6,8}){
        unsigned shift=64-8*N;U singleton=0,residue=0,buckets=0;long double work=0;
        std::vector<std::pair<U,U>> top; std::array<U,64> multiplicity{};
        std::unordered_map<U,U> terminal;
        for(U p=n-std::min<U>(n,N-1);p<n;++p){U k=0;for(U j=0;j<N;++j)k=(k<<8)|(p+j<n?text.at(p+j):0);++terminal[k];}
        for(U lo=0;lo<n;){U k=a[lo]>>shift,hi=lo+1;while(hi<n&&(a[hi]>>shift)==k)++hi;
            // Finite suffixes are distinct keys even when zero padding collides
            // with legal NUL extensions. Each short suffix is a singleton.
            auto end=terminal.find(k); U ends=end==terminal.end()?0:end->second,size=hi-lo-ends;singleton+=ends;
            multiplicity[0]+=ends; if(size)++multiplicity[63-__builtin_clzll(size)];
            if(size==1)++singleton;
            if(size>1){residue+=size;++buckets;work+=(long double)size*std::log2((long double)size);if(top.size()<10||size>top.back().first){top.push_back({size,k});std::sort(top.begin(),top.end(),std::greater<std::pair<U,U>>());if(top.size()>10)top.pop_back();}}
            lo=hi;
        }
        std::printf("{\"N\":%u,\"suffixes\":%lu,\"singleton_count\":%lu,\"singleton_fraction\":%.12g,\"residue_suffixes\":%lu,\"residue_buckets\":%lu,\"projected_b_log2_b\":%.0Lf,\"prefix_symbol_floor_projection\":%.0Lf,\"largest\":[",N,n,singleton,double(singleton)/n,residue,buckets,work,work*N);
        for(U j=0;j<top.size();++j)std::printf("%s{\"key_hex\":\"%0*lx\",\"count\":%lu}",j?",":"",N*2,top[j].second,top[j].first);
        std::printf("],\"multiplicity_log2_bucket_counts\":[");for(unsigned j=0;j<64;++j)std::printf("%s%lu",j?",":"",multiplicity[j]);std::printf("]}\n");std::fflush(stdout);
    }
}
