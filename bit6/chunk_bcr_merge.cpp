// Merge SXCR chunks with the proven dynamic BCR insertion primitive.
// Build: c++ -O3 -std=c++17 bit6/chunk_bcr_merge.cpp -o chunk_bcr_merge
// BCR prepends symbols, so concatenated chunks must be consumed in reverse
// file order; each chunk is recovered in reverse text order through LF.
#define main bcr_frontend_v2_unused_main
#include "bcr_frontend_v2.cpp"
#undef main
#include <chrono>
#include <limits>
#include <sys/wait.h>

using Clock = std::chrono::steady_clock;
static U read_le(std::ifstream& f, unsigned bytes) {
    uint8_t raw[8]{};
    f.read(reinterpret_cast<char*>(raw), bytes);
    check(bool(f), "truncated SXCR file");
    U value=0;
    for(unsigned i=0;i<bytes;++i)value|=U(raw[i])<<(8*i);
    return value;
}

static void insert_chunk(BCR& b, const std::filesystem::path& path,
                         U& expected_end, unsigned index,
                         std::ofstream* reverse_out=nullptr) {
    auto start=Clock::now();
    std::ifstream f(path,std::ios::binary);
    check(bool(f),"open SXCR chunk");
    char magic[4]{};f.read(magic,4);
    check(bool(f) && !memcmp(magic,"SXCR",4),"SXCR magic mismatch");
    check(read_le(f,4)==1,"unsupported SXCR version");
    U offset=read_le(f,8),n=read_le(f,8),runs=read_le(f,8);
    check(n && n<=UINT32_MAX && runs && offset+n==expected_end,
          "invalid or out-of-order SXCR chunk");
    check(std::filesystem::file_size(path)==32+21*runs,"SXCR file size mismatch");
    // One read of the chunk artifact. The compact LF array permits direct
    // reverse-cyclic traversal without materializing the source text or SA.
    std::vector<uint8_t> bwt(n);
    std::vector<uint32_t> lf(n);
    std::array<U,256> freq{},seen{};
    U row=0,best_sa=n,best_row=0;
    uint8_t prev=0;
    for(U i=0;i<runs;++i) {
        uint8_t c=uint8_t(read_le(f,1));
        U length=read_le(f,4),head=read_le(f,8),tail=read_le(f,8);
        check(length && row+length<=n && head<n && tail<n,"invalid SXCR run");
        check(i==0 || c!=prev,"noncanonical SXCR runs");
        if(head<best_sa) {best_sa=head;best_row=row;}
        std::fill(bwt.begin()+row,bwt.begin()+row+length,c);
        freq[c]+=length;row+=length;prev=c;
    }
    check(row==n && f.peek()==EOF,"SXCR length mismatch");
    U base=0;
    for(unsigned c=0;c<256;++c) {U count=freq[c];freq[c]=base;base+=count;}
    check(base==n,"SXCR counts mismatch");
    for(U i=0;i<n;++i) {
        uint8_t c=bwt[i];
        lf[i]=uint32_t(freq[c]+seen[c]++);
    }
    row=best_row;
    for(U i=0;i<best_sa;++i)row=lf[row];
    check(bwt[row]==30,"chunk does not end at separator or LF anchor failed");
    U primary=row;
    for(U i=0;i<n;++i) {
        uint8_t c=bwt[row];b.prepend(c);
        if(reverse_out)reverse_out->put(char(c));
        row=lf[row];
    }
    check(!reverse_out || bool(*reverse_out),"write reverse tree stream");
    check(row==primary,"chunk LF cycle incomplete (periodic chunk)");
    expected_end=offset;
    double wall=std::chrono::duration<double>(Clock::now()-start).count();
    std::printf("BATCH index=%u offset=%llu n=%llu runs=%llu insert_wall_seconds=%.6f merged_n=%llu merged_segments=%llu\n",
        index,(unsigned long long)offset,(unsigned long long)n,
        (unsigned long long)runs,wall,(unsigned long long)b.length,
        (unsigned long long)b.root->nruns);
    std::fflush(stdout);
}

