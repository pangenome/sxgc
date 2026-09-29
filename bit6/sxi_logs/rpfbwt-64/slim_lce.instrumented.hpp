// Two-level sampled fingerprints. No suffix arrays or dense text bitvectors.
// Hash guesses are ALWAYS checked over the entire proposed prefix and at its
// boundary. Exactness therefore costs O(answer), NOT O(tau). See SLIM_HANDOFF.md.
#pragma once
#include <sdsl/sd_vector.hpp>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <stdexcept>
#include <unordered_map>

static void slim_phase(const char* label, double& last) {
    struct rusage u{}; getrusage(RUSAGE_SELF, &u);
    double now = tnow();
    fprintf(stderr, "SLIM_PHASE %s wall=%.3f cumulative=%.3f peak_rss_kib=%ld\n",
            label, now-last, now-G_T0, u.ru_maxrss); last=now;
}
static void slim_fail(const char* s) { fprintf(stderr,"FATAL SLIM: %s\n",s); exit(2); }

// Read-only dictionary; optional bounded per-thread four-page pread cache.
// The virtual prefix matches dictionary.hpp's padding to W dollars.
struct SlimDict {
    int fd=-1; uint64_t disk_size=0, pad=0, size_=0;
    std::vector<uint8_t> resident;
    bool streamed;
    SlimDict(const std::string& path, uint64_t w, bool stream): streamed(stream) {
        fd=open(path.c_str(),O_RDONLY); struct stat st{};
        if(fd<0 || fstat(fd,&st)) slim_fail("open dictionary");
        disk_size=st.st_size;
        uint8_t prefix[64]; ssize_t got=pread(fd,prefix,sizeof prefix,0);
        uint64_t dollars=0;
        while(dollars<(uint64_t)std::max<ssize_t>(got,0) && prefix[dollars]==2) ++dollars;
        if(!dollars || dollars>w) slim_fail("dictionary dollar prefix");
        pad=w-dollars; size_=disk_size+pad;
        if(!streamed) {
            resident.resize(disk_size);
            uint64_t done=0;
            while(done<disk_size) {
                ssize_t n=pread(fd,resident.data()+done,std::min<uint64_t>(disk_size-done,1<<26),done);
                if(n<=0) slim_fail("read dictionary"); done+=n;
            }
        }
        if((*this)[size_-1]!=0) slim_fail("dictionary terminator");
    }
    ~SlimDict(){ if(fd>=0) close(fd); }
    uint64_t size() const {return size_;}
    uint8_t operator[](uint64_t i) const {
        if(i>=size_) slim_fail("dictionary bounds");
        if(i<pad) return 2;
        i-=pad;
        if(!streamed) return resident[i];
        constexpr uint64_t B=65536;
        struct Cache { uint64_t next=0; const SlimDict* owner=nullptr; std::array<uint64_t,4> tags{{INF,INF,INF,INF}}; std::array<std::array<uint8_t,B>,4> data; };
        thread_local Cache c;
        if(c.owner!=this) { c.owner=this; c.tags.fill(INF); }
        uint64_t page=i/B, slot=0;
        while(slot<4 && c.tags[slot]!=page)++slot;
        if(slot==4) {
            slot=c.next++%4;
            uint64_t start=page*B, need=std::min(B,disk_size-start), done=0;
            while(done<need) { ssize_t n=pread(fd,c.data[slot].data()+done,need-done,start+done);
                if(n<=0) slim_fail("pread dictionary page"); done+=n; }
            c.tags[slot]=page;
        }
        return c.data[slot][i%B];
    }
};

