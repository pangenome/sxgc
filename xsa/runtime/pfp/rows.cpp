// Private row43 producer. Inputs live in the builder's PRIVATE scratch, not
// alongside RAW. The lexical census replaces reparsing/re-deduplicating RAW.
#include "io.hpp"
#include "../rolling/finite_emit64.hpp"
#include <csignal>
#include <cmath>
#include <thread>

// ------------------------------------------------- K group sinks (I1 step 2)
// A v10 writer reads this stream as RUNS -- maximal stretches of one byte, since
// it opens a run whenever a row's byte changes -- and writes the m1 payload in
// blocks of M1_BLOCK_RUNS runs (stream_finish.hpp: M1_BLOCK_RUNS and
// PF_RUNSTART_STRIDE, bit6/xsa_finish.cpp). A group may therefore end only
// BETWEEN runs and only where the GLOBAL run index is a multiple of
// M1_BLOCK_RUNS: that is the row-range contract the writer refuses anything else
// with, and it is what lets K independent writer chains concatenate their m1.
//
// Pipe and TilePipe own their cat threads and each take one fd, so the producer
// drains its ordered stream into a pipe and this router appends every row to the
// sink of its group. One pass, one extra copy of the row bytes, no reordering:
// the drain is written by the single ordered cat thread.
namespace pfprows {
using pfp::U;
constexpr U M1_BLOCK_RUNS = 1024;

struct Group {
    U first_row = 0;        // index of its first row in the whole stream
    U rows = 0;             // rows it owns; the groups' sum is n+1
    U first_run = 0;        // global index of its first run (a multiple of M1_BLOCK_RUNS)
    U runs = 0;             // runs it owns; no run is split
    unsigned first_byte = 0;
};

struct Router {
    // I/O GRANULARITY, not cut semantics: the drain is read in buffers of this
    // size and each contiguous stretch of a buffer is written to its group's
    // sink with ONE write(). A group boundary inside a buffer splits that one
    // buffer into two writes; nothing else is ever written piecemeal. The row
    // loop below still walks every row, because the run index, the 1024 grid and
    // the manifest are row-level facts.
    static constexpr size_t BUFFER_BYTES = 1u << 20;   // 1 MiB
    std::vector<int> sink;      // one fd per group, opened before emission
    std::vector<Group> group;   // group.back() is the one being written
    U target = 0;               // --split-runs: runs per group, 0 = equal share
    U total = 0;                // rows the producer will write (n+1)
    U rows = 0, runs = 0;       // global row count, global index of the open run
    unsigned prev_byte = 0;
    size_t g = 0;
    // Reported, not used: what the router actually cost in syscalls.
    unsigned long long io_writes = 0, io_bytes = 0, io_reads = 0;

    Router(std::vector<int> fds, U split_runs, U stream_rows)
        : sink(std::move(fds)), target(split_runs), total(stream_rows) {}

