// Bounded seam classes over the padded RLBWT. No expanded-text scan or LF
// fallback: every row enumerated belongs to an admitted seam interval.
struct SeamRepair {
 struct Class { U lo,hi,first,before=0,after=0;std::vector<Run> runs; };
 Source& source;
 std::vector<Class> classes;
 U limit,total=0,depth=0;
 static void refuse(const char* kind,U size,U limit) {
  fprintf(stderr,"CYCLIC_SEAM_REFUSED %s=%llu limit=%llu policy=max(1000,raw_r/1000); no O(n) fallback\n",kind,(unsigned long long)size,(unsigned long long)limit);
  throw std::runtime_error("cyclic seam repair exceeds compressed-work policy; no O(n) fallback");
 }
 explicit SeamRepair(Source& source,bool certified):source(source),limit(std::max<U>(1000,source.r/1000)) {
  if(certified)return;
  const U N=source.n,n=N-10,r=source.r;
  std::vector<Run> raw;raw.reserve(r);
  std::vector<U> starts;starts.reserve(r);
  std::array<std::vector<std::pair<U,U>>,256> bychar;
  std::array<U,256> counts{},C{};
  std::ifstream b(source.prefix+".rlebwt",std::ios::binary),h(source.prefix+".ssa",std::ios::binary),t(source.prefix+".ssa_t",std::ios::binary);
  integer(h);integer(t);U rows=0;
  for(U i=0;i<r;++i) {
   Run v{};U word;
   do {word=integer(b,4);if(v.len)check(v.c==(word&255),"RLE continuation character");v.c=word&255;v.len+=(word>>8)&0x7fffff;}while(word>>31);
   v.h=integer(h);v.t=integer(t);
   check(v.len&&v.len<=N-rows&&v.h<N&&v.t<N,"padded RLE bounds");
   bychar[v.c].emplace_back(i,counts[v.c]);counts[v.c]+=v.len;
   starts.push_back(rows);rows+=v.len;raw.push_back(v);
  }
  check(rows==N&&b.peek()==EOF,"padded RLE size");
  for(U c=1;c<256;++c)C[c]=C[c-1]+counts[c-1];
  auto rank=[&](U c,U row) {
   auto& v=bychar[c];
   auto it=std::lower_bound(v.begin(),v.end(),row,[&](auto pair,U x){return starts[pair.first]<x;});
   if(it==v.begin())return U(0);
   --it;U id=it->first;return it->second+std::min(row-starts[id],raw[id].len);
  };
  auto runof=[&](U row){return U(std::upper_bound(starts.begin(),starts.end(),row)-starts.begin()-1);};
  // In padded order, the suffix beginning at n (the ten dollars) is row 0.
  // Backward search gives the interval of every non-unique terminal suffix.
  // Its leftmost row is the shortest suffix, SA=n-depth. Prefix intervals
  // are disjoint or nested; their maximal union is exactly the set of rows
  // in which a finite-suffix comparison can reach an end before a mismatch.
  U lo=0,hi=N,row=0;
  check(raw[0].h==n,"padding suffix must begin at row zero");
  while(depth<n) {
   if(depth==limit)refuse("discovery_steps",depth+1,limit);
   U c=raw[runof(row)].c;
   check(c!=2,"unexpected dollar during seam discovery");
   lo=C[c]+rank(c,lo);hi=C[c]+rank(c,hi);row=C[c]+rank(c,row);++depth;
   check(lo==row&&hi>lo&&lo>=10,"terminal suffix interval");
   if(hi-lo==1)break;
   if(hi-lo>limit)refuse("class_size",hi-lo,limit);
   classes.push_back({lo-10,hi-10,n-depth,0,0,{}});
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
  // by (head SA, previous run tail SA). Its inverse uses (tail,next head).
  // Both searches cost O(log r); no LF walk resolves interior samples.
  std::vector<std::pair<U,U>> phi;phi.reserve(r);
  for(U i=0;i<r;++i)phi.emplace_back(raw[i].h,raw[(i+r-1)%r].t);
  auto order=[&](){std::sort(phi.begin(),phi.end());};order();
  auto lookup=[&](U pos) {
   auto it=std::upper_bound(phi.begin(),phi.end(),std::make_pair(pos,INF));
   if(it==phi.begin())it=phi.end();--it;
   U delta=(pos+N-it->first)%N;return (it->second+delta)%N;
  };
  for(auto& cl:classes)cl.before=lookup(cl.first);
  for(auto& pair:phi)std::swap(pair.first,pair.second);
  order();
  // Rank structures are no longer needed; release before loading PFP LCE.
  std::vector<Run>().swap(raw);std::vector<U>().swap(starts);
  for(auto& v:bychar)std::vector<std::pair<U,U>>().swap(v);
  SlimLCE lce(source.prefix,r,0,0,true,false,true);
  check(lce.n==N,"seam PFP length mismatch");
  // Existing slim hashes verify guesses exactly. Bound that verifier too;
  // a long verified prefix must refuse, never become a corpus-sized walk.
  U bits=0;for(U value=N;value;value>>=1)++bits;
  U queryLimit=std::max<U>(1000,bits*bits*bits);
  lce.ph->verificationLimit=queryLimit;lce.dh->verificationLimit=queryLimit;
  fprintf(stderr,"CYCLIC_SEAM_LCE_POLICY max_probe_and_verification=%llu max(1000,bit_width(raw_n)^3)\n",(unsigned long long)queryLimit);
  auto symbol=[&](U pos) {
   U shifted=pos+10,id=lce.pr(shifted+1);
   return lce.d[lce.dstart(lce.p[id-1])+shifted-lce.ps(id)];
  };
  for(auto& cl:classes) {
   std::vector<U> sa;sa.reserve(cl.hi-cl.lo);U pos=cl.first;
   for(U row=cl.lo;row<cl.hi;++row){check(pos<n,"seam sample outside T");sa.push_back(pos);pos=lookup(pos);}
   cl.after=pos;
   std::sort(sa.begin(),sa.end(),[&](U a,U b){
    U len=lce.collection_lce(a,b);
    return len==n ? a<b : symbol((a+len)%n)<symbol((b+len)%n);
   });
   for(U pos:sa) {
    U c=symbol((pos+n-1)%n);
    if(!cl.runs.empty()&&cl.runs.back().c==c){++cl.runs.back().len;cl.runs.back().t=pos;}
    else cl.runs.push_back({c,1,pos,pos});
   }
  }
  fprintf(stderr,"CYCLIC_SEAM_REPAIRED classes=%zu rows=%llu discovery_steps=%llu limit=%llu phi_samples=%llu\n",classes.size(),(unsigned long long)total,(unsigned long long)depth,(unsigned long long)limit,(unsigned long long)r);
  lce.ph->report("seam-parse");lce.dh->report("seam-dict");
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