// A tree node keeps the exact padded BCR state. The padding and primary row
// are essential: dropping either changes later prepends and the final samples.
static void save_state(const BCR& b, const std::filesystem::path& path,
                       U offset) {
    check(!std::filesystem::exists(path),"tree state exists; refusing to clobber");
    std::ofstream f(path,std::ios::binary);
    check(bool(f),"open tree state");
    f.write("SXBT",4);u32(f,1);u64(f,offset);u64(f,b.length-10);
    u64(f,b.primary);u64(f,b.root->nruns);
    U runs=0;uint8_t last=0;U carry=0;
    auto flush=[&]() {
        while(carry) {U take=std::min<U>(carry,UINT32_MAX);
            u32(f,uint32_t(take));f.put(char(last));carry-=take;++runs;}
    };
    auto emit=[&](Run x) {if(carry && x.c!=last)flush();last=x.c;carry+=x.n;};
    BCR::walk(b.root,emit);flush();
    // The header counts coalesced runs; a run above UINT32_MAX has segments.
    check(bool(f) && runs>=b.root->nruns,"write tree state");
}

static Node* build_nodes(std::vector<Node*>& leaves,size_t lo,size_t hi) {
    if(lo==hi)return nullptr;
    size_t mid=lo+(hi-lo)/2;
    Node* p=leaves[mid];
    p->l=build_nodes(leaves,lo,mid);
    p->r=build_nodes(leaves,mid+1,hi);
    pull(p);return p;
}

static void load_state(BCR& b,const std::filesystem::path& path,
                       U offset,U n) {
    std::ifstream f(path,std::ios::binary);check(bool(f),"open tree state");
    char magic[4]{};f.read(magic,4);
    check(bool(f) && !memcmp(magic,"SXBT",4),"tree state magic mismatch");
    check(read_le(f,4)==1 && read_le(f,8)==offset && read_le(f,8)==n,
          "tree state coverage mismatch");
    U primary=read_le(f,8),expected_runs=read_le(f,8);
    check(expected_runs>0 && primary<n+10,"invalid tree state header");
    std::vector<Node*> leaves;Node* leaf=new Node;
    U total=0,runs=0;uint8_t prev=0;
    while(f.peek()!=EOF) {
        U amount=read_le(f,4);uint8_t c=uint8_t(read_le(f,1));
        check(amount && amount<=n+10-total,"invalid tree state run");
        if(!runs || c!=prev)++runs;
        if(leaf->a.size()==CAP) {leaves.push_back(leaf);leaf=new Node;}
        leaf->a.push_back({uint32_t(amount),c});total+=amount;prev=c;
    }
    check(total==n+10 && runs==expected_runs,"tree state length or runs mismatch");
    if(!leaf->a.empty())leaves.push_back(leaf);
    delete b.root;b.root=build_nodes(leaves,0,leaves.size());
    b.length=total;b.primary=primary;b.counts=b.root->freq;b.steps=n;
    check(b.counts[2]==10,"tree state padding mismatch");
}

static U chunk_header(const std::filesystem::path& path,U offset) {
    std::ifstream f(path,std::ios::binary);check(bool(f),"open SXCR header");
    char magic[4]{};f.read(magic,4);
    check(bool(f) && !memcmp(magic,"SXCR",4) && read_le(f,4)==1,
          "invalid SXCR header");
    U at=read_le(f,8),n=read_le(f,8),runs=read_le(f,8);
    check(at==offset && n && n<=UINT32_MAX && runs &&
          std::filesystem::file_size(path)==32+21*runs,
          "SXCR coverage or size mismatch");
    return offset+n;
}

static void consume_reverse(BCR& b,const std::filesystem::path& path,U n,
                            std::ofstream* reverse_out) {
    check(std::filesystem::file_size(path)==n,"reverse tree stream size mismatch");
    std::ifstream f(path,std::ios::binary);check(bool(f),"open reverse tree stream");
    std::array<char,1<<20> buf{};
    auto start=Clock::now();U left=n;
    while(left) {
        U take=std::min<U>(left,buf.size());
        f.read(buf.data(),take);check(U(f.gcount())==take,"read reverse tree stream");
        if(reverse_out)reverse_out->write(buf.data(),take);
        for(U i=0;i<take;++i)b.prepend(uint8_t(buf[i]));
        left-=take;
    }
    check(!reverse_out || bool(*reverse_out),"write reverse tree stream");
    std::printf("TREE_INSERT_REVERSE n=%llu wall_seconds=%.6f\n",
        (unsigned long long)n,std::chrono::duration<double>(Clock::now()-start).count());
}

static void copy_reverse(const std::filesystem::path& path,U n,std::ofstream& out) {
    check(std::filesystem::file_size(path)==n,"right reverse stream size mismatch");
    std::ifstream f(path,std::ios::binary);check(bool(f),"open right reverse stream");
    std::array<char,1<<20> buf{};U left=n;
    while(left) {
        U take=std::min<U>(left,buf.size());f.read(buf.data(),take);
        check(U(f.gcount())==take,"read right reverse stream");
        out.write(buf.data(),take);check(bool(out),"write right reverse stream");
        left-=take;
    }
}

