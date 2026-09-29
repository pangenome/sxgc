#include <array>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstdint>
int main(int argc,char** argv) {
 if(argc != 4) return 2;
 std::ifstream f(argv[1],std::ios::binary); if(!f) return 3;
 uint64_t start=std::stoull(argv[2]), left=std::stoull(argv[3]); f.seekg(start);
 std::array<uint64_t,256> count{}; std::vector<unsigned char> buf(8<<20);
 uint64_t total=0; while(left && f) { f.read((char*)buf.data(),std::min<uint64_t>(left,buf.size())); auto n=f.gcount(); for(int64_t i=0;i<n;++i) ++count[buf[i]]; total+=n; left-=n; }
 std::cout<<"{\"path\":\""<<argv[1]<<"\",\"offset\":"<<start<<",\"bytes\":"<<total<<",\"counts\":[";
 for(int i=0;i<256;++i) std::cout<<(i?",":"")<<count[i]; std::cout<<"]}\n";
}
