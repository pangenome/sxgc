#include "finite_route64.hpp"
#include <charconv>
#include <cinttypes>
#include <string_view>
using namespace route64;
static U number(const char* x){U v=0;std::string_view s(x);auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),v);need(e==std::errc{} && p==s.data()+s.size(),"unsigned argument");return v;}
int main(int ac,char** av){
 need(ac==10,"finite_route64 RAW INVERSE NEW_PAYLOAD THREADS ARRAY_BYTES OTHER_FILE_BYTES ROW_TARGET STOP DEADLINE");
 U t=number(av[4]),grant=number(av[5]),other=number(av[6]),target=number(av[7]),end=number(av[9]);need(end<=INT64_MAX && t<=U(omp_get_num_procs()),"team/deadline");rawview::Stop stop{av[8],end};stop.check();
 rawview::Input raw(av[1]),inv(av[2]);U n=raw.size,m=inv.size/8;need(!(inv.size%8) && rawview::budget(n,m,t)<=grant && other>=inv.size && Big(other)<=2*Big(n),"input geometry/grant/scratch");
 omp_set_dynamic(0);rawview::Mem text;text.open(n,raw.fd);rawview::View v(text.bytes(),n,m,int(t),stop);v.build(inv.fd,grant);
 Route index(v,text.bytes(),n,m,int(t),stop);index.build(av[3],target,grant,other);double proof_start=omp_get_wtime();std::vector<viewproof::Digest> roots(index.bands.size());U oversize=0,largest=0;
#pragma omp parallel for schedule(dynamic,1) num_threads(t) reduction(+:oversize) reduction(max:largest)
 for(U b=0;b<index.bands.size();++b){auto& band=index.bands[b];viewproof::Sha sha;sha.navigation(band.lo,band.hi);sha.navigation(band.load.d,band.load.rows);U count=0,rows=0,last=0;
  index.visit(b,[&](U r,U off,U token){auto w=v.word(r);U owned=w.len-(w.next?8:0);need(off<owned && (!count || token>last),"decoded offset/order");last=token;
   unsigned cell=Route::cell_at(text.bytes()+w.p,w.len,off);need(cell>=band.lo && cell<band.hi,"wrong routing band");sha.navigation(r,off);++count;rows+=v.after_head(r)-r;});
  need(count==band.load.d && rows==band.load.rows,"decoded band coverage");roots[b]=sha.finish();oversize+=rows>target;largest=std::max(largest,count);}
 viewproof::Sha sha;for(auto& h:roots)sha.put(h.data(),h.size());stop.check();
 std::printf("FINITE_ROUTE n=%" PRIu64 " m=%" PRIu64 " team=%" PRIu64 " k=%" PRIu64 " descriptors=%" PRIu64 " bands=%zu oversize_cells=%" PRIu64 " max_band_descriptors=%" PRIu64 " pages=%" PRIu64 " directory=%" PRIu64 " arrays=%" PRIu64 " wire=%" PRIu64 " populated=%" PRIu64 " bound_files=%" PRIu64 " root=%s view_s=%.6f census_s=%.6f plan_s=%.6f encode_s=%.6f sync_s=%.6f proof_s=%.6f\n",n,m,t,v.stats.groups,v.stats.owned,index.bands.size(),oversize,largest,index.pages.count,U(Pages::bytes(index.pages.count)),index.array_bound,index.wire,index.transferred,index.wire+other,viewproof::hex(sha.finish()).c_str(),v.stats.build,index.census_s,index.plan_s,index.encode_s,index.fsync_s,omp_get_wtime()-proof_start);
}