static int tree_worker(int argc,char** argv) {
    check(argc==13,"invalid tree worker arguments");
    std::filesystem::path dir=argv[2];
    unsigned first=std::stoul(argv[3]),mid=std::stoul(argv[4]),last=std::stoul(argv[5]);
    U begin=std::stoull(argv[6]),middle=std::stoull(argv[7]),end=std::stoull(argv[8]);
    std::string left_rev=argv[9],right=argv[10],right_rev=argv[11],out=argv[12];
    check(first<mid && mid<last && begin<middle && middle<end,"invalid tree pair");
    BCR b(UINT64_MAX);U expected_end=end;auto start=Clock::now();
    bool final=out.size()<6 || out.substr(out.size()-6)!=".state";
    std::ofstream rev;
    if(!final) {
        auto rev_path=std::filesystem::path(out).replace_extension(".rev");
        check(!std::filesystem::exists(rev_path),"reverse tree stream exists");
        rev.open(rev_path,std::ios::binary);check(bool(rev),"open reverse tree output");
    }
    if(right=="-") {
        check(last==mid+1,"missing right tree state");
        insert_chunk(b,dir/("chunk-"+std::to_string(mid)+".crle"),expected_end,mid,
                     final?nullptr:&rev);
    } else {
        load_state(b,right,middle,end-middle);expected_end=middle;
        if(!final)copy_reverse(right_rev,end-middle,rev);
    }
    if(left_rev=="-") {
        check(mid==first+1,"missing left tree reverse stream");
        insert_chunk(b,dir/("chunk-"+std::to_string(first)+".crle"),expected_end,first,
                     final?nullptr:&rev);
    } else {
        consume_reverse(b,left_rev,middle-begin,final?nullptr:&rev);
        expected_end=begin;
    }
    check(expected_end==begin && b.length==end-begin+10,"tree pair coverage mismatch");
    if(!final) {rev.close();check(bool(rev),"close reverse tree stream");save_state(b,out,begin);}
    else b.write(out);
    std::printf("TREE_NODE first=%u mid=%u last=%u n=%llu runs=%llu wall_seconds=%.6f\n",
        first,mid,last,(unsigned long long)(b.length-10),
        (unsigned long long)b.root->nruns,
        std::chrono::duration<double>(Clock::now()-start).count());
    return 0;
}

