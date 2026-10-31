#define main xsa_finish_main
#include "../../bit6/xsa_finish.cpp"
#undef main
int main() {
    char dir[] = "/tmp/lcp-release-XXXXXX";
    pf_check(mkdtemp(dir), "test directory");
    auto slice = [&](uint64_t index, uint64_t base, std::vector<uint8_t> data,
                     std::vector<uint64_t> escapes, uint64_t maxLcp) {
        const std::string path = std::string(dir)+"/band_"+std::to_string(index)+".lcp";
        FILE* f = fopen(path.c_str(), "wb"); pf_check(f, "test slice");
        uint32_t header[2]{0x534c5853, 1};
        uint64_t fields[5]{data.size(), escapes.size()/2, maxLcp, base, 1};
        fwrite(header, 1, 8, f); fwrite(fields, 1, 40, f);
        fwrite(data.data(), 1, data.size(), f);
        fwrite(escapes.data(), 8, escapes.size(), f); fclose(f);
        f = fopen((path+".done").c_str(), "w"); pf_check(f, "test marker"); fclose(f);
    };
    slice(0, 0, {0,2,255,4}, {2,300}, 300);
    slice(1, 4, {5,6}, {}, 6);
    LcpColumn c;
    c.openBands(dir, 6, 1);
    LcpColumn::Band b;
    c.load(b, 0, 2);
    pf_check(b.at(0)==0 && b.at(1)==2, "first slice values");
    pf_check(::access((std::string(dir)+"/band_0.lcp").c_str(), F_OK) != 0 &&
             ::access((std::string(dir)+"/band_0.lcp.consumed").c_str(), F_OK) == 0, "early release");
    c.load(b, 1, 5);
    pf_check(b.at(1)==2 && b.at(2)==300 && b.at(3)==4 && b.at(4)==5 && b.at(5)==6, "overlap after unlink");
    c.finishBands();
    for (int i=0; i<2; ++i) {
        const std::string base = std::string(dir)+"/band_"+std::to_string(i)+".lcp";
        unlink((base+".done").c_str()); unlink((base+".consumed").c_str());
    }
    rmdir(dir);
    puts("PASS slice credit at copy time; overlap and escaped LCP survive unlink");
}
