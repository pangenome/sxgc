// Dump the first N raw runs of band 0 (chars/lens only) + the last runs of the last band.
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
    if(argc<4)die("usage: piece_head <pieces_dir> <n> <count>");
    const char* dir=argv[1];
    u64 n=strtoull(argv[2],nullptr,10);
    u64 count=strtoull(argv[3],nullptr,10);
    char path[512];
    // start file for w
    snprintf(path,sizeof path,"%s/start",dir);
    int fd=open(path,O_RDONLY); if(fd<0)die("open start");
    char magic[4]; u64 fields[4];
    if(read(fd,magic,4)!=4||read(fd,fields,32)!=32)die("read start");
    close(fd); u32 w=(u32)fields[1];
    // terminal file for finalBand/R
    snprintf(path,sizeof path,"%s/terminal",dir);
    fd=open(path,O_RDONLY);
    u64 finalBand=0,R=0;
    if(fd>=0){
        char tm[4]; u64 tv[4];
        if(read(fd,tm,4)!=4||read(fd,tv,32)!=32)die("read terminal");
        finalBand=tv[0]; R=tv[1]; close(fd);
        fprintf(stderr,"terminal: finalBand=%llu R=%llu\n",(unsigned long long)finalBand,(unsigned long long)R);
    } else fprintf(stderr,"no terminal file\n");
    for(unsigned band=0;band<1||band==finalBand;band+=(band?finalBand:1)){
        snprintf(path,sizeof path,"%s/p-%u-0",dir,band);
        int f=open(path,O_RDONLY); if(f<0){fprintf(stderr,"band %u missing\n",band);break;}
        char hdr[44];
        if(read(f,hdr,44)!=44)die("hdr");
        u32 magic2,shard,width;u64 bnd,first,rc,zero;
        memcpy(&magic2,hdr,4);memcpy(&bnd,hdr+4,8);memcpy(&shard,hdr+12,4);
        memcpy(&first,hdr+16,8);memcpy(&rc,hdr+24,8);memcpy(&width,hdr+32,4);memcpy(&zero,hdr+36,8);
        fprintf(stderr,"band=%u first=%llu rc=%llu w=%u\n",band,(unsigned long long)first,(unsigned long long)rc,width);
        std::vector<u8> chars(rc);std::vector<u32> lens(rc);
        if(read(f,chars.data(),rc)!=(ssize_t)rc)die("chars");
        if(read(f,lens.data(),4*rc)!=(ssize_t)(4*rc))die("lens");
        close(f);
        u64 rows=0;
        u64 lim=(band==0)?count:rc;
        for(u64 i=0;i<rc;i++){
            rows+=lens[i];
            if(band==0&&i<lim)
                fprintf(stderr,"run %llu: c=0x%02x len=%u rows_end=%llu\n",
                        (unsigned long long)i,chars[i],lens[i],(unsigned long long)rows);
            if(band==finalBand&&i>=rc-count)
                fprintf(stderr,"tail run %llu: c=0x%02x len=%u\n",(unsigned long long)i,chars[i],lens[i]);
        }
        fprintf(stderr,"band %u total rows=%llu\n",band,(unsigned long long)rows);
        if(band==finalBand)break;
    }
    return 0;
}
