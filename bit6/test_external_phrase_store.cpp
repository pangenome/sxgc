// Isolated exact-dedup/ranking test, including disk-chain rehash and collisions.
#include "external/io.hpp"
#include <array>
#include <cstddef>
#include <mutex>
#include <random>
#include <set>
#include <spdlog/spdlog.h>
namespace vcfbwt {
using char_type=uint8_t;
namespace pfp {
using hash_type=uint64_t;
using size_type=uint32_t;
static bool collide=false;
hash_type string_hash(const char* p,size_t n) {
    if(collide)return 7;
    uint64_t h=14695981039346656037ULL;
    for(size_t i=0;i<n;++i){h^=uint8_t(p[i]);h*=1099511628211ULL;}
    return h;
}
template<class T>class Dictionary;
#include "external/phrase_store.hpp"
}}
int main() {
    using namespace vcfbwt::pfp;
    setenv("SXI_PHRASE_SHARDS","2",1);
    std::mt19937 rng(17);
    std::set<std::vector<uint8_t>> oracle;
    {
        Dictionary<uint8_t> d;
        for(unsigned i=0;i<100000;++i) {
            std::vector<uint8_t> phrase(20+rng()%90);
            for(auto& c:phrase)c=6+rng()%250;
            auto h=d.check_and_add(phrase);
            sxi_external::require(d.check_and_add(phrase)==h,"duplicate hash");
            oracle.insert(phrase);
        }
        sxi_external::require(d.size()==oracle.size(),"dedup count");
        size_t rank=0;
        for(const auto& phrase:oracle) {
            sxi_external::require(d.hash_to_rank(d.get(phrase))==rank+1,"lexical rank");
            sxi_external::require(d.sorted_entry_at(rank)==phrase,"dictionary emission");++rank;
        }
    }
    collide=true;
    {
        Dictionary<uint8_t> d;d.check_and_add({6,7,8});bool refused=false;
        try {d.check_and_add({6,7,9});}catch(const std::runtime_error&){refused=true;}
        sxi_external::require(refused,"collision must fail closed");
    }
    fprintf(stderr,"EXTERNAL_PHRASE_STORE_PASS unique=100000 duplicates=100000 rank_checks=100000 collision_refused=1 rehash=1\n");
}
