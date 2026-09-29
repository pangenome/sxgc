// Differential oracle: independent whole-dictionary gSACA-K versus disk runs.
#include <pfp/dictionary.hpp>
#include <fstream>
#include <random>
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const std::string prefix=argv[1];
    setenv("SXI_SA_BLOCK_BYTES","4096",1);
    setenv("SXI_SA_RUN_BUFFER_BYTES","64",1);
    setenv("SXI_DICT_CACHE_BYTES","65536",1);
    std::mt19937 rng(731);
    for(unsigned trial=0;trial<4;++trial) {
        std::vector<uint8_t> raw{2};
        for(unsigned p=0;p<500;++p) {
            for(unsigned j=0,n=11+rng()%70;j<n;++j)raw.push_back(6+rng()%(trial==0?2:trial==1?120:250));
            // Equal suffixes across runs exercise generalized-SA tie ordering.
            for(unsigned j=0;j<20;++j)raw.push_back(65);
            raw.push_back(1);
        }
        raw.push_back(0);
        {std::ofstream f(prefix+".dict",std::ios::binary);f.write((char*)raw.data(),raw.size());}
        std::less<uint8_t> cmp;
        pfpds::dictionary<uint8_t> dict(prefix,10,cmp);
        raw.insert(raw.begin(),9,2);
        uint64_t boundary_count=0;
        for(uint64_t i=0;i<raw.size();++i) {
            bool boundary=i==0 || i+1==raw.size() || raw[i-1]==1;
            sxi_external::require(dict.rank_b_d(i)==boundary_count,"boundary rank");
            sxi_external::require(dict.b_d[i]==boundary,"boundary membership");
            if(boundary){++boundary_count;sxi_external::require(dict.select_b_d(boundary_count)==i,"boundary select");}
        }
        std::vector<uint32_t> oracle(raw.size());
        sxi_external::require(sxi_sa32(raw.data(),oracle.data(),raw.size(),256)>=0,"oracle build");
        for(unsigned pass=0;pass<3;++pass)for(uint64_t i=0;i<raw.size();++i) {
            if(dict.saD[i]!=oracle[i]) {fprintf(stderr,"SA mismatch trial=%u i=%llu actual=%llu expected=%u\n",trial,(unsigned long long)i,(unsigned long long)dict.saD[i],oracle[i]);return 1;}
            uint64_t lcp=0;
            if(i)while(raw[oracle[i]+lcp]>1 && raw[oracle[i]+lcp]==raw[oracle[i-1]+lcp])++lcp;
            sxi_external::require(dict.lcpD[i]==lcp,"LCP mismatch");
        }
    }
    // Exercise the clipped on-disk LCP escape. Check selected LCPs directly
    // instead of doing a quadratic full brute scan on this unary fixture.
    {
        setenv("SXI_SA_BLOCK_BYTES","80000",1);
        std::vector<uint8_t> raw{2};
        for(unsigned p=0;p<3;++p){raw.insert(raw.end(),70000,65);raw.push_back(1);}
        raw.push_back(0);
        {std::ofstream f(prefix+".dict",std::ios::binary);f.write((char*)raw.data(),raw.size());}
        std::less<uint8_t> cmp;pfpds::dictionary<uint8_t> dict(prefix,10,cmp);
        raw.insert(raw.begin(),9,2);std::vector<uint32_t> oracle(raw.size());
        sxi_external::require(sxi_sa32(raw.data(),oracle.data(),raw.size(),256)>=0,"long oracle");
        for(uint64_t i=0;i<raw.size();++i) {
            sxi_external::require(dict.saD[i]==oracle[i],"clipped LCP SA");
            if(i && (i%1024==0 || i+1==raw.size())) {
                uint64_t lcp=0;while(raw[oracle[i]+lcp]>1 && raw[oracle[i]+lcp]==raw[oracle[i-1]+lcp])++lcp;
                sxi_external::require(dict.lcpD[i]==lcp,"clipped LCP exact extension");
            }
        }
    }
    fprintf(stderr,"EXTERNAL_DICTIONARY_PASS random_cases=4 passes_each=3 full_sa_lcp=1 long_case_full_sa=1 long_case_sampled_lcp=1 clipped_lcp=1\n");
}
