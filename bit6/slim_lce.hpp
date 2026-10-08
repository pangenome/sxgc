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
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static void slim_phase(const char* label, double& last) {
    struct rusage u{}; getrusage(RUSAGE_SELF, &u);
    double now = tnow();
    fprintf(stderr, "SLIM_PHASE %s wall=%.3f cumulative=%.3f peak_rss_kib=%ld\n",
            label, now-last, now-G_T0, u.ru_maxrss); last=now;
}
static void slim_fail(const char* s) { fprintf(stderr,"FATAL SLIM: %s\n",s); exit(2); }

// Per-thread sharded counter. The parse-free query loop fires billions of
// relaxed adds per run; one shared cache line serializes them (the measured
// "~3x per-run LCE cost" of the single-word journal). Each charging thread owns
// shards[slot]; add() flushes to `global` every `stride` units. Work loops that
// spawn short-lived worker threads MUST call slim_set_thread_shard(t) with the
// worker's index first (the band loops below do); unregistered threads fall
// back to a process-wide tid, and that fallback pool is bounded and shared —
// fine for the few long-lived threads (walk, self-check), never for the
// per-band spawn/join pattern. read() is exact once every charging thread has
// called flushThread() (every worker loop does); in-flight reads are a lower
// bound, never larger.
static inline unsigned& slim_shard_slot() { thread_local unsigned s = 0xFFFFFFFFu; return s; }
static inline void slim_set_thread_shard(unsigned i) { slim_shard_slot() = i; }
static inline unsigned slim_tid() {
    unsigned& s = slim_shard_slot();
    if (s != 0xFFFFFFFFu) return s;
    static std::atomic<unsigned> nextTid{0};
    thread_local unsigned t = nextTid.fetch_add(1, std::memory_order_relaxed);
    return t < 512 ? t : 511u;
}
struct SlimSharded {
    static constexpr unsigned CAP = 512;
    mutable std::atomic<uint64_t> global{0};
    mutable std::vector<uint64_t> shards;
    uint64_t stride = 1 << 16;
    SlimSharded(): shards(CAP, 0) {}
    inline void add(uint64_t v) const {
        uint64_t& s = shards[slim_tid()];
        s += v;
        if (s >= stride) { global.fetch_add(s, std::memory_order_relaxed); s = 0; }
    }
    inline void flushThread() const {
        uint64_t& s = shards[slim_tid()];
        if (s) { global.fetch_add(s, std::memory_order_relaxed); s = 0; }
    }
    uint64_t read() const {
        uint64_t v = global.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < CAP; ++i) v += shards[i];
        return v;
    }
};

#include "ext_columns.hpp"

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

// Seam-only work policy. One work unit is one phrase-ID/byte comparison,
// one hash probe, or one directly verified symbol. A hash probe reads at
// most probeLimit symbols to reconstruct one sampled value; those reads are
// a bounded structure constant (tau <= 8*max(P,D)/r), so each probe charges
// one unit instead of one per symbol - per-symbol charging multiplies probe
// cost by tau and exhausts honest budgets on small structures. Preprocessing
// remains O(P+D+r), never an expanded-text walk. Each direct verification is
// capped at 2^26 units; all seam queries share at most
// min(2^32, max(10^6, (P+D)/8)) units. The floor admits bounded tiny fixtures;
// the absolute ceilings make additional verification sublinear as n grows.
// Hashes only propose answers: the complete prefix and boundary stay exact.
struct SlimSeamWork {
    // Sharded fail-loud budget journal. Semantics: every probe, quick
    // compare and verified symbol is charged; a corpus whose total work
    // exceeds the limit refuses (exhaustion never degrades). Sharding keeps
    // the single-CAS-word contention off the per-run hot path: each thread
    // charges a private shard and folds it into the shared total every
    // `stride` units. Bounded overdraft before a refusal: at most
    // (threads-1)*stride + one query's verification cap, i.e. <= ~1.2% of the
    // limit at gate scales (stride = limit>>12) — the 128n / 16n caps stay
    // fail-loud with that documented tolerance.
    static constexpr unsigned CAP = 512;
    uint64_t limit, stride;
    mutable std::atomic<uint64_t> used{0};
    mutable std::vector<uint64_t> shards;
    void init(uint64_t lim) {
        limit = lim;
        stride = std::max<uint64_t>(1, lim >> 12);
    }
    explicit SlimSeamWork(uint64_t size) {
        init(std::min<uint64_t>(1ULL<<32, std::max<uint64_t>(1000000, size/8)));
        shards.assign(CAP, 0);
    }
    // Explicit-limit journal (parse-free backend): the parse it replaces charged
    // ~11 work units per text byte at fragment scale, so the parse-free budget is
    // 64 units/text byte with no 2^32 ceiling. Exhaustion still fails loudly.
    explicit SlimSeamWork(uint64_t size, uint64_t ceiling) {
        init(std::min<uint64_t>(ceiling, std::max<uint64_t>(1000000, size)));
        shards.assign(CAP, 0);
    }
    inline uint64_t& mine() const { return shards[slim_tid()]; }
    bool reserve(uint64_t count) const {
        uint64_t& s = mine();
        if (s >= stride) { used.fetch_add(s, std::memory_order_relaxed); s = 0; }
        if (used.load(std::memory_order_relaxed) + s + count > limit) return false;
        s += count;
        return true;
    }
    void flushThread() const {
        uint64_t& s = mine();
        if (s) { used.fetch_add(s, std::memory_order_relaxed); s = 0; }
    }
    // Exact once all charging threads flushed; lower bound in flight.
    uint64_t totalUsed() const {
        uint64_t v = used.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < CAP; ++i) v += shards[i];
        return v;
    }
};

