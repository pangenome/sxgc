from pathlib import Path
p=Path('bit6/slim_lce.hpp');s=p.read_text()
pos=s.index('// Polynomial suffix hash')
s=s[:pos]+'''// Seam-only work policy. One work unit is a phrase-ID/byte comparison or
// one dictionary/parse symbol read while reconstructing a sampled hash.
// Preprocessing remains O(P+D+r), never an expanded-text walk. Each direct
// verification is capped at 2^26 units; all seam queries share at most
// min(2^32, max(10^6, (P+D)/8)) units. The floor admits bounded tiny fixtures;
// the absolute ceilings make additional verification sublinear as n grows.
// Hashes only propose answers: the complete prefix and boundary stay exact.
struct SlimSeamWork {
    uint64_t limit;
    mutable std::atomic<uint64_t> used{0};
    explicit SlimSeamWork(uint64_t size):limit(std::min<uint64_t>(1ULL<<32,std::max<uint64_t>(1000000,size/8))) {}
    bool reserve(uint64_t count) const {
        uint64_t old=used.load(std::memory_order_relaxed);
        do {if(count>limit-old)return false;}
        while(!used.compare_exchange_weak(old,old+count,std::memory_order_relaxed));
        return true;
    }
};

'''+s[pos:]
s=s.replace('uint64_t verificationLimit=INF; // Adapter policy; default preserves slim behavior.', '''uint64_t verificationLimit=INF, probeLimit=INF; // Seam adapter only.
    const char* reportName="unrestricted";
    SlimSeamWork* seamWork=nullptr;
    mutable std::atomic<uint64_t> verificationWork{0}, maxRequested{0};
    void seam_policy(SlimSeamWork& work,uint64_t probe,const char* name) {
        seamWork=&work;probeLimit=probe;reportName=name;
        verificationLimit=std::min<uint64_t>(1ULL<<26,std::max<uint64_t>(1000,s.size()));
    }
    void charge(uint64_t count,const char* site,uint64_t requested=0) const {
        if(seamWork && !seamWork->reserve(count)) {
            fprintf(stderr,"CYCLIC_SEAM_REFUSED site=%s requested_lce=%llu requested_work=%llu total_work=%llu total_limit=%llu no_O_n_fallback=1\\n",site,
                (unsigned long long)requested,(unsigned long long)count,(unsigned long long)seamWork->used.load(),(unsigned long long)seamWork->limit);
            report(reportName);slim_fail("CYCLIC_SEAM_REFUSED: total compressed-work budget exhausted; no O(n) fallback");
        }
    }''')
s=s.replace('if(end-i>verificationLimit)slim_fail("CYCLIC_SEAM_REFUSED: fingerprint probe exceeds polylog-work policy; no O(n) fallback");', '''if(end-i>probeLimit) {
            fprintf(stderr,"CYCLIC_SEAM_REFUSED probe_symbols=%llu probe_limit=%llu\\n",(unsigned long long)(end-i),(unsigned long long)probeLimit);
            report(reportName);slim_fail("CYCLIC_SEAM_REFUSED: fingerprint probe exceeds polylog-work policy; no O(n) fallback");
        }
        charge(end-i,"hash-probe");''')
s=s.replace('while(lo<quick && s[i+lo]==s[j+lo])++lo;', '''uint64_t quickWork=0;
        while(lo<quick) {
            charge(1,"quick-compare");++quickWork;
            if(s[i+lo]!=s[j+lo])break;
            ++lo;
        }
        verificationWork.fetch_add(quickWork,std::memory_order_relaxed);''')
s=s.replace('if(guess>verificationLimit) {','''uint64_t mx=maxRequested.load(std::memory_order_relaxed);
        while(guess>mx && !maxRequested.compare_exchange_weak(mx,guess,std::memory_order_relaxed)) {}
        uint64_t directWork=guess+(guess<cap);
        if(quickWork>verificationLimit || directWork>verificationLimit-quickWork) {''')
s=s.replace('(unsigned long long)quick);','(unsigned long long)quickWork);')
s=s.replace('report("seam-refusal");','report(reportName);')
s=s.replace('LCE verification exceeds polylog-work policy;', 'LCE verification exceeds compressed-work policy;')
s=s.replace('for(uint64_t k=0;k<guess;++k)', '''charge(directWork,"direct-verify",guess);
        verificationWork.fetch_add(directWork,std::memory_order_relaxed);
        for(uint64_t k=0;k<guess;++k)''')
s=s.replace('max_verified=%llu\\n",name,','max_verified=%llu max_requested_lce=%llu verification_work=%llu suffix_symbol_reads=%llu total_work=%llu total_limit=%llu verification_limit=%llu probe_limit=%llu\\n",name,')
s=s.replace('(unsigned long long)maxChecked.load());}', '''(unsigned long long)maxChecked.load(),
        (unsigned long long)maxRequested.load(),(unsigned long long)verificationWork.load(),(unsigned long long)suffixReads.load(),
        (unsigned long long)(seamWork?seamWork->used.load():0),(unsigned long long)(seamWork?seamWork->limit:INF),
        (unsigned long long)verificationLimit,(unsigned long long)probeLimit);}''')
p.write_text(s);Path('xsa/runtime/bit6/slim_lce.hpp').write_text(s)
p=Path('bit6/seam_repair.hpp');s=p.read_text()
s=s.replace('lce.ph->verificationLimit=queryLimit;lce.dh->verificationLimit=queryLimit;', '''SlimSeamWork work(lce.p.size()+lce.d.size());
  lce.ph->seam_policy(work,queryLimit,"seam-parse");lce.dh->seam_policy(work,queryLimit,"seam-dict");
  U maxSeamLCE=0;''')
s=s.replace('CYCLIC_SEAM_LCE_POLICY max_probe_and_verification=%llu max(1000,bit_width(raw_n)^3)\\n",(unsigned long long)queryLimit', 'CYCLIC_SEAM_LCE_POLICY max_probe=%llu max_verification=67108864 total_limit=%llu cost=phrase_or_byte_comparisons_plus_hash_symbol_reads fraction=(P+D)/8 floor=1000000 ceiling=4294967296\\n",(unsigned long long)queryLimit,(unsigned long long)work.limit')
s=s.replace('U len=lce.collection_lce(a,b);','U len=lce.collection_lce(a,b);maxSeamLCE=std::max(maxSeamLCE,len);')
s=s.replace('lce.ph->report("seam-parse");', 'fprintf(stderr,"CYCLIC_SEAM_WORK max_seam_lce=%llu total_work=%llu total_limit=%llu exact=1\\n",(unsigned long long)maxSeamLCE,(unsigned long long)work.used.load(),(unsigned long long)work.limit);\n  lce.ph->report("seam-parse");')
p.write_text(s);Path('xsa/runtime/bit6/seam_repair.hpp').write_text(s)
