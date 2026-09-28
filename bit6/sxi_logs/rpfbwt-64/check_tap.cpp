// O(r) structural gate: no text expansion or LF walk.
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

struct Input {
    FILE* f;
    std::array<char, 1<<20> buffer;
    explicit Input(const std::string& name): f(std::fopen(name.c_str(), "rb")) {
        if (!f) throw std::runtime_error("open " + name);
        std::setvbuf(f, buffer.data(), _IOFBF, buffer.size());
    }
    ~Input() { std::fclose(f); }
    uint64_t read(unsigned bytes=8) {
        unsigned char data[8];
        if (std::fread(data, 1, bytes, f) != bytes) throw std::runtime_error("short input");
        uint64_t value=0;
        for (unsigned i=0; i<bytes; ++i) value |= uint64_t(data[i]) << (8*i);
        return value;
    }
    void end() { if (std::fgetc(f) != EOF) throw std::runtime_error("trailing bytes"); }
};
void check(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
int main(int argc, char** argv) { try {
    check(argc==2, "usage: check_tap PREFIX");
    const std::string p=argv[1];
    Input meta(p+".rlebwt.meta"), bwt(p+".rlebwt"), heads(p+".ssa"), tails(p+".ssa_t");
    uint64_t n=meta.read(), r=meta.read(), total=0, singleton=0;
    check(n && r && heads.read()==r && tails.read()==r, "header counts");
    int previous=-1;
    for (uint64_t i=0; i<r; ++i) {
        uint64_t length=0, word=bwt.read(4), symbol=word&255;
        while (true) {
            check((word&255)==symbol, "continuation symbol");
            uint64_t part=(word>>8)&0x7fffff;
            check(part && length<=n && part<=n-length, "continuation length");
            length+=part;
            if (!(word>>31)) break;
            word=bwt.read(4);
        }
        check(int(symbol)!=previous, "unmerged adjacent runs");
        previous=symbol;
        uint64_t head=heads.read(), tail=tails.read();
        check(head<n && tail<n, "sample range");
        check((length==1)==(head==tail), "singleton endpoint identity");
        singleton+=length==1;
        check(total<=n && length<=n-total, "run length total overflow");
        total+=length;
    }
    check(total==n, "run length total");
    bwt.end(); heads.end(); tails.end();
    std::printf("{\"status\":\"PASS\",\"n\":%llu,\"r\":%llu,\"singleton_runs\":%llu}\n",
                (unsigned long long)n,(unsigned long long)r,(unsigned long long)singleton);
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1;
} }
