// Transport and file-growth limits for the single generated v10 product comparison.
// No regular-file row input/output. The last packet stays private until EOF and proof.
#pragma once
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/stat.h>
struct ProductPipe {
    static constexpr Q n=6311445, runs_cap=2*((n+61)/62)+3;
    const fs::path& flag;
    Q runs=0; U prior=~U(0);
    void open() const {
        struct stat s[2]{};
        for(int fd:{0,1}) {
            int fl=fcntl(fd,F_GETFL),cap=fcntl(fd,F_GETPIPE_SZ);
            check(!fstat(fd,&s[fd]) && S_ISFIFO(s[fd].st_mode) && fl>=0 && cap>0 && cap<=512*1024 && !fcntl(fd,F_SETFL,fl|O_NONBLOCK),"bounded product pipe");
        }
        check(s[0].st_dev!=s[1].st_dev || s[0].st_ino!=s[1].st_ino,"product pipe loop");
        signal(SIGPIPE,SIG_IGN);
    }
    size_t move(int fd,void* buf,size_t bytes) const {
        size_t at=0;
        while(at<bytes) {
            stop(flag); char* p=static_cast<char*>(buf)+at;
            auto z=fd?write(fd,p,bytes-at):read(fd,p,bytes-at);
            if(z>0) {at+=size_t(z);continue;}
            if(!fd && !z) break;
            if(z<0 && errno==EINTR) continue;
            check(z<0 && (errno==EAGAIN || errno==EWOULDBLOCK),"product pipe closed/error");
            pollfd f{fd,short(fd?POLLOUT:POLLIN),0}; check(poll(&f,1,50)>=0 || errno==EINTR,"product poll");
        }
        return at;
    }
    void count(const Row& r) {
        check(r.h<255,"generated SXLS escape before exposure");
        runs+=prior!=r.b; prior=r.b; check(runs<=runs_cap,"generated PRC1 run cap before exposure");
    }
};
