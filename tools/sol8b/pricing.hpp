// Measurement-only arrays contain keys and contributor masks, never positions.
// Pile occupancy is a Poisson projection, not an observed contributor count.
static int hybrid_price(int argc, char** argv) {
    require(argc == 5 || argc == 6, "measure --hybrid CORPUS REMAP CHUNK_DIR");
    TextSeg s; s.path=argv[2]; s.len=fs::file_size(s.path);
    std::ifstream map(argv[3],std::ios::binary); map.read((char*)s.remap.data(),256);
    require(bool(map)&&s.remap[0x1e]==0x1e,"production remap");
    CorpusText text; text.open({s},true); const U n=text.n;
    std::vector<U> ends; U total=0;
    for(U j=0;;++j) {
        std::string path=std::string(argv[4])+"/chunk-"+std::to_string(j)+".pos";
        if(!fs::exists(path))break;
        U offset,len,period; read_pos_header(path,offset,len,period);
        require(offset==total,"chunk tiling"); total+=len; ends.push_back(total);
    }
    require(total==n&&!ends.empty()&&ends.size()<=64,"complete tiling, <=64 chunks");
#ifdef SOL8B_LCE
    require(argc==6,"instrumented pricing requires fresh scratch argument");
    require(!fs::exists(std::string(argv[5])+".xsc-0"),"fresh pricing scratch");
    PairScratch sc;sc.init(argv[5],0,ends.size());
    std::vector<SideExt> sides(ends.size());
    for(size_t j=0;j<sides.size();++j)load_side_ext(std::string(argv[4])+"/chunk-"+std::to_string(j)+".crle",96,sc.dir,sides[j]);
    struct Discard : SparseWalkSink {
        U rows=0; U shardCount(U) const override{return 1;}
        void beginShard(U,U,U)override{} void row(U,U)override{++rows;} void endShard()override{}
    } sink;
    Budget measured;CoreStats stats;
    core_merge_kway(sides,true,96,measured,stats,sc,32,false,&sink);
    require(sink.rows==n,"complete pricing walk");
    std::fprintf(stderr,"HYBRID_BASELINE comparisons=%lu symbols=%lu rows=%lu\n",measured.comparisons,measured.symbols,sink.rows);
    sc.cleanup();
#endif
    struct Record { U key, mask; };
    std::vector<Record> a(n),b(n);
    U key=0; for(U j=0;j<8;++j)key=(key<<8)|(j<n?text.at(j):0);
    size_t chunk=0;
    for(U p=0;p<n;++p) {
        while(p>=ends[chunk])++chunk;
        a[p]={key,U(1)<<chunk}; key=(key<<8)|(p+8<n?text.at(p+8):0);
    }
    for(unsigned byte=0;byte<8;++byte) {
        std::array<U,256> count{},off{};
        for(const auto& r:a)++count[(r.key>>(byte*8))&255];
        for(U j=1;j<256;++j)off[j]=off[j-1]+count[j-1];
        for(const auto& r:a)b[off[(r.key>>(byte*8))&255]++]=r;
        a.swap(b); std::fprintf(stderr,"HYBRID_PRICE_PASS byte=%u\n",byte);
    }
    b.clear(); b.shrink_to_fit();
    constexpr long double baseline=60485597061.L, baselineComparisons=3674629435.L;
    const long double globalSymbolMean=baseline/baselineComparisons;
    for(unsigned d:{4,6,8}) {
        const unsigned shift=64-8*d;
        U buckets=0, singleton=0, oneChunkMass=0, fullChunkMass=0,maxMass=0;
        std::array<U,65> contributorBuckets{},contributorMass{};
        long double logMass=0,pileLogMass=0;
        long double model=0,pileModel=0;U missingMass=0,observedMass=0;
        // Keep finite short suffixes distinct from legal NUL extensions.
        std::unordered_map<U,std::vector<size_t>> terminal;
        for(U p=n-std::min<U>(n,d-1);p<n;++p) {
            U k=0;for(U j=0;j<d;++j)k=(k<<8)|(p+j<n?text.at(p+j):0);
            size_t c=std::upper_bound(ends.begin(),ends.end(),p)-ends.begin();
            terminal[k].push_back(c);
        }
        auto account=[&](U mass,U contributors) {
            ++buckets; singleton+=mass==1; maxMass=std::max(maxMass,mass);
            ++contributorBuckets[contributors];contributorMass[contributors]+=mass;
            if(contributors==1)oneChunkMass+=mass;
            if(contributors==ends.size())fullChunkMass+=mass;
            logMass+=mass*std::log2((long double)contributors);
            // Independent 1GB chunks with observed per-1GB key rate mass.
            // E[contributors] approximation; log(E[C]) != E[log C].
            long double c=1310.L*(-std::expm1(-(long double)mass));
            pileLogMass+=mass*std::log2(std::max(1.L,c));
        };
        for(U lo=0;lo<n;) {
            U k=a[lo].key>>shift, hi=lo+1;
            while(hi<n&&(a[hi].key>>shift)==k)++hi;
            std::array<U,64> counts{};
            for(U j=lo;j<hi;++j)++counts[__builtin_ctzll(a[j].mask)];
            U mass=hi-lo;
            auto t=terminal.find(k);
            if(t!=terminal.end())for(size_t c:t->second){require(counts[c]>0,"terminal conservation");--counts[c];--mass;account(1,1);}
            U contributors=0;for(U count:counts)contributors+=count!=0;
            if(mass) {
                account(mass,contributors);
#ifdef SOL8B_LCE
                if(contributors>1) {
                    auto t=sol8bTails[d==4?0:d==6?1:2].find(k);
                    if(t==sol8bTails[d==4?0:d==6?1:2].end())missingMass+=mass;
                    else {
                        observedMass+=mass;
                        long double tail=(long double)t->second.symbols/t->second.comparisons;
                        model+=mass*std::log2((long double)contributors)*tail;
                        long double c=1310.L*(-std::expm1(-(long double)mass));
                        pileModel+=mass*std::log2(c)*tail;
                    }
                }
#endif
            }
            lo=hi;
        }
        U checkedMass=0,checkedBuckets=0;
        for(U i=1;i<=ends.size();++i){checkedMass+=contributorMass[i];checkedBuckets+=contributorBuckets[i];}
        require(checkedMass==n&&checkedBuckets==buckets,"bucket conservation");
        std::printf("{\"d\":%u,\"n\":%lu,\"chunks\":%zu,\"buckets\":%lu,\"singleton_buckets\":%lu,\"one_chunk_mass\":%lu,\"all_chunks_mass\":%lu,\"max_mass\":%lu,\"mass_log2_contributors\":%.9Lf,\"weighted_log2_contributors\":%.9Lf,\"pile_poisson_weighted_log2_expected_contributors\":%.9Lf,\"global_symbol_mean_proxy\":%.9Lf,\"proxy_symbols_k8\":%.0Lf,\"proxy_symbols_pile_per_1gb\":%.0Lf,\"breakeven_weighted_tail_k8\":%s,\"prefix_savings_proxy_k8\":%.0Lf,\"independent_binary_searches_k8\":%lu,\"independent_binary_searches_pile\":%lu,\"contributor_buckets\":[",d,n,ends.size(),buckets,singleton,oneChunkMass,fullChunkMass,maxMass,logMass,logMass/n,pileLogMass/n,globalSymbolMean,logMass*std::max(0.L,globalSymbolMean-d),pileLogMass*std::max(0.L,globalSymbolMean-d),(logMass?std::to_string((double)(baseline/logMass)).c_str():"null"),logMass*d,buckets*ends.size(),buckets*1310);
        for(U i=0;i<=ends.size();++i)std::printf("%s%lu",i?",":"",contributorBuckets[i]);
        std::printf("],\"contributor_mass\":[");for(U i=0;i<=ends.size();++i)std::printf("%s%lu",i?",":"",contributorMass[i]);
        std::printf("]");
#ifdef SOL8B_LCE
        std::printf(",\"baseline_conditional_model_symbols\":%.0Lf,\"baseline_conditional_model_pile_per_1gb\":%.0Lf,\"observed_merge_mass\":%lu,\"unobserved_merge_mass\":%lu",model,pileModel,observedMass,missingMass);
#endif
        std::printf("}\n");std::fflush(stdout);
    }
    return 0;
}