// Polynomial suffix hash in Z/(2^64), base odd. Collisions cannot escape the
// direct verifier. A suffix at any position needs at most tau-1 symbol reads.
template<class Seq> struct SlimFingerprint {
    const Seq& s; uint64_t tau; std::vector<uint64_t> hashes;
    static constexpr uint64_t BASE=0x9e3779b185ebca87ULL;
    bool inject;
    uint64_t verificationLimit=INF, probeLimit=INF; // Seam adapter only.
    const char* reportName="unrestricted";
    SlimSeamWork* seamWork=nullptr;
    // File-backed checkpoints (parse-free mode): the tau-spaced suffix hashes
    // live in a PFCK sidecar (bit6/ext_columns.hpp format) and are read
    // through a tiny per-thread cache. At pile scale the checkpoint column is
    // r bytes (~480 GB at R=4.8e11) — never resident.
    int hashFd=-1; uint64_t hashByteBase=0, hashCount=0;
    mutable SlimSharded verificationWork;
    mutable std::atomic<uint64_t> maxRequested{0};
    void seam_policy(SlimSeamWork& work,uint64_t probe,const char* name) {
        seamWork=&work;probeLimit=probe;reportName=name;
        verificationLimit=std::min<uint64_t>(1ULL<<26,std::max<uint64_t>(1000,s.size()));
    }
    void charge(uint64_t count,const char* site,uint64_t requested=0) const {
        if(seamWork && !seamWork->reserve(count)) {
            fprintf(stderr,"CYCLIC_SEAM_REFUSED site=%s requested_lce=%llu requested_work=%llu total_work=%llu total_limit=%llu no_O_n_fallback=1\n",site,
                (unsigned long long)requested,(unsigned long long)count,(unsigned long long)seamWork->totalUsed(),(unsigned long long)seamWork->limit);
            report(reportName);slim_fail("CYCLIC_SEAM_REFUSED: total compressed-work budget exhausted; no O(n) fallback");
        }
    }
    mutable SlimSharded calls, checked, jumps, hashProbes, suffixReads;
    mutable std::atomic<uint64_t> maxChecked{0};
    void record_checked(uint64_t count) const {
        checked.add(count);
        uint64_t mx=maxChecked.load(std::memory_order_relaxed);
        while(count>mx && !maxChecked.compare_exchange_weak(mx,count,std::memory_order_relaxed)) {}
    }
    void flushThread() const {
        calls.flushThread(); checked.flushThread(); jumps.flushThread();
        hashProbes.flushThread(); suffixReads.flushThread(); verificationWork.flushThread();
    }
    SlimFingerprint(const Seq& seq,uint64_t t,bool fault=false):s(seq),tau(std::max<uint64_t>(1,t)),inject(fault) {
        hashes.resize(s.size()/tau+1);
        hashCount=hashes.size();
        uint64_t h=0;
        for(uint64_t i=s.size();i-->0;) {h=(uint64_t)s[i]+1+BASE*h; if(i%tau==0) hashes[i/tau]=h;}
    }
    // Adopt tau-spaced suffix-hash checkpoints computed by a single backward
    // stream (parse-free backend). The owner self-checks adopted checkpoints
    // at startup.
    SlimFingerprint(const Seq& seq,uint64_t t,bool fault,std::vector<uint64_t>&& pre)
        :s(seq),tau(std::max<uint64_t>(1,t)),hashes(std::move(pre)),inject(fault) {
        hashCount=hashes.size();
        if(hashCount!=s.size()/tau+1) slim_fail("adopted checkpoint count mismatch");
    }
    // Adopt checkpoints from a PFCK sidecar file (hash entries at
    // hashByteBase, hashCount entries). Writer contract: bit6/ext_columns.hpp.
    SlimFingerprint(const Seq& seq,uint64_t t,bool fault,int fd,uint64_t byteBase,uint64_t count)
        :s(seq),tau(std::max<uint64_t>(1,t)),inject(fault),hashFd(fd),hashByteBase(byteBase),hashCount(count) {
        if(hashCount!=s.size()/tau+1) slim_fail("adopted checkpoint count mismatch");
    }
    inline uint64_t hash_at(uint64_t k) const {
        if(k>=hashCount) slim_fail("checkpoint index out of range");
        if(hashFd<0) return hashes[k];
        struct HC { const void* o=nullptr; uint64_t tag[4]{}; uint64_t val[4]{}; unsigned nxt=0; };
        thread_local HC hc;
        if(hc.o!=(const void*)this) { hc.o=this; hc.tag[0]=hc.tag[1]=hc.tag[2]=hc.tag[3]=~0ull; }
        for(unsigned i=0;i<4;++i) if(hc.tag[i]==k) return hc.val[i];
        uint64_t v;
        if(pread(hashFd,&v,8,hashByteBase+8*k)!=8) slim_fail("checkpoint sidecar read");
        hc.tag[hc.nxt]=k; hc.val[hc.nxt]=v; hc.nxt=(hc.nxt+1)&3;
        return v;
    }
    static uint64_t power(uint64_t n) {uint64_t x=BASE,r=1; while(n){if(n&1)r*=x;x*=x;n>>=1;}return r;}
    uint64_t suffix(uint64_t i) const {
        if(i==s.size())return 0;
        uint64_t end=std::min<uint64_t>(s.size(),((i+tau-1)/tau)*tau);
        if(end-i>probeLimit) {
            fprintf(stderr,"CYCLIC_SEAM_REFUSED probe_symbols=%llu probe_limit=%llu\n",(unsigned long long)(end-i),(unsigned long long)probeLimit);
            report(reportName);slim_fail("CYCLIC_SEAM_REFUSED: fingerprint probe exceeds polylog-work policy; no O(n) fallback");
        }
        charge(1,"hash-probe");
        hashProbes.add(1);
        suffixReads.add(end-i);
        uint64_t h=end==s.size()?0:hash_at(end/tau);
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
        calls.add(1);
        // Cheap short mismatch path; no probabilistic answer is returned.
        uint64_t lo=0, quick=std::min<uint64_t>(cap,std::min<uint64_t>(tau,16));
        uint64_t quickWork=0;
        while(lo<quick) {
            charge(1,"quick-compare");++quickWork;
            if(s[i+lo]!=s[j+lo])break;
            ++lo;
        }
        verificationWork.add(quickWork);
        if(lo<quick && !inject){record_checked(lo+1);return lo;}
        uint64_t queryProbes=0;
        uint64_t hi=cap;
        // Exponential search avoids log(|D|) probes on short phrase tails.
        uint64_t step=std::max<uint64_t>(1,quick);
        while(lo<cap) {
            uint64_t next=lo+std::min(step,cap-lo);
            ++queryProbes;
            if(!equal(i,j,next)){hi=next;break;}
            lo=next; step=std::min<uint64_t>(cap,step*2);
        }
        while(lo<hi && hi-lo>1){uint64_t mid=lo+(hi-lo)/2;++queryProbes;
            if(equal(i,j,mid))lo=mid;else hi=mid;}
        // G3 explicitly perturbs a proposed answer; lowering tau is not a
        // collision injector (it merely increases sampling density).
        uint64_t guess=lo;
        if(inject && guess<cap)++guess;
        else if(inject && guess) --guess;
        uint64_t mx=maxRequested.load(std::memory_order_relaxed);
        while(guess>mx && !maxRequested.compare_exchange_weak(mx,guess,std::memory_order_relaxed)) {}
        uint64_t directWork=guess+(guess<cap);
        if(quickWork>verificationLimit || directWork>verificationLimit-quickWork) {
            fprintf(stderr,"CYCLIC_SEAM_REFUSED requested_lce=%llu sequence_size=%llu i=%llu j=%llu cap=%llu verification_limit=%llu query_hash_probes=%llu total_hash_probes=%llu suffix_symbol_reads=%llu completed_verified_symbols=%llu quick_comparisons=%llu exact_requested_lce=0\n",
                (unsigned long long)guess,(unsigned long long)s.size(),(unsigned long long)i,(unsigned long long)j,(unsigned long long)cap,(unsigned long long)verificationLimit,
                (unsigned long long)queryProbes,(unsigned long long)jumps.read(),(unsigned long long)suffixReads.read(),(unsigned long long)checked.read(),(unsigned long long)quickWork);
            report(reportName);
            slim_fail("CYCLIC_SEAM_REFUSED: LCE verification exceeds compressed-work policy; no O(n) fallback");
        }
        charge(directWork,"direct-verify",guess);
        verificationWork.add(directWork);
        for(uint64_t k=0;k<guess;++k) if(s[i+k]!=s[j+k])
            slim_fail("fingerprint verification mismatch inside proposed prefix");
        if(guess<cap && s[i+guess]==s[j+guess])
            slim_fail("fingerprint verification mismatch at boundary");
        record_checked(guess+(guess<cap));
        jumps.add(queryProbes);
        return guess;
    }
    void report(const char* name)const {fprintf(stderr,"SLIM_FP %s tau=%llu bytes=%llu queries=%llu verified_symbols=%llu hash_probes=%llu mean_verified=%.9f max_verified=%llu max_requested_lce=%llu verification_work=%llu hash_probe_charges=%llu suffix_symbol_reads=%llu total_work=%llu total_limit=%llu verification_limit=%llu probe_limit=%llu\n",name,
        (unsigned long long)tau,(unsigned long long)(hashCount*8),(unsigned long long)calls.read(),
        (unsigned long long)checked.read(),(unsigned long long)jumps.read(),
        calls.read()?double(checked.read())/calls.read():0.0,(unsigned long long)maxChecked.load(),
        (unsigned long long)maxRequested.load(),(unsigned long long)verificationWork.read(),(unsigned long long)hashProbes.read(),(unsigned long long)suffixReads.read(),
        (unsigned long long)(seamWork?seamWork->totalUsed():0),(unsigned long long)(seamWork?seamWork->limit:INF),
        (unsigned long long)verificationLimit,(unsigned long long)probeLimit);}
};

