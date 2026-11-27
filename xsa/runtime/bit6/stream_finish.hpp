// Ordered row input needs no seam repair or LCE oracle. Within a BWT run
// the head LCP is its first adjacent LCP and the interior LCP is the minimum
// of the remaining ones. Carry one open run across input packets.
#include <chrono>
namespace rowfinish {
// Wall intervals for the three consumers of this stage. Reporting only.
static inline double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Frame { uint32_t pos, lcp, tag; };
static_assert(sizeof(Frame)==12);

struct Blocks {
    ContainerOut& out;
    M1Code code;
    BitsOut heads, lengths;
    uint64_t payload = 0, runs = 0;
    std::vector<uint8_t> buffer;
    std::vector<std::array<uint64_t,2>> directory;
    StreamRunIndex samples;
    std::vector<uint64_t> sparse;

    Blocks(ContainerOut& o, const std::array<uint64_t,256>& freq) : out(o), code(canonical_huffman_m1(freq)) {
        out.beginStream(1, PF_M1_CODEC_STREAM, 0);
        std::array<uint8_t,2336> header{};
        uint64_t sum = 0;
        for (unsigned c=0; c<256; ++c) {
            put(header.data()+8*c,sum,8); sum += freq[c];
            header[2048+c] = uint8_t(code.lens[c]);
        }
        put(header.data()+2312,M1_BLOCK_RUNS,8);
        put(header.data()+2328,header.size(),8);
        out.append(header.data(),header.size());
    }
    void flush() { out.append(buffer.data(),buffer.size()); buffer.clear(); }
    void block() {
        heads.pad(); lengths.pad();
        pf_check(heads.bitPos<=65535 && lengths.bitPos<=65535,"v10 block bit counts overflow");
        directory.push_back({payload,heads.bitPos | lengths.bitPos<<32});
        buffer.insert(buffer.end(),heads.mem.begin(),heads.mem.end());
        buffer.insert(buffer.end(),lengths.mem.begin(),lengths.mem.end());
        size_t bytes = heads.mem.size()+lengths.mem.size(), pad = (0-bytes)&7;
        buffer.insert(buffer.end(),pad,0); payload += bytes+pad;
        heads.mem.clear(); lengths.mem.clear(); heads.bitPos = lengths.bitPos = 0;
        if (buffer.size()>=1<<20) flush();
    }
    void add(const NRun& r) {
        samples.observe(runs,r.c,r.len);
        if (!(runs%1024)) sparse.push_back(r.t);
        heads.msb(code.codes[r.c],unsigned(code.lens[r.c])); lengths.gamma(r.len);
        if (!(++runs%M1_BLOCK_RUNS)) block();
    }
    void finish() {
        if (runs%M1_BLOCK_RUNS) block();
        if (!buffer.empty()) flush();
        out.append(directory.data(),16*directory.size()); out.ms[0].count = runs;
        std::vector<std::array<uint64_t,2>>().swap(directory);
    }
};

// Write codec 105 in its on-disk order. Two sequential walks over the N/8
// bitmap replace both EF scratch spools. Only a partial output byte is carried.
static void write_chi(ContainerOut& out, WitnessBitmap& bitmap, uint64_t n, uint64_t chi) {
    pf_check(bitmap.totalValues==chi && !bitmap.duplicates,"chi witness count");
    unsigned l = chi ? unsigned(std::log2(double(n+1)/double(chi))) : 0;
    uint64_t highbits = chi ? (n>>l)+chi+1 : 0;
    out.begin(5,105,chi); out.num(l); out.num(chi*l); out.num(highbits);
    BitsOut bits;
    auto drain = [&] { if (!bits.mem.empty()) {out.write(bits.mem.data(),bits.mem.size()); bits.mem.clear();} };
    if (l) bitmap.for_each_sorted([&](uint64_t p){bits.lsb(p,l); if (bits.mem.size()>=65536) drain();});
    bits.pad(); drain();
    uint64_t i=0, cursor=0;
    auto zeros = [&](uint64_t gap){ while(gap) {uint64_t z=std::min<uint64_t>(gap,65536*8);bits.putzeros(z);gap-=z;if(bits.mem.size()>=65536)drain();} };
    bitmap.for_each_sorted([&](uint64_t p){uint64_t pos=(p>>l)+i++;zeros(pos-cursor);bits.bit(1);cursor=pos+1;if(bits.mem.size()>=65536)drain();});
    zeros(highbits-cursor); bits.pad(); drain(); bitmap.cleanup();
}

static uint64_t word() {uint64_t v; pf_read_full(STDIN_FILENO,&v,8,"metadata header"); return v;}
static void member(ContainerOut& out,unsigned id,uint64_t count,uint64_t bytes) {
    out.begin(id,id,count);
    std::vector<uint8_t> buf(1<<20);
    while(bytes) {size_t z=std::min<uint64_t>(bytes,buf.size());pf_read_full(STDIN_FILENO,buf.data(),z,"metadata payload");out.write(buf.data(),z);bytes-=z;}
}
} // namespace rowfinish

