#define main sol8b_retired_main
#include SOL8B_MERGE_SOURCE
#undef main
int main(int argc,char**argv) {
    require(argc==2,"lce_oracle FRESH_SCRATCH");
    require(!fs::exists(argv[1]),"fresh scratch");fs::create_directories(argv[1]);
    U cases=0;
    for(std::string data:{std::string(15,' '),std::string("abcabcabcabcabc"),std::string("\0\5\0\5\0\5\0\5\0\5\0\5\0\5\0",15),std::string("\x1e\x0a\x1e\x1e\x0a\x1e\x1e\x0a\x1e\x1e\x0a\x1e",12)}) {
        auto path=std::string(argv[1])+"/text";
        {std::ofstream out(path,std::ios::binary);out.write(data.data(),data.size());}
        TextSeg seg;seg.path=path;seg.len=data.size();for(U c=0;c<256;++c)seg.remap[c]=c;
        CorpusText text;text.open({seg},true);
        Budget budget;Keys keys;keys.init(&text,nullptr,data.size(),true,&budget);
        for(auto& m:sol8bTails)m.clear();
        std::unordered_map<U,Sol8bTail> expected[3];
        sol8bEnabled.store(true);
        for(U p=0;p<data.size();++p)for(U q=p+1;q<data.size();++q) {
            U cap=std::min(data.size()-p,data.size()-q),lce=0;
            while(lce<cap&&data[p+lce]==data[q+lce])++lce;
            int result=keys.cmp(p,q);
            auto a=data.substr(p),b=data.substr(q);
            require((result<0)==(a<b),"brute comparator sign");
            for(U d:{4u,6u,8u})if(lce>=d) {
                U key=0;for(U j=0;j<d;++j)key=(key<<8)|uint8_t(data[p+j]);
                auto& e=expected[d==4?0:d==6?1:2][key];++e.comparisons;e.symbols+=lce-d+(lce<cap);
            }
        }
        sol8bEnabled.store(false);
        for(U i=0;i<3;++i) {
            require(expected[i].size()==sol8bTails[i].size(),"conditional bucket count");
            for(auto [key,e]:expected[i]) {
                const auto& got=sol8bTails[i].at(key);
                require(got.comparisons==e.comparisons&&got.symbols==e.symbols,"exact conditional counters");
            }
            ++cases;
        }
        ::unlink(path.c_str());
    }
    fs::remove(argv[1]);
    std::printf("HYBRID_CONDITIONAL_LCE_ORACLE_PASS cases=%lu d=4,6,8 NUL=1 byte05=1 wrap_prefix=1 periodic=1 finite_ends=1 counters=brute_exact\n",cases);
}