// Random access to the walk-emitted text sidecar: optional bounded
// four-page pread cache per thread (the SlimDict streamed pattern).
struct SlimTextFile {
    int fd=-1; uint64_t size_=0;
    SlimTextFile(const std::string& path,uint64_t expected) {
        fd=open(path.c_str(),O_RDONLY); struct stat st{};
        if(fd<0 || fstat(fd,&st) || (uint64_t)st.st_size!=expected) slim_fail("text sidecar open/size");
        size_=expected;
    }
    ~SlimTextFile(){ if(fd>=0) close(fd); }
    SlimTextFile(const SlimTextFile&)=delete;
    SlimTextFile& operator=(const SlimTextFile&)=delete;
    uint64_t size() const {return size_;}
    uint8_t operator[](uint64_t i) const {
        if(i>=size_) slim_fail("text sidecar bounds");
        constexpr uint64_t B=65536;
        struct Cache { uint64_t next=0; const SlimTextFile* owner=nullptr; std::array<uint64_t,4> tags{{INF,INF,INF,INF}}; std::array<std::array<uint8_t,B>,4> data; };
        thread_local Cache c;
        if(c.owner!=this) { c.owner=this; c.tags.fill(INF); }
        uint64_t page=i/B, slot=0;
        while(slot<4 && c.tags[slot]!=page)++slot;
        if(slot==4) {
            slot=c.next++%4;
            uint64_t start=page*B, need=std::min(B,size_-start), done=0;
            while(done<need) { ssize_t n=pread(fd,c.data[slot].data()+done,need-done,start+done);
                if(n<=0) slim_fail("pread text sidecar page"); done+=n; }
            c.tags[slot]=page;
        }
        return c.data[slot][i%B];
    }
};

