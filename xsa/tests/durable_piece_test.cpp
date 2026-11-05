#include "../../bit6/durable_piece.h"
extern "C" int __real_fsync(int);
extern "C" int __real_fdatasync(int);
extern "C" int __wrap_fsync(int fd) {
    static unsigned count=0; ++count;
    const char* fail=::getenv("FAIL_FSYNC");
    if(fail && count==std::strtoul(fail,nullptr,10)){errno=EIO;return -1;}
    return __real_fsync(fd);
}
extern "C" int __wrap_fdatasync(int fd) {
    if(::getenv("FAIL_FDATASYNC")){errno=EIO;return -1;}
    return __real_fdatasync(fd);
}
int main(int argc,char**argv) {
    if(argc!=2)return 3;
    std::string dir=argv[1],tmp=dir+"/p-0-0.partial",fin=dir+"/p-0-0",journal=dir+"/checkpoint.journal";
    int j=::open(journal.c_str(),O_CREAT|O_WRONLY,0600);
    durable_piece::write_all(j,"PLAN test=1\n");::close(j);
    ::setenv("XSA_CHECKPOINT_JOURNAL",journal.c_str(),1);
    int fd=::open(tmp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
    std::string payload="independent-known-piece\n";
    if(const char* size=::getenv("PAYLOAD_BYTES")) {
        payload.resize(std::strtoull(size,nullptr,10));
        for(size_t i=0;i<payload.size();++i)payload[i]=char(i%251);
    }
    durable_piece::write_all(fd,payload);
    durable_piece::publish(fd,tmp,fin,dir,0,0,0,2,17);
}
