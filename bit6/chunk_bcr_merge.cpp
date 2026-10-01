// Merge SXCR chunks with the proven dynamic BCR insertion primitive.
// Build: c++ -O3 -std=c++17 bit6/chunk_bcr_merge.cpp -o chunk_bcr_merge
// BCR prepends symbols, so concatenated chunks must be consumed in reverse
// file order; each chunk is recovered in reverse text order through LF.
#define main bcr_frontend_v2_unused_main
#include "bcr_frontend_v2.cpp"
#undef main
#include <chrono>
#include <limits>

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
                         U& expected_end, unsigned index) {
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
        b.prepend(bwt[row]);
        row=lf[row];
    }
    check(row==primary,"chunk LF cycle incomplete (periodic chunk)");
    expected_end=offset;
    double wall=std::chrono::duration<double>(Clock::now()-start).count();
    std::printf("BATCH index=%u offset=%llu n=%llu runs=%llu insert_wall_seconds=%.6f merged_n=%llu merged_segments=%llu\n",
        index,(unsigned long long)offset,(unsigned long long)n,
        (unsigned long long)runs,wall,(unsigned long long)b.length,
        (unsigned long long)b.root->nruns);
    std::fflush(stdout);
}

int main(int argc,char** argv) {try {
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
