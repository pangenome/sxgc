#define main retired_merge_main
#include "../../bit6/cross_lcp_merge.cpp"
#undef main
#include <random>
int main(int argc,char**argv){
    require(argc==2,"oracle SCRATCH");fs::create_directories(argv[1]);
    setenv("CROSS_POOL_SLOTS","65536",1);
    std::vector<std::string> fixtures={std::string("\0",1),std::string("\0\0",2),
        std::string("\x00\x05\x1e\x00\x05",5),std::string("\x1e\x0a\x1e",3),
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "abababababababababababababababababababababababababababababababab",
        "mississippimississippi","aaaaabaaaabaaaabaaaabaaba",std::string("\0\0\0\0\x05\0",6)};
    std::mt19937_64 rng(731);for(U z=0;z<100;++z){std::string t;U n=1+rng()%200;
        for(U j=0;j<n;++j)t+=char(rng()%((z&1)?256:4));fixtures.push_back(t);}
    U cases=0,selection=0;
    for(U f=0;f<fixtures.size();++f){
        const auto& t=fixtures[f];std::string path=std::string(argv[1])+"/oracle-corpus";
        TextSeg seg;seg.path=path;seg.len=t.size();for(U c=0;c<256;++c)seg.remap[c]=uint8_t(c);
        // Nonidentity, separator-preserving map, including legal NUL/05.
        std::swap(seg.remap[1],seg.remap[255]);std::swap(seg.remap[2],seg.remap[253]);
        {fs::remove(path);OutFile file(path);for(unsigned char c:t){uint8_t raw=seg.remap[c];file.raw(&raw,1);}}
        CorpusText text;text.open({seg},true);std::string hs=path+".hs";
        build_hash_sparse(text,2*text.n,1,hs,1);HashSparse hp;hp.open_ro(hs,1,2*text.n+1);::unlink(hs.c_str());
        for(bool dollar:{false,true})for(unsigned N:{3,4,5,6,8})for(U cap:{1,7,32}){
            Budget b;Keys keys;keys.init(&text,&hp,text.n,dollar,&b);RadixWork w;
            auto want=dollar?brute_dollar(t):brute_cyclic(t);std::vector<U> got;
            radix_order(text,keys,N,cap,w,[&](U row,U p){require(row==got.size(),"oracle row continuity");got.push_back(p);});
            if(got!=want){std::fprintf(stderr,"RADIX_ORACLE_FAIL fixture=%lu dollar=%u N=%u cap=%lu\n",f,dollar,N,cap);return 1;}
            require(w.peakPositions<=2*w.cap,"oracle position high water");selection+=w.selectionPasses;++cases;
        }
    }
    std::printf("RADIX_ORACLE_PASS cases=%lu fixtures=%zu N=3,4,5,6,8 modes=cyclic,dollar cap=1,7,32 NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic_giant_bucket=1 nested_repeats=1 nonidentity_remap=1 exact_brute_SA=1 bounded_selection_passes=%lu\n",cases,fixtures.size(),selection);
}
