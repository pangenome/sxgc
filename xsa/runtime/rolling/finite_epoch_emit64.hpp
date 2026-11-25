#pragma once
#include "finite_direct_route64.hpp"
#include "finite_epoch64.hpp"
#include "finite_emit64.hpp"
#include <cinttypes>
namespace finiteepoch {
// Persistent job plan coexists with later encoders. Span table, SHA readback buffer and
// fixed control allowances are separate from the emit arena (destroyed between epochs).
inline Big route_extra(){return 2*Big(finiteemit::Planner::LIMIT)*sizeof(finiteemit::Job)+(1<<20);}
inline Big emit_extra(U bands){return Big(bands)*sizeof(routedirect::Key)+Big(CELL)*sizeof(routedirect::Bounds)+(1<<20);}
inline std::string digest(int reader,U wire,rawview::Stop guard){viewproof::Sha h;std::array<uint8_t,65536> buf;for(U at=0;at<wire;){guard.check();U bytes=std::min<U>(buf.size(),wire-at);transfer(reader,false,at,buf.data(),bytes);h.put(buf.data(),bytes);at+=bytes;}return viewproof::hex(h.finish());}
inline U emit(int sink,const char* file,finiteemit::Emit& e,routedirect::Direct& d,U slots,U grant,U other,U temporary_cap=0){auto& ix=d.ix;U limit=temporary_cap?temporary_cap:narrow(2*Big(ix.n)-other);double start=omp_get_wtime();
 auto p=plan(ix.bands.size(),ix.team,std::min(slots,d.d),limit,[&](U b){return std::pair(ix.bands[b].load.d,d.band_wire(b));},temporary_cap?ix.bands.size():LIMIT);need(p.total<=d.file_upper && p.peak<=limit,"epoch simultaneous or total file proof");
 if(!d.charged_encoder_bands){U width=0,base=ix.array_bound;for(U i=0;i<p.size;++i)width=std::max(width,p.part[i].hi-p.part[i].lo);
  d.charge_epoch_encoders(width,grant);std::fprintf(stderr,"EPOCH_ARRAY_CHARGE pre_census=%" PRIu64 " max_active_bands=%" PRIu64 " encoder_bytes=%" PRIu64 " route_arrays=%" PRIu64 "\n",base,width,narrow(Route::encoder_bytes(ix.team,width)),ix.array_bound);}
 e.prepare(slots,grant,true,false,d.retained()+(1<<20));need(e.plan.scans==0 && e.plan.jobs.size()==ix.bands.size() && e.plan.decodes==d.d,"epoch fit-only plan");ix.plan_s+=omp_get_wtime()-start-e.plan.seconds;ix.stop.check();Slot out(file,p.peak);double hash_time=0;
 for(U epoch=0;epoch<p.size;++epoch){auto part=p.part[epoch];U w0=e.waves,rows0=e.totals.rows,pop0=ix.transferred;start=omp_get_wtime();U boundary=0,bytes=d.layout(part.lo,part.hi,boundary);need(bytes==part.bytes && boundary<=bytes,"epoch census/layout mismatch");int fd=out.begin(bytes);ix.plan_s+=omp_get_wtime()-start;
  d.encode<true>(fd,part.lo,part.hi,bytes,boundary);start=omp_get_wtime();out.publish(ix.fd,!temporary_cap);ix.fsync_s+=omp_get_wtime()-start;
  std::string h;if(!temporary_cap){start=omp_get_wtime();h=digest(ix.fd,bytes,ix.stop);hash_time+=omp_get_wtime()-start;}
  e.emit_to(sink,part.hi,true);out.release(ix.fd);U rows=0,desc=0;for(U j=part.lo;j<part.hi;++j){rows+=ix.bands[j].load.rows;desc+=ix.bands[j].load.d;}
  need(e.waves-w0==part.waves && e.totals.rows-rows0==rows,"wrong epoch seam/coverage");if(!temporary_cap)std::fprintf(stderr,"EPOCH_PART i=%" PRIu64 " lo=%" PRIu64 " hi=%" PRIu64 " waves=%" PRIu64 " desc=%" PRIu64 " rows=%" PRIu64 " bytes=%" PRIu64 " interior=%" PRIu64 " populated=%" PRIu64 " sha256=%s\n",epoch,part.lo,part.hi,part.waves,desc,rows,bytes,boundary,ix.transferred-pop0,h.c_str());}
 need(ix.wire==p.total && e.waves==p.waves && d.d<=UINT64_MAX/p.size && ix.fd==-1,"unfinished rolling slot");if(!temporary_cap)std::fprintf(stderr,"EPOCH_INDEX epochs=%" PRIu64 " file_cap=%" PRIu64 " peak_extent=%" PRIu64 " total_extent=%" PRIu64 " nonposting_reserved=%" PRIu64 " encode_offset_visits=%" PRIu64 " hash_readback=%" PRIu64 " hash_s=%.6f\n",p.size,limit,p.peak,p.total,other,d.d*p.size,p.total,hash_time);return p.peak;}
}