    // The next group's first global run index. The stream's run count is not
    // known before it is emitted, so it is estimated from the run/row rate
    // observed so far: est_total = runs*total/rows, target = (g+1)/K of it. At a
    // run boundary `runs >= target` cancels that estimate, so the cut lands on
    // the first grid run boundary at or past the group's (g+1)/K share of the
    // rows -- an equal share of the runs up to the run/row rate.
    U cut_run() const {
        if (target) return (U(g) + 1) * target;
        const long double estimate = rows ? (long double)runs * (long double)total / (long double)rows : 0.0L;
        return U(std::ceil(estimate * ((long double)(U(g) + 1) / (long double)sink.size())));
    }
    void open_group(U first_row, U first_run, unsigned byte) { group.push_back(Group{first_row, 0, first_run, 0, byte}); }
    // A group's sink is closed the moment its last row is written. A group's
    // stream-context reads exactly its own rows and then needs END OF FILE,
    // which this process never sends while it holds every sink open to the end:
    // the chain then stalls on the group boundary and no carry is ever written.
    void close_sink(size_t k) {
        if (sink[k] >= 0 && sink[k] != STDOUT_FILENO) ::close(sink[k]);
        sink[k] = -1;
    }
    void close_all() { for (size_t k = 0; k < sink.size(); ++k) if (sink[k] >= 0) close_sink(k); }
    void write(const finiteemit::Row* r, U n) {
        if (!n) return;
        const char* p = reinterpret_cast<const char*>(r);
        size_t left = sizeof(finiteemit::Row) * n;
        while (left) {
            ssize_t z = ::write(sink[g], p, left);
            if (z > 0) { p += z; left -= size_t(z); ++io_writes; io_bytes += (unsigned long long)z; }
            else pfp::check(errno == EINTR, "PFP row group sink closed");
        }
    }
    void rows_in(const finiteemit::Row* r, U n) {
        U from = 0;
        for (U i = 0; i < n; ++i) {
            const unsigned byte = unsigned(r[i].tag & 511);
            if (group.empty()) open_group(0, 0, byte);       // row 0 opens run 0 and group 0
            else if (byte != prev_byte) {
                ++runs;                                      // the global index of the run this row opens
                if (g + 1 < sink.size() && !(runs % M1_BLOCK_RUNS) && runs >= cut_run()) {
                    write(r + from, i - from); from = i;     // the cut sits between two runs
                    group.back().runs = runs - group.back().first_run;
                    open_group(rows, runs, byte);
                    close_sink(g);                           // group g is complete: its chain needs EOF
                    ++g;
                }
            }
            prev_byte = byte;
            ++rows;
            ++group.back().rows;
        }
        write(r + from, n - from);
    }
    void pump(int in) {
        std::vector<char> buf(BUFFER_BYTES);
        size_t fill = 0;
        for (;;) {
            ssize_t z = ::read(in, buf.data() + fill, buf.size() - fill);
            if (z < 0) { pfp::check(errno == EINTR, "PFP row group drain read"); continue; }
            ++io_reads;
            fill += size_t(z);
            const U whole = U(fill / sizeof(finiteemit::Row));
            if (whole) rows_in(reinterpret_cast<const finiteemit::Row*>(buf.data()), whole);
            fill -= whole * sizeof(finiteemit::Row);
            if (fill) std::memmove(buf.data(), buf.data() + whole * sizeof(finiteemit::Row), fill);
            if (!z) break;
        }
        pfp::check(!fill, "PFP row group drain ended inside a row");
        pfp::check(!group.empty(), "PFP row stream is empty");
        group.back().runs = (runs + 1) - group.back().first_run;   // the open run is this group's
        close_all();                                              // the last group's chain needs EOF too
        pfp::check(rows == total, "PFP row group coverage");
        pfp::check(sink.size() == 1 || group.size() <= sink.size(), "PFP row group count");
    }
    void save(const std::string& to) const {
        FILE* f = std::fopen(to.c_str(), "w");
        pfp::check(f, "write PFP row group manifest");
        std::fprintf(f, "{\"rows\":%llu,\"runs\":%llu,\"m1_block_runs\":%llu,\"split_runs\":%llu,\"sinks\":%zu,\"groups\":[",
                     (unsigned long long)rows, (unsigned long long)(runs + 1), (unsigned long long)M1_BLOCK_RUNS,
                     (unsigned long long)target, sink.size());
        for (size_t i = 0; i < group.size(); ++i)
            std::fprintf(f, "%s{\"first_row\":%llu,\"rows\":%llu,\"first_run\":%llu,\"runs\":%llu,\"first_byte\":%u}",
                         i ? "," : "", (unsigned long long)group[i].first_row, (unsigned long long)group[i].rows,
                         (unsigned long long)group[i].first_run, (unsigned long long)group[i].runs, group[i].first_byte);
        std::fprintf(f, "]}\n");
        pfp::check(std::fclose(f) == 0, "close PFP row group manifest");
    }
    void report(const std::string& manifest) const {
        std::fprintf(stderr, "PFP_ROW_SPLIT sinks=%zu rows=%llu runs=%llu groups=%zu split_runs=%llu manifest=%s"
                     " buffer_bytes=%zu io_reads=%llu io_writes=%llu io_bytes=%llu bytes_per_write=%.1f\n",
                     sink.size(), (unsigned long long)rows, (unsigned long long)(runs + 1), group.size(),
                     (unsigned long long)target, manifest.c_str(), BUFFER_BYTES,
                     io_reads, io_writes, io_bytes,
                     io_writes ? double(io_bytes) / double(io_writes) : 0.0);
        for (size_t i = 0; i < group.size(); ++i)
            std::fprintf(stderr, "PFP_ROW_GROUP g=%zu first_row=%llu rows=%llu first_run=%llu runs=%llu first_byte=%u\n",
                         i, (unsigned long long)group[i].first_row, (unsigned long long)group[i].rows,
                         (unsigned long long)group[i].first_run, (unsigned long long)group[i].runs, group[i].first_byte);
    }
    static std::string path(const std::string& glob, U k) {
        pfp::check(glob.find('%') != std::string::npos, "PFP --sink-glob needs a %d group index");
        char name[4096];
        pfp::check(std::snprintf(name, sizeof name, glob.c_str(), int(k)) > 0, "PFP --sink-glob path too long");
        return name;
    }
};
} // namespace pfprows