// Parse-free LCE backend, externalized. No run-scale column is resident:
// the merged structure's columns live in files (bit6/ext_columns.hpp) and
// are read through bounded pread windows; the only run-scale RAM is the
// small coarse starts index (8R/64) plus per-thread caches.
//
// Two modes:
//  * WALK (default): ExtRuns (record column + sorted head-SA seeds, built by
//    a two-pass external form) drives ONE structural LF walk over the built
//    rows, emitting the cyclic text to a private sidecar while a single
//    backward read writes tau-spaced rolling suffix-hash checkpoints to a
//    PFCK sidecar. The corpus is never read - THE LAW holds; the walk is over
//    the built structure only.
//  * ADOPT (--pf-text/--pf-checkpoints): the FINAL cross-LCP merge pass
//    already materialized every text byte (M.M) and now emits text+checkpoints
//    as side streams (cross_lcp_merge --emit-pf). The slim consumes them and
//    skips the walk entirely: no seeds, no record column, no LF steps. This
//    is the pile-scale variant (the 1.31e12-step walk is the thing it kills).
//
// Queries use the proven SlimFingerprint discipline: checked 16-symbol fast
// path, galloping hash probes, bisection, FULL direct verification of every
// proposed prefix and its mismatch boundary, over the sidecar through a
// bounded four-page pread cache per thread. Probes and verified symbols are
// journaled against a fail-loud sharded budget; a hash/verification
// disagreement aborts. Walk-mode artifacts (sidecar, PFCK) are unlinked on
// clean completion and kept for inspection after any failure; adopt-mode
// inputs are never removed (the merge owns them).
struct SlimLCEParseFree {
    double started=tnow();
    mutable SlimSharded seedQueries;
    uint64_t n=0, w=10, textLen=0, rowsTotal=0, tau=0, seedCount=0, walkEmissions=0, selfChecks=0;
    bool cyclic=false, walked=false;
    std::vector<uint64_t> stringEnds; // newline byte positions (text coords); cyclic: {textLen}
    std::unique_ptr<SlimTextFile> sidecar;
    std::unique_ptr<SlimFingerprint<SlimTextFile>> fh;
    std::unique_ptr<SlimSeamWork> work;
    std::string sidecarPath, ckptPath;
    int ckptFd=-1;

