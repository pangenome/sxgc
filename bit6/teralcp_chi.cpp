// teralcp_chi.cpp — rung 4a.3: χ (smallest suffixient set) directly from a
// TeraLCP lcp_index in O(r) space, O(n) parallel time. No flat text, no O(n)
// tables; the only inputs are the index and (optionally) the rlbwt heads/len
// for cross-checks.
//
// Pipeline (all validated pieces, new wiring):
//   1. load lcp_index: totalLen, F (run chars), Psi (run lengths),
//      Phi (move-structure start table), PLCPsamples.
//   2. per-string self-terminating LF walks (rlbwt_sampler pattern, gated
//      712/712 and yeast-gated): walking LF from the bare-sentinel row of
//      string i (rows 0..k-1, string order — BCR convention validated in
//      bit6/teralcp_gate_tiny.py) visits every suffix of string i at its
//      exact BWT row; the walk terminates when it lands on a bare-sentinel
//      row, giving len_i and absolute positions by accumulation from
//      string 1 (fstart = 0).
//   3. per-row LCP[k] = PLCP[SA[k]] = PLCPsamples[phiInterval(SA[k])] -
//      offset(SA[k]) — position->(interval,offset) by binary search on the
//      Phi start positions (interval starts are sorted).
//   4. per-SPLIT-run aggregates: topLCP (LCP at first row), interiorMin (min
//      LCP over rows s+1..e; +INF if the run is a single row), saFirst,
//      saLast — atomic-min reductions across parallel walkers.
//   5. inline scan-rs state machine (lean/Sxgc.lean mirror, Bit-2 gated)
//      fed with per-run values; char-change logic merges adjacent same-char
//      split runs (the multi-endmarker '\n' runs) exactly as the per-row
//      stream would. '\n' is mapped to char 0 (sentinel, never emitted).
//      Position convention: candidates are stored as N - sa (scan-rs / C++
//      pfp_suffixient convention); with -A the emitted set is (N - sa - 1)
//      in the build_index convention.
//
// Gates: ft30 + s200 (this tool's triples vs teralcp_brute_thr / divsufsort
// brute force; χ positions vs scan-rs on brute triples), then yeast235
// (χ count must equal 85,404,240).
//
// Usage:
//   teralcp_chi <prefix.lcp_index.lcp_index> --rlbwt BASE [options]
//     --rlbwt BASE   grlBWT rlbwt (BASE.bwt.heads + BASE.bwt.len, 5-byte LE)
//                   for the run structure (the index F column is the SORTED
//                   column, not the BWT; our pipeline always has the rlbwt).
//                   The index contributes totalLen, Phi starts, PLCPsamples.
//     -o FILE    write χ positions (u64 LE) to FILE
//     -t N       threads (default: all)
//     --triples  dump per-run aggregates to stdout (debug/gating)
//     -A         emit (N - sa - 1) in the build_index convention
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
#include <limits>

#include <sdsl/int_vector.hpp>
// structures from TeraTools (public structs in moveStructure.h); the index
// file layout is pinned from TeraLCP::serialize/load: totalLen, F, Psi,
// intAtTop, Phi, PLCPsamples (in that order). We deserialize directly
// instead of using the class (its data members are private).
#include "moveStructure/moveStructure.h"

struct ChiIndex {
    uint64_t totalLen = 0;
    sdsl::int_vector<> F;          // run chars (split-run order)
    MoveStructureTable Psi;        // psi move structure; Psi.data.get<2>(i) = run length
    sdsl::int_vector<> intAtTop;
    MoveStructureStartTable Phi;   // phi move structure over positions
    sdsl::int_vector<> PLCPsamples; // PLCP at phi-interval starts
    void load(std::istream& in, bool slim = false) {
        sdsl::load(totalLen, in);
        sdsl::load(F, in);
        if (slim) skip_packed_triple(in); else sdsl::load(Psi, in);
        if (slim) skip_int_vector(in); else sdsl::load(intAtTop, in);
        sdsl::load(Phi, in);
        sdsl::load(PLCPsamples, in);
    }
    // sdsl int_vector<> on-disk layout (this vendored sdsl): u64 size IN
    // BITS, u8 width, then ceil(bits/64)*8 bytes of data. Seek past it
    // without loading. (Validated position-for-position vs a fat load.)
    static void skip_int_vector(std::istream& in) {
        uint64_t size; uint8_t width;
        in.read((char*)&size, 8);
        in.read((char*)&width, 1);
        uint64_t bytes = ((size + 63) / 64) * 8;
        in.seekg((std::streamoff)bytes, std::ios::cur);
    }
    // packedTripleVector layout: 4 raw u8 (a,b,c,width) then a bit_vector
    // = int_vector<1>: header is u64 size ONLY (fixed-width t_width=1 skips
    // the width byte — see sdsl int_vector::read_header), then
    // ceil(size/64)*8 bytes of data.
    static void skip_packed_triple(std::istream& in) {
        in.seekg(4, std::ios::cur);
        uint64_t size;
        in.read((char*)&size, 8);
        uint64_t bytes = ((size + 63) / 64) * 8;
        in.seekg((std::streamoff)bytes, std::ios::cur);
    }
};