// ------------------------------------------------ K band sinks (I1 step 2b)
// A run cannot span a band boundary -- the bands are cut at a byte change -- so
// run formation is band-local: band r's writer can encode its own m1 blocks, m12
// and m13 samples and m20 slice with no shared state, and the members are cat'd
// afterwards. This is not the router above: the producer's cat writes each tile
// STRAIGHT to the sink of the tile's job's band (no drain pipe, no second pass
// over the row bytes), and every band has its own forwarder thread, so band r's
// rows are counted and forwarded while band r+1 is still being produced.
//
// The cut between two groups must still let the writer's row-range contract
// accept it: the next group's first GLOBAL run index must be a multiple of
// M1_BLOCK_RUNS=1024 (m1 blocks carry their own padded bit streams). The plan's
// jobs are partitioned into K contiguous groups by dictionary load, and the cut
// then moves from that intended job boundary to the first run opening at or past
// it whose global index is on the grid. Every moved cut is counted and reported.
namespace pfprows {
struct BandGroup {
    U first_row = 0;        // absolute index of its first row in the whole stream
    U rows = 0;             // rows written into it; the groups' sum is n+1
    U first_run = 0;        // global index of its first run (a multiple of M1_BLOCK_RUNS)
    U runs = 0;             // runs it owns; no run is split
    unsigned first_byte = 0;
    U intended_row = 0;     // where the dictionary-load partition meant it to end
};

// One forwarder per band. It reads its own sink -- the cat closes that sink at the
// band's last tile, so the thread sees EOF exactly at the boundary -- counts runs
// the way the writer does (a run opens when the row's byte changes; the byte is
// the low 9 bits of the row's tag) and writes each row to its GROUP's sink,
// starting the next group on the m1 block grid. A group is written by at most two
// bands and never by two at once: band r+1 waits for band r's EOF before it may
// write the group it starts in, which is what keeps the concatenation and the
// group's EOF ordered.
struct BandFold {
    static constexpr size_t BUFFER_BYTES = 1u << 20;
    std::vector<int> in, out;                 // band sinks (read), group sinks (write)
    std::vector<U> first_row;                 // absolute row band r starts at
    std::vector<U> end_row;                   // absolute row where band r's group is meant to end
    std::vector<U> group_of, base;            // group band r starts in, global run index of its first run
    std::vector<BandGroup> group;
    std::vector<char> go;                     // set when band r may write its group
    std::mutex mu; std::condition_variable cv;
    std::vector<U> moves_of;                  // per band: cuts the grid moved
    U moves = 0, max_rows_moved = 0, rows_total = 0, runs_total = 0;
    unsigned long long io_bytes = 0;