    // ---- WALK mode ------------------------------------------------------
    SlimLCEParseFree(ExtRuns& ext, int threads, uint64_t tauOverride,
                     const std::string& outPrefix, bool fault, bool cyclicText,
                     uint64_t w1, uint64_t workLimit)
        : w(w1), textLen(ext.textLen), rowsTotal(ext.rowsTotal), cyclic(cyclicText), walked(true) {
        if(w1<3 || w1>512) slim_fail("parse-free window out of range");
        if(textLen>rowsTotal) slim_fail("parse-free text length exceeds walked rows");
        n=textLen+w;
        seedCount=ext.R;
        double last=started;
        sidecarPath=outPrefix+".pftext";
        ckptPath=outPrefix+".pfck";
        int out=open(sidecarPath.c_str(),O_CREAT|O_EXCL|O_RDWR,0666);
        if(out<0) slim_fail("create parse-free text sidecar (refusing to clobber an existing file)");
        slim_phase("parse-free-ext-ready",last);
        // Structural walk: at row(p), BWT[row]=T[p-1 mod n]; one LF step moves
        // p -> p-1. Seed idx>=1 emits offsets [P[idx-1],P[idx]); task 0 emits
        // [0,P[0]) then wraps to [P[R-1],textLen). Every task ends ON the next
        // lower seed's row; that exact row identity is asserted per task.
        {
        int tc=std::max(1,std::min(threads,64));
        constexpr uint64_t B=65536, BATCH=4096;
        std::atomic<uint64_t> emitted{0}, nextBatch{0};
        std::vector<std::array<uint64_t,256>> freqs(tc);
        std::vector<std::vector<uint64_t>> newlineLists(tc);
        std::vector<std::thread> ts;
        for(int t=0;t<tc;++t) ts.emplace_back([&,t](){
            std::array<uint64_t,256>& freq=freqs[t]; freq.fill(0);
            std::vector<uint64_t>& nl=newlineLists[t];
            std::vector<uint8_t> buf(B);
            uint64_t curb=UINT64_MAX, curLo=0, curHi=0;
            auto flush=[&]() {
                if(curb==UINT64_MAX) return;
                uint64_t start=curb*B+curLo, len=curHi-curLo;
                const char* p=(const char*)buf.data()+curLo;
                while(len) { ssize_t z=pwrite(out,p,len,start); if(z<=0) slim_fail("write text sidecar"); p+=z; start+=z; len-=z; }
                curb=UINT64_MAX;
            };
            auto put=[&](uint64_t off,uint8_t b) {
                uint64_t blk=off/B, o=off%B;
                if(curb!=UINT64_MAX && (blk!=curb || o+1!=curLo)) flush();
                if(curb==UINT64_MAX) { curb=blk; curHi=o+1; }
                buf[o]=b; curLo=o;
                ++freq[b];
                emitted.fetch_add(1,std::memory_order_relaxed);
                if(!cyclic && b==0x0a) nl.push_back(off);
            };
            auto runTask=[&](uint64_t idx) {
                uint64_t pos,run; ext.seed(idx,pos,run);
                ExtRec rec; ext.rec_at(run,rec);
                uint64_t row=rec.starts, steps, stopPos, stopRun;
                bool wrap=idx==0;
                if(wrap) { uint64_t lp,lr; ext.seed(ext.R-1,lp,lr); steps=pos+rowsTotal-lp; stopPos=lp; stopRun=lr; }
                else { uint64_t lp,lr; ext.seed(idx-1,lp,lr); steps=pos-lp; stopPos=lp; stopRun=lr; }
                for(uint64_t k=0;k<steps;++k) {
                    uint64_t r2=ext.run_of(row,rec);
                    (void)r2;
                    uint64_t off=pos? pos-1 : rowsTotal-1;
                    if(off<textLen) put(off,rec.a);
                    row=rec.lfBase+(row-rec.starts);
                    pos=off;
                }
                ExtRec stopRec; ext.rec_at(stopRun,stopRec);
                if(row!=stopRec.starts || pos!=stopPos)
                    slim_fail("parse-free walk: LF chain did not close on the next seed row");
            };
            for(;;) {
                uint64_t lo=nextBatch.fetch_add(BATCH);
                if(lo>=ext.R) break;
                uint64_t hi=std::min(lo+BATCH,ext.R);
                for(uint64_t idx=hi; idx-->lo; ) runTask(idx); // contiguous descent
                flush();
            }
            flush();
        });
        for(auto& t:ts) t.join();
        walkEmissions=emitted.load();
        if(walkEmissions!=textLen) slim_fail("parse-free walk: emitted count != text length");
        for(unsigned c=0;c<256;++c) {
            uint64_t expect=ext.totals[c];
            if(rowsTotal>textLen && c==ext.skippedByte) expect-=rowsTotal-textLen;
            uint64_t got=0; for(int t=0;t<tc;++t) got+=freqs[t][c];
            if(got!=expect) slim_fail("parse-free walk: emitted byte multiset disagrees with the runs");
        }
        if(cyclic) stringEnds.push_back(textLen);
        else {
            for(int t=0;t<tc;++t) stringEnds.insert(stringEnds.end(),newlineLists[t].begin(),newlineLists[t].end());
            std::sort(stringEnds.begin(),stringEnds.end());
        }
        fprintf(stderr,"SLIM_STRING_ENDS count=%zu bytes=%zu from_parse_dict=0 cyclic=%d\n",stringEnds.size(),stringEnds.capacity()*8,(int)cyclic);
        }
        slim_phase("parse-free-walk",last);
        // tau-spaced suffix-hash checkpoints from one backward read of the
        // sidecar (our own emitted bytes; the corpus stays untouched),
        // written straight to the PFCK sidecar — never resident at any scale.
        tau=tauOverride? tauOverride : std::max<uint64_t>(1,(8*textLen+ext.R-1)/ext.R);
        uint64_t count=textLen/tau+1;
        ckptFd=open(ckptPath.c_str(),O_CREAT|O_EXCL|O_RDWR,0666);
        if(ckptFd<0) slim_fail("create checkpoint sidecar (refusing to clobber an existing file)");
        {
            char hdr[32]; uint32_t magic=0x4B434650; // "PFCK"
            memcpy(hdr,&magic,4); uint32_t ver=1; memcpy(hdr+4,&ver,4);
            memcpy(hdr+8,&textLen,8); memcpy(hdr+16,&tau,8); memcpy(hdr+24,&count,8);
            // bytes 24..32 = count; reserved u64 appended below
            char hdr2[40]; memcpy(hdr2,hdr,32); uint64_t rsv=0; memcpy(hdr2+32,&rsv,8);
            ext_pwrite(ckptFd,hdr2,40,0,"write checkpoint header");
            constexpr uint64_t RB=1<<23;
            std::vector<uint8_t> rb(std::min<uint64_t>(RB,std::max<uint64_t>(1,textLen)));
            std::vector<uint64_t> ent; ent.reserve(RB/tau+2);
            uint64_t h=0;
            for(uint64_t base=textLen; base>0; ) {
                uint64_t lo=base>rb.size()? base-rb.size():0, len=base-lo, done=0;
                while(done<len) { ssize_t z=pread(out,rb.data()+done,len-done,lo+done); if(z<=0) slim_fail("read sidecar for fingerprints"); done+=z; }
                ent.clear();
                for(uint64_t i=len;i-->0;) {
                    uint64_t p=lo+i; h=(uint64_t)rb[i]+1+SlimFingerprint<SlimTextFile>::BASE*h;
                    if(p%tau==0) ent.push_back(h);
                }
                // ent holds indices (firstIdx..] in DESCENDING order: reverse
                uint64_t firstIdx=((lo+tau-1)/tau);
                std::reverse(ent.begin(),ent.end());
                if(ent.size()) ext_pwrite(ckptFd,ent.data(),8*ent.size(),40+8*firstIdx,"write checkpoints");
                base=lo;
            }
            if(textLen%tau==0) { uint64_t zero=0; ext_pwrite(ckptFd,&zero,8,40+8*(textLen/tau),"write checkpoints"); }
        }
        slim_phase("parse-free-fingerprints",last);
        if(close(out)) slim_fail("close text sidecar");
        sidecar=std::make_unique<SlimTextFile>(sidecarPath,textLen);
        fh=std::make_unique<SlimFingerprint<SlimTextFile>>(*sidecar,tau,fault,ckptFd,40,count);
        finish_setup(workLimit);
    }

