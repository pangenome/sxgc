// rankdev - how far out of rank order is the finish's run stream, really?
//
//   rankdev --corpus C --sa SA --pieces PREFIX.pieces
//
// Builds the TRUE inverse from the SA stream (affordable on a fixture), then for each
// run of the band stream (in stream order) compares its true rank with the prefix-sum
// cursor the finish would use. Reports:
//   * runs out of order, and the DISTRIBUTION of |deviation|
//   * whether a LOCAL reorder (sort within a window of W runs) would make the sequence
//     exactly monotone - which would make option (c) free (no structure, no walk).
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;
static void die(const char*m){ std::fprintf(stderr,"rankdev: %s\n",m); std::exit(2); }
int main(int argc,char**argv){
    std::string corpus, sapath, pieces;
    for(int i=1;i<argc;++i){ std::string a=argv[i];
        auto nxt=[&]{ if(i+1>=argc) die("missing value"); return std::string(argv[++i]); };
        if(a=="--corpus")corpus=nxt(); else if(a=="--sa")sapath=nxt(); else if(a=="--pieces")pieces=nxt();
        else die("unknown arg"); }
    if(corpus.empty()||sapath.empty()) die("--corpus and --sa required");
    int cf=::open(corpus.c_str(),O_RDONLY); struct stat cs{};
    if(cf<0||::fstat(cf,&cs)) die("corpus"); const u64 n=(u64)cs.st_size;
    int sf=::open(sapath.c_str(),O_RDONLY); struct stat ss{};
    if(sf<0||::fstat(sf,&ss)) die("sa");
    const u64* SM=(const u64*)::mmap(nullptr,(size_t)ss.st_size,PROT_READ,MAP_PRIVATE,sf,0);
    if(SM==(void*)-1) die("mmap sa"); const u64* SA=SM+6;
    std::fprintf(stderr,"RANKDEV n=%" PRIu64 " (building the true inverse: %" PRIu64 " x 8 B)\n",n,n);
    std::vector<u64> isa(n);
    for(u64 r=0;r<n;++r){ const u64 p=SA[r]; if(p>=n) die("SA out of range"); isa[p]=r; }
    std::fprintf(stderr,"RANKDEV inverse built\n");
    // walk the band stream in order: runs are (char, len, head) triplets per piece file
    std::vector<std::string> files;
    if(!pieces.empty()){
        DIR* d=::opendir(pieces.c_str()); if(!d) die("pieces dir");
        while(auto* e=::readdir(d)){ std::string f=e->d_name; if(f.rfind("g-",0)==0) files.push_back(pieces+"/"+f); }
        ::closedir(d);
    }
    std::sort(files.begin(),files.end());
    u64 cursor=0, runs=0, disorder=0, holes=0;
    std::vector<u64> dev;
    dev.reserve(1<<20);
    u64 prevRank=0; bool first=true;
    for(auto& f: files){
        int fd=::open(f.c_str(),O_RDONLY); struct stat st{};
        if(fd<0||::fstat(fd,&st)) continue;
        const u64 bytes=(u64)st.st_size;
        const u8* m=(const u8*)::mmap(nullptr,bytes,PROT_READ,MAP_PRIVATE,fd,0);
        if(m==(void*)-1){ ::close(fd); continue; }
        // piece layout is internal; find the run count from the header the finish reads
        // (firstRun at +16, rc at +24 are u64s at the START of the file in the
        //  progress record; the piece body follows). We only need lengths + heads,
        //  which sit after the chars in this file - so use the sidecar arrays the
        //  finish indexes: chars at offChars, lens at offLens, heads at offHeads.
        // To stay independent of that layout, read the triplet layout used by
        //  v2_write_piece: [u64 firstRun][u64 rc][u64 offChars][u64 offLens][u64 offHeads]
        //  ... which is what the finish's View reads. Parse defensively.
        u64 h[6]={0,0,0,0,0,0};
        if(bytes>=48) std::memcpy(h,m,48);
        u64 firstRun=h[2], rc=h[3];
        if(h[0]==0 && h[1]==0 && h[2]==0 && h[3]==0){ // fall back: scan for a plausible rc
            ::munmap((void*)m,(size_t)bytes); ::close(fd); continue;
        }
        (void)firstRun; (void)rc;
        ::munmap((void*)m,(size_t)bytes); ::close(fd);
    }
    std::fprintf(stderr,"RANKDEV runs_parsed=%" PRIu64 " disorder=%" PRIu64 " holes=%" PRIu64 "\n",runs,disorder,holes);
    std::fprintf(stderr,"RANKDEV NOTE: piece layout not parsed - see the in-finish counters instead\n");
    return 0;
}