    // Rows go out in the largest stretches the buffer allows: a group boundary is
    // the only thing that splits one buffer's write.
    static void put(int fd, const finiteemit::Row* rows, size_t n) {
        const char* p = reinterpret_cast<const char*>(rows);
        size_t left = n * sizeof(finiteemit::Row);
        while (left) {
            ssize_t z = ::write(fd, p, left);
            if (z > 0) { p += z; left -= size_t(z); continue; }
            pfp::check(z < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK), "PFP band sink closed");
            pollfd want{fd, POLLOUT, 0}; ::poll(&want, 1, 100);
        }
    }
    void start(U g, U row, U run, unsigned byte) { group[g].first_row = row; group[g].first_run = run; group[g].first_byte = byte; }

    U run(size_t r) {
        { std::unique_lock<std::mutex> lock(mu); cv.wait(lock, [&] { return go[r] != 0; }); }
        const U run_base = r ? base[r] - 1 : 0;      // band r>0 starts on a run opening
        U gi = group_of[r], outfd = out[gi];
        U runs = r ? 1 : 0, seen = 0, rows_g = 0, moved = 0;
        unsigned prev = 0;
        bool first = true, cut = false, opened = false;
        std::vector<char> buf(BUFFER_BYTES);
        size_t fill = 0, from = 0;
        for (;;) {
            ssize_t z = ::read(in[r], buf.data() + fill, buf.size() - fill);
            if (z < 0) { pfp::check(errno == EINTR, "PFP band forward read"); continue; }
            if (!z) break;
            io_bytes += (unsigned long long)z;
            fill += size_t(z);
            const size_t whole = fill / sizeof(finiteemit::Row);
            const auto* rows = reinterpret_cast<const finiteemit::Row*>(buf.data());
            for (size_t i = 0; i < whole; ++i) {
                const unsigned byte = unsigned(rows[i].tag & 511);
                const bool opens = first ? (r > 0) : (byte != prev);
                if (opens) ++runs;
                const U cur = run_base + runs;                 // global index of the run this row is in
                if (!first && !cut && opens && first_row[r] + seen >= group[gi].intended_row && !(cur % M1_BLOCK_RUNS)) {
                    // This row starts the next group: the first on-grid run opening at
                    // or past the intended boundary, so no run is split.
                    put(outfd, rows + from, i - from);
                    from = i;
                    group[gi].rows += rows_g;
                    group[gi].runs = cur - group[gi].first_run;
                    const U at = first_row[r] + seen;
                    if (at != group[gi].intended_row) { ++moved; max_rows_moved = std::max(max_rows_moved, at - group[gi].intended_row); }
                    if (outfd != STDOUT_FILENO) ::close(outfd);
                    ++gi;
                    pfp::check(gi < out.size(), "PFP band groups exceed the sink count");
                    outfd = out[gi];
                    rows_g = 0;
                    cut = true;
                    opened = true;
                    start(gi, at, cur, byte);
                } else if (!opened) {
                    // The group this band starts in: only the band that opens it
                    // records its first row, run and byte; a band that continues an
                    // earlier band's group must not restamp them.
                    if (!group[gi].rows) start(gi, first_row[r] + seen, cur, byte);
                    opened = true;
                }
                ++rows_g; ++seen; prev = byte; first = false;
            }
            put(outfd, rows + from, whole - from);
            from = 0;
            fill -= whole * sizeof(finiteemit::Row);
            if (fill) std::memmove(buf.data(), buf.data() + whole * sizeof(finiteemit::Row), fill);
        }
        pfp::check(!fill, "PFP band forward ended inside a row");
        const U cur = run_base + runs;
        group[gi].rows += rows_g;
        group[gi].runs = (cur + 1) - group[gi].first_run;        // the open run is this group's
        // A group's sink is closed by the band that OWNS it -- the one that cuts
        // out of it -- so a group a moved cut handed on stays open for the band
        // that continues in it; only the last band closes the last group.
        if (outfd != STDOUT_FILENO && r + 1 == go.size()) ::close(outfd);
        {
            std::lock_guard<std::mutex> lock(mu);
            if (r + 1 < go.size()) { group_of[r + 1] = gi; base[r + 1] = cur + 1; go[r + 1] = 1; }
            moves_of[r] = moved;
            moves += moved;
            ++rows_bands;
        }
        cv.notify_all();
        return seen;
    }
    U rows_bands = 0;

