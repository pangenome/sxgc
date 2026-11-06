#pragma once
// Finish checkpoint storage. Formats use explicit little-endian fields; no
// process pointers, native struct padding, or mutable mmap is a commit record.
#include "durable_piece.h"
#include <filesystem>
#include <stdexcept>
#include <limits>
#include <map>
#include <array>
#include <type_traits>
#include <sys/file.h>
namespace finish_spool {
inline bool enabled() {
    static const bool on=[] {const char* p=getenv("XSA_FINISH_SPOOL");return p&&std::string(p)=="1";}();
    return on;
}
inline void require(bool b,const char* s) { if(!b) throw std::runtime_error(s); }
inline void io(int fd,void* p,size_t n,bool write,uint64_t off) {
    auto* q=static_cast<unsigned char*>(p);
    while(n) { ssize_t z=write ? ::pwrite(fd,q,n,off) : ::pread(fd,q,n,off);
        if(z<0&&errno==EINTR)continue; require(z>0,"finish spool I/O"); q+=z;n-=z;off+=z; }
}
inline void syncdir(const std::string& path) {
    int fd=::open(path.c_str(),O_RDONLY|O_DIRECTORY); require(fd>=0,"open spool directory");
    int rc=::fsync(fd);::close(fd);require(rc==0,"sync spool directory");
}
struct Archive {
    std::vector<uint8_t> data; size_t at=0; bool reading=false;
    Archive()=default;
    explicit Archive(std::vector<uint8_t> v):data(std::move(v)),reading(true){}
    void bytes(void* p,size_t n) {
        if(!n)return;
        if(reading){require(at<=data.size()&&n<=data.size()-at,"truncated finish state");std::memcpy(p,data.data()+at,n);at+=n;}
        else {auto* q=static_cast<uint8_t*>(p);data.insert(data.end(),q,q+n);}
    }
    template<class T> void scalar(T& v) {
        static_assert(std::is_integral<T>::value,"integers only");
        uint64_t u=uint64_t(v);uint8_t b[8];
        if(!reading)for(unsigned i=0;i<8;++i)b[i]=uint8_t(u>>(8*i));
        bytes(b,8);if(reading){u=0;for(unsigned i=0;i<8;++i)u|=uint64_t(b[i])<<(8*i);v=T(u);}
    }
    template<class T> void vector(std::vector<T>& v) {
        uint64_t n=v.size();scalar(n);
        if(reading){require(n<=(data.size()-at)/8,"state vector size");v.resize(n);}
        for(auto& x:v)scalar(x);
    }
    template<class T,size_t N> void array(T (&v)[N]) {for(auto& x:v)scalar(x);}
    template<class T,size_t N> void array(std::array<T,N>& v) {for(auto& x:v)scalar(x);}
    template<class T> void map(std::map<uint64_t,T>& v) {
        uint64_t n=v.size();scalar(n);if(reading){require(n<=(data.size()-at)/16,"state map size");v.clear();
            for(uint64_t i=0;i<n;++i){uint64_t key=0;scalar(key);T val{};value(val);require(v.emplace(key,std::move(val)).second,"duplicate state map key");}}
        else for(auto& kv:v){uint64_t key=kv.first;scalar(key);value(kv.second);}
    }
    void value(uint64_t& x){scalar(x);} void value(std::vector<uint64_t>& x){vector(x);}
    void end(){require(!reading||at==data.size(),"trailing finish state bytes");}
};
struct Meta { uint64_t piece=0,block=0,lo=0,hi=0,rows=0,runs=0; };
// 160 byte envelope: magic, six u64 metadata fields, size, SHA256 as
// 64 ASCII hex bytes, 32 reserved zero bytes. SHA binds metadata AND payload.
inline constexpr size_t HEADER=160;
inline std::vector<uint8_t> envelope(const std::vector<uint8_t>& payload,Meta m) {
    Archive h;char magic[8]={'F','S','P','O','O','L','0','1'};h.bytes(magic,8);
    h.scalar(m.piece);h.scalar(m.block);h.scalar(m.lo);h.scalar(m.hi);h.scalar(m.rows);h.scalar(m.runs);
    uint64_t n=payload.size();h.scalar(n);durable_piece::Sha256 sha;sha.update(h.data.data(),h.data.size());sha.update(payload.data(),payload.size());
    std::string sum=sha.finish();h.bytes(sum.data(),64);h.data.resize(HEADER,0);return h.data;
}
inline bool load(const std::string& path,std::vector<uint8_t>& payload,Meta* meta=nullptr) {
    int fd=::open(path.c_str(),O_RDONLY);if(fd<0){require(errno==ENOENT,"open finish spool");return false;}
    struct stat st{};if(::fstat(fd,&st)||st.st_size<off_t(HEADER)){::close(fd);throw std::runtime_error("short finish spool");}
    std::vector<uint8_t> hdr(HEADER);io(fd,hdr.data(),hdr.size(),false,0);
    Archive h(hdr);char magic[8];h.bytes(magic,8);require(memcmp(magic,"FSPOOL01",8)==0,"finish spool magic");
    Meta m;h.scalar(m.piece);h.scalar(m.block);h.scalar(m.lo);h.scalar(m.hi);h.scalar(m.rows);h.scalar(m.runs);
    uint64_t n=0;h.scalar(n);require(n==uint64_t(st.st_size)-HEADER,"finish spool length");
    payload.resize(n);io(fd,payload.data(),payload.size(),false,HEADER);::close(fd);
    auto expected=envelope(payload,m);require(expected==hdr,"finish spool checksum or metadata mismatch");if(meta)*meta=m;return true;
}
inline void publish(const std::string& path,const std::vector<uint8_t>& payload,Meta m={}) {
    std::string tmp=path+".part";int fd=::open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);require(fd>=0,"create finish spool part");
    auto hdr=envelope(payload,m);io(fd,hdr.data(),hdr.size(),true,0);io(fd,const_cast<uint8_t*>(payload.data()),payload.size(),true,HEADER);
    int rc=::fdatasync(fd);::close(fd);require(rc==0,"fdatasync finish spool");require(::rename(tmp.c_str(),path.c_str())==0,"rename finish spool");
    syncdir(std::filesystem::path(path).parent_path().string());
}
inline void journal(const std::string& line) {
    const char* path=getenv("XSA_CHECKPOINT_JOURNAL");if(!path)return;
    int fd=::open(path,O_WRONLY|O_APPEND);require(fd>=0,"open checkpoint journal");
    require(::flock(fd,LOCK_EX)==0,"lock checkpoint journal");
    durable_piece::write_all(fd,line+"\n");require(::fsync(fd)==0,"sync checkpoint journal");
    require(::flock(fd,LOCK_UN)==0,"unlock checkpoint journal");require(::close(fd)==0,"close checkpoint journal");
}
inline void before_unlink() {
    const char* p=getenv("XSA_KILL_AT");if(p&&std::string(p)=="after_fsync_before_unlink"){
        std::fprintf(stderr,"XSA_KILL_AT after_fsync_before_unlink\n");std::fflush(nullptr);::_exit(86);}
}
inline void file(Archive& a,int fd,uint64_t length) {
    a.scalar(length);require(length<=SIZE_MAX,"checkpoint file length");
    if(a.reading){require(a.at<=a.data.size()&&length<=a.data.size()-a.at,"checkpoint embedded file length");
        require(::ftruncate(fd,0)==0,"truncate restored spool");io(fd,a.data.data()+a.at,length,true,0);a.at+=length;
        require(::lseek(fd,length,SEEK_SET)>=0,"seek restored spool");}
    else {size_t at=a.data.size();a.data.resize(at+length);io(fd,a.data.data()+at,length,false,0);}
}
struct Lock {
    int fd=-1;explicit Lock(const std::string& dir){std::filesystem::create_directories(dir);syncdir(std::filesystem::path(dir).parent_path().string());
        fd=::open((dir+"/lock").c_str(),O_RDWR|O_CREAT,0600);require(fd>=0&&::flock(fd,LOCK_EX|LOCK_NB)==0,"finish spool already locked");}
    ~Lock(){if(fd>=0)::close(fd);}
};
} // namespace finish_spool
