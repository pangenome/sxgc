// Independent direct-text checks of sampled sweep witnesses. No text SA/BWT.
#include "sxi_format.hpp"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <map>
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>
#include <iostream>
using namespace sxi;
struct Text {
    Remap sigma=identity_remap();
    int fd=-1, request=-1; pid_t child=-1; U bytes=0;
    struct Cache { U start=UINT64_MAX; std::vector<unsigned char> data; };
    mutable std::array<Cache,2> cache;
    mutable unsigned slot=0;
    static void transfer(int fd, void* buffer, size_t length, bool writing) {
        auto* p=static_cast<unsigned char*>(buffer);
        while(length){ssize_t got=writing?write(fd,p,length):read(fd,p,length);
            if(got<0&&errno==EINTR)continue;
            check(got>0,"archive range service broke/short response");p+=got;length-=got;}
    }
    explicit Text(const char* path, const char* archive=nullptr, const char* names=nullptr, const char* tool=nullptr){
        if(!archive){fd=open(path,O_RDONLY);check(fd>=0,"audit text open");struct stat st{};check(!fstat(fd,&st)&&st.st_size>0,"audit text stat");bytes=st.st_size;return;}
        int in[2],out[2];check(!pipe(in)&&!pipe(out),"archive pipes");
        child=fork();check(child>=0,"archive fork");
        if(!child){
            dup2(in[0],STDIN_FILENO);dup2(out[1],STDOUT_FILENO);
            close(in[0]);close(in[1]);close(out[0]);close(out[1]);
            execl(tool,tool,archive,"--serve-ranges",names,"--revlines","--upper","--sep","1e",(char*)nullptr);
            _exit(127);
        }
        close(in[0]);close(out[1]);request=in[1];fd=out[0];
        signal(SIGPIPE,SIG_IGN);
        unsigned char header[8];transfer(fd,header,8,false);
        for(unsigned i=0;i<8;i++)bytes|=U(header[i])<<(8*i);
        check(bytes>0,"empty archive range service");
    }
    void finish(){
        if(request>=0){close(request);request=-1;int status=0;pid_t result;
            do{result=waitpid(child,&status,0);}while(result<0&&errno==EINTR);
            child=-1;check(result>0&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"archive range service failed");}
    }
    ~Text(){if(request>=0)close(request);if(fd>=0)close(fd);if(child>0){kill(child,SIGTERM);waitpid(child,nullptr,0);}}
    void at(U offset,unsigned char* dst,size_t length)const{
        check(offset<=bytes&&length<=bytes-offset,"audit text range");
        auto* begin=dst;size_t total=length;
        while(length){
            if(request<0){ssize_t got=pread(fd,dst,length,offset);if(got<0&&errno==EINTR)continue;check(got>0,"audit text read");offset+=got;dst+=got;length-=got;continue;}
            U base=offset/65536*65536;Cache* found=nullptr;
            for(auto& c:cache)if(c.start==base)found=&c;
            if(!found){found=&cache[slot++%cache.size()];found->start=base;found->data.resize(std::min<U>(65536,bytes-base));
                unsigned char query[16];U len=found->data.size();
                for(unsigned i=0;i<8;i++){query[i]=base>>(8*i);query[i+8]=len>>(8*i);}
                transfer(request,query,16,true);transfer(fd,found->data.data(),len,false);}
            size_t take=std::min<size_t>(length,found->data.size()-(offset-base));
            memcpy(dst,found->data.data()+(offset-base),take);offset+=take;dst+=take;length-=take;
        }
        for(size_t i=0;i<total;++i)begin[i]=sigma[begin[i]];
    }
    unsigned char byte(U offset)const{unsigned char c;at(offset,&c,1);return c;}
};
// Separate bounded streams: no mmap/address-space growth with corpus size.
struct Stream {
    std::vector<char> buffer;std::ifstream in;
    Stream(const char* path,U offset):buffer(1<<20){in.rdbuf()->pubsetbuf(buffer.data(),buffer.size());in.open(path,std::ios::binary);check(bool(in),"audit stream open");in.seekg(offset);}
    U next(unsigned bytes=8){return integer(in,bytes);}
};
int main(int argc,char**argv){try{
    check(argc>=6,"usage: sxi_text_audit TEXT RI4 AGG CHI N [--remap TABLE] [--agc ARCHIVE --names TSV --agc2flat TOOL]");
    std::map<std::string,std::string> opts;
    for(int i=6;i<argc;i+=2){check(i+1<argc,"missing audit option value");std::string key=argv[i];
        check(key=="--remap"||key=="--agc"||key=="--names"||key=="--agc2flat","unknown audit option");
        check(opts.emplace(key,argv[i+1]).second,"duplicate audit option");}
    bool archive=opts.count("--agc");check(archive==(opts.count("--names")!=0)&&archive==(opts.count("--agc2flat")!=0),"incomplete audit archive options");
    U requested=std::stoull(argv[5]);check(requested>0&&requested<=100000,"sample N must be 1..100000");
    Text text(argv[1],archive?opts["--agc"].c_str():nullptr,archive?opts["--names"].c_str():nullptr,archive?opts["--agc2flat"].c_str():nullptr);
    if(opts.count("--remap"))text.sigma=load_remap(opts["--remap"]);
    std::ifstream ri(argv[2],std::ios::binary),agg(argv[3],std::ios::binary);
    check(bool(ri)&&bool(agg),"audit index open");U ribytes=size(ri),aggbytes=size(agg);
    check(ribytes>=2080&&integer(ri)==0x0000000452585349ULL,"audit ri4 header");
    U n=integer(ri);integer(ri);U r=integer(ri);check(n==text.bytes&&r<=(UINT64_MAX-2080)/32&&ribytes>=2080+5*r,"audit dimensions");
    check(aggbytes==12+32*r&&integer(agg,4)==0x31415243&&integer(agg)==r,"audit agg dimensions");
    Stream chars(argv[2],2080);bool rs=false,nl=false;
    for(U i=0;i<r;i++){U c=chars.next(1);rs|=c==30;nl|=c==10;}
    chars.in.clear();chars.in.seekg(2080);
    Stream lcps(argv[3],12),heads(argv[3],12+8*r),tails(argv[3],12+16*r),interiors(argv[3],12+24*r);
    check(rs||!nl,"text audit currently requires cyclic corpus convention");
    std::ifstream cf(argv[4],std::ios::binary);check(bool(cf),"audit chi open");U sizechi=size(cf);check(sizechi%8==0,"audit chi size");U count=sizechi/8,take=std::min(count,requested);
    std::map<U,bool> samples;
    // Stratified deterministic witnesses across the complete emission stream.
    for(U i=0;i<take;i++){cf.seekg((i*(count/take)+(i*(count%take))/take)*8);U v=integer(cf);check(v<n,"audit witness range");check(samples.emplace(v,false).second,"audit duplicate sample");}
    struct Candidate {int64_t len=-1;U pos=0,other=0,lcp=0;bool active=false;int symbol=0,other_symbol=0;};
    std::array<Candidate,256> candidates{};U comparisons=0,verified=0,emitted=0;
    auto witness=[&](U pos){check(pos<n,"audit endpoint range");return pos? n-pos:0;};
    std::array<unsigned char,65536> left{},right{};
    auto emit=[&](const Candidate& c){
        ++emitted;U w=witness(c.pos);auto found=samples.find(w);if(found==samples.end())return;
        check(!found->second,"audit repeated sampled witness");
        check(c.other<n&&c.lcp<=n,"audit context range");
        U a=c.pos,b=c.other;
        check(text.byte((a+n-1)%n)==c.symbol&&text.byte((b+n-1)%n)==c.other_symbol,"audit witness symbol FAIL");
        check(text.byte((a+n-1)%n)!=text.byte((b+n-1)%n),"audit right-context distinctness FAIL");
        for(U j=0;j<c.lcp;){
            U x=(a+j)%n,y=(b+j)%n;
            size_t length=std::min({U(left.size()),c.lcp-j,n-x,n-y});
            text.at(x,left.data(),length);text.at(y,right.data(),length);comparisons+=length;
            check(!memcmp(left.data(),right.data(),length),"audit shared context FAIL");j+=length;
        }
        if(c.lcp<n){++comparisons;check(text.byte((a+c.lcp)%n)!=text.byte((b+c.lcp)%n),"audit LCP maximality FAIL");}
        found->second=true;++verified;
    };
    int prev=-1;U prevtail=0,interior=UINT64_MAX;int64_t m=INT64_MAX;
    for(U i=0;i<r;i++){
        U lcp=lcps.next(),head=heads.next(),tail=tails.next(),within=interiors.next();
        int c=chars.next(1);check(c<256,"audit alphabet");
        if(i){int64_t mm=std::min(m,interior==UINT64_MAX?INT64_MAX:int64_t(interior));
            if(c!=prev){int64_t m3=std::min(mm,int64_t(lcp));
                for(int ch=1;ch<256;ch++)if(m3<candidates[ch].len){if(candidates[ch].active)emit(candidates[ch]);candidates[ch].len=m3;candidates[ch].active=false;}
                if(int64_t(lcp)>candidates[prev].len)candidates[prev]={int64_t(lcp),prevtail,head,lcp,true,prev,c};
                if(int64_t(lcp)>candidates[c].len)candidates[c]={int64_t(lcp),head,prevtail,lcp,true,c,prev};
                m=INT64_MAX;
            }else m=std::min(mm,int64_t(lcp));
        }
        prev=c;prevtail=tail;interior=within;
    }
    for(int c=1;c<256;c++)if(candidates[c].active)emit(candidates[c]);
    check(emitted==count&&verified==take,"audit sampled witness missing/count mismatch");
    text.finish();
    std::cout<<"TEXT_SAMPLE_PASS requested="<<requested<<" verified="<<verified<<" chi="<<count<<" compared_bytes="<<comparisons<<"\n";
}catch(const std::exception& e){std::cerr<<"TEXT_SAMPLE_FAIL "<<e.what()<<"\n";return 1;}}
