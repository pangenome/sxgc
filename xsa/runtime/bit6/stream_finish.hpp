// Ordered row input needs no seam repair or LCE oracle. Within a BWT run
// the head LCP is its first adjacent LCP and the interior LCP is the minimum
// of the remaining ones. Carry one open run across input packets.
//
// THE ROW-RANGE CONTRACT (I1). One ordered row stream can be written by K
// writers, one per contiguous rank range [A,B) of the same finite stream, and
// their members concatenated in group order. A writer started at A from a
// GroupCarry is the writer that had consumed rows 0..A-1, so `--rows 0:n` with
// no carry files is the unchanged single-group path and every group writes its
// own slice:
//
//   m1    payload blocks concatenate and their (payload, bitPos) directory
//         prefix-sums -- but only if every group but the last ends on the m1
//         block grid, so a continued group must start at a global run index
//         divisible by M1_BLOCK_RUNS (refused otherwise, with the numbers);
//   m12   run-start ROWS are global (`samples.row` is seeded) and m13 run
//         indices are global (`run_base`), so the arrays concatenate and the
//         member headers are rebuilt for the whole;
//   m13   write() closes each symbol's last sample with the distance to the end
//         of the OBSERVED stream. For a group that is the group's own end, so a
//         continued group reports the stride write() would have written for the
//         previous boundary's entry and the merge re-stamps those bytes;
//   m5    the groups OR their witnesses into one SHARED bitmap file and the
//         group that ends the stream writes the member once, over the union;
//   m20/m15  a group consumes rows only: its trailer belongs to the context
//         worker and is not part of this contract yet.
#include <chrono>
#include <optional>
namespace rowfinish {
// Wall intervals for the three consumers of this stage. Reporting only.
static inline double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Frame { uint32_t pos, lcp, tag; };
static_assert(sizeof(Frame)==12);

// --------------------------------------------------------------- group carry
// The state of a row writer at one row boundary: the open run neither m1 nor the
// chi sweep has seen yet, the sweep's 256-entry running maxima (SweepImpl) with
// its four scalars, and the two run directories' GLOBAL sampling state -- their
// samples are keyed by global run index and by per-symbol occurrence counts, not
// by rows.
//
// sample_group[c] names the group (by its row_base) whose m13 member holds
// symbol c's latest sample entry, NO_GROUP while c has no sample at all; a
// sample taken by a later group is what closes that entry's stride, so the group
// that takes it reports the repair in `fixes`. Every field is a plain value so
// the three states can be compared and serialized field for field.
static constexpr uint64_t NO_GROUP = ~0ull;

struct GroupCarry {
    static constexpr uint64_t MAGIC = 0x3130505247574f52ull;   // "ROWGRP01", little endian
    static constexpr uint64_t VERSION = 2;
    static constexpr size_t HEADER_WORDS = 21, ARRAY_WORDS = 8*256, GROUPS = 256;
    uint64_t n = 0;             // corpus extent this state belongs to
    uint64_t row_base = 0;      // A: the row this state starts at
    uint64_t run_base = 0;      // global index of the first run this group closes
    uint64_t prev = 0;          // position of row A-1
    bool open = false;          // an open run ends at row A-1
    uint8_t c = 0;
    uint64_t len = 0, h = 0, t = 0, top = 0, interior = 0;
    // The group ended AT a run boundary: it closed its own last run (a producer
    // cuts on a byte change, so the row at B starts a run that belongs to the
    // next group), `c` still names that run's code, and the continuing group's
    // first row must open a DIFFERENT run. Without this the writer would leave
    // its last run open, stamp run_base one run short of the producer's cut and
    // put every continued group off the m1 block grid.
    bool boundary_fresh = false;
    bool first = true;          // the sweep has not advanced yet
    int64_t m = 0;
    int32_t p = -1;
    uint64_t p_sa_last = 0, previous_interior = 0;
    std::array<int64_t,GROUPS> rr_len{};
    std::array<uint64_t,GROUPS> rr_pos{}, rr_act{};
    uint64_t idx_row = 0;       // rows counted by the run directories
    std::array<uint64_t,GROUPS> occ{}, cum{}, lastSample{}, lastSampleOcc{}, sample_group{};
    // (owner: row_base of the group whose m13 holds the entry, global run index
    //  of that entry, symbol, stride write() would have written for it)
    std::vector<std::array<uint64_t,4>> fixes;
    GroupCarry() {
        rr_len.fill(-1);
        sample_group.fill(NO_GROUP);
        m = INT64_MAX;
        previous_interior = INF;
    }
    size_t words() const { return HEADER_WORDS + ARRAY_WORDS + 4*fixes.size(); }
    std::vector<uint64_t> pack() const {
        std::vector<uint64_t> w;
        w.reserve(words());
        for (uint64_t v : {MAGIC, VERSION, n, row_base, run_base, prev, uint64_t(open), uint64_t(c),
                           len, h, t, top, interior, uint64_t(first), uint64_t(m), uint64_t(int64_t(p)),
                           p_sa_last, previous_interior, idx_row, uint64_t(fixes.size()),
                           uint64_t(boundary_fresh)})
            w.push_back(v);
        for (int64_t v : rr_len) w.push_back(uint64_t(v));
        for (const auto* a : {&rr_pos, &rr_act, &occ, &cum, &lastSample, &lastSampleOcc, &sample_group})
            for (uint64_t v : *a) w.push_back(v);
        for (const auto& f : fixes) { w.push_back(f[0]); w.push_back(f[1]); w.push_back(f[2]); w.push_back(f[3]); }
        return w;
    }
    static GroupCarry unpack(const std::vector<uint64_t>& w) {
        GroupCarry g;
        pf_check(w.size() >= HEADER_WORDS && w[0] == MAGIC,
                 "row-range carry is not a group carry (magic " + std::to_string(w.empty() ? 0 : w[0]) +
                 ", expected " + std::to_string(MAGIC) + ")");
        pf_check(w[1] == VERSION, "row-range carry is version " + std::to_string(w[1]) +
                                  ", this writer reads version " + std::to_string(VERSION));
        g.n = w[2]; g.row_base = w[3]; g.run_base = w[4]; g.prev = w[5];
        g.open = w[6] != 0; g.c = uint8_t(w[7]);
        g.len = w[8]; g.h = w[9]; g.t = w[10]; g.top = w[11]; g.interior = w[12];
        g.first = w[13] != 0; g.m = int64_t(w[14]); g.p = int32_t(int64_t(w[15]));
        g.p_sa_last = w[16]; g.previous_interior = w[17]; g.idx_row = w[18];
        const uint64_t fixes = w[19];
        g.boundary_fresh = w[20] != 0;
        pf_check(w.size() == HEADER_WORDS + ARRAY_WORDS + 4*fixes,
                 "row-range carry length " + std::to_string(w.size()) + " is not " +
                 std::to_string(HEADER_WORDS) + " + " + std::to_string(ARRAY_WORDS) + " + 4*" +
                 std::to_string(fixes) + " words");
        size_t at = HEADER_WORDS;
        auto take = [&](std::array<uint64_t,GROUPS>& a) { for (auto& v : a) v = w[at++]; };
        for (auto& v : g.rr_len) v = int64_t(w[at++]);
        take(g.rr_pos); take(g.rr_act); take(g.occ); take(g.cum); take(g.lastSample);
        take(g.lastSampleOcc); take(g.sample_group);
        for (uint64_t k = 0; k < fixes; ++k) g.fixes.push_back({w[at], w[at+1], w[at+2], w[at+3]}), at += 4;
        return g;
    }
    void save(const std::string& path) const {
        const std::vector<uint64_t> w = pack();
        int fd = ::open(path.c_str(), O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0600);
        pf_check(fd >= 0, "create row-range carry " + path);
        pf_write_full(fd, w.data(), w.size()*8, "row-range carry");
        pf_check(::close(fd) == 0, "close row-range carry " + path);
    }
    static GroupCarry load(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY|O_CLOEXEC);
        pf_check(fd >= 0, "open row-range carry " + path);
        std::array<uint64_t,HEADER_WORDS> head{};
        pf_read_full(fd, head.data(), sizeof(head), "row-range carry head");
        const uint64_t fixes = head[19];
        pf_check(fixes <= (1u<<20), "row-range carry claims " + std::to_string(fixes) + " m13 repairs");
        std::vector<uint64_t> w(head.begin(), head.end());
        w.resize(HEADER_WORDS + ARRAY_WORDS + 4*size_t(fixes));
        pf_read_full(fd, w.data() + HEADER_WORDS, (ARRAY_WORDS + 4*size_t(fixes))*8, "row-range carry body");
        char extra; pf_check(::read(fd, &extra, 1) == 0, "row-range carry " + path + " has trailing bytes");
        pf_check(::close(fd) == 0, "close row-range carry " + path);
        return unpack(w);
    }
};

