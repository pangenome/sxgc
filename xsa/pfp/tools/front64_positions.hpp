// Low-reservation alternative for front64.cpp (same LE64 dictionary and text-order keys).
// Included after the common Linux mapping/publication, FNV, bin-meter and quota helpers.
namespace posfront {
static constexpr U WORD=64, STRIPE=512, PER=STRIPE/WORD;
static U ceildiv(U n,U d) {return n/d+U(n%d!=0);}
static U scheduled(U n,U m,U threads) {
 ck(n && m && m<=n && threads>=1 && threads<=64,"position front dimensions");
 Big pos=8*Big(m);
 Big common=8*Big(ceildiv(n,WORD))+8*(Big(ceildiv(n,STRIPE))+1)+pos+8*Big(ceildiv(m,WORD));
 // Per-bin head padding, lane histograms, meters and IO buffers fit the same control allowance.
 Big bytes=std::max(Big(n),pos)+common+(4ull<<20)+threads*(2ull<<20);
 ck(bytes<=PTRDIFF_MAX,"position front address extent");return U(bytes);
}
struct Bits {
 U n,words,blocks;U *bits,*before;
 Bits(U size):n(size),words(ceildiv(n,WORD)),blocks(ceildiv(n,STRIPE)),
     bits((U*)anon(8*words)),before((U*)anon(8*(blocks+1))) {}
 ~Bits(){munmap(bits,8*words);munmap(before,8*(blocks+1));}
 Bits(const Bits&)=delete;
 // The caller supplies a set bit. Exclusive rank in text order, incl. the mandatory start at zero.
 U ordinal(U p) const {
  U b=p/STRIPE,w=p/WORD,k=before[b];
  for(U j=b*PER;j<w;++j)k+=std::popcount(bits[j]);
  return k+std::popcount(bits[w]&((U(1)<<(p%WORD))-1));
 }
 U following(U p) const {
  U w=p/WORD,remaining=bits[w]&((UINT64_MAX<<(p%WORD))<<1);
  if(remaining)return WORD*w+std::countr_zero(remaining);
  // Inspect at most the rest of this stripe and the next one. Binary skip an arbitrarily long hole.
  U j=w+1,e=std::min(words,(p/STRIPE+2)*PER);
  for(;j<e;++j)if(bits[j])return j*WORD+std::countr_zero(bits[j]);
  if(j==words)return n;
  U b=j/PER,prior=before[b];
  auto q=std::upper_bound(before+b+1,before+blocks+1,prior);
  if(q==before+blocks+1)return n;
  U found=U(q-before)-1;
  for(j=found*PER;j<std::min(words,(found+1)*PER);++j)if(bits[j])return j*WORD+std::countr_zero(bits[j]);
  fail("invalid start rank directory");
 }
};
// Bin number already fixes the first 12 prefix bits. Store as many FOLLOWING bits as fit
// above the complete unsigned physical start. Never truncate a global coordinate or key.
struct Project {
 unsigned position_bits,projection_bits;U mask;
 explicit Project(U n):position_bits(unsigned(std::bit_width(n-1))),
  projection_bits(std::min(52u,64-position_bits)),mask(position_bits==64?UINT64_MAX:(U(1)<<position_bits)-1){}
 U entry(U p,U prefix)const{
  ck((p&mask)==p,"unrepresentable source position");
  if(!projection_bits)return p;
  return (((prefix<<12)>>(64-projection_bits))<<position_bits)|p;
 }
 U pos(U entry) const{return entry&mask;}
};
static void run(const char* input,const fs::path& out,U threads,U grant,U scratch,bool link_manifest=false){
 ck(threads>=1 && threads<=64 && threads<=U(omp_get_num_procs()),"worker extent");
 omp_set_dynamic(0);omp_set_num_threads(int(threads));int actual=0;
#pragma omp parallel
 {
#pragma omp single
  actual=omp_get_num_threads();
 }
 ck(U(actual)==threads,"wrong OpenMP team");
 int fd=open(input,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);struct stat st{};
 ck(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>0 && !fs::exists(out),"nonempty regular input and fresh destination");
 U n=U(st.st_size);ck(scheduled(n,1,threads)<=grant,"raw map and start bitmap exceed grant");
 auto raw=(const uint8_t*)mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);ck(raw!=MAP_FAILED,"raw mapping");
 double t0=omp_get_wtime();Bits starts(n);Project project(n);
 auto prefix=[&](U p){U bytes=0;std::memcpy(&bytes,raw+p,std::min<U>(8,n-p));return __builtin_bswap64(bytes);};
 std::vector<std::array<U,BINS>> hist(threads);std::vector<U> count(threads),base(threads);
 // Disjoint bitmap words, one pass over possible FNV starts. Use the same lane cuts when scattering.
#pragma omp parallel
 {
  U t=omp_get_thread_num(),local=0;auto& h=hist[t];
  for(U b=cut(starts.blocks,t,threads),end=cut(starts.blocks,t+1,threads);b<end;++b){
   if(!(b%2048)){guard();}
   starts.before[b]=local;
   for(U w=b*PER;w<std::min(starts.words,(b+1)*PER);++w){
    U bits=0,hi=std::min(n,WORD*(w+1));
    for(U p=WORD*w;p<hi;++p){if(p==0 || (n-p>=W && trigger(raw+p))){
      bits|=U(1)<<(p%WORD);++h[prefix(p)>>52];++local;
    }}starts.bits[w]=bits;
   }
  }
  count[t]=local;
 }
 U m=0;for(U t=0;t<threads;++t){base[t]=m;m+=count[t];}starts.before[starts.blocks]=m;
 U arrays=scheduled(n,m,threads);ck(arrays<=grant && 8*Big(m)<=std::min<Big>(scratch,2*Big(n)),"front arrays or key column exceed grant");
 std::array<U,BINS+1> bins{},heads_at{},col{};U present=0,maxbin=0;
 for(U b=0;b<BINS;++b){U s=bins[b];for(auto& h:hist){U c=h[b];h[b]=s;s+=c;}bins[b+1]=s;
  U span=s-bins[b];present+=span!=0;maxbin=std::max(maxbin,span);heads_at[b+1]=heads_at[b]+ceildiv(span,WORD);
 }
 ck(bins.back()==m && heads_at.back()<=ceildiv(m,WORD)+BINS-1,"bin extents");
 double ts=omp_get_wtime();auto entries=(U*)anon(8*m);auto heads=(U*)anon(8*heads_at.back());U above=0;
#pragma omp parallel reduction(+:above)
 {
  U t=omp_get_thread_num();auto& h=hist[t];
  for(U b=cut(starts.blocks,t,threads),end=cut(starts.blocks,t+1,threads);b<end;++b){
   if(!(b%2048)){guard();}
   starts.before[b]+=base[t];
   for(U w=b*PER;w<std::min(starts.words,(b+1)*PER);++w){
    for(U x=starts.bits[w];x;x&=x-1){U p=w*WORD+std::countr_zero(x),pr=prefix(p);
     entries[h[pr>>52]++]=project.entry(p,pr);above+=p>UINT32_MAX;
    }
   }
  }
 }
 auto length=[&](U p,U next){return next-p+U(next<n)*W;};
 auto less=[&](U a,U b){
  if(((a^b)&~project.mask)!=0)return a<b;
  U x=project.pos(a),y=project.pos(b),px=prefix(x),py=prefix(y);if(px!=py)return px<py;
  U nx=starts.following(x),ny=starts.following(y),lx=length(x,nx),ly=length(y,ny),len=std::min(lx,ly);
  if(len>8){int c=std::memcmp(raw+x+8,raw+y+8,len-8);if(c)return c<0;}
  return lx!=ly?lx<ly:nx!=n && ny==n;
 };
 std::vector<BinMeter> meters(BINS);double tp=omp_get_wtime();U k=0,owned=0,term=0,longest=0;
#pragma omp parallel for schedule(dynamic,1) reduction(+:k,owned,term) reduction(max:longest)
 for(U b=0;b<BINS;++b){guard();U lo=bins[b],hi=bins[b+1],calls=0;double begin=omp_get_wtime();
  std::sort(entries+lo,entries+hi,[&](U x,U y){if(!(++calls%BLOCK))guard();return less(x,y);});
  double sorted=omp_get_wtime();U bytes=0;U* head=heads+heads_at[b];
  for(U j=lo;j<hi;++j){if(!(j%65536))guard();if(j==lo || less(entries[j-1],entries[j])){
    head[(j-lo)/WORD]|=U(1)<<((j-lo)%WORD);U p=project.pos(entries[j]),next=starts.following(p),len=length(p,next);
    ++k;owned+=next-p;term+=next==n;longest=std::max(longest,len);bytes+=8+len;
  }}col[b+1]=bytes;meters[b].order_lane=omp_get_thread_num();meters[b].sort=sorted-begin;meters[b].label=omp_get_wtime()-sorted;
 }
 double ordered=omp_get_wtime();Big total=0;for(U b=1;b<=BINS;++b){total+=col[b];ck(total<=INT64_MAX,"dictionary extent");col[b]=U(total);}
 ck(term==1 && total==Big(owned)+16*Big(k)-8 && total+8*Big(m)+(link_manifest?48:0)<=std::min<Big>(scratch,2*Big(n)),"column scratch/geometry");
 guard();fs::path partial=out;partial+=".partial";ck(fs::create_directory(partial),"fresh partial directory");int dict=file(partial/"dictionary.lex64",U(total));double td=omp_get_wtime();
#pragma omp parallel for schedule(dynamic,1)
 for(U b=0;b<BINS;++b){double begin=omp_get_wtime();Buffer dest{dict,col[b],0,{}};U lo=bins[b],hi=bins[b+1];
  const U* head=heads+heads_at[b];for(U j=lo;j<hi;++j){if(!(j%65536))guard();if((head[(j-lo)/WORD]>>((j-lo)%WORD))&1){
    U p=project.pos(entries[j]),next=starts.following(p),len=length(p,next),tag=(len<<1)|U(next==n);dest.put(&tag,8);dest.put(raw+p,len);
  }}dest.flush();ck(dest.at==col[b+1],"dictionary range end");meters[b].publish_lane=omp_get_thread_num();meters[b].publish=omp_get_wtime()-begin;
 }
 double published=omp_get_wtime();finish(dict);ck(!munmap(const_cast<uint8_t*>(raw),n) && !close(fd),"release raw before scatter");double tw=omp_get_wtime();
 // The text map is gone. Recompute just the distinct record spans from the start bitmap, not their contents.
 auto keys=(U*)anon(8*m);U high_keys=0;
#pragma omp parallel for schedule(dynamic,1) reduction(+:high_keys)
 for(U b=0;b<BINS;++b){U at=col[b],key=at,lo=bins[b],hi=bins[b+1];const U* head=heads+heads_at[b];
  for(U j=lo;j<hi;++j){if(!(j%65536))guard();U p=project.pos(entries[j]);if((head[(j-lo)/WORD]>>((j-lo)%WORD))&1){
    key=at;at+=8+length(p,starts.following(p));
   }U ordinal=starts.ordinal(p);ck(ordinal<m,"text ordinal extent");keys[ordinal]=key;high_keys+=key>UINT32_MAX;
  }ck(at==col[b+1],"scatter range end");
 }
 munmap(entries,8*m);munmap(heads,8*heads_at.back());int keyfd=file(partial/"keys.u64",8*m);writeat(keyfd,(const uint8_t*)keys,8*m,0);finish(keyfd);munmap(keys,8*m);syncdir(partial);guard();
 if(link_manifest){
  // Some quota filesystems have hard links but no RENAME_NOREPLACE for a
  // directory. OUT in this explicit ABI is an exclusive regular-file commit
  // record, not a directory. Neither column is read until BOTH are durable.
  U seal[]={0x3143465058ull,n,m,k,owned,U(total)};fs::path stamp=partial/"manifest";
  int sealed=file(stamp,sizeof(seal));writeat(sealed,(const uint8_t*)seal,sizeof(seal),0);
  finish(sealed);syncdir(partial);guard();ck(!link(stamp.c_str(),out.c_str()),"exclusive column manifest");
 }else ck(syscall(SYS_renameat2,AT_FDCWD,partial.c_str(),AT_FDCWD,out.c_str(),RENAME_NOREPLACE)==0,"publish columns");
 syncdir(out.has_parent_path()?out.parent_path():fs::path("."));double end=omp_get_wtime();
 report_bins(bins,meters,actual,false,ordered-tp);report_bins(bins,meters,actual,true,published-td);
 std::printf("FRONT64_LAYOUT position_bits=%u projected_prefix_bits=%u start_bytes=%llu rank_anchor_bytes=%llu entry_bytes=%llu head_bytes=%llu\n",
   project.position_bits,project.projection_bits,(unsigned long long)(8*starts.words),(unsigned long long)(8*(starts.blocks+1)),(unsigned long long)(8*m),(unsigned long long)(8*heads_at.back()));
 std::printf("FRONT64 n=%llu m=%llu k=%llu descriptors=%llu max_phrase=%llu starts_above_u32=%llu keys_above_u32=%llu dictionary_bytes=%llu key_bytes=%llu arrays=%llu team=%d nonempty_bins=%llu max_bin=%llu scan_s=%.6f partition_s=%.6f sort_dedup_s=%.6f publish_dict_s=%.6f publish_keys_s=%.6f raw_s=%.6f\n",(unsigned long long)n,(unsigned long long)m,(unsigned long long)k,(unsigned long long)owned,(unsigned long long)longest,(unsigned long long)above,(unsigned long long)high_keys,(unsigned long long)total,(unsigned long long)(8*m),(unsigned long long)arrays,actual,(unsigned long long)present,(unsigned long long)maxbin,ts-t0,tp-ts,td-tp,tw-td,end-tw,end-t0);
}
} // namespace posfront