    // ---- ADOPT mode ----------------------------------------------------
    SlimLCEParseFree(const std::string& textPath, const std::string& pfckPath,
                     uint64_t textLen_, bool cyclicText, uint64_t w1, bool fault,
                     uint64_t workLimit)
        : w(w1), textLen(textLen_), rowsTotal(textLen_), cyclic(cyclicText), walked(false) {
        if(w1<3 || w1>512) slim_fail("parse-free window out of range");
        n=textLen+w;
        double last=started;
        sidecarPath=textPath; ckptPath=pfckPath;
        sidecar=std::make_unique<SlimTextFile>(textPath,textLen);
        int fd=open(pfckPath.c_str(),O_RDONLY);
        if(fd<0) slim_fail("open checkpoint sidecar");
        char hdr[40];
        if(pread(fd,hdr,40,0)!=40) slim_fail("read checkpoint header");
        uint32_t magic,ver; uint64_t hText,hTau,hCount,hRsv;
        memcpy(&magic,hdr,4); memcpy(&ver,hdr+4,4); memcpy(&hText,hdr+8,8);
        memcpy(&hTau,hdr+16,8); memcpy(&hCount,hdr+24,8); memcpy(&hRsv,hdr+32,8);
        if(magic!=0x4B434650 || ver!=1 || hText!=textLen || !hTau || hRsv ||
           hCount!=textLen/hTau+1) slim_fail("checkpoint sidecar header mismatch");
        tau=hTau; ckptFd=fd;
        fh=std::make_unique<SlimFingerprint<SlimTextFile>>(*sidecar,tau,fault,fd,40,hCount);
        slim_phase("adopt-text+checkpoints",last);
        if(cyclic) stringEnds.push_back(textLen);
        else {
            // Newline corpora: collect string ends by one sequential scan of
            // the sidecar (bounded window; never resident).
            constexpr uint64_t RB=1<<22;
            std::vector<uint8_t> rb(RB);
            uint64_t off=0;
            while(off<textLen) {
                uint64_t want=std::min<uint64_t>(RB,textLen-off), done=0;
                while(done<want) { ssize_t z=pread(sidecar->fd,rb.data()+done,want-done,off+done);
                    if(z<=0) slim_fail("scan text for terminators"); done+=z; }
                for(uint64_t i=0;i<want;++i) if(rb[i]==0x0a) stringEnds.push_back(off+i);
                off+=want;
            }
            std::sort(stringEnds.begin(),stringEnds.end());
        }
        fprintf(stderr,"SLIM_STRING_ENDS count=%zu bytes=%zu from_parse_dict=0 cyclic=%d\n",stringEnds.size(),stringEnds.capacity()*8,(int)cyclic);
        finish_setup(workLimit);
    }