static const uint64_t INF = std::numeric_limits<uint64_t>::max();

// portable atomic min (GCC __atomic_fetch_min is C-only on this g++)
static inline void atomic_min_u64(uint64_t* p, uint64_t v) {
    uint64_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (v < cur &&
           !__atomic_compare_exchange_n(p, &cur, v, false,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

// ---- LF/rank machinery over the RLBWT (rlbwt_sampler pattern) ----
struct Runs {
    std::vector<unsigned char> a;   // run symbols (raw bytes; '\n' = 0x0A)
    std::vector<uint32_t> l;        // run lengths
    std::vector<uint64_t> starts;   // BWT row where each run starts
    std::vector<uint64_t> C;        // F offsets per byte value
    uint64_t n = 0, R = 0;
    std::vector<uint64_t> runRank;  // rows of char c before run i
};

// runs from grlBWT rlbwt files (heads + 5-byte LE lens)
static void build_runs_files(const std::string& base, Runs& R) {
    std::ifstream hf(base + ".bwt.heads", std::ios::binary);
    std::ifstream lf(base + ".bwt.len", std::ios::binary);
    if (!hf || !lf) { fprintf(stderr, "cannot open rlbwt %s.*\n", base.c_str()); exit(1); }
    std::vector<unsigned char> heads((std::istreambuf_iterator<char>(hf)),
                                     std::istreambuf_iterator<char>());
    const uint64_t runs = heads.size();
    R.R = runs;
    R.a = heads;
    R.l.resize(runs); R.starts.resize(runs); R.runRank.resize(runs);
    uint64_t pos = 0;
    for (uint64_t i = 0; i < runs; ++i) {
        uint64_t L = 0;
        char b[5];
        if (!lf.read(b, 5)) { fprintf(stderr, "short len file\n"); exit(1); }
        for (int k = 0; k < 5; ++k) L |= (uint64_t)(unsigned char)b[k] << (8 * k);
        if (L == 0 || L > 0xFFFFFFFFULL) { fprintf(stderr, "bad run len %llu\n", (unsigned long long)L); exit(1); }
        R.l[i] = (uint32_t)L;
        R.starts[i] = pos;
        pos += L;
    }
    R.n = pos;
    R.C.assign(256, 0);
    for (uint64_t i = 0; i < runs; ++i) R.C[R.a[i]] += R.l[i];
    uint64_t tot = 0;
    for (int c = 0; c < 256; ++c) { uint64_t t = R.C[c]; R.C[c] = tot; tot += t; }
    std::vector<uint64_t> seen(256, 0);
    for (uint64_t i = 0; i < runs; ++i) {
        R.runRank[i] = seen[R.a[i]];
        seen[R.a[i]] += R.l[i];
    }
}

// LF(row) using run-level rank: walk is hot, so precompute per-run
// LF(row within run i, offset o) = C[c] + runRank[i] + o
static inline uint64_t lf_of(const Runs& R, uint64_t run, uint64_t off) {
    unsigned char c = R.a[run];
    return R.C[c] + R.runRank[run] + off;
}

// row -> run index by binary search over starts (used for walk init only)
static uint64_t run_of_row(const Runs& R, uint64_t row) {
    uint64_t lo = 0, hi = R.R - 1;
    while (lo < hi) {
        uint64_t mid = (lo + hi + 1) / 2;
        if (R.starts[mid] <= row) lo = mid; else hi = mid - 1;
    }
    return lo;
}

// ---- position -> phi interval by binary search on Phi starts ----
struct PhiLookup {
    std::vector<uint64_t> start;   // fat: full copy of get<2>(j), j in 0..nInt
    std::vector<uint64_t> accel;  // slim: every ACCEL_STRIDE-th start
    static const uint64_t ACCEL_STRIDE = 16;
    const MoveStructureStartTable* phi;
    uint64_t nInt;
    bool slim = false;
};

static void build_phi_lookup(const ChiIndex& idx, PhiLookup& P, bool slim = false) {
    P.phi = &idx.Phi;
    P.slim = slim;
    P.nInt = idx.Phi.data.size() - 1;
    if (slim) {
        const uint64_t nblocks = P.nInt / PhiLookup::ACCEL_STRIDE;
        P.accel.resize(nblocks + 1);
        for (uint64_t j = 0; j <= nblocks; ++j)
            P.accel[j] = idx.Phi.data.get<2>(j * PhiLookup::ACCEL_STRIDE);
        return;
    }
    P.start.resize(P.nInt + 1);
    for (uint64_t j = 0; j <= P.nInt; ++j)
        P.start[j] = idx.Phi.data.get<2>(j);
    // sanity: monotone
    for (uint64_t j = 0; j < P.nInt; ++j)
        if (P.start[j] > P.start[j + 1]) {
            fprintf(stderr, "FATAL: Phi starts not monotone at %llu\n",
                    (unsigned long long)j);
            exit(1);
        }
}

static inline uint64_t lcp_at_pos(const ChiIndex& idx, const PhiLookup& P, uint64_t pos) {
    // find largest j with start[j] <= pos
    uint64_t lo;
    if (P.slim) {
        // coarse: largest sample block whose start <= pos, then refine in the
        // packed structure (<= ACCEL_STRIDE get<2> probes, binary search)
        uint64_t a = 0, h = P.accel.size() - 1;
        while (a < h) {
            uint64_t mid = (a + h + 1) / 2;
            if (P.accel[mid] <= pos) a = mid; else h = mid - 1;
        }
        uint64_t blo = a * PhiLookup::ACCEL_STRIDE;
        uint64_t bhi = std::min(blo + PhiLookup::ACCEL_STRIDE - 1, P.nInt);
        lo = blo;
        while (lo < bhi) {
            uint64_t mid = (lo + bhi + 1) / 2;
            if (P.phi->data.get<2>(mid) <= pos) lo = mid; else bhi = mid - 1;
        }
    } else {
        uint64_t hi = P.nInt;
        lo = 0;
        while (lo < hi) {
            uint64_t mid = (lo + hi + 1) / 2;
            if (P.start[mid] <= pos) lo = mid; else hi = mid - 1;
        }
    }
    uint64_t st = P.slim ? P.phi->data.get<2>(lo) : P.start[lo];
    return idx.PLCPsamples[lo] - (pos - st);
}

// ---- per-run aggregates ----
struct RunAgg {
    std::vector<uint64_t> topLCP, interiorMin, saFirst, saLast;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.lcp_index.lcp_index> --rlbwt BASE [-o OUT] [-t N] [--triples] [-A]\n", argv[0]);
        return 1;
    }
    std::string inPath = argv[1], outPath, rlbwtBase, sidecarPath, samplesPath;
    int nthreads = std::thread::hardware_concurrency();
    bool dumpTriples = false, convA = false, slim = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outPath = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--triples")) dumpTriples = true;
        else if (!strcmp(argv[i], "-A")) convA = true;
        else if (!strcmp(argv[i], "--rlbwt") && i + 1 < argc) rlbwtBase = argv[++i];
        else if (!strcmp(argv[i], "--sidecar") && i + 1 < argc) sidecarPath = argv[++i];
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) samplesPath = argv[++i];
        else if (!strcmp(argv[i], "--slim")) slim = true;
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 1; }
    }
    if (rlbwtBase.empty()) { fprintf(stderr, "--rlbwt is required\n"); return 1; }
    if (!samplesPath.empty() && sidecarPath.empty()) {
        fprintf(stderr, "--samples requires --sidecar (forward-flat offsets)\n"); return 1;
    }

    ChiIndex idx;
    {
        std::ifstream in(inPath, std::ios::binary);
        if (!in.is_open()) { fprintf(stderr, "cannot open %s\n", inPath.c_str()); return 1; }
        idx.load(in, slim);
    }
    Runs R; build_runs_files(rlbwtBase, R);
    PhiLookup P; build_phi_lookup(idx, P, slim);
    const uint64_t n = idx.totalLen;
    if (R.n != n) { fprintf(stderr, "FATAL: run total %llu != totalLen %llu\n",
                            (unsigned long long)R.n, (unsigned long long)n); return 1; }
    // strings = total '\n' symbols in the rlbwt (raw byte 0x0A)
    uint64_t K = 0, nlRuns = 0;
    for (uint64_t i = 0; i < R.R; ++i) if (R.a[i] == 0x0A) { K += R.l[i]; ++nlRuns; }
    // cross-check: the index's split-run count (F.size()) must equal
    // R + (endmarkers - merged endmarker runs)
    if (idx.F.size() != R.R + K - nlRuns) {
        fprintf(stderr, "FATAL: index runs %llu != rlbwt splits %llu\n",
                (unsigned long long)idx.F.size(), (unsigned long long)(R.R + K - nlRuns));
        return 1;
    }
    fprintf(stderr, "loaded: n=%llu runs=%llu strings=%llu\n",
            (unsigned long long)n, (unsigned long long)R.R, (unsigned long long)K);

    // ---- per-run aggregates ----
    // fat: native u64 vectors (INF sentinels). slim: topLCP/saFirst/saLast
    // packed to bits(n-1) (single-writer cells, written bitmap for
    // validation), interiorMin stays native for the atomic-min CAS loop.
    const uint8_t wpos = [&]{ uint8_t w = 1; while (((uint64_t)1 << w) <= (n ? n - 1 : 1)) ++w; return w; }();
    struct RunAgg {
        // walk-side aggregates stay native u64: distinct runs use distinct
        // cells (no false sharing races), while packed cells would share
        // 64-bit words across runs and require CAS loops. The slim memory
        // win is on the load side (Psi/intAtTop skipped) and the phi lookup
        // (accelerated binary search instead of the full start copy).
        std::vector<uint64_t> topLCP, saFirst, saLast, interiorMin;
        std::vector<uint64_t> written;       // atomic-OR bitmap (samples check)
    } A;
    (void)wpos;
    A.topLCP.assign(R.R, INF); A.saFirst.assign(R.R, INF);
    A.saLast.assign(R.R, INF); A.interiorMin.assign(R.R, INF);
    A.written.assign(R.R/64 + 1, 0);
    std::vector<uint64_t> saV4;                                 // run-end samples (.ri4 v4)
    if (!samplesPath.empty()) saV4.assign(R.R, INF);
    std::vector<uint64_t> fsFwd;               // sidecar forward-flat starts
    if (!sidecarPath.empty()) {
        FILE* sf = fopen(sidecarPath.c_str(), "r");
        if (!sf) { fprintf(stderr, "cannot open sidecar %s\n", sidecarPath.c_str()); return 1; }
        char name[4096]; unsigned long long fs, fl;
        while (fscanf(sf, "%4095s\t%llu\t%llu", name, &fs, &fl) == 3)
            fsFwd.push_back(fs);
        fclose(sf);
        if (fsFwd.size() != K) {
            fprintf(stderr, "FATAL: sidecar rows %llu != strings %llu\n",
                    (unsigned long long)fsFwd.size(), (unsigned long long)K);
            return 1;
        }
        fprintf(stderr, "sidecar: %llu rows loaded\n", (unsigned long long)fsFwd.size());
    }

    // ---- per-string self-terminating LF walks (two phases) ----
    // Every BWT row is visited exactly once across all strings' walks: the
    // walk of string i visits its bare-sentinel row i (position p_i) and the
    // suffixes at positions p_i-1 .. fstart[i] (rows reached by LF steps).
    // Rows 0..K-1 are the bare sentinels in string order (BCR convention,
    // rlbwt_sampler-gated), so a walk terminates when LF lands on a row < K.
    // Phase 1 measures len_i; phase 2 (after prefix-summing fstart) fills the
    // per-run aggregates with absolute positions and PLCP lookups.
    std::vector<uint64_t> len(K, 0), fstart(K, 0);

    // visit(row, pos): aggregate lcp/sa into run stats; single-writer for
    // topLCP/saFirst/saLast (first/last row visited once), atomic-min for
    // interiorMin. Row k's lcp is PLCP[SA[k]] and counts as interior iff
    // k is not the run's first row (scan-rs's m accumulates rows s+1..e).
    auto visit = [&](uint64_t row, uint64_t pos, uint64_t si) {
        uint64_t run = run_of_row(R, row);
        uint64_t l = lcp_at_pos(idx, P, pos);
        if (row == R.starts[run]) {
            A.topLCP[run] = l; A.saFirst[run] = pos;
        } else {
            atomic_min_u64(&A.interiorMin[run], l);
        }
        if (row == R.starts[run] + R.l[run] - 1) {
            A.saLast[run] = pos;
            __atomic_fetch_or(&A.written[run >> 6], 1ULL << (run & 63), __ATOMIC_RELAXED);
            if (!samplesPath.empty())
                saV4[run] = fsFwd[si] + (fstart[si] + len[si] - pos);
        }
    };

    {
        std::atomic<uint64_t> next{0};
        auto measure = [&](void) {
            for (;;) {
                uint64_t i = next.fetch_add(1);
                if (i >= K) return;
                uint64_t row = i, steps = 0;
                for (;;) {
                    uint64_t run = run_of_row(R, row);
                    row = lf_of(R, run, row - R.starts[run]);
                    ++steps;
                    if (row < K) break;
                }
                len[i] = steps - 1;
            }
        };
        std::vector<std::thread> th;
        for (int t = 0; t < nthreads; ++t) th.emplace_back(measure);
        for (auto& t : th) t.join();
    }
    {
        uint64_t acc = 0;
        for (uint64_t i = 0; i < K; ++i) { fstart[i] = acc; acc += len[i] + 1; }
        if (acc != n) { fprintf(stderr, "FATAL: string lengths sum %llu != n %llu\n",
                                (unsigned long long)acc, (unsigned long long)n); return 1; }
    }
    {
        std::atomic<uint64_t> next{0};
        auto fill = [&](void) {
            for (;;) {
                uint64_t i = next.fetch_add(1);
                if (i >= K) return;
                uint64_t row = i, pos = fstart[i] + len[i];
                for (uint64_t t = 0; ; ++t) {
                    visit(row, pos, i);
                    if (t == len[i]) break;
                    uint64_t run = run_of_row(R, row);
                    row = lf_of(R, run, row - R.starts[run]);
                    --pos;
                }
            }
        };
        std::vector<std::thread> th;
        for (int t = 0; t < nthreads; ++t) th.emplace_back(fill);
        for (auto& t : th) t.join();
    }
    // validation: every run filled (interiorMin may stay INF for
    // single-row runs — the empty interior min)
    for (uint64_t i = 0; i < R.R; ++i) {
        bool ok = (A.topLCP[i] != INF && A.saFirst[i] != INF && A.saLast[i] != INF);
        if (!ok) {
            fprintf(stderr, "FATAL: run %llu unfilled\n", (unsigned long long)i);
            return 1;
        }
    }
    fprintf(stderr, "all %llu runs aggregated\n", (unsigned long long)R.R);

    // accessor shims (slim/fat)
    auto getTop  = [&](uint64_t i) { return A.topLCP[i]; };
    auto getFirst= [&](uint64_t i) { return A.saFirst[i]; };
    auto getLast = [&](uint64_t i) { return A.saLast[i]; };

    if (dumpTriples) {
        for (uint64_t i = 0; i < R.R; ++i)
            printf("%u %llu %llu %llu %llu %llu\n", R.a[i],
                   (unsigned long long)getTop(i), (unsigned long long)A.interiorMin[i],
                   (unsigned long long)getFirst(i), (unsigned long long)getLast(i),
                   (unsigned long long)R.l[i]);
    }

    // ---- scan-rs state machine over runs (char-change logic) ----
    const uint64_t N = n + 1;   // scan-rs convention: N = text length + 1
    const int SIGMA = 128;
    struct Cand { int64_t len; uint64_t pos; bool active; };
    std::vector<Cand> r(SIGMA, {-1, 0, false});
    std::vector<uint64_t> out;
    int64_t m = INT64_MAX;
    auto eval = [&](int64_t l, std::vector<Cand>& rr, std::vector<uint64_t>& oo) {
        for (int c = 1; c < SIGMA; ++c) {
            if (l < rr[c].len) {
                if (rr[c].active) oo.push_back(rr[c].pos);
                rr[c] = {l, 0, false};
            }
        }
    };
    auto upd = [](std::vector<Cand>& rr, int c, int64_t l, uint64_t pos) {
        if (l > rr[c].len) rr[c] = {l, pos, true};
    };
    // feed runs; '\n' (0x0A) maps to sentinel char 0 (never emitted)
    int p = -1;                 // previous run's char (stream char)
    uint64_t p_saLast = 0;      // saLast of the previous run
    for (uint64_t i = 0; i < R.R; ++i) {
        int c = (R.a[i] == 0x0A) ? 0 : (int)R.a[i];
        int64_t lcpB = (int64_t)getTop(i);   // boundary lcp
        if (i == 0) { p = c; p_saLast = getLast(i); m = INT64_MAX; continue; }
        int64_t m2 = (A.interiorMin[i - 1] == INF) ? INT64_MAX
                                                      : (int64_t)A.interiorMin[i - 1];
        int64_t mm = std::min(m, m2);
        if (c != p) {
            int64_t m3 = std::min(mm, lcpB);
            eval(m3, r, out);
            upd(r, p, lcpB, N - p_saLast);
            upd(r, c, lcpB, N - getFirst(i));
            m = INT64_MAX;
        } else {
            m = std::min(mm, lcpB);
        }
        p = c; p_saLast = getLast(i);
    }
    eval(-1, r, out);
    fprintf(stderr, "chi: %llu positions (N=%llu)\n",
            (unsigned long long)out.size(), (unsigned long long)N);

    if (!samplesPath.empty()) {
        // v4 .ri4: "SXRI" u32, version=4, n u64, k u64, R u64, C[256] u64,
        // run_char[R] u8, run_len[R] u32, sa_sample packed to bits(n-1)
        FILE* f = fopen(samplesPath.c_str(), "wb");
        if (!f) { fprintf(stderr, "cannot open %s\n", samplesPath.c_str()); return 1; }
        for (uint64_t i = 0; i < R.R; ++i)
            if (!((A.written[i >> 6] >> (i & 63)) & 1)) { fprintf(stderr, "FATAL: run %llu unsampled\n", (unsigned long long)i); return 1; }
        uint32_t magic = 0x52585349;  // matches rlbwt_sampler byte stream ("ISXR")
        uint32_t version = 4;
        fwrite(&magic, 4, 1, f); fwrite(&version, 4, 1, f);
        fwrite(&R.n, 8, 1, f); fwrite(&K, 8, 1, f); fwrite(&R.R, 8, 1, f);
        fwrite(R.C.data(), 8, 256, f);
        fwrite(R.a.data(), 1, R.R, f);
        fwrite(R.l.data(), 4, R.R, f);
        uint8_t w = 1;
        while (w < 64 && ((uint64_t)1 << w) <= (R.n ? R.n - 1 : 1)) ++w;
        {   // write fixed header via FILE*, then the packed samples via ofstream
            long hdr_end = ftell(f);
            (void)hdr_end;
            fclose(f);
            std::ofstream of(samplesPath, std::ios::binary | std::ios::app);
            sdsl::int_vector<> sa_iv(R.R, 0, w);
            for (uint64_t r = 0; r < R.R; ++r) sa_iv[r] = saV4[r];
            sa_iv.serialize(of);
            of.close();
        }
        fprintf(stderr, "wrote %s (R=%llu, sa_w=%u)\n",
                samplesPath.c_str(), (unsigned long long)R.R, (unsigned)w);
    }

    if (!outPath.empty()) {
        std::ofstream o(outPath, std::ios::binary);
        if (convA) {
            for (uint64_t x : out) { uint64_t y = x - 1; o.write((const char*)&y, 8); }
        } else {
            for (uint64_t x : out) o.write((const char*)&x, 8);
        }
    } else if (!dumpTriples) {
        for (uint64_t x : out) printf("%llu\n", (unsigned long long)x);
    }
    return 0;
}
