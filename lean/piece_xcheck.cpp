// Cross-check the raw piece stream against the SA tape: for each probed raw run,
// print its ROW index (prefix sum of lens) and compare head-1 with sa[row-10].
// Also print the runs at given ROW indices. usage:
//   piece_xcheck <pieces_dir> <sa> <n> [row ...] [--find <headpos>]
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
static void die(const char* m){fprintf(stderr,"FATAL %s\n",m);exit(1);}

int main(int argc,char**argv){
    if(argc<4)die("usage: piece_xcheck <pieces_dir> <sa> <n> [row ...] --find <pos>");
    const char* dir=argv[1]; const char* saPath=argv[2];
    const u64 n=strtoull(argv[3],nullptr,10);
    std::vector<u64> rowsWant; u64 findPos=~0ull;
    for(int i=4;i<argc;i++){
        if(!strcmp(argv[i],"--find")&&i+1<argc)findPos=strtoull(argv[++i],nullptr,10);
        else rowsWant.push_back(strtoull(argv[i],nullptr,10));
    }
    int sfd=open(saPath,O_RDONLY); if(sfd<0)die("open sa");
    std::vector<u64> sa(n);
    { size_t got=0; while(got<n*8){ssize_t r=pread(sfd,((char*)sa.data())+got,n*8-got,48+got);
        if(r<=0)die("sa read");got+=size_t(r);} }
    auto isa=[&](u64 p)->u64{ // build lazily? n up to 1e8: full inverse ok
        return 0; };
    char path[512];
    snprintf(path,sizeof path,"%s/start",dir);
    int fd=open(path,O_RDONLY); if(fd<0)die("open start");
    char magic[4]; u64 fields[4];
    if(read(fd,magic,4)!=4||read(fd,fields,32)!=32)die("read start");
    close(fd); if(memcmp(magic,"BPS1",4))die("magic");
    u32 w=(u32)fields[1];
    const u64 B=65536;
    u64 runBase=0,row=0,foundRun=~0ull,foundRow=0,foundBand=0,foundI=0;
    u64 prevRun=0,prevRowBase=0;
    std::sort(rowsWant.begin(),rowsWant.end());
    for(unsigned band=0;;band++){
        snprintf(path,sizeof path,"%s/p-%u-0",dir,band);
        int f=open(path,O_RDONLY);
        if(f<0)break;
        char hdr[44];
        if(read(f,hdr,44)!=44)die("read hdr");
        u32 magic2,shard,width;u64 bnd,first,rc,zero;
        memcpy(&magic2,hdr,4);memcpy(&bnd,hdr+4,8);memcpy(&shard,hdr+12,4);
        memcpy(&first,hdr+16,8);memcpy(&rc,hdr+24,8);memcpy(&width,hdr+32,4);memcpy(&zero,hdr+36,8);
        if(magic2!=0x31435250||bnd!=band||shard!=0||zero!=0||width!=w)die("piece header");
        if(first!=runBase)die("firstRun mismatch");
        u64 words=(u64(w)*rc+63)/64;
        std::vector<u8> chars(rc);std::vector<u32> lens(rc);
        std::vector<u64> hp(words?words:1),tp(words?words:1);
        if(read(f,chars.data(),rc)!=(ssize_t)rc)die("chars");
        if(read(f,lens.data(),4*rc)!=(ssize_t)(4*rc))die("lens");
        if(words){if(read(f,hp.data(),8*words)!=(ssize_t)(8*words))die("heads");
                  if(read(f,tp.data(),8*words)!=(ssize_t)(8*words))die("tails");}
        close(f);
        auto unpack=[&](const std::vector<u64>&buf,u64 i){
            u64 b=u64(w)*i,wi=b>>6,o=b&63;
            u64 v=buf[wi]>>o;
            if(o+w>64) v|=buf[wi+1]<<(64-o);
            return v&((u64(1)<<w)-1);
        };
        for(u64 i=0;i<rc;i++){
            u64 head=unpack(hp,i),tail=unpack(tp,i);
            // rows covered: [row, row+len)
            if(findPos!=~0ull&&head==findPos+1){
                foundRun=runBase+i;foundRow=row;foundBand=band;foundI=i;
                fprintf(stderr,"XCHECK found pos=%llu at raw run %llu row=%llu (band %u i %llu) len=%u c=%u tail=%llu\n",
                    (unsigned long long)findPos,(unsigned long long)foundRun,(unsigned long long)foundRow,
                    band,(unsigned long long)i,lens[i],chars[i],(unsigned long long)(tail-1));
            }
            while(!rowsWant.empty()&&rowsWant.front()<row+lens[i]){
                u64 r=rowsWant.front();rowsWant.erase(rowsWant.begin());
                if(r>=row&&r<row+lens[i]){
                    u64 saAt = (r>=10)? sa[r-10] : ~0ull;
                    fprintf(stderr,"XCHECK row=%llu raw_run=%llu c=%u len=%u head=%llu(pos=%llu) tail=%llu sa[row-10]=%llu match=%d\n",
                        (unsigned long long)r,(unsigned long long)(runBase+i),chars[i],lens[i],
                        (unsigned long long)head,(unsigned long long)(head-1),
                        (unsigned long long)(tail-1),(unsigned long long)saAt,
                        int(head-1==saAt));
                }
            }
            row+=lens[i];
        }
        runBase+=rc;
    }
    fprintf(stderr,"XCHECK total_raw_runs=%llu total_rows=%llu\n",(unsigned long long)runBase,(unsigned long long)row);
    if(findPos!=~0ull){
        u64 trueRank=~0ull;
        for(u64 r=0;r<n;r++)if(sa[r]==findPos){trueRank=r;break;}
        fprintf(stderr,"XCHECK pos=%llu true_rank=%llu found_row=%llu delta_rows=%lld\n",
            (unsigned long long)findPos,(unsigned long long)trueRank,(unsigned long long)foundRow,
            (long long)((int64_t)foundRow-(int64_t)trueRank-10));
    }
    return 0;
}
