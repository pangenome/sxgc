// chi_rspace_dump.cpp — YEAST LANE Stage A+B: per-run aggregates from
// r-space structures ONLY (.ri4 + lcp_index + PFP artifacts). No O(n)
// pass, no per-position walks, no SA materialization.
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

#include <sdsl/int_vector.hpp>
#include "pfp/utils.hpp"
typedef pfpds::long_type long_type;
#include "moveStructure/moveStructure.h"

// pfp_ds (vendored in r-pfbwt) + gsacak
#include "pfp/pfp.hpp"
#include "pfp/sa_support.hpp"
#include "pfp/lce_support.hpp"
extern "C" {
#include "gsacak.h"
}

static const uint64_t INF = std::numeric_limits<uint64_t>::max();

// ---------------- .ri4 loader (runs only) ----------------
struct Ri4 {
    uint64_t n = 0, k = 0, R = 0;
    std::vector<uint64_t> C;      // 256
    std::vector<uint8_t> a;       // run chars
    std::vector<uint32_t> l;      // run lens
    std::vector<uint64_t> starts; // run row starts
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

int main(int argc, char** argv) {
    std::string ri4Path, parsePrefix, lcpIndexPath, outPath, flatPath;
    int nthreads = std::thread::hardware_concurrency();
    uint64_t calibRows = 256;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--ri4") && i + 1 < argc) ri4Path = argv[++i];
        else if (!strcmp(argv[i], "--parse") && i + 1 < argc) parsePrefix = argv[++i];
        else if (!strcmp(argv[i], "--lcp-index") && i + 1 < argc) lcpIndexPath = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outPath = argv[++i];
        else if (!strcmp(argv[i], "--flat") && i + 1 < argc) flatPath = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--calib-rows") && i + 1 < argc) calibRows = strtoull(argv[++i], nullptr, 10);
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 1; }
    }
    if (ri4Path.empty() || parsePrefix.empty() || lcpIndexPath.empty() || outPath.empty()) {
        fprintf(stderr, "usage: chi_rspace_dump --ri4 F.ri4 --parse PFP_PREFIX --lcp-index F.lcp_index.lcp_index -o OUT.agg [-t N] [--flat F] [--calib-rows N]\n");
        return 1;
    }

    Ri4 ri4; ri4.load(ri4Path);
    fprintf(stderr, "ri4: n=%llu k=%llu R=%llu\n",
            (unsigned long long)ri4.n, (unsigned long long)ri4.k, (unsigned long long)ri4.R);

    ChiIndex idx;
    {
        std::ifstream in(lcpIndexPath, std::ios::binary);
        if (!in.is_open()) { fprintf(stderr, "cannot open %s\n", lcpIndexPath.c_str()); return 1; }
        idx.load(in);
    }
    if (idx.totalLen != ri4.n) {
        fprintf(stderr, "FATAL: lcp_index totalLen %llu != ri4 n %llu\n",
                (unsigned long long)idx.totalLen, (unsigned long long)ri4.n);
        return 1;
    }
    PhiLookup P; build_phi_lookup(idx, P);
    fprintf(stderr, "lcp_index: pieces=%llu totalLen=%llu\n",
            (unsigned long long)(P.nInt), (unsigned long long)idx.totalLen);

    // ---- PFP machinery ----
    const long_type W = 10;
    std::less<uint8_t> u8comp;
    fprintf(stderr, "building pfpds dictionary...\n");
    pfpds::dictionary<uint8_t> D(parsePrefix, W, u8comp, true, true, true, true, true, true, true);
    fprintf(stderr, "dict: phrases=%llu size=%llu\n",
            (unsigned long long)D.n_phrases(), (unsigned long long)D.d.size());
    fprintf(stderr, "building parse (saP/isaP/lcpP/rmq)...\n");
    pfpds::parse PP(parsePrefix, D.n_phrases() + 1, true, true, true, true);
    fprintf(stderr, "building pf_parsing (b_p, b_bwt, M, W-wavelet)...\n");
    pfpds::pf_parsing<uint8_t> PF(D, PP, true, false);
    if ((long_type)PF.n != ri4.n + W) {
        fprintf(stderr, "WARNING: pfp n %llu != ri4 n+W %llu (separators/endmarker "
                "convention differs; the flat calibration below is authoritative)\n",
                (unsigned long long)PF.n, (unsigned long long)(ri4.n + W));
    }
    fprintf(stderr, "pfp: n=%llu parse=%llu |M|=%llu\n",
            (unsigned long long)PF.n, (unsigned long long)PP.p.size() - 1,
            (unsigned long long)PF.M.size());
    pfpds::pfp_sa_support<uint8_t> SA_sup(PF);
    pfpds::pfp_lce_support<uint8_t> LCE_sup(PF);

    const uint64_t n = ri4.n;
    // ---- row-space convention (empirically pinned via DUMP_ROWS on
    // random-4-2k: resolved[j+W-1] == SA(text)[j-1], i.e. machinery row m
    // <-> SA-of-text row m-W; the parse covers the same bytes the rlbwt
    // indexes (separators INCLUDED), so resolve_row(j) = SA_sup(j+W)
    // uniformly; no special cases; the flat spot-check validates) ----
    if (getenv("DUMP_ROWS")) {
        uint64_t lim = strtoull(getenv("DUMP_ROWS"), nullptr, 10);
        if (lim > ri4.n) lim = ri4.n;
        FILE* rf = fopen((outPath + ".rows").c_str(), "wb");
        for (uint64_t j = 0; j < lim; ++j) {
            uint64_t p = (uint64_t)SA_sup((long_type)j + W);
            uint64_t w = p; fwrite(&w, 8, 1, rf);
        }
        fclose(rf);
        fprintf(stderr, "dumped %llu rows\n", (unsigned long long)lim);
    }
    // ---- row-space calibration: full-probe offset selection ----
    // For each candidate offset, score over the full probe set (run-starts +
    // spread rows); require a UNIQUE zero-bad offset. The battery pins W.
    long_type ROW_OFF = W;
    bool rowOffPinned = false;
    if (!flatPath.empty()) {
        std::ifstream calFlat(flatPath, std::ios::binary);
        std::vector<uint64_t> calRows;
        for (uint64_t t = 0; t < calibRows; ++t)
            calRows.push_back((uint64_t)(((double)t + 0.5) / calibRows * ri4.R));
        for (uint64_t t = 0; t < calibRows && t < ri4.n; ++t)
            calRows.push_back(t * ri4.n / calibRows);
        long_type bestOff = W; uint64_t bestBad = ~0ULL; uint64_t nZero = 0;
        for (long_type off = W - 2; off <= (long_type)W + 2; ++off) {
            uint64_t bad = 0, chk = 0;
            for (uint64_t jj : calRows) {
                if (jj == 0 || jj >= ri4.n) continue;
                uint64_t pos = (uint64_t)SA_sup((long_type)jj + off);
                if (pos == 0) continue;
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
        return (uint64_t)SA_sup((long_type)j + ROW_OFF);
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
            uint64_t pos = (uint64_t)SA_sup((long_type)j + moff0);
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
                    lfPos = (uint64_t)SA_sup((long_type)rlf + moff0);
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
                    uint64_t posd = (uint64_t)SA_sup((long_type)jd + moff0);
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
                    uint64_t p2 = (uint64_t)SA_sup((long_type)j + off);
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

    // ---- per-run aggregates (parallel) ----
    std::vector<uint64_t> topLCP(ri4.R, INF), saFirst(ri4.R, INF),
        saLast(ri4.R, INF), interiorMin(ri4.R, INF);
    std::atomic<uint64_t> lceChecks{0}, lceMismatch{0};
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
                saFirst[i] = pf;
                saLast[i] = pl;
                topLCP[i] = (a == 0) ? 0 : lcp_at_pos(idx, P, pf);
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
    uint64_t unfilled = 0;
    for (uint64_t i = 0; i < ri4.R; ++i)
        if (topLCP[i] == INF || saFirst[i] == INF || saLast[i] == INF) unfilled++;
    if (unfilled) { fprintf(stderr, "FATAL: %llu runs unfilled\n", (unsigned long long)unfilled); return 1; }
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
    fprintf(stderr, "wrote %s (%llu runs x 4 u64)\n", outPath.c_str(), (unsigned long long)ri4.R);
    return 0;
}