// Polynomial suffix hash in Z/(2^64), base odd. Collisions cannot escape the
// direct verifier. A suffix at any position needs at most tau-1 symbol reads.
template<class Seq> struct SlimFingerprint {
    const Seq& s; uint64_t tau; std::vector<uint64_t> hashes;
    static constexpr uint64_t BASE=0x9e3779b185ebca87ULL;
    bool inject;
    uint64_t verificationLimit=INF; // Adapter policy; default preserves slim behavior.
    mutable std::atomic<uint64_t> calls{0}, checked{0}, jumps{0}, maxChecked{0}, suffixReads{0};
    void record_checked(uint64_t count) const {
        checked.fetch_add(count,std::memory_order_relaxed);
        uint64_t mx=maxChecked.load(std::memory_order_relaxed);
        while(count>mx && !maxChecked.compare_exchange_weak(mx,count,std::memory_order_relaxed)) {}
    }
    SlimFingerprint(const Seq& seq,uint64_t t,bool fault=false):s(seq),tau(std::max<uint64_t>(1,t)),inject(fault) {
        hashes.resize(s.size()/tau+1);
        uint64_t h=0;
        for(uint64_t i=s.size();i-->0;) {h=(uint64_t)s[i]+1+BASE*h; if(i%tau==0) hashes[i/tau]=h;}
    }
    static uint64_t power(uint64_t n) {uint64_t x=BASE,r=1; while(n){if(n&1)r*=x;x*=x;n>>=1;}return r;}
    uint64_t suffix(uint64_t i) const {
        if(i==s.size())return 0;
        uint64_t end=std::min<uint64_t>(s.size(),((i+tau-1)/tau)*tau);
        if(end-i>verificationLimit)slim_fail("CYCLIC_SEAM_REFUSED: fingerprint probe exceeds polylog-work policy; no O(n) fallback");
        suffixReads.fetch_add(end-i,std::memory_order_relaxed);
        uint64_t h=end==s.size()?0:hashes[end/tau];
        while(end>i){--end;h=(uint64_t)s[end]+1+BASE*h;}return h;
    }
    bool equal(uint64_t i,uint64_t j,uint64_t len) const {
        uint64_t p=power(len);
        return suffix(i)-p*suffix(i+len)==suffix(j)-p*suffix(j+len);
    }
    uint64_t lce(uint64_t i,uint64_t j,uint64_t limit=INF) const {
        if(i>=s.size()||j>=s.size())slim_fail("fingerprint bounds");
        uint64_t cap=std::min({limit,s.size()-i,s.size()-j});
        if(i==j)return cap;
        ++calls;
        // Cheap short mismatch path; no probabilistic answer is returned.
        uint64_t lo=0, quick=std::min<uint64_t>(cap,std::min<uint64_t>(tau,16));
        while(lo<quick && s[i+lo]==s[j+lo])++lo;
        if(lo<quick && !inject){record_checked(lo+1);return lo;}
        uint64_t queryProbes=0;
        uint64_t hi=cap;
        // Exponential search avoids log(|D|) probes on short phrase tails.
        uint64_t step=std::max<uint64_t>(1,quick);
        while(lo<cap) {
            uint64_t next=lo+std::min(step,cap-lo);
            ++jumps;++queryProbes;
            if(!equal(i,j,next)){hi=next;break;}
            lo=next; step=std::min<uint64_t>(cap,step*2);
        }
        while(lo<hi && hi-lo>1){uint64_t mid=lo+(hi-lo)/2;++jumps;++queryProbes;
            if(equal(i,j,mid))lo=mid;else hi=mid;}
        // G3 explicitly perturbs a proposed answer; lowering tau is not a
        // collision injector (it merely increases sampling density).
        uint64_t guess=lo;
        if(inject && guess<cap)++guess;
        else if(inject && guess) --guess;
        if(guess>verificationLimit) {
            fprintf(stderr,"CYCLIC_SEAM_REFUSED requested_lce=%llu sequence_size=%llu i=%llu j=%llu cap=%llu verification_limit=%llu query_hash_probes=%llu total_hash_probes=%llu suffix_symbol_reads=%llu completed_verified_symbols=%llu quick_comparisons=%llu exact_requested_lce=0\n",
                (unsigned long long)guess,(unsigned long long)s.size(),(unsigned long long)i,(unsigned long long)j,(unsigned long long)cap,(unsigned long long)verificationLimit,
                (unsigned long long)queryProbes,(unsigned long long)jumps.load(),(unsigned long long)suffixReads.load(),(unsigned long long)checked.load(),(unsigned long long)quick);
            report("seam-refusal");
            slim_fail("CYCLIC_SEAM_REFUSED: LCE verification exceeds polylog-work policy; no O(n) fallback");
        }
        for(uint64_t k=0;k<guess;++k) if(s[i+k]!=s[j+k])
            slim_fail("fingerprint verification mismatch inside proposed prefix");
        if(guess<cap && s[i+guess]==s[j+guess])
            slim_fail("fingerprint verification mismatch at boundary");
        record_checked(guess+(guess<cap));
        return guess;
    }
    void report(const char* name)const {fprintf(stderr,"SLIM_FP %s tau=%llu bytes=%llu queries=%llu verified_symbols=%llu hash_probes=%llu mean_verified=%.9f max_verified=%llu\n",name,
        (unsigned long long)tau,(unsigned long long)(hashes.size()*8),(unsigned long long)calls.load(),
        (unsigned long long)checked.load(),(unsigned long long)jumps.load(),
        calls.load()?double(checked.load())/calls.load():0.0,(unsigned long long)maxChecked.load());}
};