static int finish_row_stream(const char* histogram,const char* scratch,const char* output) {
    using namespace rowfinish;
    const double t_entry=now_s();
    std::array<uint64_t,256> hist{},freq{};
    int fd=::open(histogram,O_RDONLY|O_CLOEXEC);pf_check(fd>=0,"open byte histogram");
    pf_read_full(fd,hist.data(),sizeof(hist),"byte histogram");::close(fd);
    uint8_t map[256];unsigned code=1;uint64_t n=1;
    for(unsigned b=0;b<256;++b)if(hist[b]) {
        pf_check(code<256 && hist[b]<(1ull<<43) && n<=(1ull<<43)-hist[b],"v10 corpus extent/alphabet");
        map[b]=uint8_t(code);freq[code++]=hist[b];n+=hist[b];
    }
    for(unsigned b=0;b<256;++b)if(!hist[b])map[b]=uint8_t(code++);
    freq[0]=1;pf_check(n>1,"empty corpus");
    ContainerOut out(output,10,true); out.hashDiagnostics=false;
    Blocks blocks(out,freq); WitnessBitmap bitmap(n,scratch);
    SweepImpl<WitnessBatch> sweep(n,WitnessBatch{&bitmap,-1});
    NRun run{};uint64_t top=0,interior=INF,prev=n-1;
    double spend_add=0,spend_sweep=0;
    auto close_run=[&]{if(run.len){double s=now_s();blocks.add(run);double m=now_s();sweep.advance(run,top,interior);spend_add+=m-s;spend_sweep+=now_s()-m;}};
    double t_begin=now_s(),spend_read=0,spend_row=0;
    std::vector<Frame> packet(131072);   // 1.5 MiB per read: fewer syscalls, deeper readahead
    for(uint64_t rank=0;rank<n;) {
        size_t take=std::min<uint64_t>(packet.size(),n-rank);double s=now_s();
        pf_read_full(0,packet.data(),take*sizeof(Frame),"ordered rows");
        spend_read+=now_s()-s;s=now_s();
        for(size_t j=0;j<take;++j,++rank) {
            auto f=packet[j];unsigned b=f.tag&511;
            uint64_t p=f.pos | uint64_t((f.tag>>9)&2047)<<32;
            uint64_t h=f.lcp | uint64_t((f.tag>>20)&2047)<<32;
            pf_check(f.tag>>31 && p<n && (p==n-1)==(rank==0) && b<=256 && (b==256)==(p==0) && h<=n-1-std::max(p,prev),"invalid ordered row");
            auto c=b==256?uint8_t(0):map[b];prev=p;
            if(!run.len || run.c!=c) {close_run();run={c,1,p,p};top=h;interior=INF;}
            else {++run.len;run.t=p;interior=std::min(interior,h);}
        }
        spend_row+=now_s()-s;
    }
    close_run();double sweep_s=now_s();sweep.finish();blocks.finish();
    const double sweep_finish_s=now_s()-sweep_s;
    pf_check(std::equal(freq.begin(),freq.end(),blocks.samples.cum),"BWT histogram differs from corpus");
    double s=now_s();
    write_chi(out,bitmap,n,sweep.chi);
    const double chi_s=now_s()-s;s=now_s();
    out.begin(4,4,0);out.num(0x434e4158,4);out.num(0);
    out.begin(7,7,256);out.write(map,sizeof(map));out.begin(10,110,blocks.runs);
    out.begin(11,111,blocks.sparse.size());out.num(10);out.num(blocks.sparse.size());out.write(blocks.sparse.data(),8*blocks.sparse.size());
    blocks.samples.write(out,blocks.runs);
    // The context worker sends these only after EXACT EOF on the sorter's pipe.
    member(out,20,n-1,word());uint64_t docs=word();pf_check(docs && docs<n,"document count");member(out,15,docs,word());
    char extra;pf_check(::read(0,&extra,1)==0,"extra bytes after document metadata");
    out.writeVersion=10;out.finish(n,1,blocks.runs,1|8|16|PF_FLAG_M1_BLOCKED|256|512);
    fprintf(stderr,"XSA_FINISH_TIME total=%.6f pre=%.6f read=%.6f rows=%.6f add=%.6f sweep=%.6f"
        " sweep_finish=%.6f chi=%.6f tail=%.6f runs=%llu rows_n=%llu\n",
        now_s()-t_entry,t_begin-t_entry,spend_read,spend_row,spend_add,spend_sweep,sweep_finish_s,chi_s,
        now_s()-s,blocks.runs,n);
    return 0;
}