    // Shared tail: fail-loud journal + startup self-check.
    void finish_setup(uint64_t workLimit) {
        double last=tnow();
        // Journaled fail-loud budget (sharded per thread). The legacy parse
        // slim charged mixed phrase/byte units and ran UNCAPPED in production
        // (total_limit=INF); its yeast run verified ~1500 bytes/query = 63n
        // bytes overall, the same reads this backend performs. The
        // byte-honest cap is 128n (2x the worst measured corpus); any corpus
        // exceeding it fails loudly.
        uint64_t limit=workLimit? workLimit : std::max<uint64_t>(1000000000ULL,128*textLen);
        work=std::make_unique<SlimSeamWork>(limit,UINT64_MAX);
        fh->seam_policy(*work,tau,"parse-free-text");
        // Startup self-check: adopted checkpoints against direct window reads.
        {
            selfChecks=std::min<uint64_t>(256,textLen);
            for(uint64_t k=0;k<selfChecks;++k) {
                uint64_t p=textLen? (k*2654435761ULL+0x9e3779b9ULL)%textLen : 0;
                uint64_t len=std::min<uint64_t>(tau,textLen-p);
                if(!len) continue;
                // h(p) - BASE^len * h(p+len) == polynomial hash of T[p..p+len)
                // with the FIRST byte at coefficient BASE^0 (SlimFingerprint order).
                uint64_t direct=0, pk=1;
                for(uint64_t i=0;i<len;++i) { direct+=((uint64_t)(*sidecar)[p+i]+1)*pk; pk*=FBase(); }
                uint64_t fromFp=fh->suffix(p)-Fpw(len)*fh->suffix(p+len);
                if(direct!=fromFp) slim_fail("parse-free fingerprint self-check mismatch");
            }
            fprintf(stderr,"SLIM_PF_SELF_CHECK checks=%llu passed=1\n",(unsigned long long)selfChecks);
        }
        slim_phase("parse-free-self-check",last);
        fprintf(stderr,"SLIM_PARSE_FREE_STRUCT mode=%s n=%llu text_len=%llu rows=%llu tau=%llu checkpoints=%llu checkpoint_bytes=%llu sidecar_bytes=%llu seeds=%llu emissions=%llu cyclic=%d work_limit=%llu no_SA_ISA_LCP_RMQ=1 no_M_b_bwt_w_wt=1 no_parse_dict=1 no_phrase_ids=1 corpus_reads=0\n",
            walked?"walk":"adopt",
            (unsigned long long)n,(unsigned long long)textLen,(unsigned long long)rowsTotal,(unsigned long long)tau,
            (unsigned long long)fh->hashCount,(unsigned long long)(fh->hashCount*8),
            walked?(unsigned long long)textLen:0ULL,(unsigned long long)seedCount,(unsigned long long)walkEmissions,(int)cyclic,
            (unsigned long long)work->limit);
    }
    void flush_thread() const {
        fh->flushThread(); work->flushThread(); seedQueries.flushThread();
    }
    uint8_t text_byte(uint64_t pos) const { if(pos>=textLen) slim_fail("parse-free text byte bounds"); return (*sidecar)[pos]; }
    static uint64_t FBase() { return SlimFingerprint<SlimTextFile>::BASE; }
    static uint64_t Fpw(uint64_t e) { return SlimFingerprint<SlimTextFile>::power(e); }
    ~SlimLCEParseFree() {
        if(!walked) return;   // adopt-mode inputs are owned by the merge
        if(sidecar) {
            const char* keep=getenv("SLIM_PF_KEEP_TEXT");
            sidecar.reset();
            if(!keep) unlink(sidecarPath.c_str());
            else fprintf(stderr,"SLIM_PF_KEEP_TEXT sidecar=%s retained\n",sidecarPath.c_str());
        }
        if(ckptFd>=0) {
            const char* keep=getenv("SLIM_PF_KEEP_TEXT");
            close(ckptFd); ckptFd=-1;
            if(!keep) unlink(ckptPath.c_str());
            else fprintf(stderr,"SLIM_PF_KEEP_TEXT ckpt=%s retained\n",ckptPath.c_str());
        }
    }
    SlimLCEParseFree(const SlimLCEParseFree&)=delete;
    SlimLCEParseFree& operator=(const SlimLCEParseFree&)=delete;
    // Collection LCE semantics are identical to SlimLCE: cyclic corpora compare
    // the one cyclic byte string with at most two seam crossings, clamped so a
    // raw query never leaves [0,textLen); newline corpora stop before the
    // terminator on either side. Positions are text coordinates throughout.
    uint64_t collection_lce(uint64_t i,uint64_t j) const {
        if(cyclic) {
            uint64_t size=textLen, answer=0;
            if(i>=size||j>=size) slim_fail("parse-free cyclic LCE bounds");
            if(i==j){seedQueries.add(1);return size;}
            while(answer<size) {
                uint64_t cap=std::min({size-answer,size-i,size-j});
                uint64_t got=std::min(fh->lce(i,j,cap),cap);
                answer+=got;
                if(got<cap) break;
                i=(i+got)%size; j=(j+got)%size;
            }
            return answer;
        }
        auto remaining=[&](uint64_t pos) {
            auto end=std::lower_bound(stringEnds.begin(),stringEnds.end(),pos);
            if(end==stringEnds.end()) slim_fail("parse-free: missing collection terminator");
            return *end-pos;
        };
        uint64_t cap=std::min(remaining(i),remaining(j));
        if(!cap){seedQueries.add(1);return 0;}
        return std::min(fh->lce(i,j,cap),cap);
    }
    void report() const {
        fh->report("parse-free-text");
        fprintf(stderr,"SLIM_PARSE_FREE mode=%s text_len=%llu rows=%llu sidecar_bytes=%llu tau=%llu checkpoint_bytes=%llu seeds=%llu emissions=%llu cyclic=%d string_ends=%zu work_used=%llu work_limit=%llu sidecar_owned=%d corpus_reads=0\n",
            walked?"walk":"adopt",
            (unsigned long long)textLen,(unsigned long long)rowsTotal,
            walked?(unsigned long long)textLen:0ULL,(unsigned long long)tau,
            (unsigned long long)(fh->hashCount*8),(unsigned long long)seedCount,(unsigned long long)walkEmissions,
            (int)cyclic,stringEnds.size(),(unsigned long long)work->totalUsed(),(unsigned long long)work->limit,(int)walked);
    }
};