struct SlimLCE {
    double started=tnow();
    mutable std::atomic<uint64_t> seedQueries{0};
    SlimDict d; std::vector<uint32_t> p;
    sdsl::sd_vector<> bd,bp;
    sdsl::sd_vector<>::select_1_type ds,ps;
    sdsl::sd_vector<>::rank_1_type pr;
    uint64_t n=0,w=10;
    bool cyclic;
    std::vector<uint64_t> stringEnds; // legacy newline ends, or one cyclic end
    std::unique_ptr<SlimFingerprint<SlimDict>> dh;
    std::unique_ptr<SlimFingerprint<std::vector<uint32_t>>> ph;
    uint64_t dstart(uint64_t id)const {return ds(id);}
    uint64_t length(uint64_t id)const {return ds(id+1)-ds(id)-1;}
    SlimLCE(const std::string& prefix,uint64_t r,uint64_t t1,uint64_t t2,bool stream,bool fault=false,bool cyclicText=false):d(prefix+".dict",10,stream),cyclic(cyclicText) {
        double last=started; slim_phase("dict-read",last);
        // Recover collection terminators during the existing dictionary scan.
        // Only phrases containing newline need metadata; phrase overlaps are
        // excluded when expanding their positions through the parse below.
        std::unordered_map<uint32_t,std::vector<uint64_t>> phraseEnds;
        uint64_t count=1,phraseStart=0;uint32_t phrase=1;
        for(uint64_t i=1;i<d.size();++i) {
            uint8_t c=d[i-1];
            if(!cyclic && c==0x0a)phraseEnds[phrase].push_back(i-1-phraseStart);
            if(c==1 || i==d.size()-1)++count;
            if(c==1){++phrase;phraseStart=i;}
        }
        sdsl::sd_vector_builder db(d.size(),count);db.set(0);
        for(uint64_t i=1;i<d.size();++i)if(d[i-1]==1 || i==d.size()-1)db.set(i);
        bd=sdsl::sd_vector<>(db);ds=sdsl::sd_vector<>::select_1_type(&bd);
        slim_phase("dict-boundaries",last);
        // Exact-sized fixed-width input; never instantiate pfpds::parse.
        std::ifstream f(prefix+".parse",std::ios::binary|std::ios::ate);
        if(!f || f.tellg()<0 || (uint64_t)f.tellg()%4)slim_fail("parse file");
        uint64_t bytes=f.tellg();p.resize(bytes/4+1); f.seekg(0);
        if(!f.read((char*)p.data(),bytes))slim_fail("read parse");p.back()=0;
        if(p.size()<3)slim_fail("parse too short");
        for(uint64_t j=0;j+1<p.size();++j) {
            if(!p[j] || p[j]>=count || length(p[j])<w)slim_fail("invalid phrase id/length");
            uint64_t take=length(p[j])-w;
            auto ends=phraseEnds.find(p[j]);
            if(ends!=phraseEnds.end())for(uint64_t off:ends->second)
                if(off<take)stringEnds.push_back(n+off);
            n+=take;
        }
        // 0x1e records form ONE cyclic byte string. Record separators are
        // ordinary compared bytes; they neither end nor truncate an LCE.
        if(cyclic)stringEnds.push_back(n);
        fprintf(stderr,"SLIM_STRING_ENDS count=%zu bytes=%zu from_parse_dict=1 cyclic=%d\n",stringEnds.size(),stringEnds.capacity()*8,int(cyclic));
        sdsl::sd_vector_builder pb(n,p.size()-1);pb.set(0);uint64_t pos=0;
        for(uint64_t j=0;j+2<p.size();++j){pos+=length(p[j])-w;pb.set(pos);}
        // compute_b_p marks every phrase start: 0 plus P-1 increments.
        // p includes its zero terminator; there are p.size()-1 phrases.
        bp=sdsl::sd_vector<>(pb);ps=sdsl::sd_vector<>::select_1_type(&bp);pr=sdsl::sd_vector<>::rank_1_type(&bp);
        slim_phase("parse+boundaries",last);
        // Eight times P/r and D/r remains the requested class, with <=r/8
        // samples at each level. Explicit --tau overrides support sensitivity.
        if(!t1)t1=std::max<uint64_t>(1,(8*p.size()+r-1)/r);
        if(!t2)t2=std::max<uint64_t>(1,(8*d.size()+r-1)/r);
        ph=std::make_unique<SlimFingerprint<std::vector<uint32_t>>>(p,t1,fault);
        slim_phase("parse-fingerprints",last);
        dh=std::make_unique<SlimFingerprint<SlimDict>>(d,t2,fault);
        slim_phase("dict-fingerprints",last);
        fprintf(stderr,"SLIM_STRUCT n=%llu P=%zu D=%llu phrases=%llu parse_bytes=%zu bd_bytes=%zu bp_bytes=%zu dict_resident=%zu dict_cache_per_thread=262144 no_SA_ISA_LCP_RMQ=1 no_M_b_bwt_w_wt=1\n",
            (unsigned long long)n,p.size()-1,(unsigned long long)d.size(),(unsigned long long)(count-1),p.size()*4,sdsl::size_in_bytes(bd),sdsl::size_in_bytes(bp),d.resident.size());
    }
    // Collection LCP excludes the newline sentinel itself, unlike raw LCE
    // on the concatenated PFP text. Sparse ends are derived without a text scan.
    uint64_t collection_lce(uint64_t i,uint64_t j)const {
        if(cyclic) {
            uint64_t size=n-w,answer=0;
            if(i>=size||j>=size)slim_fail("cyclic LCE bounds");
            if(i==j){seedQueries.fetch_add(1,std::memory_order_relaxed);return size;}
            // At most two seam crossings in size symbols. Each raw PFP LCE
            // is capped before its dollar padding; no raw text is read.
            while(answer<size) {
                uint64_t cap=std::min({size-answer,size-i,size-j});
                uint64_t got=std::min((*this)(i,j),cap);
                answer+=got;
                if(got<cap)break;
                i=(i+got)%size;j=(j+got)%size;
            }
            return answer;
        }
        auto remaining=[&](uint64_t pos) {
            pos+=w;
            auto end=std::lower_bound(stringEnds.begin(),stringEnds.end(),pos);
            if(end==stringEnds.end())slim_fail("missing collection terminator");
            return *end-pos;
        };
        uint64_t cap=std::min(remaining(i),remaining(j));
        if(!cap){seedQueries.fetch_add(1,std::memory_order_relaxed);return 0;}
        return std::min((*this)(i,j),cap);
    }
    uint64_t operator()(uint64_t i,uint64_t j)const {
        seedQueries.fetch_add(1,std::memory_order_relaxed);
        i=(i+w)%n;j=(j+w)%n;if(i==j)return n-i;
        uint64_t pi=pr(i+1),pj=pr(j+1), oi=i-ps(pi),oj=j-ps(pj);
        uint64_t a=p[pi-1],b=p[pj-1],k=std::min(length(a)-oi,length(b)-oj);
        uint64_t tail=dh->lce(dstart(a)+oi,dstart(b)+oj,k);
        if(tail<k)return tail;
        if(k<w)slim_fail("phrase overlap shorter than W");
        uint64_t common=ph->lce(pi,pj);
        if(pi+common>=p.size()||pj+common>=p.size()||!p[pi+common]||!p[pj+common])slim_fail("parse terminal reached");
        uint64_t span=ps(pi+1+common)-ps(pi+1);
        a=p[pi+common];b=p[pj+common];
        return k-w+span+dh->lce(dstart(a),dstart(b),std::min(length(a),length(b)));
    }
};

