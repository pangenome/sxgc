// Bounded seam classes over the padded RLBWT. No expanded-text scan or LF
// fallback: every row enumerated belongs to an admitted seam interval.
// Externalized: no run-scale vector is resident. One sequential RLE decode
// writes normalized temp columns (char, len) and a per-char run-id list; the
// shared two-pass external form (bit6/ext_columns.hpp) provides the record
// column (starts/lfBase/char), sorted head seeds and run_of(). The phi
// translation is answered from the sorted seeds plus .ssa_t tails (O(log r)
// bounded pread windows), replacing the former 16r sorted-pair array.
struct SeamRepair {
 struct Class { U lo,hi,first,before=0,after=0;std::vector<Run> runs; };
 Source& source;
 std::vector<Class> classes;
 U limit,total=0,depth=0;
 static void refuse(const char* kind,U size,U limit) {
  fprintf(stderr,"CYCLIC_SEAM_REFUSED %s=%llu limit=%llu policy=max(1000,raw_r/1000); no O(n) fallback\n",kind,(unsigned long long)size,(unsigned long long)limit);
  throw std::runtime_error("cyclic seam repair exceeds compressed-work policy; no O(n) fallback");
 }
 explicit SeamRepair(Source& source,bool certified,const std::string& workPrefix,
                   const std::string& pfText="",const std::string& pfCkpt="")
  :source(source),limit(std::max<U>(1000,source.r/1000)) {
  if(certified)return;
  const U N=source.n,n=N-source.w1,r=source.r;
  const char* env=getenv("SLIM_PF_THREADS");
  int pfThreads=env? std::max(1,std::min(64,atoi(env))) : 16;
  // ---- pass 1: normalized temp columns (char u8, len u32) + counts ----
  std::string paPath=workPrefix+".xpa",plPath=workPrefix+".xplen",pcPath=workPrefix+".xpc";
  std::array<U,256> counts{},nruns{},C{},segStart{};
  {
   std::ifstream b(source.prefix+".rlebwt",std::ios::binary);check(bool(b),"open padded RLE");
   ExtWriter wa,wl;wa.open(paPath);wl.open(plPath);
   U rows=0;
   for(U i=0;i<r;++i) {
    U c=0,len=0,word;
    do {word=integer(b,4);if(len)check(c==(word&255),"RLE continuation character");c=word&255;len+=(word>>8)&0x7fffff;}while(word>>31);
    check(len&&len<=N-rows&&len<=UINT32_MAX,"padded RLE length (or run too long for the parse-free walk)");
    uint8_t cc=(uint8_t)c;wa.put(&cc,1);wl.put(&len,4);
    counts[c]+=len;++nruns[c];rows+=len;
   }
   check(rows==N&&b.peek()==EOF,"padded RLE size");
   wa.finish();wl.finish();
  }
  {U acc=0;for(U c=0;c<256;++c){segStart[c]=acc;acc+=nruns[c];}}
  // ---- pass 2: per-char run-id list (segments laid out by char) ----
  {
   std::ifstream b(source.prefix+".rlebwt",std::ios::binary);check(bool(b),"open padded RLE");
   int fd=open(pcPath.c_str(),O_CREAT|O_EXCL|O_RDWR,0666);check(fd>=0,"create per-char run list");
   std::vector<U> cnt(256,0);std::vector<std::vector<U>> buf(256);
   auto flushc=[&](U c){if(buf[c].empty())return;ext_pwrite(fd,buf[c].data(),8*buf[c].size(),8*(segStart[c]+cnt[c]),"per-char list write");cnt[c]+=buf[c].size();buf[c].clear();};
   for(U i=0;i<r;++i) {
    U c=0,len=0,word;
    do {word=integer(b,4);if(len)check(c==(word&255),"RLE continuation character");c=word&255;len+=(word>>8)&0x7fffff;}while(word>>31);
    (void)len;
    buf[c].push_back(i);if(buf[c].size()>=4096)flushc(c);
   }
   for(U c=0;c<256;++c)flushc(c);
   check(close(fd)==0,"close per-char run list");
  }
  // ---- shared external structure: records + sorted seeds ----
  ExtCol aSrc,lSrc,chrCol,ssaCol,ssaT;
  aSrc.open_ro(paPath,1,r,0,"temp padded a");
  lSrc.open_ro(plPath,4,r,0,"temp padded len");
  chrCol.open_ro(pcPath,8,r,0,"per-char run list");
  ssaCol.open_ro(source.prefix+".ssa",8,r,8,"padded head samples");
  ssaT.open_ro(source.prefix+".ssa_t",8,r,8,"padded tail samples");
  ExtRuns ext;ext.R=r;ext.textLen=n;ext.rowsTotal=N;ext.skippedByte=2;
  ext.runScan=[&](U i,uint8_t* a,U* l,U n){
   aSrc.read(i,a,n);
   static thread_local std::vector<uint32_t> tmp;
   if(tmp.size()<n)tmp.resize(n);
   lSrc.read(i,tmp.data(),n);
   for(U k=0;k<n;++k)l[k]=tmp[k];
  };
  ext.headScan=[&](U i,U* h,U n){ssaCol.read(i,h,n);};
  ext.tailScan=[&](U i,U* t,U n){ssaT.read(i,t,n);};   // enables the phi-inverse column
  ext.workPrefix=workPrefix;
  ext.build(pfThreads);
  for(U c=1;c<256;++c)C[c]=C[c-1]+counts[c-1];
  // rank(c,row): rows <row whose BWT char is c, from the per-char list plus
  // the record column's lfBase (prefix within char = lfBase - Cless).
  auto rank=[&](U c,U row) {
   if(!nruns[c])return U(0);
   U lo=segStart[c],hi=segStart[c]+nruns[c];
   while(lo+1<hi){U mid=(lo+hi)/2;if(ext.starts_at(chrCol.at(mid))<row)lo=mid;else hi=mid;}
   U id=chrCol.at(lo);
   if(ext.starts_at(id)>=row)return U(0);
   return ext.lfbase_at(id)-ext.Cless[c]+std::min(row-ext.starts_at(id),lSrc.at(id));
  };
  // In padded order, the suffix beginning at n (the w1 dollars) is row 0.
  // Backward search gives the interval of every non-unique terminal suffix.
  // Its leftmost row is the shortest suffix, SA=n-depth. Prefix intervals
  // are disjoint or nested; their maximal union is exactly the set of rows
  // in which a finite-suffix comparison can reach an end before a mismatch.
  U lo=0,hi=N,row=0;
  check(ssaCol.at(0)==n,"padding suffix must begin at row zero");
  while(depth<n) {
   if(depth==limit)refuse("discovery_steps",depth+1,limit);
   U c=ext.a_at(ext.run_of(row));
   check(c!=2,"unexpected dollar during seam discovery");
   lo=C[c]+rank(c,lo);hi=C[c]+rank(c,hi);row=C[c]+rank(c,row);++depth;
   check(lo==row&&hi>lo&&lo>=source.w1,"terminal suffix interval");
   if(hi-lo==1)break;
   if(hi-lo>limit)refuse("class_size",hi-lo,limit);
   classes.push_back({lo-source.w1,hi-source.w1,n-depth,0,0,{}});
  }
  std::sort(classes.begin(),classes.end(),[](const Class& a,const Class& b){return a.lo<b.lo||(a.lo==b.lo&&a.hi>b.hi);});
  size_t used=0;
  for(size_t i=0;i<classes.size();++i) {
   if(used&&classes[i].lo<classes[used-1].hi){check(classes[i].hi<=classes[used-1].hi,"crossing prefix intervals");continue;}
   if(i!=used)classes[used]=std::move(classes[i]);++used;
  }
  classes.resize(used);
  for(auto& cl:classes)total+=cl.hi-cl.lo;
  if(total>limit)refuse("total_class_rows",total,limit);
  // Phi(x)=previous SA row is a circular piecewise translation, anchored
  // by (head SA, previous run tail SA). Answered externally: the largest
  // sorted-seed position <= x identifies the anchoring run; the previous
  // run's tail comes from .ssa_t. Both searches are O(log r) bounded
  // preads; the former 16r sorted-pair array is gone.
  auto lookup=[&](U pos) {
   U slo=0,shi=r;
   while(slo+1<shi){U mid=(slo+shi)/2;U sp,srn;ext.seed(mid,sp,srn);if(sp<=pos)slo=mid;else shi=mid;}
   U sp0,srn0;ext.seed(0,sp0,srn0);
   if(sp0>pos)slo=r-1;   // x below every head: wrap to the maximum seed
   U sp,srun;ext.seed(slo,sp,srun);
   U prevTail=ssaT.at((srun+r-1)%r);
   check(prevTail<N,"padded tail sample out of range");
   return (prevTail+(pos+N-sp)%N)%N;
  };
  for(auto& cl:classes)cl.before=lookup(cl.first);
  // The row enumeration walks phi's INVERSE (the original code swaps its
  // sorted pair array and re-sorts by tail): the largest tail <= x anchors
  // run k, and the answer is the NEXT run's head shifted by the delta —
  // the position of the next row, in row order.
  auto lookupNext=[&](U pos) {
   U slo=0,shi=r;
   while(slo+1<shi){U mid=(slo+shi)/2;U sp,srn;ext.seedt(mid,sp,srn);if(sp<=pos)slo=mid;else shi=mid;}
   U sp0,srn0;ext.seedt(0,sp0,srn0);
   if(sp0>pos)slo=r-1;
   U sp,srun;ext.seedt(slo,sp,srun);
   U nextHead=ssaCol.at((srun+1)%r);
   return (nextHead+(pos+N-sp)%N)%N;
  };
  if(getenv("SLIM_SEAM_DEBUG"))
   for(auto& cl:classes)fprintf(stderr,"SLIM_SEAM_CLASS lo=%llu hi=%llu first=%llu before=%llu\n",
    (unsigned long long)cl.lo,(unsigned long long)cl.hi,(unsigned long long)cl.first,(unsigned long long)cl.before);
  // Parse-free LCE (the finish sequence no longer reads any PFP artifact):
  // the external structural walk over the PADDED runs - all n+w1 rotations
  // of P=M.0x02^w1 are distinct, so LF is closed by construction - emits
  // the normalized text M to a private sidecar and writes tau-spaced rolling
  // suffix-hash checkpoints. Every probe and verified symbol is journaled
  // and budgeted; hash/verification disagreement aborts. Corpus is not read.
  // Byte-denominated journaled budget (sharded per thread). The parse-based
  // policy charged mixed phrase/byte units with limit max(1e6,(P+D)/8); at
  // fragment scale one phrase unit covered ~102 bytes, so that limit was
  // ~14n byte-equivalents. The honest byte-level replacement is max(1e8,
  // 16n): comparable stringency, with the floor admitting bounded tiny
  // fixtures. Every probe and verified byte is charged; exhaustion fails
  // loudly, never degrades.
  U pfWorkLimit=std::max<U>(100000000,16*n);
  // ADOPT (--pf-text/--pf-checkpoints): the final merge pass emitted the
  // text and tau-spaced checkpoints as side streams (cross_lcp_merge
  // --emit-pf); the repair consumes them and skips the padded walk. The
  // merge's tau uses final_runs == the padded run count r, so the sidecar
  // is exactly the one the walk would have produced.
  std::unique_ptr<SlimLCEParseFree> pf;
  if(!pfText.empty())
   pf=std::make_unique<SlimLCEParseFree>(pfText,pfCkpt,n,true,source.w1,false,pfWorkLimit);
  else
   pf=std::make_unique<SlimLCEParseFree>(ext,pfThreads,0,workPrefix,false,true,source.w1,pfWorkLimit);
  check((*pf).n==N,"seam parse-free length mismatch");
  U maxSeamLCE=0;
  fprintf(stderr,"CYCLIC_SEAM_LCE_POLICY parse_free=1 probe_limit=%llu max_verification=67108864 total_limit=%llu cost=byte_comparisons_plus_hash_probes_plus_verified_bytes fraction=16n floor=100000000 exact=1 no_parse_dict=1\n",(unsigned long long)pf->tau,(unsigned long long)pf->work->limit);
  auto symbol=[&](U pos) {
   return pf->text_byte(pos);
  };
  for(auto& cl:classes) {
   std::vector<U> sa;sa.reserve(cl.hi-cl.lo);U pos=cl.first;
   for(U row=cl.lo;row<cl.hi;++row){check(pos<n,"seam sample outside T");sa.push_back(pos);pos=lookupNext(pos);}
   cl.after=pos;
   std::sort(sa.begin(),sa.end(),[&](U a,U b){
    U len=pf->collection_lce(a,b);maxSeamLCE=std::max(maxSeamLCE,len);
    return len==n ? a<b : symbol((a+len)%n)<symbol((b+len)%n);
   });
   for(U pos:sa) {
    U c=symbol((pos+n-1)%n);
    if(!cl.runs.empty()&&cl.runs.back().c==c){++cl.runs.back().len;cl.runs.back().t=pos;}
    else cl.runs.push_back({c,1,pos,pos});
   }
  }
  if(getenv("SLIM_SEAM_DEBUG"))
   for(auto& cl:classes){fprintf(stderr,"SLIM_SEAM_CLASS2 lo=%llu hi=%llu after=%llu runs=%zu:",(unsigned long long)cl.lo,(unsigned long long)cl.hi,(unsigned long long)cl.after,cl.runs.size());
    for(auto&v:cl.runs)fprintf(stderr," (%02x,%llu,h=%llu,t=%llu)",v.c,(unsigned long long)v.len,(unsigned long long)v.h,(unsigned long long)v.t);
    fprintf(stderr,"\n");}
  fprintf(stderr,"CYCLIC_SEAM_REPAIRED classes=%zu rows=%llu discovery_steps=%llu limit=%llu phi_samples=%llu\n",classes.size(),(unsigned long long)total,(unsigned long long)depth,(unsigned long long)limit,(unsigned long long)r);
  fprintf(stderr,"CYCLIC_SEAM_WORK max_seam_lce=%llu total_work=%llu total_limit=%llu exact=1\n",(unsigned long long)maxSeamLCE,(unsigned long long)pf->work->totalUsed(),(unsigned long long)pf->work->limit);
  pf->report();
  // Derived temps are external artifacts: unlink on success; any failure
  // path keeps them for inspection (the exception propagates, main prints
  // FATAL and the SLIM_EXT_BUILT line names the work prefix).
  ext.cleanup();
  unlink(paPath.c_str());unlink(plPath.c_str());unlink(pcPath.c_str());
 }
 void scan(const std::function<void(Run)>& emit) {
  U row=0;size_t next=0;Run pending{};bool have=false;
  auto append=[&](Run v) {
   if(!v.len)return;
   if(have&&pending.c==v.c){pending.len+=v.len;pending.t=v.t;}
   else {if(have)emit(pending);pending=v;have=true;}
  };
  source.scan([&](Run v){
   U end=row+v.len;
   while(row<end) {
    if(next<classes.size()&&row>=classes[next].lo) {
     auto& cl=classes[next];
     if(row==cl.lo)for(auto piece:cl.runs)append(piece);
     row=std::min(end,cl.hi);
     if(row==cl.hi){v.h=cl.after;++next;}
    } else {
     U stop=next<classes.size()?std::min(end,classes[next].lo):end;
     Run piece=v;piece.len=stop-row;
     if(stop<end)piece.t=classes[next].before;
     append(piece);row=stop;
    }
   }
  });
  if(have)emit(pending);
 }
};