struct SlimLCE {
    double started=tnow();
    mutable SlimSharded seedQueries;
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
    SlimLCE(const std::string& prefix,uint64_t r,uint64_t t1,uint64_t t2,bool stream,bool fault=false,bool cyclicText=false,uint64_t w1=10):d(prefix+".dict",w1,stream),w(w1),cyclic(cyclicText) {
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
            if(i==j){seedQueries.add(1);return size;}
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
        if(!cap){seedQueries.add(1);return 0;}
        return std::min((*this)(i,j),cap);
    }
    void flush_thread() const { ph->flushThread(); dh->flushThread(); seedQueries.flushThread(); }
    uint64_t operator()(uint64_t i,uint64_t j)const {
        seedQueries.add(1);
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
                     const std::string& headSaPath="",uint64_t w1=10) {
    if(!profileOnly && (!ri.haveSa||ri.sampleAllInf()))slim_fail("usable ri4 samples required");
    double last=tnow();
    SlimHeads heads(headSaPath.empty()?ri.sxiPath:headSaPath,ri.R,headSaPath.empty()?ri.headOffset:0);
    // Auto-detect using the existing RLE alphabet, with no input text scan.
    // New collections use reserved 0x1e; texts without newline are cyclic
    // too. Preserve legacy newline collection behavior otherwise.
    bool hasRS=false,hasNL=false;
    for(uint64_t run=0;run<ri.R;++run){hasRS|=ri.a[run]==0x1e;hasNL|=ri.a[run]==0x0a;}
    // Parse-free backend (no --parse): LCE from the merged structure itself.
    // Legacy backend unchanged when a PFP prefix is supplied.
    std::unique_ptr<SlimLCE> lgce;
    lgce=std::make_unique<SlimLCE>(prefix,ri.R,t1,t2,stream,fault,hasRS||!hasNL,w1);
    uint64_t lceN=lgce->n;
    size_t lceEnds=lgce->stringEnds.size();
    if(lceN!=ri.n+w1)slim_fail("parse/ri4 length mismatch");
    if(lceEnds!=ri.k)slim_fail("parse/ri4 string-end count mismatch");
    auto collection_lce=[&](uint64_t i,uint64_t j){return lgce->collection_lce(i,j);};
    slim_phase("lce-build-total",last);
    LfIndex lf;lf.build(ri); // full tables: --flat calibration and resolver fallback walk interior rows via lf()
    Anchors anc;if(!anchorsPath.empty())anc.load(anchorsPath);
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
    uint64_t lfbytes=0;for(auto& v:lf.charRuns)lfbytes+=v.capacity()*sizeof(uint64_t);for(auto& v:lf.charSum)lfbytes+=v.capacity()*8;
    fprintf(stderr,"SLIM_RI runs=%llu starts=%llu samples=%llu lf_capacity=%llu\n",(unsigned long long)(ri.R*5),(unsigned long long)(ri.R*8),(unsigned long long)(ri.saWords.size()*8),(unsigned long long)lfbytes);
    if(profileOnly){ lgce->ph->report("parse");lgce->dh->report("dict"); fprintf(stderr,"SLIM_PROFILE_ONLY no queries or aggregate produced\n");return 0; }
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
        fprintf(stderr,"SLIM_CALIBRATION ri4_ROW_OFF=0 PFP_text_shift=%llu checked=%llu bad=0\n",(unsigned long long)w1,(unsigned long long)checked);
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
            buf[0][k]=a?collection_lce(prev,first):0;
            buf[3][k]=a==b?INF:collection_lce(first,tail);
        };
        lgce->flush_thread();};
        auto parallel=[&](auto& worker){next=0;std::vector<std::thread> ts;for(int t=0;t<std::max(1,std::min(threads,64));++t)ts.emplace_back([&,t]{slim_set_thread_shard((unsigned)t);worker();});for(auto& t:ts)t.join();};
        double began=tnow();parallel(resolveWorker);measure(began,resolveWall,resolvePeak);
        began=tnow();parallel(lceWorker);measure(began,lceWall,lcePeak);
        began=tnow();
        for(uint64_t field=0;field<4;++field)put(buf[field].data(),count*8,12+8*(field*ri.R+start));
        measure(began,writeWall,writePeak);
        if(start%(CHUNK*16)==0){fprintf(stderr,"SLIM_PROGRESS runs=%llu/%llu resolve_wall=%.3f lce_wall=%.3f write_wall=%.3f lf_steps=%llu cache_hits=%llu\n",(unsigned long long)(start+count),(unsigned long long)ri.R,resolveWall,lceWall,writeWall,(unsigned long long)resolver.sumSteps.load(),(unsigned long long)resolver.viaCache.load());slim_phase("query-chunks",last);}
    }
    if(close(fd)||rename(tmp.c_str(),out.c_str()))slim_fail("publish aggregate");
    slim_phase("queries+stream-write-final",last);
    lgce->ph->report("parse");lgce->dh->report("dict");
    fprintf(stderr,"SLIM_QUERY_PHASE resolve_wall=%.6f resolve_peak_kib=%ld lce_wall=%.6f lce_peak_kib=%ld write_wall=%.6f write_peak_kib=%ld\n",resolveWall,resolvePeak,lceWall,lcePeak,writeWall,writePeak);
    {const auto& seeds=lgce->seedQueries;
     const auto& checked=lgce->ph->checked;
     const auto& maxChecked=lgce->ph->maxChecked;
     const char* unit="phrases";
    fprintf(stderr,"SLIM_SEEDS queries=%llu mean_verified_%s=%.9f max_verified_%s=%llu (includes_zero_parse_work_seeds; boundary_included)\n",(unsigned long long)seeds.read(),unit,seeds.read()?double(checked.read())/seeds.read():0.0,unit,(unsigned long long)maxChecked.load());
    }
    fprintf(stderr,"SLIM_RESOLVE walks=%llu steps=%llu max=%llu anchor=%llu failed=%llu cache_hits=%llu\n",(unsigned long long)resolver.nWalks.load(),(unsigned long long)resolver.sumSteps.load(),(unsigned long long)resolver.maxSteps.load(),(unsigned long long)resolver.viaAnchor.load(),(unsigned long long)resolver.hit0a.load(),(unsigned long long)resolver.viaCache.load());
    fprintf(stderr,"SLIM_BOUNDARY_RESOLVE head_direct=%llu tail_direct=%llu head_lf_steps=%llu\n",
        (unsigned long long)resolver.directHeads.load(),(unsigned long long)resolver.directTails.load(),
        (unsigned long long)(resolver.sumSteps.load()-queryStepsBefore));
    return 0;
}

#endif // SLIM_LCE_CORE_ONLY