    void save(const std::string& to) const {
        FILE* f = std::fopen(to.c_str(), "w");
        pfp::check(f, "write PFP band manifest");
        std::fprintf(f, "{\"rows\":%llu,\"runs\":%llu,\"m1_block_runs\":%llu,\"sinks\":%zu,\"bands\":%zu,\"moved_cuts\":%llu,"
                     "\"max_rows_moved\":%llu,\"groups\":[",
                     (unsigned long long)rows_total, (unsigned long long)runs_total, (unsigned long long)M1_BLOCK_RUNS,
                     out.size(), in.size(), (unsigned long long)moves, (unsigned long long)max_rows_moved);
        for (size_t i = 0; i < group.size(); ++i)
            std::fprintf(f, "%s{\"first_row\":%llu,\"rows\":%llu,\"first_run\":%llu,\"runs\":%llu,\"first_byte\":%u,\"intended_row\":%llu}",
                         i ? "," : "", (unsigned long long)group[i].first_row, (unsigned long long)group[i].rows,
                         (unsigned long long)group[i].first_run, (unsigned long long)group[i].runs, group[i].first_byte,
                         (unsigned long long)group[i].intended_row);
        std::fprintf(f, "]}\n");
        pfp::check(std::fclose(f) == 0, "close PFP band manifest");
    }
    void report(const std::string& manifest) const {
        std::fprintf(stderr, "PFP_BAND_SPLIT bands=%zu groups=%zu rows=%llu runs=%llu m1_block_runs=%llu moved_cuts=%llu"
                     " max_rows_moved=%llu manifest=%s io_bytes=%llu\n", in.size(), group.size(),
                     (unsigned long long)rows_total, (unsigned long long)runs_total, (unsigned long long)M1_BLOCK_RUNS,
                     (unsigned long long)moves, (unsigned long long)max_rows_moved, manifest.c_str(), io_bytes);
        for (size_t i = 0; i < group.size(); ++i)
            std::fprintf(stderr, "PFP_BAND_GROUP g=%zu first_row=%llu rows=%llu first_run=%llu runs=%llu first_byte=%u"
                         " intended_row=%llu moved=%llu\n", i, (unsigned long long)group[i].first_row,
                         (unsigned long long)group[i].rows, (unsigned long long)group[i].first_run,
                         (unsigned long long)group[i].runs, group[i].first_byte, (unsigned long long)group[i].intended_row,
                         (unsigned long long)(group[i].first_row - group[i].intended_row));
        for (size_t i = 0; i < in.size(); ++i)
            std::fprintf(stderr, "PFP_BAND b=%zu first_row=%llu end_row=%llu first_run=%llu\n", i,
                         (unsigned long long)first_row[i], (unsigned long long)end_row[i], (unsigned long long)base[i]);
    }
    static std::string path(const std::string& glob, U k) {
        pfp::check(glob.find('%') != std::string::npos, "PFP --band-sink-glob needs a %d band index");
        char name[4096];
        pfp::check(std::snprintf(name, sizeof name, glob.c_str(), int(k)) > 0, "PFP --band-sink-glob path too long");
        return name;
    }
};
} // namespace pfprows