struct Blocks {
    ContainerOut& out;
    M1Code code;
    BitsOut heads, lengths;
    // The m1 block grid, the m12 run-start rows and the m13 run indices are all
    // global: `run_base` is the index of the first run this writer closes.
    uint64_t run_base = 0, runs = 0, payload = 0;
    uint64_t group = 0;                 // this group's row_base, i.e. the owner of its m13 tails
    std::vector<uint8_t> buffer;
    std::vector<std::array<uint64_t,2>> directory;
    StreamRunIndex samples;
    std::vector<uint64_t> sparse;
    std::array<uint64_t,256> sample_group{};
    std::vector<std::array<uint64_t,4>> fixes;

    Blocks(ContainerOut& o, const std::array<uint64_t,256>& freq) : out(o), code(canonical_huffman_m1(freq)) {
        sample_group.fill(NO_GROUP);
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
    // Continue the writer that produced this carry: its global run index, the
    // two run directories' GLOBAL sampling state, and the m13 repairs earlier
    // groups already reported. A cold writer restores the default state.
    void restore(const GroupCarry& g, uint64_t row_base) {
        run_base = g.run_base; group = row_base;
        sample_group = g.sample_group; fixes = g.fixes;
        samples.row = g.idx_row;
        for (unsigned c=0; c<256; ++c) {
            samples.occ[c] = g.occ[c]; samples.cum[c] = g.cum[c];
            samples.lastSample[c] = g.lastSample[c]; samples.lastSampleOcc[c] = g.lastSampleOcc[c];
        }
    }
    void add(const NRun& r) {
        const uint64_t gk = run_base + runs;                 // this run's global index
        const uint64_t previous_occ = samples.lastSampleOcc[r.c], previous_group = sample_group[r.c];
        const uint64_t previous_run = samples.lastSample[r.c];
        samples.observe(gk,r.c,r.len);
        // A new sample closes the previous one. Inside this group observe() has
        // already stamped it; when the previous sample belongs to an EARLIER
        // group, that group's member is written and only the merge can re-stamp
        // it, so carry the stride write() would have written for it.
        if (samples.lastSample[r.c] == gk) {
            if (previous_group != NO_GROUP && previous_group != group)
                fixes.push_back({previous_group, previous_run, r.c, samples.lastSampleOcc[r.c] - previous_occ});
            sample_group[r.c] = group;
        }
        if (!(gk%PF_RUNSTART_STRIDE)) sparse.push_back(r.t);
        heads.msb(code.codes[r.c],unsigned(code.lens[r.c])); lengths.gamma(r.len);
        if (!(++runs%M1_BLOCK_RUNS)) block();
    }
    void finish(bool last) {
        // The stream ends here: every entry an earlier group left open is closed
        // by the final symbol counts, exactly as write() closes this group's own.
        if (last) for (unsigned c=0; c<256; ++c)
            if (sample_group[c] != NO_GROUP && sample_group[c] != group)
                fixes.push_back({sample_group[c], samples.lastSample[c], c, samples.occ[c] - samples.lastSampleOcc[c]});
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

// `--rows A:B` as parsed by the caller; `to == 0` means the histogram's extent.
struct RowRange {
    uint64_t from = 0, to = 0;
    std::string carry_in, carry_out, chi_shared;
};
} // namespace rowfinish

static int finish_row_stream(const char* histogram,const char* scratch,const char* output,
                             const rowfinish::RowRange& range = {}) {
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
    // ---- the row range and the carried state a continued group starts from
    const uint64_t from=range.from, to=range.to?range.to:n;
    const bool group = from || to != n;
    pf_check(from<to && to<=n,
             "row range " + std::to_string(from) + ":" + std::to_string(to) +
             " is not a non-empty range inside 0:" + std::to_string(n));
    GroupCarry carry; bool carried=false;
    if (!range.carry_in.empty()) {
        carry=GroupCarry::load(range.carry_in);carried=true;
        pf_check(carry.n==n, "row-range carry " + range.carry_in + " is for corpus n=" +
                             std::to_string(carry.n) + ", this corpus is n=" + std::to_string(n));
        pf_check(carry.row_base==from, "row-range carry " + range.carry_in + " starts at row " +
                                       std::to_string(carry.row_base) + ", this group starts at row " +
                                       std::to_string(from));
        pf_check(carry.run_base%M1_BLOCK_RUNS==0,
                 "row-range carry continues at global run " + std::to_string(carry.run_base) +
                 ", which is " + std::to_string(carry.run_base%M1_BLOCK_RUNS) + " past the m1 block grid of " +
                 std::to_string(M1_BLOCK_RUNS) + " runs: group it at a run index divisible by " +
                 std::to_string(M1_BLOCK_RUNS) + " or its m1 blocks cannot concatenate");
        // The carried open run ends at row from-1 by construction (the writer
        // produced it after its last row); its tail is a TEXT position, so only
        // its presence and its ends are checkable here. What continues it is
        // checked against the first row below.
        pf_check(carry.open == (carry.len != 0),
                 "row-range carry " + range.carry_in + " declares open=" + std::to_string(int(carry.open)) +
                 " with a run of " + std::to_string(carry.len) + " rows");
        pf_check(!carry.open || (carry.h < n && carry.t < n),
                 "row-range carry leaves an open run at positions " + std::to_string(carry.h) + ".." +
                 std::to_string(carry.t) + ", outside the corpus n=" + std::to_string(n));
    }
    if (group) {
        pf_check(!range.carry_in.empty() || from==0, "a group that does not start at row 0 needs --carry-in");
        pf_check(!carried || !carry.boundary_fresh || !carry.open,
                 "row-range carry " + range.carry_in + " claims a run boundary AND an open run");
        pf_check(!range.chi_shared.empty(), "a group run ORs its witnesses into a shared bitmap: pass --chi-shared PATH");
        pf_check(!range.carry_out.empty(), "a group run reports its continuation state: pass --carry-out PATH");
    }
    ContainerOut out(output,10,true); out.hashDiagnostics=false;
    Blocks blocks(out,freq);
    blocks.restore(carry,from);   // the default carry is a cold writer
    std::optional<WitnessBitmap> bitmap;
    if (group) bitmap.emplace(n,range.chi_shared,WitnessBitmap::SHARED_FILE);
    else bitmap.emplace(n,scratch);
    SweepImpl<WitnessBatch> sweep(n,WitnessBatch{&*bitmap,-1});
    if (carried) {
        sweep.m=carry.m; sweep.p=carry.p; sweep.p_sa_last=carry.p_sa_last;
        sweep.previous_interior=carry.previous_interior; sweep.first=carry.first;
        for (unsigned cc=0;cc<256;++cc) {
            sweep.rr_len[cc]=carry.rr_len[cc]; sweep.rr_pos[cc]=carry.rr_pos[cc]; sweep.rr_act[cc]=carry.rr_act[cc]!=0;
        }
        // The heap is a function of rr_len alone: the fired set is everything
        // above the threshold and its witnesses are emitted in ascending order.
        sweep.heapInit();
    }
    NRun run{};uint64_t top=0,interior=INF,prev=n-1;
    if (carried) {
        prev=carry.prev;
        if (carry.open) { run={carry.c,carry.len,carry.h,carry.t}; top=carry.top; interior=carry.interior; }
    }
    double spend_add=0,spend_sweep=0;
    auto close_run=[&]{if(run.len){double s=now_s();blocks.add(run);double m=now_s();sweep.advance(run,top,interior);spend_add+=m-s;spend_sweep+=now_s()-m;}};
    double t_begin=now_s(),spend_read=0,spend_row=0;
    std::vector<Frame> packet(131072);   // 1.5 MiB per read: fewer syscalls, deeper readahead
    for(uint64_t rank=from;rank<to;) {
        size_t take=std::min<uint64_t>(packet.size(),to-rank);double s=now_s();
        pf_read_full(0,packet.data(),take*sizeof(Frame),"ordered rows");
        spend_read+=now_s()-s;s=now_s();
        for(size_t j=0;j<take;++j,++rank) {
            auto f=packet[j];unsigned b=f.tag&511;
            uint64_t p=f.pos | uint64_t((f.tag>>9)&2047)<<32;
            uint64_t h=f.lcp | uint64_t((f.tag>>20)&2047)<<32;
            pf_check(f.tag>>31 && p<n && (p==n-1)==(rank==0) && b<=256 && (b==256)==(p==0) && h<=n-1-std::max(p,prev),"invalid ordered row");
            auto c=b==256?uint8_t(0):map[b];prev=p;
            const bool opened = !run.len || run.c!=c;
            if(opened) {close_run();run={c,1,p,p};top=h;interior=INF;}
            else {++run.len;run.t=p;interior=std::min(interior,h);}
            // The carried run ends at row from-1: this row either continues it
            // (the boundary falls inside a run) or closes it and opens a new one
            // (the boundary falls between runs). Nothing else continues the
            // stream, so the first inherited run always ends between these rows.
            if (carried && rank==from && carry.open)
                pf_check(run.c==carry.c ? (run.len==carry.len+1 && run.t==p) : (run.len==1 && run.c==c),
                         "row " + std::to_string(rank) + " neither continues the carried run (code " +
                         std::to_string(int(carry.c)) + ", " + std::to_string(carry.len) +
                         " rows) nor opens a new one (code " + std::to_string(int(c)) + ", " +
                         std::to_string(run.len) + " rows)");
            // A producer cuts on a byte change, so the group before this one
            // closed its own last run and this row must open a different one.
            if (carried && rank==from && carry.boundary_fresh)
                pf_check(run.len==1 && run.c!=carry.c,
                         "row " + std::to_string(rank) + " does not open a fresh run: the group carry "
                         "declares a run boundary after code " + std::to_string(int(carry.c)) +
                         " but this row's code is " + std::to_string(int(c)) + " (" +
                         std::to_string(run.len) + " rows in the open run)");
        }
        spend_row+=now_s()-s;
    }
    // A group that does not end the stream ends AT a run boundary: the row at
    // `to` starts the next group's first run, so this group's own last run is
    // complete. Close it here, exactly as that row would have, so the runs this
    // group contributes are all of them and the carry's run_base lands on the m1
    // block grid the producer cut on.
    double sweep_finish_s=0;
    bool boundary_fresh=false;
    unsigned boundary_code=0;
    if (to==n) {
        close_run();double sweep_s=now_s();sweep.finish();blocks.finish(true);
        sweep_finish_s=now_s()-sweep_s;
    } else {
        close_run();boundary_code=run.c;run={};boundary_fresh=true;
        blocks.finish(false);
    }
    if (to==n) pf_check(std::equal(freq.begin(),freq.end(),blocks.samples.cum),"BWT histogram differs from corpus");
    else for(unsigned c=0;c<256;++c)
        pf_check(blocks.samples.cum[c]<=freq[c],"group run histogram overruns the corpus histogram");
    double s=now_s();
    if (to==n) {
        if (group) bitmap->rescan();   // m5 is one pass over the union of every group
        write_chi(out,*bitmap,n,group?bitmap->totalValues:sweep.chi);
    }
    const double chi_s=now_s()-s;s=now_s();
    out.begin(4,4,0);out.num(0x434e4158,4);out.num(0);
    out.begin(7,7,256);out.write(map,sizeof(map));out.begin(10,110,blocks.runs);
    out.begin(11,111,blocks.sparse.size());out.num(10);out.num(blocks.sparse.size());out.write(blocks.sparse.data(),8*blocks.sparse.size());
    blocks.samples.write(out,blocks.runs);
    const uint64_t global_runs=blocks.run_base+blocks.runs;
    if (group) {
        // A group consumes rows only: the m20/m15 trailer is the context
        // worker's and a group has no trailer of its own yet.
        char extra;pf_check(::read(0,&extra,1)==0,"a group's rows must end the input");
    } else {
        // The context worker sends these only after EXACT EOF on the sorter's pipe.
        member(out,20,n-1,word());uint64_t docs=word();pf_check(docs && docs<n,"document count");member(out,15,docs,word());
        char extra;pf_check(::read(0,&extra,1)==0,"extra bytes after document metadata");
    }
    out.writeVersion=10;
    out.finish(n,1,global_runs,(to==n?1ull:0ull)|8|16|PF_FLAG_M1_BLOCKED|256|512);
    if (!range.carry_out.empty()) {
        GroupCarry next;
        next.n=n;next.row_base=to;next.run_base=global_runs;next.prev=prev;
        next.open=run.len!=0;next.c=boundary_fresh?uint8_t(boundary_code):run.c;
        next.len=run.len;next.h=run.h;next.t=run.t;
        next.top=top;next.interior=interior;next.boundary_fresh=boundary_fresh;
        next.first=sweep.first;next.m=sweep.m;next.p=sweep.p;next.p_sa_last=sweep.p_sa_last;
        next.previous_interior=sweep.previous_interior;
        for (unsigned cc=0;cc<256;++cc) {
            next.rr_len[cc]=sweep.rr_len[cc];next.rr_pos[cc]=sweep.rr_pos[cc];next.rr_act[cc]=sweep.rr_act[cc]?1:0;
        }
        next.idx_row=blocks.samples.row;
        for(unsigned cc=0;cc<256;++cc) {
            next.occ[cc]=blocks.samples.occ[cc];next.cum[cc]=blocks.samples.cum[cc];
            next.lastSample[cc]=blocks.samples.lastSample[cc];next.lastSampleOcc[cc]=blocks.samples.lastSampleOcc[cc];
            next.sample_group[cc]=blocks.sample_group[cc];
        }
        next.fixes=blocks.fixes;
        next.save(range.carry_out);
        fprintf(stderr,"XSA_FINISH_ROW_CARRY out=%s next_row=%llu row_base=%llu run_base=%llu open_run=%llu "
                       "open_code=%u boundary_fresh=%u runs=%llu m13_fixes=%zu\n",
                range.carry_out.c_str(),(unsigned long long)to,(unsigned long long)from,
                (unsigned long long)global_runs,(unsigned long long)run.len,
                unsigned(next.c),unsigned(boundary_fresh),(unsigned long long)blocks.runs,
                blocks.fixes.size());
    }
    if (group)
        fprintf(stderr,"XSA_FINISH_ROWS from=%llu to=%llu carried=%d run_base=%llu runs=%llu open_run=%llu\n",
                (unsigned long long)from,(unsigned long long)to,int(carried),
                (unsigned long long)blocks.run_base,(unsigned long long)blocks.runs,(unsigned long long)run.len);
    fprintf(stderr,"XSA_FINISH_TIME total=%.6f pre=%.6f read=%.6f rows=%.6f add=%.6f sweep=%.6f"
        " sweep_finish=%.6f chi=%.6f tail=%.6f runs=%llu rows_n=%llu\n",
        now_s()-t_entry,t_begin-t_entry,spend_read,spend_row,spend_add,spend_sweep,sweep_finish_s,chi_s,
        now_s()-s,blocks.runs,n);
    return 0;
}
