// A measured semi-external SA workspace probe, not a complete frontend backend.
// mmap delegates paging to the OS; run under an explicit cgroup MemoryMax.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <fstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
extern "C" int sxi32_gsacak(unsigned char*,uint32_t*,int32_t*,uint32_t*,uint32_t);
int main(int argc,char**argv){try {
    if(argc!=3)throw std::runtime_error("usage: sa_disk_probe DICT MEMORY_OR_NEW_DISK_FILE");
    std::ifstream input(argv[1],std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("open dictionary");
    size_t n=input.tellg();if(n>INT32_MAX||!n)throw std::runtime_error("probe requires 32-bit dictionary");
    input.seekg(0);std::vector<unsigned char>d(n);input.read((char*)d.data(),n);
    if(!input||d.back()!=0)throw std::runtime_error("invalid dictionary");
    const bool disk=strcmp(argv[2],"memory")!=0;
    int fd=-1;uint32_t *sa=nullptr;std::vector<uint32_t>v;
    if(disk){
        fd=open(argv[2],O_RDWR|O_CREAT|O_EXCL,0600);if(fd<0)throw std::runtime_error("create unique workspace");
        int err=posix_fallocate(fd,0,4*n);if(err)throw std::runtime_error(strerror(err));
        sa=(uint32_t*)mmap(nullptr,4*n,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(sa==MAP_FAILED)throw std::runtime_error("mmap");
    }else{v.resize(n);sa=v.data();}
    auto start=std::chrono::steady_clock::now();
    if(sxi32_gsacak(d.data(),sa,nullptr,nullptr,n)<0)throw std::runtime_error("gsacak failed");
    uint64_t hash=14695981039346656037ULL;
    for(size_t i=0;i<n;++i){hash^=sa[i];hash*=1099511628211ULL;}
    if(disk&&msync(sa,4*n,MS_SYNC))throw std::runtime_error("msync");
    struct rusage u;getrusage(RUSAGE_SELF,&u);
    printf("mode=%s D=%zu sa_bytes=%zu hash=%016llx wall_s=%.6f peak_kib=%ld major_faults=%ld input_blocks=%ld output_blocks=%ld\n",disk?"disk":"memory",n,4*n,(unsigned long long)hash,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(),u.ru_maxrss,u.ru_majflt,u.ru_inblock,u.ru_oublock);
    if(disk){munmap(sa,4*n);close(fd);unlink(argv[2]);}
    return 0;
}catch(const std::exception&e){fprintf(stderr,"ERROR %s\n",e.what());return 1;}}
