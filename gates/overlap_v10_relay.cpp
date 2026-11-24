// Linux fixture relay: one position-band credit. Only ctx-tap removes .sa.
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Q = uint64_t;
using U = uint32_t;
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
constexpr size_t BAND = 32768;
struct Row { Q p,h; U b; };
constexpr Row decode(U p,U h,U t,bool wide) {
    if (!wide) return {p,h,t};
    if (!(t >> 31)) throw std::runtime_error("missing row43 tag");
    return {p | (Q((t >> 9) & 2047) << 32), h | (Q((t >> 20) & 2047) << 32), t & 511};
}
static_assert(decode(0x12345678,0x98765432,0x80200300,true).p == 0x112345678ull);
static_assert(decode(0x12345678,0x98765432,0x80200300,true).h == 0x298765432ull);
static void require(bool ok,const char *msg) { if (!ok) throw std::runtime_error(msg); }
static const char *stop_path;
static void checkpoint() {
    if (fs::exists(stop_path)) { std::fputs("STOP_OBSERVED phase=v10-relay\n",stderr); std::exit(75); }
}
// Reads, writes, and credit waits must all observe STOP even with a live but
// stalled peer. Parent supplies the end-to-end deadline and supervises peers.
static size_t transfer(int fd,void *buf,size_t size,bool writing) {
    size_t pos=0;
    while (pos<size) {
        checkpoint();
        char *p=static_cast<char*>(buf)+pos;
        ssize_t k=writing ? write(fd,p,size-pos) : read(fd,p,size-pos);
        if (k>0) { pos+=size_t(k); continue; }
        if (!writing && !k) break;
        if (k<0 && errno==EINTR) continue;
        require(k<0 && (errno==EAGAIN || errno==EWOULDBLOCK),writing?"write failure":"read failure");
        pollfd peer{fd,short(writing?POLLOUT:POLLIN),0};
        require(poll(&peer,1,50)>=0 || errno==EINTR,"poll failure");
    }
    return pos;
}
static void publish(const fs::path &p,const void *data,size_t len) {
    int fd=open(p.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600); require(fd>=0,"exclusive band creation failed");
    if (len) transfer(fd,const_cast<void*>(data),len,true);
    require(close(fd)==0,"close band");
}
int main(int argc,char **argv) try {
    require(argc==6,"relay narrow|row43 CORPUS EXACT_N NEW_BANDS STOP");
    bool wide=std::string_view(argv[1])=="row43";
    require(wide || std::string_view(argv[1])=="narrow","unknown format");
    std::string_view s=argv[3]; Q n=0;
    auto parsed=std::from_chars(s.begin(),s.end(),n);
    require(parsed.ec==std::errc() && parsed.ptr==s.end() && n && n<(Q(1)<<(wide?43:32)),"invalid n");
    require(fs::file_size(argv[2])==n,"corpus extent mismatch");
    struct stat st; require(fstat(0,&st)==0,"stat input");
    if (S_ISREG(st.st_mode)) require(Q(st.st_size)==12*(n+1),"row file extent mismatch");
    fs::path dir=argv[4]; stop_path=argv[5]; checkpoint();
    require(mkdir(dir.c_str(),0700)==0,"bands directory must be new");
    signal(SIGPIPE,SIG_IGN);
    for (int fd:{0,1}) {
        int flags=fcntl(fd,F_GETFL); require(flags>=0 && fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0,"nonblocking fd");
    }
    std::array<U,BAND*3> buf; std::array<Q,BAND> positions;
    Q rank=0,band=0,ends=0,prev=n;
    while (rank<=n) {
        size_t rows=std::min<Q>(BAND,n+1-rank),bytes=rows*12;
        require(transfer(0,buf.data(),bytes,false)==bytes,"short rows");
        for (size_t i=0;i<rows;++i) {
            Row r=decode(buf[3*i],buf[3*i+1],buf[3*i+2],wide);
            require((rank+i==0)==(r.p==n) && r.p<=n && r.h<=n-std::max(prev,r.p) &&
                    (r.p==0?r.b==256:r.b<256),"invalid row extent/END");
            ends+=r.p==0; require(ends<=1,"duplicate END"); prev=positions[i]=r.p;
        }
        if (rank+rows==n+1) {
            char extra; require(ends==1 && transfer(0,&extra,1,false)==0,"missing END or extra rows");
        }
        auto prefix=(dir/("band_"+std::to_string(band))).string();
        publish(prefix+".sa",positions.data(),rows*8); publish(prefix+".opened",nullptr,0);
        transfer(1,buf.data(),bytes,true);
        while (!fs::exists(prefix+".consumed")) { checkpoint(); usleep(2000); }
        require(!fs::exists(prefix+".sa"),"premature credit");
        require(fs::remove(prefix+".opened") && fs::remove(prefix+".consumed"),"missing marker");
        rank+=rows; ++band;
    }
    checkpoint(); auto count=std::to_string(band); publish(dir/"end",count.data(),count.size()); publish(dir/"end.ready",nullptr,0);
    std::fprintf(stderr,"relay rows=%llu bands=%llu credit=1\n",(unsigned long long)rank,(unsigned long long)band);
    return 0;
} catch (const std::exception &e) { std::fprintf(stderr,"relay: %s\n",e.what()); return 70; }
