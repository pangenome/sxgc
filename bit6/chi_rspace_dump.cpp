// chi_rspace_dump.cpp — per-run aggregates from PFP artifacts ONLY
// (.ri4 + PFP parse/dict; NO lcp_index required).  No O(n) pass, no
// per-position walks, no SA materialization.
//
// topLCP is PLCP at the run-head row = LCP(SA[a-1], SA[a]) (the classic
// adjacent-row LCP), computed with the vendored pfp LCE support
// (dictionary RMQ + parse ISA/RMQ + rank/select: polylog/call).  The
// former lcp_index lookup was redundant: PLCP(sa_row a) IS that LCP.
// --lcp-index is retained ONLY for an optional cross-check (CHECK_TOP),
// and G0_ALL validates the identity at every row/position.
//
// Per BWT run i (rows [a,b]):
//   posFirst/posLast  = pfp_sa_support(resolve) of rows a,b   [O(log)]
//   topLCP            = piece law (phi-interval) at posFirst  [production]
//                       cross-checked vs LCE(S_{a-1}, S_a) on a sample
//   interiorMin       = LCP(S_a, S_b) via the classic identity
//                       min_{k in (a,b]} LCP[k] = LCP(S_a,S_b), computed
//                       as pfp_lce_support clamp(n - max(p_a,p_b))
//                       (INF for single-row runs)
//
// Slim --head-sa FILE consumes exactly R raw little-endian u64 head SA
// values. Boundary queries use known run IDs in O(1); absent the sidecar,
// head queries retain the sample/anchor LF resolver (and optional cache).
//
// Output (aggregates sidecar): "CRA1" u32, R u64, then R*4 u64 LE:
//   topLCP[], saFirst[], saLast[], interiorMin[]  (u64max = INF)
//
// Row-space convention (pinned by tools/parse_resolve_proto.py, gated):
//   machinery row m <-> .ri4/linear row m - W + 1  (rows 1..N-1);
//   row 0 = the trailing-terminator suffix (position n-1); calibrated at
//   startup: the machinery dollar row resolving to n-1 is found by scan
//   (W probes) and asserted.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <atomic>
#include <fstream>
#include <iostream>
#include <cassert>
#include <mutex>
#include <memory>
#include <functional>
#include <chrono>

#include <sdsl/int_vector.hpp>
#include "pfp/utils.hpp"
typedef pfpds::long_type long_type;
#include "moveStructure/moveStructure.h"

// pfp_ds (vendored in r-pfbwt) + gsacak.  bit6/pfp_ds_vendor/pfp/ is a
// verbatim copy of the vendored headers EXCEPT pfp.hpp, which gains a
// defer-build constructor: the standard one cannot skip
// build_b_bwt_and_M() (O(n)-bit b_bwt + |M| ~ 0.1n), which is exactly the
// cost the --pfp-index load path exists to avoid.  The vendor dir must be
// FIRST on the include path so "pfp/*.hpp" resolves there.
#include "pfp/pfp.hpp"
#include "pfp/sa_support.hpp"
#include "pfp/lce_support.hpp"
extern "C" {
#include "gsacak.h"
}

static const uint64_t INF = std::numeric_limits<uint64_t>::max();
static const uint32_t IDX_MAGIC = 0x31465058; // "XPF1" (bit6/pfp_index_build.cpp)
static const uint32_t IDX_VERSION = 2;

// phase timing (G3 cost table)
static double G_T0;
static double tnow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static void phase(const char* what) {
    fprintf(stderr, "PHASE %-18s %8.2f s\n", what, tnow() - (double)G_T0);
}

// ---------------- .ri4 loader (runs only) ----------------
struct Ri4 {
    uint64_t n = 0, k = 0, R = 0;
    std::vector<uint64_t> C;      // 256
    std::vector<uint8_t> a;       // run chars
    std::vector<uint32_t> l;      // run lens
    std::vector<uint64_t> starts; // run row starts
    // Per-run SA samples (v4 mirrored convention: sample = (n-1) - SA[run_end]).
    uint64_t saBits = 0;
    uint8_t saW = 0;
    std::vector<uint64_t> saWords;
    bool haveSa = false;
    void load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); exit(1); }
        uint32_t magic, ver;
        f.read((char*)&magic, 4); f.read((char*)&ver, 4);
        if (magic != 0x52585349 || ver != 4) { fprintf(stderr, "bad .ri4 magic/version\n"); exit(1); }
        f.read((char*)&n, 8); f.read((char*)&k, 8); f.read((char*)&R, 8);
        C.resize(256); f.read((char*)C.data(), 8 * 256);
        a.resize(R); f.read((char*)a.data(), R);
        l.resize(R); f.read((char*)l.data(), 4 * R);
        starts.resize(R);
        uint64_t acc = 0;
        for (uint64_t i = 0; i < R; ++i) { starts[i] = acc; acc += l[i]; }
        if (acc != n) { fprintf(stderr, "FATAL: run sum %llu != n %llu\n",
                                 (unsigned long long)acc, (unsigned long long)n); exit(1); }
        // Optional trailing sdsl int_vector<saW>: per-run SA samples.
        f.read((char*)&saBits, 8); f.read((char*)&saW, 1);
        if (f && saW > 0 && saW <= 64 && saBits == R * (uint64_t)saW) {
            size_t nw = (size_t)((saBits + 63) / 64);
            saWords.resize(nw);
            f.read((char*)saWords.data(), (std::streamsize)(8 * nw));
            if (!f) { fprintf(stderr, "FATAL: .ri4 sample array truncated\n"); exit(1); }
            haveSa = true;
        }
    }
    // LSB-first packed read (mirrors sdsl::int_vector<w>).
    inline uint64_t sample(uint64_t i) const {
        uint64_t bit0 = i * saW, w = bit0 >> 6, b0 = bit0 & 63;
        if (b0 + saW <= 64) return (saWords[w] >> b0) & ((1ULL << saW) - 1);
        return ((saWords[w] >> b0) | (saWords[w + 1] << (64 - b0))) & ((1ULL << saW) - 1);
    }
    bool sampleAllInf() const {
        return haveSa && sample(0) == ((1ULL << saW) - 1);
    }
};

// ---------------- .ri4-sample position resolver (the standalone-LCE path) ----------------
// SA at a .ri4 row WITHOUT pfp_sa_support (no M, no b_bwt, no w_wt).
// Mirrors xsa/src/main.rs `s_at` (gated byte-exact at 466 in the Rust sweep):
//   sample(i) is stored at each run END in the mirrored convention
//   sample(i) = (n-1) - SA[run_end(i)];  each LF step raises S by 1, so
//   SA[row] = (n-1) - (sample(run') - steps),  run' = the run end the LF
//   walk from `row` first lands on.
// LF from an interior 0x0A row is undefined (the sentinel-erasure law); such a
// walk terminates only via a string-start anchor (--anchors), else it FAILS
// LOUDLY.
struct LfIndex {
    const Ri4* ri = nullptr;
    std::vector<uint64_t> Cless;                        // 256: rows with char < c
    std::vector<std::vector<uint32_t>> charRuns;        // per char: run ids
    std::vector<std::vector<uint64_t>> charSum;         // per char: prefix lens
    void build(const Ri4& r) {
        ri = &r;
        Cless.assign(256, 0);
        std::vector<uint64_t> Ctot(256, 0);
        for (uint64_t i = 0; i < r.R; ++i) Ctot[r.a[i]] += r.l[i];
        uint64_t acc = 0;
        for (int c = 0; c < 256; ++c) { Cless[c] = acc; acc += Ctot[c]; }
        charRuns.assign(256, {});
        charSum.assign(256, {});
        for (uint64_t i = 0; i < r.R; ++i) charRuns[r.a[i]].push_back((uint32_t)i);
        for (int c = 0; c < 256; ++c) {
            if (charRuns[c].empty()) continue;
            charSum[c].resize(charRuns[c].size() + 1, 0);
            for (size_t t = 0; t < charRuns[c].size(); ++t)
                charSum[c][t + 1] = charSum[c][t] + r.l[charRuns[c][t]];
        }
    }
    inline uint64_t run_of(uint64_t row) const {
        return (uint64_t)(std::upper_bound(ri->starts.begin(), ri->starts.end(), row)
                          - ri->starts.begin() - 1);
    }
    inline uint64_t lf(uint64_t row) const {
        uint64_t r = run_of(row);
        uint8_t ch = ri->a[r];
        uint64_t o = row - ri->starts[r];
        auto& vr = charRuns[ch];
        uint64_t k = (uint64_t)(std::lower_bound(vr.begin(), vr.end(), (uint32_t)r) - vr.begin());
        return Cless[ch] + charSum[ch][k] + o;
    }
};

