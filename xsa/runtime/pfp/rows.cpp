// Private row43 producer. Inputs live in the builder's PRIVATE scratch, not
// alongside RAW. The lexical census replaces reparsing/re-deduplicating RAW.
#include "io.hpp"
#include "../rolling/finite_emit64.hpp"
#include <csignal>

int main(int argc,char** argv) try {
    using pfp::U; using pfp::File; using pfp::fs::path; using pfp::check;
    check(argc==6,"usage: pfp_rows RAW PRIVATE_COLUMNS WORKERS SORT_BYTES POSTING_BYTES");
    path dir=argv[2]; U t=pfp::number(argv[3]), grant=pfp::number(argv[4]), cap=pfp::number(argv[5]);
    check(t && t<=64 && t<=U(omp_get_num_procs()) && cap,"PFP row worker/scratch extent");
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
    U slots=std::min(d,U((grant-base)/32));check(slots,"no PFP dictionary descriptor slots");
    U target=pfp::ceildiv(slots,2*t);
    Route ix(view,text.bytes(),n,m,int(t),stop); Route::Disjoint budget{cap,slots};
    ix.build((dir/"postings").c_str(),target,grant,0,&budget);
    Emit e(ix); e.prepare(slots,grant,true,false);
    check(!e.plan.scans && e.plan.jobs.size()==ix.bands.size(),"PFP ranges require replay");
    e.emit_to(STDOUT_FILENO,e.plan.jobs.size(),true);
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
