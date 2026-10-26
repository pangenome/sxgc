// Derive the emit's symbol stream (M[p-1], position 0 -> 2) and the position
// anchors (ISA[p] for p % k == 0) from an SXSA tape + the corpus + remap.
// usage: sa_derive <sa> <corpus> <remap> <sym.out> <isamp.out> <k>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
static void die(const char* m){fprintf(stderr,"FATAL %s\n",m);exit(1);}
int main(int argc,char**argv){
    if(argc<7)die("usage: sa_derive <sa> <corpus> <remap> <sym.out> <isamp.out> <k>");
    int sfd=open(argv[1],O_RDONLY); if(sfd<0)die("open sa");
    struct stat st{}; if(fstat(sfd,&st))die("stat sa");
    if(st.st_size<48)die("sa size");
    u64 hdr[6];
    if(pread(sfd,hdr,48,0)!=48)die("sa header");
    if((u32)hdr[0]!=0x41535853u)die("sa magic");
    u64 n=hdr[1];
    if((u64)st.st_size!=48+8*n)die("sa size mismatch");
    int cfd=open(argv[2],O_RDONLY); if(cfd<0)die("open corpus");
    struct stat cst{}; if(fstat(cfd,&cst))die("stat corpus");
    if((u64)cst.st_size!=n)die("corpus size");
    u8* T=(u8*)mmap(nullptr,n,PROT_READ,MAP_PRIVATE,cfd,0);
    if(T==MAP_FAILED)die("mmap corpus");
    int rfd=open(argv[3],O_RDONLY); if(rfd<0)die("open remap");
    u8 remap[256];
    if(read(rfd,remap,256)!=256)die("remap read");
    close(rfd);
    u64 k=strtoull(argv[6],nullptr,10);
    if(!k)die("k");
    int ofd=open(argv[4],O_WRONLY|O_CREAT|O_TRUNC,0666); if(ofd<0)die("open sym out");
    u64 entries=(n+k-1)/k;
    int ifd=open(argv[5],O_WRONLY|O_CREAT|O_TRUNC,0666); if(ifd<0)die("open isamp out");
    u64 ih[4]={1363761235ull,1ull,n,k};
    if(write(ofd,&ih,0)<0){} // silence
    if(write(ifd,ih,32)!=32)die("isamp header");
    std::vector<u64> isamp(entries);
    // stream the SA in 16M-entry chunks
    const u64 CH=16u<<20;
    std::vector<u64> buf(CH);
    u64 done=0;
    std::vector<u8> syms(CH);
    while(done<n){
        u64 want=n-done<CH?n-done:CH;
        size_t got=0;
        while(got<want*8){
            ssize_t r=pread(sfd,((char*)buf.data())+got,(want*8)-got,48+done*8+got);
            if(r<=0)die("sa read");
            got+=size_t(r);
        }
        for(u64 i=0;i<want;i++){
            u64 p=buf[i];
            if(p>=n)die("sa position");
            syms[i]=p?remap[T[p-1]]:(u8)2;
            if(p%k==0){
                u64 idx=p/k;
                if(idx>=entries)die("anchor index");
                isamp[idx]=done+i;
            }
        }
        size_t sg=0;
        while(sg<want){ssize_t r=write(ofd,syms.data()+sg,want-sg);if(r<=0)die("sym write");sg+=size_t(r);}
        done+=want;
    }
    size_t ig=0;
    while(ig<entries*8){ssize_t r=write(ifd,((char*)isamp.data())+ig,entries*8-ig);if(r<=0)die("isamp write");ig+=size_t(r);}
    close(ofd);close(ifd);
    fprintf(stderr,"SA_DERIVE_DONE n=%llu sym_bytes=%llu anchors=%llu k=%llu\n",
            (unsigned long long)n,(unsigned long long)n,(unsigned long long)entries,(unsigned long long)k);
    return 0;
}
