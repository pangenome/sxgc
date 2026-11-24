#include "raw_rank_view64.hpp"
#include <charconv>
#include <cinttypes>
#include <string_view>
using namespace rawview;
static U number(const char* arg){std::string_view s(arg);U x=0;auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),x);need(e==std::errc{} && p==s.data()+s.size(),"unsigned decimal argument");return x;}
static std::string decimal(Big v){std::string s;do{s.push_back(char('0'+v%10));v/=10;}while(v);std::reverse(s.begin(),s.end());return s;}
static double cpu(){timespec now{};need(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&now),"process clock");return now.tv_sec+now.tv_nsec*1e-9;}
int main(int ac,char**av){
 if(ac==5 && std::string_view(av[1])=="plan"){auto n=number(av[2]),m=number(av[3]);std::printf("RAW_VIEW_PLAN n=%" PRIu64 " m=%" PRIu64 " pos_bytes=%u rank_bytes=%u arrays=%" PRIu64 "\n",n,m,width(n),width(m),budget(n,m,number(av[4])));return 0;}
 need(ac==7,"raw_rank_view64 RAW INVERSE.u64 THREADS ARRAY_BYTES STOP DEADLINE (or plan N M THREADS)");
 U t=number(av[3]),grant=number(av[4]),end=number(av[6]);need(end<=INT64_MAX && t<=U(omp_get_num_procs()),"team/deadline");Stop stop{av[5],end};stop.check();
 Input raw(av[1]),inv(av[2]);need(inv.size%8==0,"inverse extent");U n=raw.size,m=inv.size/8,arrays=budget(n,m,t);need(arrays<=grant,"view array grant");
 omp_set_dynamic(0);Mem map;map.open(n,raw.fd);View view(map.bytes(),n,m,int(t),stop);double c0=cpu();view.build(inv.fd,grant);double c1=cpu();const auto& s=view.stats;
 for(unsigned i=0;i<t;++i){std::printf("VIEW_STRIPE lane=%u rows=%" PRIu64 " extended=%s plcp_s=%.9f\n",i,view.stripe_count(i),decimal(view.stripe_extended(i)).c_str(),view.stripe_plcp_s(i));}
 std::printf("RAW_VIEW n=%" PRIu64 " m=%" PRIu64 " team=%" PRIu64 " arrays=%" PRIu64 " pos_bytes=%u rank_bytes=%u k=%" PRIu64 " owned=%" PRIu64 " tape=%" PRIu64 " starts_above_u32=%" PRIu64 " max_lcp=%" PRIu64 " extended=%s carried=%s scan_s=%.6f scatter_s=%.6f groups_s=%.6f plcp_s=%.6f rmq_s=%.6f ready_s=%.6f ready_cpu_s=%.6f\n",n,m,t,arrays,width(n),width(m),s.groups,s.owned,s.tape,s.high,s.max_h,decimal(s.extended).c_str(),decimal(s.carried).c_str(),s.scan,s.scatter,s.groups_s,s.plcp,s.minima,s.build,c1-c0);std::fflush(stdout);
 double begin=omp_get_wtime();std::array<viewproof::Digest,viewproof::shards> a{},b{},q{};U high_keys=0;
#pragma omp parallel for num_threads(t) schedule(static) reduction(+:high_keys)
 for(unsigned i=0;i<viewproof::shards;++i){viewproof::Sha ha,hb,hq;const auto& g=view.shard(i);U running=g.begin_key,key=g.carry_key,after=0,heads=0;
  for(U r=viewproof::boundary(m,i)+1,hi=viewproof::boundary(m,i+1);r<=hi;++r){if(!(r%16384))stop.check();
   if(view.is_head(r)){need(!after || after==r,"head interval mismatch");after=view.after_head(r);key=running;running+=8+view.word(r).len;++heads;}
   need(!after || r<after,"short occurrence interval");U p=view.position(r);ha.raw(p,view.previous_lcp(r),view.predecessor(p));hb.navigation(key,view.successor(r));high_keys+=key>UINT32_MAX;
  }
  need(heads==g.group_count && running==g.begin_key+g.bytes,"bad canonical key cursor");
  viewproof::queries(m,i,[&](U x,U y){hq.query(x,y,view.lce(x,y));});a[i]=ha.finish();b[i]=hb.finish();q[i]=hq.finish();
 }
 stop.check();std::printf("VIEW_PROOF rows=%" PRIu64 " keys_above_u32=%" PRIu64 " raw_root=%s nav_root=%s query_root=%s proof_s=%.6f proof_cpu_s=%.6f\n",m,high_keys,viewproof::root(a).c_str(),viewproof::root(b).c_str(),viewproof::root(q).c_str(),omp_get_wtime()-begin,cpu()-c1);
}
