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
    void load(std::istream& in) {
        sdsl::load(totalLen, in);
        sdsl::load(F, in);
        sdsl::load(Psi, in);
        sdsl::load(intAtTop, in);
        sdsl::load(Phi, in);
        sdsl::load(PLCPsamples, in);
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
    std::vector<uint64_t> start;   // get<2>(j) for j in 0..numIntervals (incl. terminal)
    const MoveStructureStartTable* phi;
    uint64_t nInt;
};

static void build_phi_lookup(const ChiIndex& idx, PhiLookup& P) {
    P.phi = &idx.Phi;
    P.nInt = idx.Phi.data.size() - 1;
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
    // binary search: largest j with start[j] <= pos
    uint64_t lo = 0, hi = P.nInt;
    while (lo < hi) {
        uint64_t mid = (lo + hi + 1) / 2;
        if (P.start[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return idx.PLCPsamples[lo] - (pos - P.start[lo]);
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
    std::string inPath = argv[1], outPath, rlbwtBase;
    int nthreads = std::thread::hardware_concurrency();
    bool dumpTriples = false, convA = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outPath = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--triples")) dumpTriples = true;
        else if (!strcmp(argv[i], "-A")) convA = true;
        else if (!strcmp(argv[i], "--rlbwt") && i + 1 < argc) rlbwtBase = argv[++i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 1; }
    }
    if (rlbwtBase.empty()) { fprintf(stderr, "--rlbwt is required\n"); return 1; }

    ChiIndex idx;
    {
        std::ifstream in(inPath, std::ios::binary);
        if (!in.is_open()) { fprintf(stderr, "cannot open %s\n", inPath.c_str()); return 1; }
        idx.load(in);
    }
    Runs R; build_runs_files(rlbwtBase, R);
    PhiLookup P; build_phi_lookup(idx, P);
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

    RunAgg A;
    A.topLCP.assign(R.R, INF); A.interiorMin.assign(R.R, INF);
    A.saFirst.assign(R.R, INF); A.saLast.assign(R.R, INF);

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
    auto visit = [&](uint64_t row, uint64_t pos) {
        uint64_t run = run_of_row(R, row);
        uint64_t l = lcp_at_pos(idx, P, pos);
        if (row == R.starts[run]) {
            A.topLCP[run] = l;
            A.saFirst[run] = pos;
        } else {
            atomic_min_u64(&A.interiorMin[run], l);
        }
        if (row == R.starts[run] + R.l[run] - 1)
            A.saLast[run] = pos;
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
                    visit(row, pos);
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
        if (A.topLCP[i] == INF || A.saFirst[i] == INF || A.saLast[i] == INF) {
            fprintf(stderr, "FATAL: run %llu unfilled (top=%llu f=%llu l=%llu)\n",
                    (unsigned long long)i, (unsigned long long)A.topLCP[i],
                    (unsigned long long)A.saFirst[i], (unsigned long long)A.saLast[i]);
            return 1;
        }
    }
    fprintf(stderr, "all %llu runs aggregated\n", (unsigned long long)R.R);

    if (dumpTriples) {
        for (uint64_t i = 0; i < R.R; ++i)
            printf("%u %llu %llu %llu %llu %llu\n", R.a[i],
                   (unsigned long long)A.topLCP[i], (unsigned long long)A.interiorMin[i],
                   (unsigned long long)A.saFirst[i], (unsigned long long)A.saLast[i],
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
        uint64_t top = A.topLCP[i];
        int64_t lcpB = (top == INF) ? (int64_t)0 : (int64_t)top; // boundary lcp
        if (i == 0) { p = c; p_saLast = A.saLast[i]; m = INT64_MAX; continue; }
        int64_t m2 = (A.interiorMin[i - 1] == INF) ? INT64_MAX
                                                      : (int64_t)A.interiorMin[i - 1];
        int64_t mm = std::min(m, m2);
        if (c != p) {
            int64_t m3 = std::min(mm, lcpB);
            eval(m3, r, out);
            upd(r, p, lcpB, N - p_saLast);
            upd(r, c, lcpB, N - A.saFirst[i]);
            m = INT64_MAX;
        } else {
            m = std::min(mm, lcpB);
        }
        p = c; p_saLast = A.saLast[i];
    }
    eval(-1, r, out);
    fprintf(stderr, "chi: %llu positions (N=%llu)\n",
            (unsigned long long)out.size(), (unsigned long long)N);

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
