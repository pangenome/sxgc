#include <pfp/dictionary.hpp>
#include <cstdio>
int main(int argc,char**argv){
    if(argc!=2)return 2;
    std::less<uint8_t> comp;
#ifdef SXI_MEMORY_DICTIONARY
    const bool da=false;
#else
    const bool da=true;
#endif
    pfpds::dictionary<uint8_t> d(argv[1],10,comp,true,true,da,true,false,true,false);
    auto report=[&](const char*stage){
        fprintf(stderr,"LIVE stage=%s D=%zu d_capacity=%zu SA=%zu ISA=%zu DA=%zu LCP=%zu boundaries=%zu rank=%zu select=%zu colex=%zu inv_colex=%zu\n",stage,d.d.size(),d.d.capacity(),sdsl::size_in_bytes(d.saD),sdsl::size_in_bytes(d.isaD),sdsl::size_in_bytes(d.daD),sdsl::size_in_bytes(d.lcpD),sdsl::size_in_bytes(d.b_d),sdsl::size_in_bytes(d.rank_b_d),sdsl::size_in_bytes(d.select_b_d),sdsl::size_in_bytes(d.colex_id),sdsl::size_in_bytes(d.inv_colex_id));
    };
    report("built");
#ifdef SXI_MEMORY_DICTIONARY
    d.isaD=sdsl::int_vector<>();d.isaD_flag=false;
    report("released-ISA");
#endif
}