struct TreePart {unsigned first,last;U begin,end;std::string state,reverse;};
static int tree_main(int argc,char** argv) {
    check(argc==7 || argc==8,
          "usage: chunk_bcr_merge --tree CHUNK_DIR COUNT TOTAL_BYTES OUT_PREFIX STATE_DIR [JOBS]");
    std::filesystem::path dir=argv[2],state_dir=argv[6];
    unsigned count=std::stoul(argv[3]),jobs=argc==8?std::stoul(argv[7]):8;
    U total=std::stoull(argv[4]);std::string prefix=argv[5];
    check(count>0 && count<1000000 && jobs>0 && jobs<=32 && total>0,
          "invalid tree count, jobs or total");
    for(const char* ext:{".rlebwt",".rlebwt.meta",".ssa",".ssa_t"})
        check(!std::filesystem::exists(prefix+ext),"output exists; refusing to clobber");
    check(!std::filesystem::exists(state_dir),"tree state directory exists; refusing to clobber");
    std::vector<TreePart> parts;U offset=0;
    for(unsigned i=0;i<count;++i) {
        U end=chunk_header(dir/("chunk-"+std::to_string(i)+".crle"),offset);
        parts.push_back({i,i+1,offset,end,"",""});offset=end;
    }
    check(offset==total,"tree chunk coverage does not match total");
    std::filesystem::create_directories(state_dir);
    auto all_start=Clock::now();unsigned level=0;
    while(parts.size()>1) {
        std::vector<TreePart> next;std::vector<pid_t> active;
        auto level_start=Clock::now();
        for(size_t j=0;j<parts.size();j+=2) {
            if(j+1==parts.size()) {next.push_back(parts[j]);continue;}
            auto left=parts[j],right=parts[j+1];
            std::string stem="level-"+std::to_string(level)+"-"+std::to_string(j/2);
            bool final=parts.size()==2;
            std::string out=final?prefix:(state_dir/(stem+".state")).string();
            std::string log=(state_dir/(stem+".log")).string();
            pid_t pid=fork();check(pid>=0,"fork tree worker");
            if(pid==0) {
                int fd=open(log.c_str(),O_WRONLY|O_CREAT|O_EXCL,0666);
                if(fd<0 || dup2(fd,1)<0 || dup2(fd,2)<0)_exit(127);
                close(fd);
                std::vector<std::string> a={argv[0],"--tree-worker",dir.string(),
                    std::to_string(left.first),std::to_string(right.first),
                    std::to_string(right.last),std::to_string(left.begin),
                    std::to_string(right.begin),std::to_string(right.end),
                    left.reverse.empty()?"-":left.reverse,
                    right.state.empty()?"-":right.state,
                    right.reverse.empty()?"-":right.reverse,out};
                std::vector<char*> ptr;for(auto& x:a)ptr.push_back(x.data());ptr.push_back(nullptr);
                execv(argv[0],ptr.data());_exit(127);
            }
            active.push_back(pid);
            std::string rev=final?"":(state_dir/(stem+".rev")).string();
            next.push_back({left.first,right.last,left.begin,right.end,final?"":out,rev});
            std::printf("TREE_START level=%u pair=%zu first=%u last=%u pid=%d log=%s\n",
                level,j/2,left.first,right.last,int(pid),log.c_str());std::fflush(stdout);
            if(active.size()==jobs) {
                bool failed=false;
                for(pid_t child:active) {
                    int status=0;check(waitpid(child,&status,0)==child,"wait tree worker");
                    failed|=!(WIFEXITED(status)&&WEXITSTATUS(status)==0);
                }
                active.clear();check(!failed,"tree worker failed; see node log");
            }
        }
        bool failed=false;
        for(pid_t child:active) {
            int status=0;check(waitpid(child,&status,0)==child,"wait tree worker");
            failed|=!(WIFEXITED(status)&&WEXITSTATUS(status)==0);
        }
        check(!failed,"tree worker failed; see node log");
        std::printf("TREE_LEVEL level=%u pairs=%zu wall_seconds=%.6f\n",level,
            parts.size()/2,std::chrono::duration<double>(Clock::now()-level_start).count());
        std::fflush(stdout);parts=std::move(next);++level;
    }
    if(count==1) {
        BCR b(UINT64_MAX);U expected_end=total;
        insert_chunk(b,dir/"chunk-0.crle",expected_end,0);
        check(expected_end==0,"single chunk coverage mismatch");b.write(prefix);
    }
    std::printf("TREE_PASS chunks=%u levels=%u n=%llu wall_seconds=%.6f\n",count,level,
        (unsigned long long)total,std::chrono::duration<double>(Clock::now()-all_start).count());
    return 0;
}

int main(int argc,char** argv) {try {
    if(argc>1 && !strcmp(argv[1],"--tree-worker"))return tree_worker(argc,argv);
    if(argc>1 && !strcmp(argv[1],"--tree"))return tree_main(argc,argv);
    check(argc==5,"usage: chunk_bcr_merge CHUNK_DIR COUNT TOTAL_BYTES OUT_PREFIX");
    std::filesystem::path dir=argv[1];
    unsigned count=std::stoul(argv[2]);
    check(count>0 && count<1000000,"invalid chunk count");
    U expected_end=std::stoull(argv[3]);
    check(expected_end>0,"invalid total bytes");
    std::string prefix=argv[4];
    for(const char* ext:{".rlebwt",".rlebwt.meta",".ssa",".ssa_t"})
        check(!std::filesystem::exists(prefix+ext),"output exists; refusing to clobber");
    BCR b(UINT64_MAX);
    auto start=Clock::now();
    for(unsigned i=count;i;) {
        --i;
        insert_chunk(b,dir/("chunk-"+std::to_string(i)+".crle"),expected_end,i);
    }
    check(expected_end==0,"chunk coverage does not start at zero");
    double insert_wall=std::chrono::duration<double>(Clock::now()-start).count();
    std::printf("INSERT_PASS chunks=%u n=%llu padded_n=%llu insert_wall_seconds=%.6f\n",
        count,(unsigned long long)(b.length-10),(unsigned long long)b.length,insert_wall);
    std::fflush(stdout);
    b.write(prefix);
    double total=std::chrono::duration<double>(Clock::now()-start).count();
    std::printf("MERGE_PASS chunks=%u merged_n=%llu padded_n=%llu runs=%llu merge_wall_seconds=%.6f\n",
        count,(unsigned long long)(b.length-10),(unsigned long long)b.length,
        (unsigned long long)b.root->nruns,total);
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"CHUNK_BCR_MERGE_FATAL %s\n",e.what());return 1;
}}