#ifndef SLIM_LCE_CORE_ONLY
// Headerless little-endian u64 column, exactly one SA value per ri4 run.
// Mapping charges up to 8R resident bytes; it never copies the whole column.
struct SlimHeads {
    const uint64_t* data=nullptr; size_t bytes=0; void* mapping=nullptr;
    SlimHeads(const std::string& path,uint64_t runs,uint64_t offset=0) {
        if(path.empty())return;
        const uint16_t endian=1;
        if(*(const uint8_t*)&endian!=1)slim_fail("head-SA needs little-endian host");
        if(runs>SIZE_MAX/8)slim_fail("head-SA size overflow");
        bytes=runs*8;int fd=open(path.c_str(),O_RDONLY);struct stat st{};
        if(fd<0 || fstat(fd,&st) || offset>(uint64_t)st.st_size || bytes>(uint64_t)st.st_size-offset || (!offset && (uint64_t)st.st_size!=bytes) || offset%8 || !bytes)
            slim_fail("head-SA must contain exactly R raw u64 values");
        uint64_t page=uint64_t(sysconf(_SC_PAGESIZE)),base=offset/page*page,delta=offset-base;
        bytes+=delta;void* p=mmap(nullptr,bytes,PROT_READ,MAP_PRIVATE,fd,base);close(fd);
        if(p==MAP_FAILED)slim_fail("mmap head-SA");
        mapping=p;data=reinterpret_cast<const uint64_t*>(static_cast<const char*>(p)+delta);
        fprintf(stderr,"SLIM_HEAD_SA bytes=%zu rows=%llu mmap=1\n",bytes,(unsigned long long)runs);
    }
    ~SlimHeads(){if(mapping)munmap(mapping,bytes);}
    SlimHeads(const SlimHeads&)=delete;
    SlimHeads& operator=(const SlimHeads&)=delete;
};

