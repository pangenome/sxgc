// Inspect raw piece runs: find the run index of a given head position, and dump
// runs around given indices. usage: piece_probe <pieces_dir> <n> <band_of_interest> ...
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
static void die(const char* m){fprintf(stderr,"FATAL %s\n",m);exit(1);}

int main(int argc,char**argv){
    if(argc<3)die("usage: piece_probe <pieces_dir> <n> [find_head] [around_idx ...]");
    const char* dir=argv[1];
    const u64 n=strtoull(argv[2],nullptr,10);
    u64 findHead=argc>3?strtoull(argv[3],nullptr,10):~0ull;
    // start file: BPS1 magic, n, w, depth, window
    char path[512];
    snprintf(path,sizeof path,"%s/start",dir);
    int fd=open(path,O_RDONLY); if(fd<0)die("open start");
    char magic[4]; u64 fields[4];
    if(read(fd,magic,4)!=4)die("read magic");
    if(read(fd,fields,32)!=32)die("read fields");
    close(fd);
    if(memcmp(magic,"BPS1",4))die("magic");
    u64 w=fields[1];
    unsigned nbands=(unsigned)((n+10+65535)/65536);
    fprintf(stderr,"PIECE_PROBE n=%llu w=%llu depth=%llu window=%llu\n",
            (unsigned long long)n,(unsigned long long)w,
            (unsigned long long)fields[2],(unsigned long long)fields[3]);
    // find which band contains raw run indices of interest: bands are RUN bands of B=65536
    const u64 B=65536;
    u64 runBase=0;
    std::vector<u64> around;
    for(int i=4;i<argc;i++)around.push_back(strtoull(argv[i],nullptr,10));
    u64 foundRun=~0ull,foundBand=0;
    for(unsigned band=0;band<2000;band++){
        snprintf(path,sizeof path,"%s/p-%u-0",dir,band);
        int f=open(path,O_RDONLY);
        if(f<0){fprintf(stderr,"PIECE_PROBE band %u missing (stopping)\n",band);break;}
        char hdr[44];
        if(read(f,hdr,44)!=44)die("read hdr");
        u32 magic2,shard;u32 width;
        u64 bnd,first,rc,zero;
        memcpy(&magic2,hdr,4);memcpy(&bnd,hdr+4,8);memcpy(&shard,hdr+12,4);
        memcpy(&first,hdr+16,8);memcpy(&rc,hdr+24,8);memcpy(&width,hdr+32,4);memcpy(&zero,hdr+36,8);
        if(magic2!=0x31435250||bnd!=band||shard!=0||zero!=0)die("piece header");
        if(first!=runBase)die("firstRun mismatch");
        u64 words=(u64(width)*rc+63)/64;
        struct stat st{};fstat(f,&st);
        if(u64(st.st_size)!=44+5*rc+16*words)die("piece size");
        std::vector<u8> chars(rc);std::vector<u32> lens(rc);
        std::vector<u64> hp(words?words:1),tp(words?words:1);
        if(read(f,chars.data(),rc)!=(ssize_t)rc)die("chars");
        if(read(f,lens.data(),4*rc)!=(ssize_t)(4*rc))die("lens");
        if(words){if(read(f,hp.data(),8*words)!=(ssize_t)(8*words))die("heads");
                  if(read(f,tp.data(),8*words)!=(ssize_t)(8*words))die("tails");}
        close(f);
        auto unpack=[&](const std::vector<u64>&buf,u64 i){
            u64 b=u64(width)*i,wi=b>>6,o=b&63;
            u64 v=buf[wi]>>o;
            if(o+width>64) v|=buf[wi+1]<<(64-o);
            return v&((u64(1)<<width)-1);
        };
        for(u64 i=0;i<rc;i++){
            u64 head=unpack(hp,i),tail=unpack(tp,i);
            u64 idx=runBase+i;
            if(head==findHead){foundRun=idx;foundBand=band;
                fprintf(stderr,"PIECE_PROBE found head=%llu at raw run %llu (band %u i %llu) len=%u char=%u tail=%llu\n",
                        (unsigned long long)findHead,(unsigned long long)idx,band,
                        (unsigned long long)i,lens[i],chars[i],(unsigned long long)tail);}
            for(u64 q:around){
                if(idx>=q&&idx<=q+3)
                    fprintf(stderr,"PIECE_PROBE around %llu: raw_run=%llu c=%u len=%u head=%llu tail=%llu\n",
                            (unsigned long long)q,(unsigned long long)idx,chars[i],lens[i],
                            (unsigned long long)head,(unsigned long long)tail);
            }
        }
        runBase+=rc;
        if(band>2000)break;
    }
    fprintf(stderr,"PIECE_PROBE total_raw_runs=%llu found=%llu\n",
            (unsigned long long)runBase,(unsigned long long)foundRun);
    return 0;
}