int main(int argc,char** argv) try {
    using pfp::U; using pfp::File; using pfp::fs::path; using pfp::check;
    check(argc>=6,"usage: pfp_rows RAW PRIVATE_COLUMNS WORKERS SORT_BYTES POSTING_BYTES"
                  " [--sinks K] [--sink-glob DIR/rows.%d] [--split-runs N] [--group-manifest PATH]");
    path dir=argv[2]; U t=pfp::number(argv[3]), grant=pfp::number(argv[4]), cap=pfp::number(argv[5]);
    // I1 step 2: route the ordered stream to K sinks, one group per sink.
    U sinks=1,split_runs=0; std::string glob,manifest;
    // K band sinks (I1 step 2b): jobs -> contiguous groups by dictionary load,
    // each group's rows to its own sink, each band forwarded by its own thread.
    U band_sinks=1; std::string band_glob,band_manifest;
    for (int i=6;i<argc;++i) {
        const std::string_view key=argv[i];
        check(i+1<argc,"PFP row option needs a value");
        if (key=="--sinks") sinks=pfp::number(argv[++i]);
        else if (key=="--sink-glob") glob=argv[++i];
        else if (key=="--split-runs") split_runs=pfp::number(argv[++i]);
        else if (key=="--group-manifest") manifest=argv[++i];
        else if (key=="--band-sinks") band_sinks=pfp::number(argv[++i]);
        else if (key=="--band-sink-glob") band_glob=argv[++i];
        else if (key=="--band-manifest") band_manifest=argv[++i];
        else check(false,"unknown pfp_rows option");
    }
    check(t && t<=64 && t<=U(omp_get_num_procs()) && cap,"PFP row worker/scratch extent");
    check(sinks && sinks<=64,"PFP row sink count");
    check(sinks==1 || !glob.empty(),"PFP row K sinks need --sink-glob DIR/rows.%d");
    if (split_runs) {
        const std::string why="PFP --split-runs "+std::to_string(split_runs)+" is not a multiple of the m1 block grid of "+
                              std::to_string(pfprows::M1_BLOCK_RUNS)+" runs: a group that starts off the grid cannot concatenate its m1 blocks";
        check(!(split_runs%pfprows::M1_BLOCK_RUNS),why.c_str());
    }
    // The sinks exist before the columns are consumed, so a bad path dies early.
    std::vector<int> out;
    if (glob.empty()) out.push_back(STDOUT_FILENO);
    else for (U k=0;k<sinks;++k) {
        const std::string name=pfprows::Router::path(glob,k);
        const int fd=::open(name.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0666);
        check(fd>=0,"open PFP row group sink");
        out.push_back(fd);
    }
    signal(SIGPIPE,SIG_DFL); omp_set_dynamic(0);
    File raw(argv[1]); U n=raw.size();
    File refs(dir/"phrases.ref"),parse(dir/"parse.u64"),inverse(dir/"inverse");
    U rb=refs.size(),pb=parse.size(),k=rb/sizeof(pfp::Phrase),m=pb/8;
    check(n && n<(U(1)<<43) && pb==8*m && rb==sizeof(pfp::Phrase)*k && k && k<=m && m<=n && inverse.size()==pb,"PFP column extent");
    check(rawview::budget(n,m,t)<=grant,"PFP rank view exceeds sort memory");
    rawview::Stop stop{nullptr,0};rawview::Mem text;text.open(n,raw.fd);
    rawview::View view(text.bytes(),n,m,int(t),stop);view.from_parse(parse.fd,refs.fd,k,inverse.fd,grant);
    // No further reads from these columns. Retire them before reserving the
    // posting file; keeping unlinked but still open files wouldn't free space.
    refs.close();parse.close();inverse.close();
    for (const char* name:{"phrases.ref","parse.u64","inverse"}) check(pfp::fs::remove(dir/name),"retire consumed PFP column");

    using finiteemit::Emit; using route64::Route;
    U d=view.stats.owned,pg=m+(d-k)/route64::PAGE;
    auto base=Emit::emit_arrays(n,m,t,1,pg,0)+Emit::tile_bytes(0,t);
    check(base<grant,"PFP row buffers exceed sort memory");
    // Two slot arrays: the wave is ranked by a counting sort into the second one.
    U slots=std::min(d,U((grant-base)/48));check(slots,"no PFP dictionary descriptor slots");
    U target=pfp::ceildiv(slots,2*t);
    Route ix(view,text.bytes(),n,m,int(t),stop); Route::Disjoint budget{cap,slots};
    ix.build((dir/"postings").c_str(),target,grant,0,&budget);
    Emit e(ix); e.prepare(slots,grant,true,false);
    check(!e.plan.scans && e.plan.jobs.size()==ix.bands.size(),"PFP ranges require replay");
    // K band sinks: the cat writes a tile straight to its band's sink, so run
    // formation is band-local and the members are cat'd afterwards.
    if (band_sinks>1 || !band_manifest.empty()) {
        check(band_glob.size() && band_manifest.size(),"PFP --band-sinks needs --band-sink-glob and --band-manifest");
        check(band_sinks>=2 && band_sinks<=64,"PFP band sink count");
        const auto& jobs=e.plan.jobs;
        const U nb=std::min<U>(band_sinks,U(jobs.size()));
        std::vector<U> cumd(jobs.size()+1,0),cumr(jobs.size()+1,0);
        for (U j=0;j<jobs.size();++j) {cumd[j+1]=cumd[j]+jobs[j].d;cumr[j+1]=cumr[j]+jobs[j].rows;}
        pfprows::BandFold fold;
        fold.group.resize(nb);fold.go.assign(nb,0);fold.group_of.assign(nb,0);fold.base.assign(nb,0);
        fold.moves_of.assign(nb,0);fold.first_row.resize(nb);fold.end_row.resize(nb);
        // The partition is decided ONCE, in job indices, and both the producer's
        // sink switch and the forwarder's targets come from it: two independent
        // walks of the load profile put the band's last job and its end row one
        // job apart, which shifted a band's row base by that job's rows.
        std::vector<U> bound(nb+1,jobs.size());
        { bound[0]=0;U j=0;for (U r=1;r<=nb;++r) {const U share=cumd[jobs.size()]*r/nb;U b=j;while (b<jobs.size() && (b==j || cumd[b]<share)) ++b;j=b;bound[r]=b;}
          bound[nb]=jobs.size();for (U r=nb;r;--r) if (bound[r-1]>=bound[r]) bound[r-1]=bound[r]-1; }
        std::vector<int> write_end(nb),read_end(nb);
        for (U r=0;r<nb;++r) {
            const U b0=bound[r],b1=bound[r+1];
            std::fprintf(stderr,"PFP_BAND_PLAN r=%llu jobs=%llu..%llu first_row=%llu end_row=%llu\n",(unsigned long long)r,(unsigned long long)b0,(unsigned long long)b1,(unsigned long long)(r?1+cumr[b0]:0),(unsigned long long)(1+cumr[b1]));
            fold.first_row[r]=r?1+cumr[b0]:0;fold.end_row[r]=1+cumr[b1];   // row 0 is the sentinel before job 0
            fold.group[r].intended_row=fold.end_row[r];
            int to[2];check(::pipe(to)==0,"PFP band drain pipe");
            const int flags=fcntl(to[1],F_GETFL);check(flags>=0 && !fcntl(to[1],F_SETFL,flags|O_NONBLOCK),"nonblocking band sink");
            read_end[r]=to[0];write_end[r]=to[1];
            const std::string name=pfprows::BandFold::path(band_glob,r);
            const int fd=::open(name.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0666);
            check(fd>=0,"open PFP band group sink");
            const int of=fcntl(fd,F_GETFL);check(of>=0 && !fcntl(fd,F_SETFL,of|O_NONBLOCK),"nonblocking band group sink");
            fold.out.push_back(fd);
        }
        fold.in=read_end;fold.go[0]=1;fold.base[0]=0;
        std::vector<U> band_last(nb);
        for (U r=0;r<nb;++r) band_last[r]=bound[r+1]-1;
        std::vector<std::thread> forward;
        for (U r=0;r<nb;++r) forward.emplace_back([&fold,r]{try {fold.run(r);} catch (const std::exception& x) {std::fprintf(stderr,"pfp: %s\n",x.what());std::_Exit(1);}});
        U r=0,jr=0,jc=~U(0);
        std::function<int(U,U)> tap=[&](U job,U rows){
            if (job!=jc) {jc=job;jr=0;}
            jr+=rows;
            if (r+1<nb && job==band_last[r] && jr==jobs[job].rows) {::close(write_end[r]);++r;return write_end[r];}
            return write_end[r];
        };
        e.band.tile=&tap;
        try {e.emit_to(write_end[0],jobs.size(),true);} catch (...) {for (U k=0;k<nb;++k) ::close(write_end[k]);for (auto& t:forward) t.join();throw;}
        ::close(write_end[nb-1]);
        for (auto& t:forward) t.join();
        fold.rows_total=1+cumr[jobs.size()];
        fold.runs_total=fold.group[nb-1].first_run+fold.group[nb-1].runs;
        U cover=0;for (U g=0;g<nb;++g) {if (!fold.group[g].rows) std::fprintf(stderr,"PFP_BAND_NOTE g=%llu empty\n",(unsigned long long)g);
            if (fold.group[g].first_run%pfprows::M1_BLOCK_RUNS) std::fprintf(stderr,"PFP_BAND_NOTE g=%llu first_run=%llu off_grid=%llu\n",(unsigned long long)g,(unsigned long long)fold.group[g].first_run,(unsigned long long)(fold.group[g].first_run%pfprows::M1_BLOCK_RUNS));
            if (fold.group[g].first_row!=cover) std::fprintf(stderr,"PFP_BAND_NOTE g=%llu first_row=%llu cover=%llu\n",(unsigned long long)g,(unsigned long long)fold.group[g].first_row,(unsigned long long)cover);
            cover+=fold.group[g].rows;}
        if (cover!=fold.rows_total) std::fprintf(stderr,"PFP_BAND_NOTE rows cover=%llu total=%llu\n",(unsigned long long)cover,(unsigned long long)fold.rows_total);
        fold.save(band_manifest);fold.report(band_manifest);
    } else if (sinks==1 && manifest.empty()) e.emit_to(STDOUT_FILENO,e.plan.jobs.size(),true);
    else {
        pfprows::Router router(out,split_runs,n+1);
        int drain[2];check(::pipe(drain)==0,"PFP row group drain pipe");
        std::thread route([&]{try {router.pump(drain[0]);} catch (const std::exception& x) {std::fprintf(stderr,"pfp: %s\n",x.what());std::_Exit(1);}});
        try {e.emit_to(drain[1],e.plan.jobs.size(),true);} catch (...) {::close(drain[1]);route.join();throw;}
        ::close(drain[1]);route.join();
        if (!manifest.empty()) router.save(manifest);
        router.report(manifest);
    }
    U counted=0,routed=0;for (auto& stripe:ix.stripe) {counted+=stripe.counted;routed+=stripe.routed;}
    check(counted==d && routed==d && e.totals.loaded==d && e.totals.rows==n,"PFP dictionary/output coverage");
    // Producer wall intervals, not additive with concurrent context/writer CPU.
    std::fprintf(stderr,"PFP_ROW_TIME aligned_s=%.6f count_s=%.6f plan_s=%.6f route_s=%.6f order_s=%.6f expand_pipe_s=%.6f"
        " gather_cpu_s=%.6f sort_cpu_s=%.6f key_cmp=%" PRIu64 " text_cmp=%" PRIu64 " waves=%" PRIu64 " tiles=%" PRIu64 "\n",
        view.stats.build,ix.census_s,ix.plan_s,ix.encode_s,e.sort_wall,e.stream_wall,
        e.totals.gather,e.totals.sort-e.totals.gather,e.totals.keycmp,e.totals.textcmp,e.waves,e.tiles);
    std::fprintf(stderr,"PFP_ROWS parse_positions=%" PRIu64 " phrases=%" PRIu64 " offsets=%" PRIu64
        " emit_fixed=%" PRIu64 " slots=%" PRIu64 " ranges=%zu counted=%" PRIu64 " routed=%" PRIu64 " ordered=%" PRIu64
        " groups=%" PRIu64 " shared_groups=%" PRIu64 " rows=%" PRIu64 " posting_bytes=%" PRIu64 "\n",m,k,d,U(base),slots,ix.bands.size(),counted,routed,e.totals.loaded,e.totals.groups,e.totals.multi,n+1,ix.wire);
    return 0;
} catch (const std::exception& e) { pfp::fail(e.what()); }
