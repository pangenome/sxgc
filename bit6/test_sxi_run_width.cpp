// Synthetic header from the real writer, with a run count above 2^32.
#define SXI_WRITE_TEST
#include "sxi_write.cpp"
#include <cassert>
int main(int argc,char**argv) {
    assert(argc==2);
    const U r=(U(1)<<33)+17;
    {
        Output out(argv[1],5);
        for(unsigned id=1;id<=5;++id)out.begin(id,id<=3?r:0);
        out.finish(r,1,r,false,8);
    }
    std::ifstream in(argv[1],std::ios::binary);
    unsigned char b[64];read(in,b,sizeof(b));
    assert(!memcmp(b,"SXI1",4));
    assert(get(b+24)==r);
    assert(get(b+24)!=U(uint32_t(r)));
    bool reached_sizes=false;
    try {sxi::Container parsed(argv[1],false);(void)parsed;}
    catch(const std::runtime_error& e) {reached_sizes=std::string(e.what()).find("run/sample sizes")!=std::string::npos;}
    assert(reached_sizes); // Header parsing accepted high R before sparse-body refusal.
    std::vector<U> ids{(U(1)<<32)-1,(U(1)<<32)+1,r-1,r};
    assert(std::lower_bound(ids.begin(),ids.end(),r)==ids.begin()+3);
    fprintf(stderr,"RUN_WIDTH_HEADER_PASS R=%llu\n",(unsigned long long)r);
}