// String-start anchor table (XANC: u32 magic 0x434E4158, u64 k, k*(u64 row, u64 S)).
struct Anchors {
    std::vector<uint64_t> rows, s;
    void load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { fprintf(stderr, "cannot open anchors %s\n", path.c_str()); exit(1); }
        uint32_t magic; uint64_t k;
        f.read((char*)&magic, 4);
        if (magic != 0x434E4158) { fprintf(stderr, "anchors: bad magic\n"); exit(1); }
        f.read((char*)&k, 8);
        rows.resize(k); s.resize(k);
        for (uint64_t i = 0; i < k; ++i) { f.read((char*)&rows[i], 8); f.read((char*)&s[i], 8); }
        fprintf(stderr, "anchors: %llu string-start rows\n", (unsigned long long)k);
    }
    inline bool lookup(uint64_t row, uint64_t& s0) const {
        auto it = std::lower_bound(rows.begin(), rows.end(), row);
        if (it != rows.end() && *it == row) { s0 = s[(size_t)(it - rows.begin())]; return true; }
        return false;
    }
};

// Optional bounded exact memoization of LF-derived positions. Hash collisions
// only evict entries: full row keys are compared under a per-slot lock. This
// is neither a text-row map nor a dense n-sized structure; its cap is explicit.
struct ResolveCache {
    struct Entry {
        std::atomic_flag busy=ATOMIC_FLAG_INIT;
        std::atomic<uint64_t> row{INF};
        uint64_t sa=INF;
    };
    size_t slots;
    std::unique_ptr<Entry[]> entries;
    explicit ResolveCache(size_t count):slots(count),entries(new Entry[count]) {
        if(!count || (count&(count-1))) {fprintf(stderr,"invalid resolve cache size\n");exit(2);}
    }
    size_t slot(uint64_t row)const {
        row^=row>>30;row*=0xbf58476d1ce4e5b9ULL;row^=row>>27;
        row*=0x94d049bb133111ebULL;row^=row>>31;
        return row&(slots-1);
    }
    bool lookup(uint64_t row,uint64_t& sa) {
        auto& e=entries[slot(row)];
        // Most probes miss. Reading an atomic tag first avoids dirtying a
        // random cache line for every LF step. Recheck after taking the lock.
        if(e.row.load(std::memory_order_relaxed)!=row)return false;
        if(e.busy.test_and_set(std::memory_order_acquire))return false;
        bool found=e.row.load(std::memory_order_relaxed)==row;if(found)sa=e.sa;
        e.busy.clear(std::memory_order_release);return found;
    }
    void put(uint64_t row,uint64_t sa) {
        auto& e=entries[slot(row)];
        if(e.busy.test_and_set(std::memory_order_acquire))return;
        e.sa=sa;e.row.store(row,std::memory_order_relaxed);e.busy.clear(std::memory_order_release);
    }
    size_t bytes()const{return slots*sizeof(Entry);}
};

struct SampleResolver {
    const Ri4* ri = nullptr;
    const LfIndex* lf = nullptr;
    const Anchors* anc = nullptr;
    ResolveCache* cache = nullptr;
    // Optional raw u64 SA values, indexed by run; tails remain mirrored in ri4.
    const uint64_t* headSa = nullptr;
    mutable std::atomic<uint64_t> directHeads{0}, directTails{0};
    inline uint64_t sa_tail(uint64_t run) const {
        directTails.fetch_add(1,std::memory_order_relaxed);
        uint64_t sample=ri->sample(run);
        return sample<ri->n ? ri->n-1-sample : INF;
    }
    inline uint64_t sa_head(uint64_t run) const {
        if(!headSa) return sa_at(ri->starts[run]);
        directHeads.fetch_add(1,std::memory_order_relaxed);
        uint64_t value=headSa[run];
        if(value>=ri->n || (ri->l[run]==1 && value!=ri->n-1-ri->sample(run))) {
            fprintf(stderr,"FATAL: head-SA value/range or singleton mismatch at run %llu\n",(unsigned long long)run);
            exit(2);
        }
        return value;
    }
    mutable std::atomic<uint64_t> nWalks{0}, sumSteps{0}, maxSteps{0}, hit0a{0}, viaAnchor{0}, viaCache{0};
    inline uint64_t sa_at(uint64_t row) const {
        if(headSa) {
            uint64_t run=lf->run_of(row);
            if(row==ri->starts[run]) return sa_head(run);
            if(row==ri->starts[run]+ri->l[run]-1) return sa_tail(run);
        }
        uint64_t pos = row, steps = 0;
        struct Point {uint64_t row, distance;};
        Point path[4096]; // uninitialized; bounded 64 KiB of stack per worker
        size_t used=0;uint64_t stride=8;
        auto finish=[&](uint64_t sample)->uint64_t {
            nWalks++;sumSteps+=steps;
            uint64_t mx=maxSteps.load();
            while(steps>mx && !maxSteps.compare_exchange_weak(mx,steps)) {}
            uint64_t answer=(ri->n-1)-(sample-steps);
            if(cache && answer<ri->n) {
                for(size_t i=0;i<used;++i)
                    if(answer>=path[i].distance)cache->put(path[i].row,answer-path[i].distance);
            }
            return answer;
        };
        for (;;) {
            uint64_t r = lf->run_of(pos);
            uint64_t e = ri->starts[r] + ri->l[r];
            if (pos == e - 1) {
                return finish(ri->sample(r));
            }
            uint64_t cached;
            if(cache && cache->lookup(pos,cached)) {
                viaCache++;
                return finish((ri->n-1)-cached);
            }
            if (ri->a[r] == 0x0A) {
                uint64_t s0;
                if (anc && anc->lookup(pos, s0)) {
                    viaAnchor++;
                    return finish(s0);
                }
                hit0a++;
                return INF;
            }
            if(cache && steps%stride==0) {
                if(used==4096) {
                    // Double checkpoint spacing, preserving coverage of the
                    // whole walk without an unbounded path allocation.
                    for(size_t i=0;i<2048;++i)path[i]=path[2*i];
                    used=2048;stride*=2;
                }
                if(steps%stride==0)path[used++]={pos,steps};
            }
            pos = lf->lf(pos);
            steps++;
        }
    }
};

// ---------------- lcp_index (pieces) — pattern from bit6/teralcp_chi.cpp ----------------
struct ChiIndex {
    uint64_t totalLen = 0;
    sdsl::int_vector<> F;
    MoveStructureTable Psi;
    sdsl::int_vector<> intAtTop;
    MoveStructureStartTable Phi;
    sdsl::int_vector<> PLCPsamples;
    void load(std::istream& in) {
        sdsl::load(totalLen, in);
        sdsl::load(F, in);
        sdsl::load(Psi, in);
        sdsl::load(intAtTop, in);
        sdsl::load(Phi, in);
        sdsl::load(PLCPsamples, in);
    }
};

struct PhiLookup {
    std::vector<uint64_t> start;
    const MoveStructureStartTable* phi;
    uint64_t nInt;
};

static void build_phi_lookup(const ChiIndex& idx, PhiLookup& P) {
    P.phi = &idx.Phi;
    P.nInt = idx.Phi.data.size() - 1;
    P.start.resize(P.nInt + 1);
    for (uint64_t j = 0; j <= P.nInt; ++j) P.start[j] = idx.Phi.data.get<2>(j);
    for (uint64_t j = 0; j < P.nInt; ++j)
        if (P.start[j] > P.start[j + 1]) { fprintf(stderr, "FATAL: Phi starts not monotone\n"); exit(1); }
}

