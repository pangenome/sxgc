// Standalone durability/fault-injection tests; no external codec dependencies.
#include "finish_spool.h"
#include <sys/wait.h>
#include <iostream>
using namespace finish_spool;
int main(int argc,char** argv) {
    require(argc==2,"usage: finish_spool_test NEW_DIRECTORY");
    std::string dir=argv[1];require(!std::filesystem::exists(dir),"test directory must be new");
    Lock lock(dir);
    bool refused=false;try{Lock other(dir);}catch(const std::exception&){refused=true;}
    require(refused,"second writer accepted");
    std::vector<uint8_t> original{0,1,2,128,255},got;
    Meta meta{7,9,100,120,20,3},read;
    publish(dir+"/input",original,meta);
    require(load(dir+"/input",got,&read)&&got==original&&read.block==9&&read.rows==20,"round trip");
    require(!load(dir+"/absent",got),"absent spool accepted");
    publish(dir+"/empty",{});require(load(dir+"/empty",got)&&got.empty(),"empty base round trip");
    int fd=::open((dir+"/input.part").c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);
    require(fd>=0,"create torn part");require(::write(fd,"torn",4)==4,"write torn part");::close(fd);
    require(load(dir+"/input",got)&&got==original,"torn part changed published input");
    for(uint64_t off:{uint64_t(8),uint64_t(HEADER)}){
        fd=::open((dir+"/input").c_str(),O_RDWR);uint8_t byte=99;io(fd,&byte,1,true,off);::close(fd);
        refused=false;try{load(dir+"/input",got);}catch(const std::exception&){refused=true;}
        require(refused,"corrupt spool accepted");publish(dir+"/input",original,meta);
    }
    pid_t child=::fork();require(child>=0,"fork fault injector");
    if(!child){::setenv("XSA_KILL_AT","after_fsync_before_unlink",1);
        publish(dir+"/next-base",original,meta);before_unlink();::unlink((dir+"/input").c_str());::_exit(1);}
    int status=0;require(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"fault hook exit");
    require(load(dir+"/next-base",got)&&got==original,"consuming state not durable at kill");
    require(load(dir+"/input",got)&&got==original,"input unlinked before consuming commit");
    require(::unlink((dir+"/input").c_str())==0,"delete committed input");syncdir(dir);
    require(load(dir+"/next-base",got)&&got==original,"committed state lost after unlink");
    Archive a;uint64_t u=0xfedcba9876543210ULL;int64_t v=-17;bool b=true;
    std::vector<uint16_t> entries{0,65535,256};a.scalar(u);a.scalar(v);a.scalar(b);a.vector(entries);
    Archive z(a.data);uint64_t x=0;int64_t y=0;bool c=false;std::vector<uint16_t> restored;
    z.scalar(x);z.scalar(y);z.scalar(c);z.vector(restored);z.end();require(x==u&&y==v&&c&&entries==restored,"state integers");
    std::cout<<"FINISH_SPOOL_TEST_PASS corruption=2 torn_part=1 writer_lock=1 kill_before_unlink=1 state_roundtrip=1\n";
}
