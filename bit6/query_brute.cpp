// Test-only independent direct-text MEM oracle. Rolling hashes only nominate
// candidates; every seed and both maximality conditions are byte verified.
#include "sxi_format.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <unordered_map>
#include <iostream>
using namespace sxi;
struct Seed {size_t read,start;};
int main(int argc,char**argv){try{
    check(argc==5,"usage: query_brute TEXT READS_TSV NAMES MIN");U min=std::stoull(argv[4]);check(min>0,"min");
    int fd=open(argv[1],O_RDONLY);check(fd>=0,"text open");struct stat st{};check(!fstat(fd,&st),"stat");U n=st.st_size;auto t=(const unsigned char*)mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);check(t!=MAP_FAILED,"mmap");
    std::vector<std::string> names,queries,strands;std::vector<U> ends,starts;std::ifstream nf(argv[3]);std::string s;
    while(std::getline(nf,s)){auto a=s.find('\t'),b=s.find('\t',a+1);U fs=std::stoull(s.substr(a+1,b-a-1)),len=std::stoull(s.substr(b+1));starts.push_back(n-1-fs-len);ends.push_back(n-1-fs);}
    check(!ends.empty(),"names");std::ifstream qf(argv[2]);
    while(std::getline(qf,s)){auto a=s.find('\t'),b=s.find('\t',a+1);names.push_back(s.substr(0,a));strands.push_back(s.substr(a+1,b-a-1));queries.push_back(s.substr(b+1));}
    std::unordered_map<U,std::vector<Seed>> seeds;
    for(size_t i=0;i<queries.size();i++)for(size_t j=0;j+min<=queries[i].size();j++){U h=0;for(U z=0;z<min;z++)h=h*131+(unsigned char)queries[i][j+z];seeds[h].push_back({i,j});}
    U pow=1;for(U z=1;z<min;z++)pow*=131;U h=0,hits=0,doc=0;
    for(U p=0;p<n;p++){
        if(p>=min)h-=U(t[p-min])*pow;
        h=h*131+t[p];if(p+1<min)continue;
        U pos=p+1-min;while(doc<ends.size()&&pos>ends[doc])++doc;if(doc==ends.size()||p>=ends[doc])continue;
        auto found=seeds.find(h);if(found==seeds.end())continue;
        for(auto seed:found->second){const auto& q=queries[seed.read];U j=seed.start;
            if(memcmp(t+pos,q.data()+j,min))continue;
            if(j&&pos>starts[doc]&&t[pos-1]==(unsigned char)q[j-1])continue;
            U len=min;while(j+len<q.size()&&pos+len<ends[doc]&&t[pos+len]==(unsigned char)q[j+len])++len;
            // Query file contains reversed-storage oriented sequences. Restore
            // original-record offset and original-read qstart for the oracle.
            U off=ends[doc]-pos-len,qstart=strands[seed.read]=="+"?q.size()-j-len:j;
            std::cout<<names[seed.read]<<'\t'<<doc<<'\t'<<off<<'\t'<<len<<'\t'<<qstart<<'\t'<<strands[seed.read]<<'\n';++hits;
        }
    }
    std::cerr<<"BRUTE_PASS bytes="<<n<<" oriented_reads="<<queries.size()<<" mems="<<hits<<"\n";munmap((void*)t,n);close(fd);
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