static inline uint64_t lcp_at_pos(const ChiIndex& idx, const PhiLookup& P, uint64_t pos) {
    uint64_t lo = 0, hi = P.nInt;
    while (lo < hi) {
        uint64_t mid = (lo + hi + 1) / 2;
        if (P.start[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return idx.PLCPsamples[lo] - (pos - P.start[lo]);
}

// ---------------- persisted PFP query index (bit6/pfp_index_build.cpp) ----------------
// Restores exactly the members the two support classes read.  D and PP are
// constructed with build flags OFF by the caller (D still builds b_d + its
// rank/select, an O(|D|) scan with no suffix sort; PP reads .parse only).
// The load overwrites every other queried member with the persisted copy.
static void load_pfp_index(const std::string& path,
                           pfpds::dictionary<uint8_t>& D,
                           pfpds::parse& PP,
                           pfpds::pf_parsing<uint8_t>& PF) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { fprintf(stderr, "cannot open pfp index %s\n", path.c_str()); exit(1); }
    uint32_t magic = 0, ver = 0;
    uint64_t W = 0, pf_n = 0, m_count = 0, d_size = 0, p_size = 0, wt_size = 0;
    in.read((char*)&magic, 4); in.read((char*)&ver, 4);
    if (magic != IDX_MAGIC || ver != IDX_VERSION) {
        fprintf(stderr, "FATAL: bad pfp index magic/version (%08x,%u)\n", magic, ver); exit(1);
    }
    in.read((char*)&W, 8); in.read((char*)&pf_n, 8); in.read((char*)&m_count, 8);
    in.read((char*)&d_size, 8); in.read((char*)&p_size, 8); in.read((char*)&wt_size, 8);

    sdsl::load(PF.b_bwt, in);
    PF.b_bwt_rank_1 = sdsl::bit_vector::rank_1_type(&PF.b_bwt);
    PF.b_bwt_select_1 = sdsl::bit_vector::select_1_type(&PF.b_bwt);
    sdsl::load(PF.b_p, in);
    PF.rank_b_p = sdsl::bit_vector::rank_1_type(&PF.b_p);
    PF.select_b_p = sdsl::bit_vector::select_1_type(&PF.b_p);
    PF.w_wt.load(in);
    sdsl::load(PP.saP, in);
    sdsl::load(PP.isaP, in);
    sdsl::load(PP.lcpP, in);
    PP.rmq_lcp_P = sdsl::rmq_succinct_sct<>(&PP.lcpP);
    PP.saP_flag = PP.isaP_flag = PP.lcpP_flag = PP.rmq_lcp_P_flag = true;
    sdsl::load(D.b_d, in);
    D.rank_b_d = sdsl::bit_vector::rank_1_type(&D.b_d);
    D.select_b_d = sdsl::bit_vector::select_1_type(&D.b_d);
    sdsl::load(D.isaD, in);
    sdsl::load(D.lcpD, in);
    D.rmq_lcp_D = sdsl::rmq_succinct_sct<>(&D.lcpD);
    D.isaD_flag = D.lcpD_flag = D.rmq_lcp_D_flag = true;
    PF.M.resize((size_t)m_count);
    {
        // Chunked: a single 3*m_count u32 temp is ~3.8 GB at yeast.
        const size_t CHUNK = 1u << 20;
        std::vector<uint32_t> buf(3 * CHUNK);
        uint64_t done = 0;
        while (done < m_count) {
            size_t n = (size_t)std::min<uint64_t>(CHUNK, m_count - done);
            in.read((char*)buf.data(), (std::streamsize)(4 * 3 * n));
            if (!in) { fprintf(stderr, "FATAL: pfp index truncated at M entry %llu\n",
                              (unsigned long long)done); exit(1); }
            for (size_t i = 0; i < n; ++i) {
                PF.M[done + i].len = (long_type)buf[3 * i + 0];
                PF.M[done + i].left = (long_type)buf[3 * i + 1];
                PF.M[done + i].right = (long_type)buf[3 * i + 2];
            }
            done += n;
        }
    }
    PF.n = (long_type)pf_n;
    PF.w = (long_type)W;
    PF.W_flag = true;

    // Cross-checks: the loaded index must agree with the (cheap) artifacts
    // it is meant to stand in for.  A mismatch here means mispaired inputs.
    if (PF.b_bwt.size() != (size_t)pf_n || PF.b_p.size() != (size_t)pf_n)
        { fprintf(stderr, "FATAL: index b_bwt/b_p size mismatch n=%llu\n", (unsigned long long)pf_n); exit(1); }
    if (PP.p.size() != (size_t)p_size)
        { fprintf(stderr, "FATAL: index parse p size %llu != .parse %zu (mispaired index?)\n",
                  (unsigned long long)p_size, PP.p.size()); exit(1); }
    if (D.b_d.size() != (size_t)d_size)
        { fprintf(stderr, "FATAL: index b_d size %zu != recorded |D| %llu\n",
                  D.b_d.size(), (unsigned long long)d_size); exit(1); }
    if ((uint64_t)PF.w_wt.size() != wt_size)
        { fprintf(stderr, "FATAL: index w_wt size %llu != header %llu\n",
                  (unsigned long long)PF.w_wt.size(), (unsigned long long)wt_size); exit(1); }
    fprintf(stderr, "pfp index LOADED: n=%llu |M|=%llu |D|=%llu |P|=%llu |wt|=%llu W=%llu (no build)\n",
            (unsigned long long)PF.n, (unsigned long long)PF.M.size(),
            (unsigned long long)d_size, (unsigned long long)PP.p.size(),
            (unsigned long long)wt_size, (unsigned long long)W);
}

// ---------------- structural digest (build-vs-load equality) ----------------
// The .agg is the semantic gate, but it only exercises the rows a given
// text happens to visit (plus calibration).  This digest covers EVERY value
// of every structure the queries read, so a build run and a --pfp-index run
// can be shown to hold identical state.  FNV-1a over logical values (not
// raw words) so packing/padding cannot mask or invent differences.
static uint64_t fnv(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
    return h;
}

static void pfp_digest(const pfpds::dictionary<uint8_t>& D,
                       pfpds::parse& PP,
                       pfpds::pf_parsing<uint8_t>& PF) {
    uint64_t h;
    h = 1469598103934665603ULL;
    for (size_t i = 0; i < PF.b_bwt.size(); ++i) h = fnv(h, PF.b_bwt[i]);
    fprintf(stderr, "digest b_bwt    %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ULL;
    for (size_t i = 0; i < PF.b_p.size(); ++i) h = fnv(h, PF.b_p[i]);
    fprintf(stderr, "digest b_p      %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ULL;
    for (size_t i = 0; i < PF.M.size(); ++i)
        h = fnv(fnv(fnv(h, (uint64_t)PF.M[i].len), (uint64_t)PF.M[i].left), (uint64_t)PF.M[i].right);
    fprintf(stderr, "digest M        %016llx (%llu entries)\n",
            (unsigned long long)h, (unsigned long long)PF.M.size());
    h = 1469598103934665603ULL;
    for (size_t i = 0; i < PP.saP.size(); ++i) h = fnv(h, (uint64_t)PP.saP[i]);
    fprintf(stderr, "digest saP      %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ULL;
    for (size_t i = 0; i < PP.isaP.size(); ++i) h = fnv(h, (uint64_t)PP.isaP[i]);
    fprintf(stderr, "digest isaP     %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ULL;
    for (size_t i = 0; i < PP.lcpP.size(); ++i) h = fnv(h, (uint64_t)PP.lcpP[i]);
    fprintf(stderr, "digest lcpP     %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ULL;
    for (size_t i = 0; i < D.isaD.size(); ++i) h = fnv(h, (uint64_t)D.isaD[i]);
    fprintf(stderr, "digest isaD     %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ULL;
    for (size_t i = 0; i < D.lcpD.size(); ++i) h = fnv(h, (uint64_t)D.lcpD[i]);
    fprintf(stderr, "digest lcpD     %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ULL;
    for (size_t i = 0; i < D.b_d.size(); ++i) h = fnv(h, D.b_d[i]);
    fprintf(stderr, "digest b_d      %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ULL;
    for (long_type i = 0; i < PF.w_wt.size(); ++i) h = fnv(h, (uint64_t)PF.w_wt[i]);
    fprintf(stderr, "digest w_wt     %016llx (size %llu)\n",
            (unsigned long long)h, (unsigned long long)PF.w_wt.size());
}

#include "slim_lce.hpp"

int main(int argc, char** argv) {
    G_T0 = tnow();
    std::string ri4Path, parsePrefix, lcpIndexPath, pfpIndexPath, outPath, flatPath, anchorsPath, headSaPath;
    int nthreads = std::thread::hardware_concurrency();
    uint64_t calibRows = 256;
    bool resolveRi4 = false, slim = false, dictStream = false, injectFault = false, profileOnly = false, resolveCache = false;
    uint64_t tau1=0, tau2=0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--ri4") && i + 1 < argc) ri4Path = argv[++i];
        else if (!strcmp(argv[i], "--parse") && i + 1 < argc) parsePrefix = argv[++i];
        else if (!strcmp(argv[i], "--lcp-index") && i + 1 < argc) lcpIndexPath = argv[++i];
        else if (!strcmp(argv[i], "--pfp-index") && i + 1 < argc) pfpIndexPath = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outPath = argv[++i];
        else if (!strcmp(argv[i], "--flat") && i + 1 < argc) flatPath = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--calib-rows") && i + 1 < argc) calibRows = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--resolve-ri4")) resolveRi4 = true;
        else if (!strcmp(argv[i], "--head-sa") && i+1<argc) headSaPath=argv[++i];
        else if (!strcmp(argv[i], "--resolve-cache")) resolveCache = true;
        else if (!strcmp(argv[i], "--slim")) slim = true;
        else if (!strcmp(argv[i], "--slim-profile-build")) profileOnly = true;
        else if (!strcmp(argv[i], "--dict-stream")) dictStream = true;
        else if (!strcmp(argv[i], "--inject-fingerprint-error")) injectFault = true;
        else if (!strcmp(argv[i], "--tau1") && i+1<argc) tau1=strtoull(argv[++i],nullptr,10);
        else if (!strcmp(argv[i], "--tau2") && i+1<argc) tau2=strtoull(argv[++i],nullptr,10);
        else if (!strcmp(argv[i], "--anchors") && i + 1 < argc) anchorsPath = argv[++i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 1; }
    }
    if ((profileOnly || dictStream || injectFault || tau1 || tau2 || resolveCache || !headSaPath.empty()) && !slim)
        slim_fail("slim options require --slim");
    if (ri4Path.empty() || parsePrefix.empty() || outPath.empty()) {
        fprintf(stderr, "usage: chi_rspace_dump [--slim --resolve-ri4 [--dict-stream] [--head-sa FILE] [--resolve-cache] [--tau1 N] [--tau2 N] [--slim-profile-build] [--inject-fingerprint-error (TEST ONLY)]] --ri4 F.ri4 --parse PFP_PREFIX -o OUT.agg [-t N] [--flat F] [--calib-rows N] [--pfp-index INDEX (load instead of building; bit6/pfp_index_build.cpp)] [--lcp-index F.lcp_index.lcp_index (LEGACY cross-check only)] [--resolve-ri4 (positions from pfp_ds_vendor/pfp/pfp.hpp defer_build_t; NO M/b_bwt/w_wt; SA via the .ri4 sample array + LF walk)] [--anchors F (string-start anchors for walks that reach an interior 0x0A row)]\n");
        return 1;
    }

    Ri4 ri4; ri4.load(ri4Path);
    fprintf(stderr, "ri4: n=%llu k=%llu R=%llu\n",
            (unsigned long long)ri4.n, (unsigned long long)ri4.k, (unsigned long long)ri4.R);

    if (slim) {
        if (!resolveRi4 || !pfpIndexPath.empty() || !lcpIndexPath.empty())
            slim_fail("--slim requires --resolve-ri4 and excludes legacy indexes");
        double begin=G_T0;slim_phase("ri4-load",begin);
        return slim_dump(ri4,parsePrefix,outPath,anchorsPath,nthreads,tau1,tau2,dictStream,injectFault,profileOnly,flatPath,calibRows,resolveCache,headSaPath);
    }

    // ---- LEGACY lcp_index: OPTIONAL.  When absent, topLCP is computed
    // from PFP artifacts alone (see the worker below).  When present it
    // is loaded only for the cross-checks (CHECK_TOP / G0_ALL). ----
    const bool haveIdx = !lcpIndexPath.empty();
    ChiIndex idx; PhiLookup P;
    if (haveIdx) {
        std::ifstream in(lcpIndexPath, std::ios::binary);
        if (!in.is_open()) { fprintf(stderr, "cannot open %s\n", lcpIndexPath.c_str()); return 1; }
        idx.load(in);
        if (idx.totalLen != ri4.n) {
            fprintf(stderr, "FATAL: lcp_index totalLen %llu != ri4 n %llu\n",
                    (unsigned long long)idx.totalLen, (unsigned long long)ri4.n);
            return 1;
        }
        build_phi_lookup(idx, P);
        fprintf(stderr, "lcp_index (LEGACY cross-check): pieces=%llu totalLen=%llu\n",
                (unsigned long long)(P.nInt), (unsigned long long)idx.totalLen);
    } else {
        fprintf(stderr, "lcp_index: NONE (PFP-artifacts-only topLCP mode)\n");
    }

    // ---- SAMPLE_RUNS: position-decode gate vs a baseline .agg (no PFP build) ----
    // Env-gated probe: resolve a spread sample of runs from the .ri4 sample
    // array + LF walk and compare saFirst/saLast against the baseline .agg's
    // entries for those runs.  Validates the .ri4-sample decode at scale
    // WITHOUT constructing the dictionary/parse (so it runs in minutes).
    if (resolveRi4 && getenv("SAMPLE_RUNS")) {
        LfIndex sl; sl.build(ri4);
        Anchors sa_;
        SampleResolver sr; sr.ri = &ri4; sr.lf = &sl;
        sr.anc = anchorsPath.empty() ? nullptr : &sa_;
        if (!anchorsPath.empty()) sa_.load(anchorsPath);
        uint64_t N = strtoull(getenv("SAMPLE_RUNS"), nullptr, 10);
        const char* basep = getenv("SAMPLE_BASELINE");
        std::ifstream bf;
        uint64_t bR = 0;
        uint64_t offFirst = 0, offLast = 0;
        if (basep) {
            bf.open(basep, std::ios::binary);
            if (!bf) { fprintf(stderr, "FATAL: cannot open baseline %s\n", basep); return 1; }
            uint32_t m; bf.read((char*)&m, 4); bf.read((char*)&bR, 8);
            if (m != 0x31415243) { fprintf(stderr, "FATAL: bad baseline magic\n"); return 1; }
            if (bR != ri4.R) { fprintf(stderr, "FATAL: baseline R %llu != .ri4 R %llu (mispaired)\n",
                                       (unsigned long long)bR, (unsigned long long)ri4.R); return 1; }
            offFirst = 12 + 8 * bR;   // topLCP[], then saFirst[], then saLast[]
            offLast  = 12 + 16 * bR;
        }
        auto base_at = [&](uint64_t off, uint64_t i, uint64_t& v) -> bool {
            bf.seekg((std::streamoff)(off + 8 * i));
            bf.read((char*)&v, 8);
            return (bool)bf;
        };
        uint64_t step = std::max<uint64_t>(1, ri4.R / N);
        uint64_t checked = 0, badF = 0, badL = 0, badTop = 0, failed = 0;
        for (uint64_t i = 0; i < ri4.R; i += step) {
            uint64_t a = ri4.starts[i], b = a + ri4.l[i] - 1;
            uint64_t pf = sr.sa_at(a);
            uint64_t pl = (b == a) ? pf : sr.sa_at(b);
            if (pf == INF || pl == INF) { failed++; continue; }
            if (basep) {
                uint64_t bF = 0, bL = 0;
                if (base_at(offFirst, i, bF) && pf != bF) { if (badF < 5) fprintf(stderr, "SAMPLEMISS first run=%llu decoded=%llu baseline=%llu\n",
                        (unsigned long long)i, (unsigned long long)pf, (unsigned long long)bF); badF++; }
                if (base_at(offLast, i, bL) && pl != bL) { if (badL < 5) fprintf(stderr, "SAMPLEMISS last  run=%llu decoded=%llu baseline=%llu\n",
                        (unsigned long long)i, (unsigned long long)pl, (unsigned long long)bL); badL++; }
            }
            checked++;
        }
        fprintf(stderr, "SAMPLE_RUNS: checked=%llu step=%llu badFirst=%llu badLast=%llu walk0aFailures=%llu "
                "walks=%llu sumSteps=%llu maxSteps=%llu\n",
                (unsigned long long)checked, (unsigned long long)step,
                (unsigned long long)badF, (unsigned long long)badL, (unsigned long long)failed,
                (unsigned long long)sr.nWalks.load(), (unsigned long long)sr.sumSteps.load(),
                (unsigned long long)sr.maxSteps.load());
        return (badF || badL || failed) ? 3 : 0;
    }

    // ---- PFP machinery ----
    // Two paths, same structures:
    //   BUILD (default): construct D/PP/PF exactly as before (suffix sorts,
    //     O(n)-bit b_p/b_bwt, |M| ~ 0.1n) — all of it a function of the
    //     parse alone, none of it dependent on the .ri4.
    //   LOAD (--pfp-index): construct D (b_d + rank/select only, no suffix
    //     sort) and PP (.parse read only), then restore every queried
    //     member from the persisted index.  The .agg must be byte-identical.
    const long_type W = 10;
    std::less<uint8_t> u8comp;
    const bool loadIdx = !pfpIndexPath.empty();
    // STANDALONE-LCE mode (--resolve-ri4): build ONLY the dependencies
    // pfp_lce_support actually reads (RESEARCH.md correction #3).
    //   dictionary: b_d/rank/select (always), saD (intermediate), isaD,
    //               lcpD, rmq_lcp_D.  NO daD, NO colex_id/colex_daD.
    //   parse:      saP/isaP/lcpP/rmq_lcp_P (all four are queried).
    //   pf_parsing: defer_build_t + compute_n() + compute_b_p() + W_flag.
    //               NO b_bwt, NO M, NO w_wt  (r-pfbwt TEXT-BWT machinery;
    //               never queried by lce_support).
    //   positions:  the .ri4 sample array + LF walk (SampleResolver).
    if (resolveRi4 && loadIdx) { fprintf(stderr, "FATAL: --resolve-ri4 and --pfp-index are exclusive\n"); return 1; }
    if (resolveRi4) fprintf(stderr, "pfp mode: STANDALONE-LCE (--resolve-ri4; no M/b_bwt/w_wt)\n");
    else if (loadIdx) fprintf(stderr, "pfp mode: LOAD (--pfp-index %s)\n", pfpIndexPath.c_str());
    else         fprintf(stderr, "pfp mode: BUILD (in-process)\n");
    // LOAD: defer-construct D (no .dict read, no b_d scan, no d materialized)
    // and PP (.parse read only, alphabet_size unused because no build runs),
    // then restore everything from the persisted index.  n_phrases() reads
    // d.size() and is therefore BUILD-path-only on purpose (see the ctor
    // comment in bit6/pfp_ds_vendor/pfp/dictionary.hpp).
    std::unique_ptr<pfpds::dictionary<uint8_t>> Dp;
    if (resolveRi4)
        Dp = std::make_unique<pfpds::dictionary<uint8_t>>(
                 parsePrefix, W, u8comp, true /*saD*/, true /*isaD*/, false /*daD*/,
                 true /*lcpD*/, true /*rmq_lcp_D*/, false /*colex_id*/, false /*colex_daD*/);
    else if (!loadIdx)
        Dp = std::make_unique<pfpds::dictionary<uint8_t>>(
                 parsePrefix, W, u8comp, true, true, true, true, true, true, true);
    else
        Dp = std::make_unique<pfpds::dictionary<uint8_t>>(
                 W, u8comp, pfpds::dictionary<uint8_t>::defer_build_t{});
    pfpds::dictionary<uint8_t>& D = *Dp;
    phase(loadIdx ? "dict-defer" : "dict-BUILD");
    // saD is only the build intermediate for isaD/lcpD (lce_support never
    // reads it); freeing it drops a Theta(|D|) * 5-byte array from RSS.
    uint64_t saD_built_bytes = 0;
    if (resolveRi4) {
        saD_built_bytes = (uint64_t)D.saD.size() * D.saD.width() / 8;
        D.saD = sdsl::int_vector<0>();
        fprintf(stderr, "freed dict saD build-intermediate: %llu B\n",
                (unsigned long long)saD_built_bytes);
    }
    if (!loadIdx)
        fprintf(stderr, "dict: phrases=%llu size=%llu\n",
                (unsigned long long)D.n_phrases(), (unsigned long long)D.d.size());
    const long_type alpha = loadIdx ? (long_type)256 : (D.n_phrases() + 1);
    // BUILD and STANDALONE both build the parse suffix structures (lce needs
    // all four); LOAD restores them from the persisted index.
    pfpds::parse PP(parsePrefix, alpha, !loadIdx, !loadIdx, !loadIdx, !loadIdx);
    phase("parse-read/build");
    std::unique_ptr<pfpds::pf_parsing<uint8_t>> PFp;
    if (resolveRi4) {
        PFp = std::make_unique<pfpds::pf_parsing<uint8_t>>(
                  D, PP, pfpds::pf_parsing<uint8_t>::defer_build_t{});
        PFp->compute_n();     // sets n (freq is unused on this path)
        PFp->compute_b_p();   // b_p + rank/select: the position<->phrase maps
        PFp->W_flag = true;   // lce_support's assert; no W structure is read
    } else if (!loadIdx)
        PFp = std::make_unique<pfpds::pf_parsing<uint8_t>>(D, PP, true, false);
    else
        PFp = std::make_unique<pfpds::pf_parsing<uint8_t>>(
                  D, PP, pfpds::pf_parsing<uint8_t>::defer_build_t{});
    pfpds::pf_parsing<uint8_t>& PF = *PFp;
    if (loadIdx) load_pfp_index(pfpIndexPath, D, PP, PF);
    phase(loadIdx ? "index-LOAD" : (resolveRi4 ? "pf-minimal-BUILD" : "pf_parsing-BUILD"));
    if ((long_type)PF.n != ri4.n + W) {
        fprintf(stderr, "WARNING: pfp n %llu != ri4 n+W %llu (separators/endmarker "
                "convention differs; the flat calibration below is authoritative)\n",
                (unsigned long long)PF.n, (unsigned long long)(ri4.n + W));
    }
    fprintf(stderr, "pfp: n=%llu parse=%llu |M|=%llu\n",
            (unsigned long long)PF.n, (unsigned long long)PP.p.size() - 1,
            (unsigned long long)PF.M.size());
    pfpds::pfp_lce_support<uint8_t> LCE_sup(PF);
    if (resolveRi4) {
        if (!ri4.haveSa) { fprintf(stderr, "FATAL: --resolve-ri4 needs the .ri4 SA sample array (absent)\n"); return 1; }
        if (ri4.sampleAllInf()) { fprintf(stderr, "FATAL: --resolve-ri4: .ri4 samples are all INF (ri4_from_rle output cannot resolve)\n"); return 1; }
    }
    // pfp_sa_support is the M/b_bwt/w_wt consumer; construct it ONLY off the
    // standalone path.
    std::unique_ptr<pfpds::pfp_sa_support<uint8_t>> SA_supP;
    if (!resolveRi4) SA_supP = std::make_unique<pfpds::pfp_sa_support<uint8_t>>(PF);
    if (getenv("PFP_DIGEST") && !resolveRi4) { pfp_digest(D, PP, PF); phase("digest"); }

    const uint64_t n = ri4.n;
    // ---- position resolution -------------------------------------------------
    // Non-standalone: machinery row m <-> .ri4 row m - ROW_OFF via
    //   pfp_sa_support (M/b_bwt/w_wt), ROW_OFF calibrated below.
    // Standalone (--resolve-ri4): .ri4 rows directly, via the sample array +
    //   LF walk (SampleResolver) — M/b_bwt/w_wt are never constructed.
    LfIndex lfIdx;
    Anchors anchors;
    SampleResolver resolver;
    if (resolveRi4) {
        lfIdx.build(ri4);
        if (!anchorsPath.empty()) anchors.load(anchorsPath);
        resolver.ri = &ri4;
        resolver.lf = &lfIdx;
        resolver.anc = anchorsPath.empty() ? nullptr : &anchors;
        phase("lf-index");
    }
    // row index space: standalone resolves .ri4 rows as-is (ROW_OFF 0).
    std::function<uint64_t(long_type)> SAfun;
    if (resolveRi4)
        SAfun = [&](long_type row) -> uint64_t { return resolver.sa_at((uint64_t)row); };
    else
        SAfun = [&](long_type row) -> uint64_t { return (uint64_t)(*SA_supP)(row); };

    if (getenv("DUMP_ROWS")) {
        uint64_t lim = strtoull(getenv("DUMP_ROWS"), nullptr, 10);
        if (lim > ri4.n) lim = ri4.n;
        FILE* rf = fopen((outPath + ".rows").c_str(), "wb");
        for (uint64_t j = 0; j < lim; ++j) {
            uint64_t p = resolveRi4 ? (uint64_t)SAfun((long_type)j)
                                    : (uint64_t)SAfun((long_type)j + W);
            uint64_t w = p; fwrite(&w, 8, 1, rf);
        }
        fclose(rf);
        fprintf(stderr, "dumped %llu rows\n", (unsigned long long)lim);
    }
    // ---- row-space calibration: full-probe offset selection ----
    // For each candidate offset, score over the full probe set (run-starts +
    // spread rows); require a UNIQUE zero-bad offset. The battery pins W;
    // standalone starts at .ri4 row 0 and scans offsets -2..+2 (an
    // independent decode-vs-flat validation).
    long_type ROW_OFF = resolveRi4 ? 0 : W;
    bool rowOffPinned = false;
    if (!flatPath.empty()) {
        std::ifstream calFlat(flatPath, std::ios::binary);
        std::vector<uint64_t> calRows;
        for (uint64_t t = 0; t < calibRows; ++t)
            calRows.push_back((uint64_t)(((double)t + 0.5) / calibRows * ri4.R));
        for (uint64_t t = 0; t < calibRows && t < ri4.n; ++t)
            calRows.push_back(t * ri4.n / calibRows);
        const long_type offLo = resolveRi4 ? -2 : W - 2;
        const long_type offHi = resolveRi4 ? 2 : (long_type)W + 2;
        long_type bestOff = ROW_OFF; uint64_t bestBad = ~0ULL; uint64_t nZero = 0;
        for (long_type off = offLo; off <= offHi; ++off) {
            uint64_t bad = 0, chk = 0;
            for (uint64_t jj : calRows) {
                if (jj == 0 || jj >= ri4.n) continue;
                uint64_t pos = (uint64_t)SAfun((long_type)jj + off);
                if (pos == 0 || pos > ri4.n) continue;
                calFlat.seekg((std::streamoff)(pos - 1));
                char ch; calFlat.read(&ch, 1);
                uint64_t rr = (uint64_t)(std::upper_bound(ri4.starts.begin(), ri4.starts.end(), jj)
                                         - ri4.starts.begin() - 1);
                ++chk;
                if ((uint8_t)ch != ri4.a[rr] && ri4.a[rr] != 0x0A && (uint8_t)ch != 0x0A) bad++;
            }
            fprintf(stderr, "calib off=%lld: bad=%llu/%llu\n", (long long)off,
                    (unsigned long long)bad, (unsigned long long)chk);
            if (bad == 0) { nZero++; if (nZero == 1) bestOff = off; }
            if (bad < bestBad) bestBad = bad;
        }
        if (nZero == 1) { ROW_OFF = bestOff; rowOffPinned = true; }
        fprintf(stderr, "calibration: ROW_OFF=%lld (zero-bad offsets: %llu)\n",
                (long long)ROW_OFF, (unsigned long long)nZero);
    }
    auto resolve_row = [&](uint64_t j) -> uint64_t {
        return resolveRi4 ? (uint64_t)SAfun((long_type)j)
                          : (uint64_t)SAfun((long_type)j + ROW_OFF);
    };

    // ---- flat spot-check (optional): BWT char at row j must be flat[pos-1] ----
    if (!flatPath.empty()) {
        std::ifstream flat(flatPath, std::ios::binary);
        if (!flat) { fprintf(stderr, "cannot open flat %s\n", flatPath.c_str()); return 1; }
        // LF support from the .ri4's own runs: Cless[c] = # BWT rows with char < c;
        // per-char run lists + prefix lens for rank within char.
        std::vector<uint64_t> Cless(256, 0);
        {
            std::vector<uint64_t> Ctot(256, 0);
            for (uint64_t i = 0; i < ri4.R; ++i) Ctot[ri4.a[i]] += ri4.l[i];
            uint64_t acc = 0;
            for (int c = 0; c < 256; ++c) { Cless[c] = acc; acc += Ctot[c]; }
        }
        std::vector<std::vector<uint32_t>> charRuns(256);
        std::vector<std::vector<uint64_t>> charSum(256);
        for (uint64_t i = 0; i < ri4.R; ++i) charRuns[ri4.a[i]].push_back((uint32_t)i);
        for (int c = 0; c < 256; ++c) {
            if (charRuns[c].empty()) continue;
            charSum[c].resize(charRuns[c].size() + 1, 0);
            for (size_t t = 0; t < charRuns[c].size(); ++t)
                charSum[c][t + 1] = charSum[c][t] + ri4.l[charRuns[c][t]];
        }
        auto run_of = [&](uint64_t row) -> uint64_t {
            return (uint64_t)(std::upper_bound(ri4.starts.begin(), ri4.starts.end(), row)
                              - ri4.starts.begin() - 1);
        };
        auto lf_row = [&](uint64_t row) -> uint64_t {
            uint64_t r = run_of(row);
            uint8_t ch = ri4.a[r];
            uint64_t o = row - ri4.starts[r];
            auto& vr = charRuns[ch];
            uint64_t k = (uint64_t)(std::lower_bound(vr.begin(), vr.end(), (uint32_t)r) - vr.begin());
            return Cless[ch] + charSum[ch][k] + o;
        };
        auto flat_at = [&](uint64_t p) -> int {
            if (p == 0 || p > (uint64_t)PF.n) return -1;
            flat.seekg((std::streamoff)(p - 1));
            char c; flat.read(&c, 1);
            return (uint8_t)c;
        };
        long_type moff0 = ROW_OFF;

        std::atomic<uint64_t> ok{0}, bad{0}, skipped{0};
        std::atomic<uint64_t> badRunStart{0}, badInterior{0};
        std::atomic<uint64_t> lfOk{0}, lfBad{0};
        std::vector<uint64_t> rows;
        std::vector<uint8_t> isRunStart;
        for (uint64_t t = 0; t < calibRows; ++t) {
            rows.push_back((uint64_t)(((double)t + 0.5) / calibRows * ri4.R));
            isRunStart.push_back(1);
        }
        for (uint64_t t = 0; t < calibRows && t < ri4.n; ++t) {
            rows.push_back(t * ri4.n / calibRows); isRunStart.push_back(0);
        }
        uint64_t dbgCap = getenv("DEBUG_BAD") ? strtoull(getenv("DEBUG_BAD"), nullptr, 10) : 0;
        uint64_t dbgSeen = 0;
        for (size_t idx = 0; idx < rows.size(); ++idx) {
            uint64_t j = rows[idx];
            if (j >= ri4.n) continue;
            uint64_t pos = (uint64_t)SAfun((long_type)j + moff0);
            if (pos == 0) { skipped++; continue; }
            int c = flat_at(pos);
            if (c < 0) { skipped++; continue; }
            uint64_t r = run_of(j);
            uint8_t bwt = ri4.a[r];
            if ((uint8_t)c == bwt) { ok++; continue; }
            if (bwt == 0x0A || (uint8_t)c == 0x0A) { skipped++; continue; }
            bad++;
            (isRunStart[idx] ? badRunStart : badInterior)++;
            // --- discriminator 3: LF-consistency (forward-only round-trip) ---
            // LF(j) from the .ri4 runs must resolve to pos-1.
            bool lfMatch = false;
            uint64_t lfPos = ~0ULL;
            {
                uint64_t rlf = lf_row(j);
                if (rlf < ri4.n) {
                    lfPos = (uint64_t)SAfun((long_type)rlf + moff0);
                    lfMatch = (lfPos + 1 == pos);
                }
            }
            (lfMatch ? lfOk : lfBad)++;
            if (dbgCap && dbgSeen++ < dbgCap) {
                fprintf(stderr, "BAD row=%llu pos=%llu bwt=%02x flat[pos-1]=%02x "
                        "runstart=%u distEnd=%llu lfMatch=%d lfPos=%llu\n",
                        (unsigned long long)j, (unsigned long long)pos, bwt, (uint8_t)c,
                        (unsigned)isRunStart[idx],
                        (unsigned long long)(PF.n - pos), (int)lfMatch,
                        (unsigned long long)lfPos);
                // --- discriminator 2: row direction (rows j-2..j+2) ---
                for (int d = -2; d <= 2; ++d) {
                    if (d == 0 || (d < 0 && j < 2) || j + d >= ri4.n) continue;
                    uint64_t jd = j + d;
                    uint64_t rd = run_of(jd);
                    uint8_t bwtd = ri4.a[rd];
                    uint64_t posd = (uint64_t)SAfun((long_type)jd + moff0);
                    int cd = posd ? flat_at(posd) : -1;
                    fprintf(stderr, "  nb d=%+d row=%llu bwt_d=%02x pos_d=%llu "
                            "flat[pos_d-1]=%02x nbGood=%d nbCarriesOurBwt=%d "
                            "ourBwtAtNbFlat=%d\n",
                            d, (unsigned long long)jd, bwtd, (unsigned long long)posd,
                            (uint8_t)(cd < 0 ? 0 : cd),
                            (int)(cd >= 0 && bwtd == (uint8_t)cd),
                            (int)(bwtd == (uint8_t)c),
                            (int)(bwt == (uint8_t)cd));
                }
                // --- offset scan: which machinery offsets would satisfy row j ---
                for (long_type off = W - 2; off <= (long_type)W + 2; ++off) {
                    uint64_t p2 = (uint64_t)SAfun((long_type)j + off);
                    int c2 = p2 ? flat_at(p2) : -1;
                    fprintf(stderr, "  off=%lld -> pos=%llu flat=%02x match=%d\n",
                            (long long)off, (unsigned long long)p2,
                            (uint8_t)(c2 < 0 ? 0 : c2), (int)(c2 >= 0 && bwt == (uint8_t)c2));
                }
                // --- structure: distance back/forward to a 0x0A in the flat ---
                {
                    uint64_t dBack = ~0ULL, dFwd = ~0ULL;
                    flat.seekg(0, std::ios::end);
                    uint64_t fsize = (uint64_t)flat.tellg();
                    uint64_t lo = pos > 8192 ? pos - 8192 : 0;
                    flat.seekg((std::streamoff)lo);
                    std::vector<char> buf(pos - lo);
                    flat.read(buf.data(), (std::streamoff)buf.size());
                    for (uint64_t t = 0; t < buf.size(); ++t)
                        if ((uint8_t)buf[t] == 0x0A) dBack = buf.size() - 1 - t + 1; // dist from pos-1
                    uint64_t hi = pos + 8192 < fsize ? pos + 8192 : fsize;
                    if (hi > pos) {
                        std::vector<char> buf2(hi - pos);
                        flat.seekg((std::streamoff)pos);
                        flat.read(buf2.data(), (std::streamoff)buf2.size());
                        for (uint64_t t = 0; t < buf2.size(); ++t)
                            if ((uint8_t)buf2[t] == 0x0A) { dFwd = t + 1; break; }
                    }
                    fprintf(stderr, "  struct: distBackTo0A=%llu distFwdTo0A=%llu\n",
                            (unsigned long long)dBack, (unsigned long long)dFwd);
                }
            }
        }
        fprintf(stderr, "flat spot-check: ok=%llu bad=%llu sentinel-skipped=%llu "
                "(badRunStart=%llu badInterior=%llu) lfOk=%llu lfBad=%llu\n",
                (unsigned long long)ok.load(), (unsigned long long)bad.load(),
                (unsigned long long)skipped.load(),
                (unsigned long long)badRunStart.load(),
                (unsigned long long)badInterior.load(),
                (unsigned long long)lfOk.load(), (unsigned long long)lfBad.load());
        if (bad.load() > 0 && !getenv("ALLOW_BAD")) {
            fprintf(stderr, "FATAL: row-space calibration FAILED\n"); return 1;
        }
    }

    // ---- G0 (all-rows / all-positions) primitive validation ----
    // PLCP(SA[m]) must equal the clamped LCE of SA[m-1] and SA[m] at
    // EVERY row m (rows cover every position exactly once).
    if (getenv("G0_ALL") && !haveIdx) {
        fprintf(stderr, "G0_ALL: skipped (--resolve-ri4 has no lcp_index to cross-check against; the flat spot-check is the decode validation)\n");
    }
    if (getenv("G0_ALL") && haveIdx) {
        std::atomic<uint64_t> mism{0}, checks{0}, nextR{1};
        auto wk = [&]() {
            for (;;) {
                uint64_t m = nextR.fetch_add(1);
                if (m >= ri4.n) return;
                uint64_t pos = (uint64_t)SAfun((long_type)m + ROW_OFF);
                uint64_t prev = (uint64_t)SAfun((long_type)(m - 1) + ROW_OFF);
                uint64_t topIdx = lcp_at_pos(idx, P, pos);
                long_type lce = LCE_sup((long_type)prev, (long_type)pos);
                uint64_t topLce = std::min((uint64_t)lce, ri4.n - std::max(prev, pos));
                checks++;
                if (topIdx != topLce) {
                    uint64_t c = ++mism;
                    if (c <= 20)
                        fprintf(stderr, "G0MIS row=%llu pos=%llu prev=%llu idx=%llu lce=%llu\n",
                                (unsigned long long)m, (unsigned long long)pos,
                                (unsigned long long)prev, (unsigned long long)topIdx,
                                (unsigned long long)topLce);
                }
            }
        };
        std::vector<std::thread> th;
        int nt = std::max(1, std::min(nthreads, 64));
        for (int t = 0; t < nt; ++t) th.emplace_back(wk);
        for (auto& t : th) t.join();
        fprintf(stderr, "G0_ALL: checks=%llu mismatches=%llu\n",
                (unsigned long long)checks.load(), (unsigned long long)mism.load());
        return mism.load() ? 2 : 0;
    }

    // ---- per-run aggregates (parallel) ----
    phase("calibration+spotchk");
    std::vector<uint64_t> topLCP(ri4.R, INF), saFirst(ri4.R, INF),
        saLast(ri4.R, INF), interiorMin(ri4.R, INF);
    std::atomic<uint64_t> lceChecks{0}, lceMismatch{0};
    std::atomic<uint64_t> resolveFail{0};
    std::mutex mtx;
    uint64_t topChecks = 0, topMismatch = 0;
    {
        std::atomic<uint64_t> next{0};
        auto worker = [&](void) {
            for (;;) {
                uint64_t i = next.fetch_add(1);
                if (i >= ri4.R) return;
                uint64_t a = ri4.starts[i];
                uint64_t b = a + ri4.l[i] - 1;
                uint64_t pf = resolve_row(a);
                uint64_t pl = (b == a) ? pf : resolve_row(b);
                if (pf == INF || pl == INF) { resolveFail++; continue; }   // fail loudly below
                saFirst[i] = pf;
                saLast[i] = pl;
                // topLCP = PLCP at the run-head row = LCP(SA[a-1], SA[a]).
                // PFP-artifacts-only: resolve the previous row and call the
                // vendored pfp LCE support (polylog/call).
                if (a == 0) {
                    topLCP[i] = 0;
                } else {
                    uint64_t pa1 = resolve_row(a - 1);
                    if (pa1 == INF) { resolveFail++; continue; }
                    long_type lce1 = LCE_sup((long_type)pa1, (long_type)pf);
                    topLCP[i] = std::min((uint64_t)lce1, n - std::max(pa1, pf));
                }
                if (haveIdx && (getenv("CHECK_TOP") || (i & 1023) == 0)) {
                    uint64_t top_idx = (a == 0) ? 0 : lcp_at_pos(idx, P, pf);
                    std::lock_guard<std::mutex> lk(mtx);
                    topChecks++;
                    if (top_idx != topLCP[i]) {
                        topMismatch++;
                        if (topMismatch <= 20)
                            fprintf(stderr, "TOPMIS run=%llu a=%llu pf=%llu idx=%llu lce=%llu\n",
                                    (unsigned long long)i, (unsigned long long)a,
                                    (unsigned long long)pf, (unsigned long long)top_idx,
                                    (unsigned long long)topLCP[i]);
                    }
                }
                if (ri4.l[i] > 1) {
                    // classic identity: min over rows (a,b] = LCP(S_a, S_b)
                    long_type lce = LCE_sup((long_type)pf, (long_type)pl);
                    uint64_t clamp = n - std::max(pf, pl);
                    uint64_t v = std::min((uint64_t)lce, clamp);
                    interiorMin[i] = v;
                    // cross-check topLCP via LCE on every 64th run:
                    if (a >= 1 && (i & 63) == 0 && a > 0) {
                        uint64_t pa1 = resolve_row(a - 1);
                        long_type lce2 = LCE_sup((long_type)pa1, (long_type)pf);
                        uint64_t v2 = std::min((uint64_t)lce2, n - std::max(pa1, pf));
                        lceChecks++;
                        if (v2 != topLCP[i]) lceMismatch++;
                    }
                }
            }
        };
        std::vector<std::thread> th;
        int nt = std::max(1, std::min(nthreads, 64));
        for (int t = 0; t < nt; ++t) th.emplace_back(worker);
        for (auto& t : th) t.join();
    }
    phase("queries (r-scaled)");
    if (resolveRi4) {
        fprintf(stderr, "resolve(ri4-samples): walks=%llu sumSteps=%llu maxSteps=%llu "
                "anchorHits=%llu zeroA-interior-failures=%llu resolveFail=%llu\n",
                (unsigned long long)resolver.nWalks.load(),
                (unsigned long long)resolver.sumSteps.load(),
                (unsigned long long)resolver.maxSteps.load(),
                (unsigned long long)resolver.viaAnchor.load(),
                (unsigned long long)resolver.hit0a.load(),
                (unsigned long long)resolveFail.load());
    }
    uint64_t unfilled = 0;
    for (uint64_t i = 0; i < ri4.R; ++i)
        if (topLCP[i] == INF || saFirst[i] == INF || saLast[i] == INF) unfilled++;
    if (unfilled) {
        fprintf(stderr, "FATAL: %llu runs unfilled%s\n", (unsigned long long)unfilled,
                resolveRi4 ? " (LF walks reached an interior 0x0A row with no anchor)" : "");
        return 1;
    }
    if (haveIdx)
        fprintf(stderr, "TOPID vs lcp_index: checks=%llu mismatches=%llu\n",
                (unsigned long long)topChecks, (unsigned long long)topMismatch);
    fprintf(stderr, "aggregates: %llu runs; LCE cross-checks=%llu mismatches=%llu\n",
            (unsigned long long)ri4.R, (unsigned long long)lceChecks.load(),
            (unsigned long long)lceMismatch.load());

    // ---- write sidecar ----
    {
        std::ofstream o(outPath, std::ios::binary);
        uint32_t magic = 0x31415243;  // "CRA1"
        o.write((const char*)&magic, 4);
        o.write((const char*)&ri4.R, 8);
        o.write((const char*)topLCP.data(), 8 * ri4.R);
        o.write((const char*)saFirst.data(), 8 * ri4.R);
        o.write((const char*)saLast.data(), 8 * ri4.R);
        o.write((const char*)interiorMin.data(), 8 * ri4.R);
    }
    phase("write-agg");
    if (resolveRi4) {
        // G2 cost table: the ACTUAL dependency set (bytes), nothing else.
        auto bv = [](size_t bits) { return (double)bits / 8.0; };
        double d_bytes = (double)D.d.size();
        double saD = (double)D.saD.size() * D.saD.width() / 8.0;
        double isaD = (double)D.isaD.size() * D.isaD.width() / 8.0;
        double lcpD = (double)D.lcpD.size() * D.lcpD.width() / 8.0;
        double bd = bv(D.b_d.size());
        double pp = (double)PP.p.size() * 4.0;
        double saP = (double)PP.saP.size() * PP.saP.width() / 8.0;
        double isaP = (double)PP.isaP.size() * PP.isaP.width() / 8.0;
        double lcpP = (double)PP.lcpP.size() * PP.lcpP.width() / 8.0;
        double bp = bv(PF.b_p.size());
        double ri4runs = (double)ri4.R * 5.0;
        double ri4sa = (double)ri4.saWords.size() * 8.0;
        double lf = (double)(ri4.starts.size() * 8) + (double)(ri4.saWords.size() * 8);
        fprintf(stderr, "COST-TABLE (bytes):\n");
        fprintf(stderr, "  dict:   d=%.0f isaD=%.0f lcpD=%.0f b_d=%.0f (saD built=%.0f then FREED)\n",
                d_bytes, isaD, lcpD, bd, (double)saD_built_bytes);
        fprintf(stderr, "  parse:  p=%.0f saP=%.0f isaP=%.0f lcpP=%.0f\n",
                pp, saP, isaP, lcpP);
        fprintf(stderr, "  pf:     b_p=%.0f (n bits); NO b_bwt, NO M, NO w_wt\n", bp);
        fprintf(stderr, "  .ri4:   runs=%.0f saSamples=%.0f\n", ri4runs, ri4sa);
        fprintf(stderr, "  LfIndex(in-RAM LF tables)=%.0f  (run starts + samples)\n", lf);
        fprintf(stderr, "  TOTAL(retained structures)=%.0f bytes\n",
                d_bytes + isaD + lcpD + bd + pp + saP + isaP + lcpP + bp + ri4runs + ri4sa + lf);
        fprintf(stderr, "  parse=%llu phrases |D|=%llu bytes n=%llu R=%llu (P/n=%.4f, |D|/n=%.4f)\n",
                (unsigned long long)(PP.p.size()-1), (unsigned long long)D.d.size(),
                (unsigned long long)ri4.n, (unsigned long long)ri4.R,
                (double)(PP.p.size()-1)/(double)ri4.n, d_bytes/(double)ri4.n);
    }
    fprintf(stderr, "wrote %s (%llu runs x 4 u64)\n", outPath.c_str(), (unsigned long long)ri4.R);
    return 0;
}