// CRA1 retains its exact array-major format. Only one row-ordered chunk is
// retained; four positioned writes place it into the corresponding arrays.
static int slim_dump(Ri4& ri,const std::string& prefix,const std::string& out,
                     const std::string& anchorsPath,int threads,uint64_t t1,uint64_t t2,
                     bool stream,bool fault,bool profileOnly=false,
                     const std::string& flatPath="",uint64_t calibRows=256,bool useResolveCache=false,
                     const std::string& headSaPath="") {
    if(!profileOnly && (!ri.haveSa||ri.sampleAllInf()))slim_fail("usable ri4 samples required");
    double last=tnow();
    SlimHeads heads(headSaPath.empty()?ri.sxiPath:headSaPath,ri.R,headSaPath.empty()?ri.headOffset:0);
    // Auto-detect using the existing RLE alphabet, with no input text scan.
    // New collections use reserved 0x1e; texts without newline are cyclic
    // too. Preserve legacy newline collection behavior otherwise.
    bool hasRS=false,hasNL=false;
    for(uint64_t run=0;run<ri.R;++run){hasRS|=ri.a[run]==0x1e;hasNL|=ri.a[run]==0x0a;}
    SlimLCE lce(prefix,ri.R,t1,t2,stream,fault,hasRS||!hasNL);
    if(lce.n!=ri.n+lce.w)slim_fail("parse/ri4 length mismatch");
    if(lce.stringEnds.size()!=ri.k)slim_fail("parse/ri4 string-end count mismatch");
    slim_phase("lce-build-total",last);
    LfIndex lf;lf.build(ri);Anchors anc;if(!anchorsPath.empty())anc.load(anchorsPath);
    SampleResolver resolver;resolver.ri=&ri;resolver.lf=&lf;resolver.anc=anchorsPath.empty()?nullptr:&anc;
    resolver.headSa=heads.data;
    slim_phase("lf-build",last);
    std::unique_ptr<ResolveCache> resolveCache;
    if(useResolveCache) {
        size_t slots=1;while(slots<std::min<uint64_t>(ri.R,1ULL<<26))slots*=2;
        resolveCache=std::make_unique<ResolveCache>(slots);resolver.cache=resolveCache.get();
        fprintf(stderr,"SLIM_RESOLVE_CACHE slots=%zu bytes=%zu checkpoint_bytes_per_worker=65536 exact_keys=1\n",slots,resolveCache->bytes());
        slim_phase("resolve-cache-build",last);
    }
    uint64_t lfbytes=0;for(auto& v:lf.charRuns)lfbytes+=v.capacity()*4;for(auto& v:lf.charSum)lfbytes+=v.capacity()*8;
    fprintf(stderr,"SLIM_RI runs=%llu starts=%llu samples=%llu lf_capacity=%llu\n",(unsigned long long)(ri.R*5),(unsigned long long)(ri.R*8),(unsigned long long)(ri.saWords.size()*8),(unsigned long long)lfbytes);
    if(profileOnly){ lce.ph->report("parse");lce.dh->report("dict");fprintf(stderr,"SLIM_PROFILE_ONLY no queries or aggregate produced\n");return 0; }
    if(!flatPath.empty()) {
        std::ifstream flat(flatPath,std::ios::binary);
        if(!flat)slim_fail("open calibration text");
        uint64_t checked=0;
        for(uint64_t k=0;k<calibRows;++k) {
            for(uint64_t row: {ri.starts[k*ri.R/calibRows],k*ri.n/calibRows}) {
                uint64_t pos=resolver.sa_at(row);
                if(pos>=ri.n)slim_fail("calibration position resolution");
                if(!pos)continue;
                flat.seekg(pos-1);char c;
                if(!flat.read(&c,1))slim_fail("calibration read");
                uint8_t bwt=ri.a[lf.run_of(row)];
                if(bwt!=0x0a && (uint8_t)c!=0x0a && bwt!=(uint8_t)c)
                    slim_fail("flat calibration mismatch");
                ++checked;
            }
        }
        fprintf(stderr,"SLIM_CALIBRATION ri4_ROW_OFF=0 PFP_text_shift=10 checked=%llu bad=0\n",(unsigned long long)checked);
        slim_phase("flat-calibration",last);
    }
    std::string tmp=out+".partial";
    int fd=open(tmp.c_str(),O_CREAT|O_TRUNC|O_WRONLY,0666);if(fd<0)slim_fail("create aggregate");
    uint32_t magic=0x31415243;
    auto put=[&](const void* data,uint64_t bytes,uint64_t offset){const char* p=(const char*)data;while(bytes){ssize_t z=pwrite(fd,p,bytes,offset);if(z<=0)slim_fail("write aggregate");p+=z;bytes-=z;offset+=z;}};
    put(&magic,4,0);put(&ri.R,8,4);
    constexpr uint64_t CHUNK=65536;
    std::array<std::vector<uint64_t>,4> buf;for(auto& b:buf)b.resize(CHUNK);
    const uint64_t queryStepsBefore=resolver.sumSteps.load();
    double resolveWall=0,lceWall=0,writeWall=0;
    long resolvePeak=0,lcePeak=0,writePeak=0;
    auto measure=[&](double began,double& wall,long& peak){
        wall+=tnow()-began;struct rusage u{};getrusage(RUSAGE_SELF,&u);peak=std::max(peak,u.ru_maxrss);
    };
    for(uint64_t start=0;start<ri.R;start+=CHUNK){
        uint64_t count=std::min(CHUNK,ri.R-start);std::atomic<uint64_t> next{0};
        // Separate chunk phases permit real wall-time accounting without a
        // timer at every seed. Reuse the topLCP buffer for the previous SA.
        auto resolveWorker=[&](){for(;;){uint64_t k=next.fetch_add(1);if(k>=count)return;uint64_t run=start+k,a=ri.starts[run],b=a+ri.l[run]-1;
            uint64_t first=resolver.sa_head(run),tail=a==b?first:resolver.sa_tail(run);
            if(first>=ri.n||tail>=ri.n)slim_fail("position resolution failed");
            uint64_t prev=a?resolver.sa_tail(run-1):0;
            if(prev>=ri.n)slim_fail("previous position resolution failed");
            buf[0][k]=prev;buf[1][k]=first;buf[2][k]=tail;
        }};
        auto lceWorker=[&](){for(;;){uint64_t k=next.fetch_add(1);if(k>=count)return;uint64_t run=start+k,a=ri.starts[run],b=a+ri.l[run]-1;
            uint64_t prev=buf[0][k],first=buf[1][k],tail=buf[2][k];
            buf[0][k]=a?lce.collection_lce(prev,first):0;
            buf[3][k]=a==b?INF:lce.collection_lce(first,tail);
        }};
        auto parallel=[&](auto& worker){next=0;std::vector<std::thread> ts;for(int t=0;t<std::max(1,std::min(threads,64));++t)ts.emplace_back(worker);for(auto& t:ts)t.join();};
        double began=tnow();parallel(resolveWorker);measure(began,resolveWall,resolvePeak);
        began=tnow();parallel(lceWorker);measure(began,lceWall,lcePeak);
        began=tnow();
        for(uint64_t field=0;field<4;++field)put(buf[field].data(),count*8,12+8*(field*ri.R+start));
        measure(began,writeWall,writePeak);
        if(start%(CHUNK*16)==0){fprintf(stderr,"SLIM_PROGRESS runs=%llu/%llu resolve_wall=%.3f lce_wall=%.3f write_wall=%.3f lf_steps=%llu cache_hits=%llu\n",(unsigned long long)(start+count),(unsigned long long)ri.R,resolveWall,lceWall,writeWall,(unsigned long long)resolver.sumSteps.load(),(unsigned long long)resolver.viaCache.load());slim_phase("query-chunks",last);}
    }
    if(close(fd)||rename(tmp.c_str(),out.c_str()))slim_fail("publish aggregate");
    slim_phase("queries+stream-write-final",last);
    lce.ph->report("parse");lce.dh->report("dict");
    fprintf(stderr,"SLIM_QUERY_PHASE resolve_wall=%.6f resolve_peak_kib=%ld lce_wall=%.6f lce_peak_kib=%ld write_wall=%.6f write_peak_kib=%ld\n",resolveWall,resolvePeak,lceWall,lcePeak,writeWall,writePeak);
    fprintf(stderr,"SLIM_SEEDS queries=%llu mean_verified_phrases=%.9f max_verified_phrases=%llu (includes_zero_parse_work_seeds; boundary_included)\n",(unsigned long long)lce.seedQueries.load(),lce.seedQueries.load()?double(lce.ph->checked.load())/lce.seedQueries.load():0.0,(unsigned long long)lce.ph->maxChecked.load());
    fprintf(stderr,"SLIM_RESOLVE walks=%llu steps=%llu max=%llu anchor=%llu failed=%llu cache_hits=%llu\n",(unsigned long long)resolver.nWalks.load(),(unsigned long long)resolver.sumSteps.load(),(unsigned long long)resolver.maxSteps.load(),(unsigned long long)resolver.viaAnchor.load(),(unsigned long long)resolver.hit0a.load(),(unsigned long long)resolver.viaCache.load());
    fprintf(stderr,"SLIM_BOUNDARY_RESOLVE head_direct=%llu tail_direct=%llu head_lf_steps=%llu\n",
        (unsigned long long)resolver.directHeads.load(),(unsigned long long)resolver.directTails.load(),
        (unsigned long long)(resolver.sumSteps.load()-queryStepsBefore));
    return 0;
}

#endif // SLIM_LCE_CORE_ONLY
