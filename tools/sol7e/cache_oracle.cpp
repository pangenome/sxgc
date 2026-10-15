// Exercise sol7a's immutable remapped RAM cache with P=16 borrowing readers.
#include <random>
#define main cross_main
#include "../../bit6/cross_lcp_merge.cpp"
#undef main
int main() {
    setenv("CROSS_POOL_SLOTS","65536",1);
    const std::string path="/mnt/nvme2n1/erikg/extcols-scratch/mergperf-100m-snap.txt";
    std::vector<TextSeg> segs;
    for(U z=0;z<3;++z) {
        TextSeg s; s.path=path;s.off=17+z*17000000;s.len=123457+z;
        for(U c=0;c<256;++c)s.remap[c]=uint8_t(c*3+z*17);
        segs.push_back(s);
    }
    CorpusText corpus;corpus.open(segs,true);
    require(bool(corpus.cached),"P16 RAM cache admitted");
    std::vector<uint8_t> want(corpus.n);
    int fd=::open(path.c_str(),O_RDONLY);require(fd>=0,"cache oracle input");
    for(U z=0;z<segs.size();++z) {
        xw(fd,want.data()+corpus.starts[z],segs[z].len,segs[z].off,"cache oracle read");
        for(U j=corpus.starts[z];j<corpus.starts[z]+segs[z].len;++j)want[j]=segs[z].remap[want[j]];
    }
    ::close(fd);
    std::vector<std::thread> readers;
    for(U t=0;t<16;++t)readers.emplace_back([&,t]{
        std::mt19937_64 rng(731+t);
        for(U q=0;q<100000;++q) {
            U len=1+rng()%128,p=rng()%(2*corpus.n-len+1);
            if(q<128)p=corpus.n-q-1;
            uint8_t temporary[128];const uint8_t* borrowed=corpus.span(p,temporary,len);
            // Hold each span while another borrow is outstanding.
            uint8_t other[128];const uint8_t* concurrent=corpus.span((p+131)%corpus.n,other,len);
            for(U j=0;j<len;++j) {
                require(borrowed[j]==want[(p+j)%corpus.n],"P16 borrowed span stability");
                require(concurrent[j]==want[((p+131)%corpus.n+j)%corpus.n],"P16 second span stability");
            }
        }
    });
    for(auto& t:readers)t.join();
    std::printf("CACHE_P16_ORACLE_PASS readers=16 spans=3200000 remaps=3 segments=3 nonzero_offsets=1 wrap=1 retained_borrows=2 immutable_shared_cache=1 decoded_front_budget_P16_k8_bytes=4194304\n");
}
