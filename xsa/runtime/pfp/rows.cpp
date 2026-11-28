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
    std::vector<int> sink;      // one fd per group, opened before emission
    std::vector<Group> group;   // group.back() is the one being written
    U target = 0;               // --split-runs: runs per group, 0 = equal share
    U total = 0;                // rows the producer will write (n+1)
    U rows = 0, runs = 0;       // global row count, global index of the open run
    unsigned prev_byte = 0;
    size_t g = 0;

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
            if (z > 0) { p += z; left -= size_t(z); }
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
        std::vector<char> buf(sizeof(finiteemit::Row) << 17);   // 1.5 MiB, a whole number of rows
        size_t fill = 0;
        for (;;) {
            ssize_t z = ::read(in, buf.data() + fill, buf.size() - fill);
            if (z < 0) { pfp::check(errno == EINTR, "PFP row group drain read"); continue; }
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
        std::fprintf(stderr, "PFP_ROW_SPLIT sinks=%zu rows=%llu runs=%llu groups=%zu split_runs=%llu manifest=%s\n",
                     sink.size(), (unsigned long long)rows, (unsigned long long)(runs + 1), group.size(),
                     (unsigned long long)target, manifest.c_str());
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

int main(int argc,char** argv) try {
    using pfp::U; using pfp::File; using pfp::fs::path; using pfp::check;
    check(argc>=6,"usage: pfp_rows RAW PRIVATE_COLUMNS WORKERS SORT_BYTES POSTING_BYTES"
                  " [--sinks K] [--sink-glob DIR/rows.%d] [--split-runs N] [--group-manifest PATH]");
    path dir=argv[2]; U t=pfp::number(argv[3]), grant=pfp::number(argv[4]), cap=pfp::number(argv[5]);
    // I1 step 2: route the ordered stream to K sinks, one group per sink.
    U sinks=1,split_runs=0; std::string glob,manifest;
    for (int i=6;i<argc;++i) {
        const std::string_view key=argv[i];
        check(i+1<argc,"PFP row option needs a value");
        if (key=="--sinks") sinks=pfp::number(argv[++i]);
        else if (key=="--sink-glob") glob=argv[++i];
        else if (key=="--split-runs") split_runs=pfp::number(argv[++i]);
        else if (key=="--group-manifest") manifest=argv[++i];
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
    // K=1 with no manifest writes straight to stdout: today's path, byte for byte.
    if (sinks==1 && manifest.empty()) e.emit_to(STDOUT_FILENO,e.plan.jobs.size(),true);
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
