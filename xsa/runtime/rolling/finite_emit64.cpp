#include "finite_emit64.hpp"
#include "finite_epoch_emit64.hpp"
#include <charconv>
#include <cinttypes>
#include <csignal>
#include <string_view>
using namespace finiteemit;
static U decimal(const char* x){U v=0;std::string_view s(x);auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),v);need(p==s.data()+s.size() && e==std::errc{},"unsigned decimal");return v;}
// Private production CLI: only the constructor, no deadline, benchmark or readback.
static int stream(char** av){
 rawview::Input raw(av[2]),inverse(av[3]);U n=raw.size,m=inverse.size/8,t=decimal(av[5]),memory=decimal(av[6]),cap=decimal(av[7]);
 need(n<(U(1)<<43) && inverse.size%8==0 && t>=1 && t<=64 && t<=U(omp_get_num_procs()),"stream extent or thread count");
 need(rawview::budget(n,m,t)<=memory && cap>0,"raw rank view exceeds sort memory or no posting space");
 signal(SIGPIPE,SIG_DFL);omp_set_dynamic(0);rawview::Stop stop{nullptr,0};
 rawview::Mem text;text.open(n,raw.fd);rawview::View view(text.bytes(),n,m,t,stop);view.build(inverse.fd,memory);
 Route ix(view,text.bytes(),n,m,t,stop);routedirect::Direct router(ix);
 // Plan from THIS parse, not an archive. Leave room for the packed rank view;
 // the census determines the largest indivisible range before the first row.
 U d=view.stats.owned,pg=narrow(Big(m)+(d-view.stats.groups)/PAGE);
 Big fixed=Emit::emit_arrays(n,m,t,1,pg,0)+finiteepoch::emit_extra(1);
 need(fixed<memory,"insufficient sort memory for row emission; increase --sort-memory");
 U slots=std::min({d,std::max<U>(65536,n/100),U((memory-fixed)/16)});
 need(slots,"no descriptor space; increase --sort-memory");
 router.prepare(std::max<U>(1,slots/(2*t)),memory,inverse.size,routedirect::RoutingMemory::staged_epochs,finiteepoch::route_extra(),inverse.size+cap);
 slots=std::max(slots,router.max_desc);
 Emit emit(ix,&router.skip);finiteepoch::emit(STDOUT_FILENO,av[4],emit,router,slots,memory,inverse.size,cap);
 return 0;
}
int main(int ac,char** av){if(ac==8 && std::string_view(av[1])=="stream")return stream(av);bool cached=ac>1 && std::string_view(av[1])=="cached";bool staged=ac>1 && std::string_view(av[1])=="staged-epochs";bool epochs=staged || (ac>1 && std::string_view(av[1])=="epochs");bool tiled=epochs || cached || (ac>1 && std::string_view(av[1])=="tiled");bool sampled=tiled || (ac>1 && std::string_view(av[1])=="sampled");if(sampled){--ac;++av;}if(ac==(epochs?10:9) && std::string_view(av[1])=="plan"){U n=decimal(av[2]),m=decimal(av[3]),k=decimal(av[4]),u=decimal(av[5]),nb=decimal(av[6]),slots=decimal(av[7]),t=decimal(av[8]);
 need(n<(U(1)<<43) && k && k<=m && k<=u && u<=n && slots && slots<=u && nb && (sampled || nb<=CELL),"plan geometry");U pg=narrow(Big(m)+(u-k)/PAGE);
 if(sampled){U vmax=narrow(Big(pg)*PAGE),desired=std::min(routedirect::MAX_BANDS,divup(u,nb)),other=epochs?decimal(av[9]):narrow(8*Big(m)+(4<<20));need(other>=8*m && Big(other)<2*Big(n),"direct other files");U limit=epochs?desired:routedirect::allowed(vmax,u,nb,t,narrow(2*Big(n)-other));
  U route=narrow(routedirect::Direct::work_arrays(n,m,t,desired,pg,std::min(u,desired*routedirect::OVER))+(epochs?finiteepoch::route_extra():0)),emit=narrow(Emit::emit_arrays(n,m,t,desired,pg,slots)+8*Big(desired)+(tiled?Emit::tile_bytes(slots,t):0)+(cached?t*finitecache::PER_LANE:0)+(epochs?finiteepoch::emit_extra(desired):0));
  if(staged){U base=narrow(routedirect::Direct::work_arrays(n,m,t,desired,pg,std::min(u,desired*routedirect::OVER),0)+finiteepoch::route_extra());std::printf("EPOCH_STAGE_BUDGET pre_census=%" PRIu64 " per_active_band=%" PRIu64 " emission_upper=%" PRIu64 " all_encoder_route=%" PRIu64 "\n",base,narrow(Route::encoder_bytes(t,1)),emit,route);}
  if(epochs){U held=other;std::printf("EPOCH_PLAN desired_bands=%" PRIu64 " total_wire_upper=%" PRIu64 " live_posting_cap=%" PRIu64 " other_reserved=%" PRIu64 " max_walks=%" PRIu64 " array_upper=%" PRIu64 " conditional_on_wave_fit=1\n",desired,narrow(routedirect::upper(vmax,u,desired,t)),2*n-held,held,finiteepoch::LIMIT,std::max(route,emit));return 0;}
  std::printf("DIRECT_PLAN max_bands=%" PRIu64 " bands_if_upper_dimensions=%" PRIu64 " files_if_upper_dimensions=%" PRIu64 " route_array_upper=%" PRIu64 " emit_array_upper=%" PRIu64 " array_upper=%" PRIu64 "\n",desired,limit,narrow(other+routedirect::upper(vmax,u,limit,t)),route,emit,std::max(route,emit));return 0;}
 U routing=narrow(Route::build_arrays(n,m,t,nb,pg)),emitting=narrow(Emit::emit_arrays(n,m,t,nb,pg,slots));need(Big(pg)*PAGE<=UINT64_MAX,"virtual extent");
 std::printf("EMIT_PLAN pages_upper=%" PRIu64 " bands=%" PRIu64 " slots=%" PRIu64 " per_team_marks=%" PRIu64 " route_arrays_upper=%" PRIu64 " emit_arrays_upper=%" PRIu64 " array_upper=%" PRIu64 "\n",pg,nb,slots,narrow(t*Marks::bytes(m)),routing,emitting,std::max(routing,emitting));return 0;}
 need(ac==11,"finite_emit64 RAW INVERSE.u64 NEW_POSTINGS WORKERS ARRAY_BYTES OTHER_FILE_BYTES ROW_TARGET TOTAL_DESCRIPTOR_SLOTS STOP DEADLINE > pipe");
 struct stat st{};need(!fstat(STDOUT_FILENO,&st) && S_ISFIFO(st.st_mode),"row sink must be a pipe, not a row file");signal(SIGPIPE,SIG_IGN);omp_set_dynamic(0);
 U t=decimal(av[4]),grant=decimal(av[5]),other=decimal(av[6]),target=decimal(av[7]),slots=decimal(av[8]),end=decimal(av[10]);need(end<=INT64_MAX && t<=U(omp_get_num_procs()),"team/deadline");rawview::Stop stop{av[9],end};stop.check();
 rawview::Input raw(av[1]),inv(av[2]);U n=raw.size,m=inv.size/8;need(!(inv.size%8) && n<(U(1)<<43) && rawview::budget(n,m,t)<=grant && other>=inv.size && Big(other)<=Big(2)*n,"stream geometry/grant/scratch");
 rawview::Mem text;text.open(n,raw.fd);rawview::View v(text.bytes(),n,m,int(t),stop);v.build(inv.fd,grant);Route ix(v,text.bytes(),n,m,int(t),stop);std::unique_ptr<routedirect::Direct> direct;
 if(sampled){direct=std::make_unique<routedirect::Direct>(ix);if(epochs)direct->prepare(target,grant,other,staged?routedirect::RoutingMemory::staged_epochs:routedirect::RoutingMemory::whole_epoch_index,finiteepoch::route_extra());else direct->build(av[3],target,grant,other);}else ix.build(av[3],target,grant,other);
 auto ranges=[&](){std::fprintf(stderr,"DIRECT_RANGES target_bands=%" PRIu64 " bands=%zu samples=%" PRIu64 " max_range_descriptors=%" PRIu64 " max_range_rows=%" PRIu64 " guaranteed_wire_upper=%" PRIu64 " explicit_write=%" PRIu64 " fragment_bytes=%" PRIu64 " sample_s=%.6f\n",direct->target_bands,ix.bands.size(),direct->samples,direct->max_desc,direct->max_rows,direct->file_upper,direct->explicit_write,direct->fragment_bytes,direct->sample_s);};if(sampled && !epochs)ranges();
 Emit e(ix,direct?&direct->skip:nullptr);U peak=ix.wire;if(epochs){peak=finiteepoch::emit(STDOUT_FILENO,av[3],e,*direct,slots,grant,other);ranges();}else e.run(STDOUT_FILENO,slots,grant,tiled,cached);auto& c=e.totals;
 if(cached){auto& a=c.order;std::fprintf(stderr,"CACHE_ORDER payload=%u cache_capacity=%" PRIu64 " per_lane_bytes=%" PRIu64 " prefix_sorts=%" PRIu64 " cache_runs=%" PRIu64 " cache_desc=%" PRIu64 " reprobes=%" PRIu64 " whole_equal=%" PRIu64 " slow_runs=%" PRIu64 " slow_desc=%" PRIu64 " slow_comps=%" PRIu64 "\n",e.payload,finitecache::K,finitecache::PER_LANE,a.prefix_sorts,a.cache_runs,a.cache_desc,a.reprobes,a.whole_equal,a.slow_runs,a.slow_desc,a.slow_comps);}
 if(tiled)std::fprintf(stderr,"EMIT_TILES grain=%" PRIu64 " tiles=%" PRIu64 " extension_descriptors=%" PRIu64 " max_tile_descriptors=%" PRIu64 " max_tile_rows=%" PRIu64 " tiles_above_ring=%" PRIu64 " plan_s=%.6f\n",Emit::GRAIN,e.tiles,e.extended_cuts,e.max_piece,e.max_piece_rows,e.over_queue,e.tile_plan_s);
 std::fprintf(stderr,"FINITE_EMIT n=%" PRIu64 " m=%" PRIu64 " team=%" PRIu64 " k=%" PRIu64 " descriptors=%" PRIu64 " groups=%" PRIu64 " rows=%" PRIu64 " multi=%" PRIu64 " bitmap_groups=%" PRIu64 " max_tie=%" PRIu64 " max_mass=%" PRIu64 " wire=%" PRIu64 " populated=%" PRIu64 " counted=%" PRIu64 " arrays=%" PRIu64 " bands=%zu leaves=%zu local_scans=%" PRIu64 " exact_bypass=%" PRIu64 " decoded=%" PRIu64 " refine_s=%.6f waves=%" PRIu64 " packets=%" PRIu64 " view_s=%.6f census_s=%.6f layout_s=%.6f encode_s=%.6f fsync_s=%.6f order_wall_s=%.6f publish_wall_s=%.6f order_lane_s=%.6f gather_lane_s=%.6f produce_lane_s=%.6f queue_full_lane_s=%.6f cat_wait_s=%.6f cat_send_s=%.6f writes=%" PRIu64 " eagain=%" PRIu64 " row_bytes=%" PRIu64 "\n",n,m,t,v.stats.groups,v.stats.owned,c.groups,n+1,c.multi,c.bitmaps,c.max_tie,c.max_mass,ix.wire,ix.transferred,peak+other,e.arrays,ix.bands.size(),e.plan.jobs.size(),e.plan.scans,e.plan.exact,e.plan.decodes,e.plan.seconds,e.waves,e.packets,v.stats.build,ix.census_s,ix.plan_s,ix.encode_s,ix.fsync_s,e.sort_wall,e.stream_wall,c.sort,c.gather,c.produce,e.queue_blocked,e.sink_stats.wait,e.sink_stats.send_s,e.sink_stats.writes,e.sink_stats.again,e.sink_stats.bytes);
}
