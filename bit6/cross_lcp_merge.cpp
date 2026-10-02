// Exact pairwise rleBWT merge of SXCR cyclic-BWT chunks (bit6/chunk_frontend.cpp
// chunk format), emitting either a merged SXCR chunk (cyclic, composable) or the
// final four files (rlebwt/meta/ssa/ssa_t) byte-identical to bit6/chunk_bcr_merge.cpp
// / bit6/bcr_frontend_v2.cpp output.
//
// Theory (checked by --selftest brute force and four-file byte gates):
//  * A chunk stores its text's cyclic rotation order, ties broken by ascending
//    source position; runs store equal preceding characters with first/last
//    row positions only.
//  * Merging left A and right B yields M=A.B. The final four files describe the
//    padded cyclic BWT of P = M.0x02^10: rows are padding rotations n..n+9
//    followed by the strict $-suffix order of M (0x02 is below every remapped
//    text byte, so all rotations are distinct). Head/tail samples are rotation
//    indices; the emitted run/stream encoding matches bcr_frontend_v2 exactly.
//  * Intermediate merges emit the cyclic rotation order of M itself as an SXCR
//    chunk, so pairwise merges compose in a tree or a serial fold.
//  * For two rows of one chunk, the local cyclic comparison and the merged
//    comparison read identical characters until the end of the shorter local
//    suffix, so the orders differ only on "prefix pairs": pairs where the
//    shorter chunk suffix S_q also occurs (as a substring, p+|S_q|<=n) at the
//    longer suffix's position p. The short side q of every such pair is an
//    "anchor": its occurrence set Occ(S_q) has size >= 2 and is contained in
//    the interval of rows whose local key starts with S_q, found by the classic
//    backward-search step on the chunk's own BWT (LF is increasing on equal
//    BWT-character rows even for position-tie-broken periodic orders). Walking
//    chunk suffixes from the last position leftward until the interval empties
//    enumerates every anchor block in O(walk steps) rank queries.
//  * The merged order equals the local order with every walked block's member
//    sequence sorted in place by merged key: real-prefix pairs always share the
//    short side's block (both are members), pairs sharing a block end up
//    merged-ordered, pairs sharing no block are never real-prefix pairs so
//    their local order is already their merged order, and prefix blocks nest
//    or are disjoint, so in-place interval sorts commute and are idempotent.
//    Blocks may contain extra "wrap match" members; sorting them by merged key
//    is harmless. Non-anchor rows are never one side of a real-prefix pair,
//    so their relative order is always already correct.
//  * Cross placement is a plain two-pointer merge of the two repaired orders
//    under the same total-order comparator.
//  * Comparisons use dense prefix fingerprints over M.M with a directly
//    checked 16-symbol fast path, galloping hash probes, bisection, and FULL
//    direct verification of every proposed equal prefix and its mismatch
//    boundary (the SLIM_FP discipline, bit6/slim_lce.hpp). A hash/verification
//    disagreement aborts the merge: no probabilistic answer is ever emitted.
//    Every probe and verified symbol is journaled (the cost-table metric).
//
// Inputs may be materialized either by an LF walk from the SXCR runs alone or
// from a sidecar file (<chunk>.sxs, text + row->position) written next to every
// intermediate; sidecars are always validated against the SXCR runs.
//
// Journal: CROSS_PHASE lines (stderr) carry per-phase wall/RSS; one CROSS_PAIR
// line (stdout) per merge carries the full cost row for the cost table.
//
// Build: c++ -O3 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -o /tmp/cross_lcp_merge
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using U = uint64_t;

static void fail(const char* s) { std::fprintf(stderr, "CROSS_LCP_FATAL %s\n", s); std::exit(2); }
static void require(bool b, const char* s) { if (!b) fail(s); }
static double now_sec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static U peak_rss_kib() { struct rusage ru{}; getrusage(RUSAGE_SELF, &ru); return (U)ru.ru_maxrss; }
static double G_T0 = 0;
static void phase(const char* name, double& last) {
    double t = now_sec();
    std::fprintf(stderr, "CROSS_PHASE %s wall=%.3f cumulative=%.3f peak_rss_kib=%llu\n",
                 name, t - last, t - G_T0, (unsigned long long)peak_rss_kib());
    std::fflush(stderr);
    last = t;
}

// ---------------------------------------------------------------- little endian
static U rd_le(std::ifstream& f, unsigned bytes) {
    uint8_t raw[8]{}; f.read(reinterpret_cast<char*>(raw), bytes);
    require(bool(f), "truncated read");
    U v = 0; for (unsigned i = 0; i < bytes; ++i) v |= U(raw[i]) << (8 * i);
    return v;
}
struct OutFile {
    std::ofstream f; std::vector<char> buf;
    explicit OutFile(const std::string& path, size_t cap = 1 << 22) {
        require(!fs::exists(path), "output exists; refusing to clobber");
        f.open(path, std::ios::binary); require(bool(f), "cannot open output");
        buf.reserve(cap);
    }
    void raw(const void* p, size_t k) {
        const char* q = static_cast<const char*>(p);
        if (buf.size() + k > buf.capacity()) { f.write(buf.data(), buf.size()); buf.clear(); }
        if (k >= buf.capacity()) { f.write(q, k); return; }
        buf.insert(buf.end(), q, q + k);
    }
    void w64(U v) { char b[8]; for (int i = 0; i < 8; ++i) b[i] = char(v >> (8 * i)); raw(b, 8); }
    void w32(uint32_t v) { char b[4]; for (int i = 0; i < 4; ++i) b[i] = char(v >> (8 * i)); raw(b, 4); }
    void w8(uint8_t v) { raw(&v, 1); }
    void flush() { if (!buf.empty()) { f.write(buf.data(), buf.size()); buf.clear(); } require(bool(f), "write failed"); }
    ~OutFile() { try { flush(); } catch (...) {} }
};

// ---------------------------------------------------------------- budget
struct Budget {
    // Everything is charged; the totals are the cost-table evidence.
    U comparisons = 0, fast_decided = 0, probe_decided = 0, cap_decided = 0;
    U probes = 0, symbols = 0, max_lce = 0, tie_decided = 0;
    U lce_over_10k = 0, lce_over_100k = 0, lce_over_1m = 0;
    void note_lce(U l) {
        if (l > max_lce) max_lce = l;
        if (l >= 10000) ++lce_over_10k;
        if (l >= 100000) ++lce_over_100k;
        if (l >= 1000000) ++lce_over_1m;
    }
};

// ---------------------------------------------------------------- fingerprints
static const U HASH_BASE = 0x9e3779b185ebca87ULL;
struct Keys {
    const uint8_t* M2 = nullptr;   // M.M, 2n bytes
    const U* H = nullptr;          // prefix hashes over M2, 2n+1 entries
    U n = 0;
    bool dollar = false;           // final $-suffix mode vs cyclic (M^infty) mode
    Budget* bud = nullptr;
    U pw2[64]{};                   // HASH_BASE^(2^k)

    void init(const uint8_t* m2, const U* h, U len, bool dol, Budget* b) {
        M2 = m2; H = h; n = len; dollar = dol; bud = b;
        pw2[0] = HASH_BASE;
        for (int k = 1; k < 64; ++k) pw2[k] = pw2[k - 1] * pw2[k - 1];
    }
    U pw(U e) const { U r = 1; for (int k = 0; e; ++k, e >>= 1) if (e & 1) r *= pw2[k]; return r; }
    // Hash equality of M2[a..a+len) and M2[b..b+len). One charged probe.
    bool eq(U a, U b, U len) const {
        ++bud->probes;
        return H[a + len] - H[a] * pw(len) == H[b + len] - H[b] * pw(len);
    }
    // Total order over merged keys of distinct positions p1 != p2.
    // dollar: key(p) = M[p..n).$^infty, distinct; the shorter suffix (larger p)
    //         is smaller once the common prefix covers it.
    // cyclic: key(p) = M^infty[p..]; LCE == n proves period |p1-p2|, a tie
    //         broken by ascending position (the chunk front end's rule).
    int cmp(U p1, U p2) const {
        require(p1 != p2 && p1 < n && p2 < n, "comparator bounds");
        ++bud->comparisons;
        U cap = dollar ? std::min(n - p1, n - p2) : n;
        const uint8_t* a = M2 + p1; const uint8_t* b = M2 + p2;
        U fast = std::min<U>(cap, 16), j = 0;
        for (; j < fast; ++j)
            if (a[j] != b[j]) {
                bud->symbols += j + 1; ++bud->fast_decided; bud->note_lce(j);
                return a[j] < b[j] ? -1 : 1;
            }
        bud->symbols += j;
        if (fast == cap) {
            ++bud->cap_decided;
            if (dollar) return p1 > p2 ? -1 : 1;   // $ is minimal: shorter first
            ++bud->tie_decided; bud->note_lce(cap);
            return p1 < p2 ? -1 : 1;               // cyclic tie: position order
        }
        // Galloping hash probes from the certified fast prefix, then bisect.
        U lo = fast, hi = cap, step = fast, np = 0;
        while (lo < hi) {
            U len = std::min(lo + step, hi); ++np;
            if (eq(p1, p2, len)) { lo = len; step *= 2; }
            else { hi = len; break; }
        }
        while (hi - lo > 1) {
            U mid = lo + (hi - lo) / 2; ++np;
            if (eq(p1, p2, mid)) lo = mid; else hi = mid;
        }
        bud->probes += np;
        // SLIM_FP discipline: fully verify the proposed prefix and its boundary.
        for (U k = fast; k < lo; ++k)
            require(M2[p1 + k] == M2[p2 + k], "hash collision inside proposed prefix");
        bud->symbols += lo - fast;
        bud->note_lce(lo);
        if (lo == cap) {
            ++bud->probe_decided;
            if (dollar) return p1 > p2 ? -1 : 1;
            ++bud->tie_decided;
            return p1 < p2 ? -1 : 1;
        }
        require(M2[p1 + lo] != M2[p2 + lo], "hash collision at mismatch boundary");
        bud->symbols += 1; ++bud->probe_decided;
        return M2[p1 + lo] < M2[p2 + lo] ? -1 : 1;
    }
};

static void build_hash(const uint8_t* M2, U n2, U* H, U threads) {
    // H[i] = hash of M2[0..i). Three passes: per-segment relative hashes in
    // place (parallel; each thread keeps its rolling hash in a local and its
    // final value in a separate slot, because the boundary slot H[s_{k+1}]
    // is written by both neighbours and must not double-book), absolute
    // segment-start values (sequential), in-place fix-up (parallel).
    if (threads <= 1 || n2 < (1u << 20)) {
        H[0] = 0;
        for (U i = 0; i < n2; ++i) H[i + 1] = H[i] * HASH_BASE + (M2[i] + 1);
        return;
    }
    U tcount = std::min(threads, (n2 + (1u << 20) - 1) / (1u << 20));
    U seg = (n2 + tcount - 1) / tcount;
    std::vector<U> relend(tcount, 0);
    std::vector<std::thread> ts;
    auto rel = [&](U k) {
        U s = k * seg, e = std::min(n2, s + seg), h = 0;
        for (U i = s; i < e; ++i) { h = h * HASH_BASE + (M2[i] + 1); H[i + 1] = h; }
        relend[k] = h;
    };
    H[0] = 0;
    for (U k = 0; k < tcount; ++k) ts.emplace_back(rel, k);
    for (auto& t : ts) t.join();
    U pw2[64]; pw2[0] = HASH_BASE;
    for (int k = 1; k < 64; ++k) pw2[k] = pw2[k - 1] * pw2[k - 1];
    auto pwfun = [&](U e) { U r = 1; for (int k = 0; e; ++k, e >>= 1) if (e & 1) r *= pw2[k]; return r; };
    U cur = 0;
    for (U k = 0; k < tcount; ++k) {
        U s = k * seg, e = std::min(n2, s + seg);
        H[s] = cur;
        cur = cur * pwfun(e - s) + relend[k];
    }
    H[n2] = cur;   // absolute hash of the whole prefix; pass 2 owns all H[s]
    ts.clear();
    auto fix = [&](U k) {
        U s = k * seg, e = std::min(n2, s + seg);
        U rr = H[s];   // absolute hash of M2[0..s)
        for (U i = s + 1; i < e; ++i) { rr = rr * HASH_BASE; H[i] += rr; }
    };
    for (U k = 0; k < tcount; ++k) ts.emplace_back(fix, k);
    for (auto& t : ts) t.join();
    // Verify absolute hashes at segment boundaries (start, middle, last): the
    // window hash H[c]-H[c-w]*B^w must equal a direct recomputation.
    U cands[] = {seg, n2 / 2 & ~(seg - 1), n2};
    for (U c : cands) {
        if (c > n2) continue;
        U w = std::min<U>(c, 4096), direct = 0;
        for (U i = c - w; i < c; ++i) direct = direct * HASH_BASE + (M2[i] + 1);
        U fromH = H[c] - H[c - w] * pwfun(w);
        require(fromH == direct, "parallel hash build mismatch at boundary");
    }
}

// ---------------------------------------------------------------- chunk loading
struct Side { // materialized left or right input
    U offset = 0, n = 0, runs = 0, period = 0; // period==n means aperiodic
    std::vector<uint8_t> text;   // local text (n bytes)
    std::vector<uint32_t> pos;   // row -> local position (the chunk's cyclic order)
};

// Minimal cyclic period d | n with T[i]==T[i+d] (cyclically); n if aperiodic.
static U min_period(const std::vector<uint8_t>& T, U n) {
    std::vector<U> divs;
    for (U d = 1; d * d <= n; ++d) if (n % d == 0) { divs.push_back(d); if (d != n / d) divs.push_back(n / d); }
    std::sort(divs.begin(), divs.end());
    for (U d : divs) {
        if (d >= n) continue;
        bool ok = true;
        for (U i = 0; i + d < n && ok; ++i) ok = T[i] == T[i + d];
        if (ok) return d;
    }
    return n;
}

static void sxcr_header(const std::string& path, U& offset, U& n, U& runs) {
    std::ifstream f(path, std::ios::binary);
    require(bool(f), "cannot open SXCR chunk");
    char magic[4]{}; f.read(magic, 4);
    require(bool(f) && !std::memcmp(magic, "SXCR", 4), "SXCR magic mismatch");
    require(rd_le(f, 4) == 1, "unsupported SXCR version");
    offset = rd_le(f, 8); n = rd_le(f, 8); runs = rd_le(f, 8);
    require(n && n < (1u << 31) && runs, "invalid SXCR header");
    require(fs::file_size(path) == 32 + 21 * runs, "SXCR file size mismatch");
}

// Validate runs against (text,pos): preceding char per row and head/tail samples.
static void validate_runs(const std::string& path, U n, U runs,
                           const std::vector<uint8_t>& text, const std::vector<uint32_t>& pos) {
    std::ifstream f(path, std::ios::binary);
    f.seekg(32);
    U row = 0; uint8_t prev = 0;
    for (U i = 0; i < runs; ++i) {
        uint8_t c = uint8_t(rd_le(f, 1));
        U len = rd_le(f, 4), h = rd_le(f, 8), t = rd_le(f, 8);
        require(len && row + len <= n && h < n && t < n, "invalid SXCR run");
        require(i == 0 || c != prev, "noncanonical SXCR runs");
        require(pos[row] == h && pos[row + len - 1] == t, "SXCR run samples disagree with order");
        for (U k = 0; k < len; ++k)
            require(text[(pos[row + k] + n - 1) % n] == c, "SXCR run text mismatch");
        row += len; prev = c;
    }
    require(row == n && f.peek() == EOF, "SXCR length mismatch");
}

// Text + row->position materialization from the SXCR runs alone.
//
// The classic LF formula C[bwt]+rank is the true rotation-by-one permutation
// only when no two rotations are equal. Periodic chunks (cyclic period d<n)
// break it inside the tied class that contains position 0: the banked BCR
// merger survives because it only recovers text, where every emitted character
// depends on the step index modulo the period. We do the same for text, then:
//  * aperiodic chunks: LF is exact, so a threaded multi-seed walk recovers the
//    full row->position map (cross-checked against the walked text);
//  * periodic chunks: tied classes are exactly the residue classes mod d,
//    ordered by ascending position (the front end's tie rule), so the whole
//    cyclic order is written out directly from the sorted class streams.
static void walk_chunk(const std::string& path, U n, U runs, U threads,
                       std::vector<uint8_t>& text, std::vector<uint32_t>& pos, U& period) {
    struct RunRec { uint8_t c; uint32_t len; U h, t; };
    std::vector<RunRec> rs(runs);
    {
        std::ifstream f(path, std::ios::binary);
        f.seekg(32);
        for (U i = 0; i < runs; ++i) {
            rs[i].c = uint8_t(rd_le(f, 1));
            rs[i].len = uint32_t(rd_le(f, 4));
            rs[i].h = rd_le(f, 8); rs[i].t = rd_le(f, 8);
            require(rs[i].len && rs[i].h < n && rs[i].t < n && (i == 0 || rs[i].c != rs[i-1].c), "invalid SXCR run");
        }
    }
    std::array<U, 256> freq{}, C{}, seen{};
    for (auto& r : rs) freq[r.c] += r.len;
    U acc = 0; for (unsigned c = 0; c < 256; ++c) { C[c] = acc; acc += freq[c]; }
    require(acc == n, "SXCR counts mismatch");
    std::vector<uint8_t> bwt(n);
    std::vector<uint32_t> lf(n);
    std::vector<std::pair<U, U>> seeds;  // (row, position)
    seeds.reserve(runs);
    U row = 0;
    for (auto& r : rs) {
        seeds.push_back({row, r.h});
        for (U k = 0; k < r.len; ++k, ++row) {
            bwt[row] = r.c;
            lf[row] = uint32_t(C[r.c] + seen[r.c]++);
        }
    }
    require(row == n, "SXCR run coverage mismatch");
    // Anchor walk: from the run head with the smallest position, walk h_min
    // steps (position congruent to 0 modulo any period), then emit n characters.
    size_t best = 0;
    for (size_t i = 1; i < seeds.size(); ++i) if (seeds[i].second < seeds[best].second) best = i;
    U r0 = seeds[best].first;
    for (U j = 0; j < seeds[best].second; ++j) r0 = lf[r0];
    text.assign(n, 0);
    U at = n;
    for (U j = 0; j < n; ++j) { text[--at] = bwt[r0]; r0 = lf[r0]; }
    period = min_period(text, n);
    if (period < n) {
        // Periodic: classes are residues mod period, tied rows are contiguous
        // and ordered by ascending position. Sort the class streams.
        U d = period, k = n / d;
        std::vector<uint32_t> classes(d);
        for (U r = 0; r < d; ++r) classes[r] = uint32_t(r);
        auto stream_less = [&](uint32_t r, uint32_t s) {
            for (U i = 0; i < d; ++i) {
                uint8_t c1 = text[(r + i) % n], c2 = text[(s + i) % n];
                if (c1 != c2) return c1 < c2;
            }
            require(r == s, "distinct residue classes with equal streams");
            return false;
        };
        std::sort(classes.begin(), classes.end(), stream_less);
        pos.assign(n, 0);
        U idx = 0;
        for (U r : classes)
            for (U m = 0; m < k; ++m) pos[idx++] = uint32_t(r + m * d);
        require(idx == n, "periodic order coverage");
    } else {
        // Aperiodic: LF is exact; threaded multi-seed walk recovers pos.
        pos.assign(n, UINT32_MAX);
        std::vector<std::atomic<uint8_t>> visited(n);
        for (auto& s : seeds) { visited[s.first].store(1, std::memory_order_relaxed); pos[s.first] = uint32_t(s.second); }
        std::atomic<size_t> next{0};
        auto worker = [&]() {
            for (;;) {
                size_t idx = next.fetch_add(1);
                if (idx >= seeds.size()) return;
                U i = seeds[idx].first, p = seeds[idx].second;
                for (;;) {
                    U j = lf[i];
                    uint8_t e = 0;
                    if (!visited[j].compare_exchange_strong(e, 1)) break;
                    pos[j] = uint32_t(p ? p - 1 : n - 1);
                    i = j; p = pos[j];
                }
            }
        };
        U tc = std::min<U>(threads ? threads : 1, 64);
        std::vector<std::thread> ts;
        for (U k = 0; k < tc; ++k) ts.emplace_back(worker);
        for (auto& t : ts) t.join();
        for (U i = 0; i < n; ++i) require(visited[i].load() && pos[i] < n, "LF walk incomplete");
        // Cross-check the two independent text derivations.
        for (U i = 0; i < n; ++i)
            require(text[(pos[i] + n - 1) % n] == bwt[i], "text disagreement between walks");
    }
    validate_runs(path, n, runs, text, pos);
}

static void load_sidecar(const std::string& sxcr, Side& s) {
    std::string p = sxcr + ".sxs";
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) fail("sidecar missing");
    U size = U(f.tellg()); f.seekg(0);
    char magic[4]{}; f.read(magic, 4);
    if (std::memcmp(magic, "SXS1", 4)) fail("sidecar magic mismatch");
    U offset = rd_le(f, 8), n = rd_le(f, 8);
    if (size != 20 + 5 * n || n != s.n || offset != s.offset) fail("sidecar inconsistent with SXCR");
    s.text.resize(n); s.pos.resize(n);
    f.read(reinterpret_cast<char*>(s.text.data()), std::streamsize(n));
    require(bool(f), "sidecar text read");
    f.read(reinterpret_cast<char*>(s.pos.data()), std::streamsize(4 * n));
    require(bool(f) && f.peek() == EOF, "sidecar pos read");
    for (U i = 0; i < n; ++i) require(s.pos[i] < n, "sidecar position out of range");
    s.period = min_period(s.text, n);
    validate_runs(sxcr, n, s.runs, s.text, s.pos);
}

static void load_side(const std::string& path, U threads, Side& s) {
    sxcr_header(path, s.offset, s.n, s.runs);
    std::string side = path + ".sxs";
    if (fs::exists(side)) load_sidecar(path, s);
    else walk_chunk(path, s.n, s.runs, threads, s.text, s.pos, s.period);
}

// ------------------------------------------------------- anchor blocks + repair
struct BlockRec { U q, lo, hi; };

struct RankIndex {
    const std::vector<uint8_t>* bwt = nullptr;
    U n = 0, nb = 0;
    static constexpr U SAMP = 4096;
    std::vector<uint32_t> tab; // [char][bucket] counts before bucket start
    void build(const std::vector<uint8_t>& b, U len) {
        bwt = &b; n = len; nb = n / SAMP + 2;
        tab.assign(size_t(256) * nb, 0);
        U counts[256]{};
        for (U k = 0; k * SAMP < n; ++k) {
            U e = std::min(n, (k + 1) * SAMP);
            for (U i = k * SAMP; i < e; ++i) ++counts[b[i]];
            for (unsigned c = 0; c < 256; ++c) tab[c * nb + k + 1] = uint32_t(counts[c]);
        }
    }
    U operator()(uint8_t c, U x) const {
        U bk = x / SAMP, base = tab[c * nb + bk];
        U e = std::min(n, x);
        for (U i = bk * SAMP; i < e; ++i) base += ((*bwt)[i] == c);
        return base;
    }
};

// Anchor block discovery. A block is the set of rows whose LOCAL key starts
// with the chunk suffix S_q; it is a contiguous row interval, and every
// real-prefix pair shares its short side's block.
// Aperiodic: backward-search walk over chunk suffixes from the last position
// leftward (LF is exact and increasing on equal-BWT rows when no two
// rotations are tied). Recorded anchors: every q with |block| >= 2.
// Periodic (minimal cyclic period d<n): every row's local key is its residue
// class's infinite stream, so a suffix S_q of length >= d has the whole class
// as its block, and the short tail suffixes (q > n-d) have the contiguous run
// of classes whose streams start with that tail. Every position is an anchor.
static void anchor_blocks(const std::vector<uint8_t>& text, const std::vector<uint8_t>& bwt,
                          const std::vector<uint32_t>& pos, U n, U base, U period,
                          std::vector<BlockRec>& out, std::vector<uint32_t>& anchors,
                          U& steps, U& rows) {
    if (period < n) {
        U d = period, k = n / d;
        for (U i = 0; i < d; ++i) {
            U r = pos[i * k] - base;
            for (U j = 0; j < k; ++j)
                require(pos[i * k + j] - base == r + j * d, "periodic class structure mismatch");
            out.push_back({pos[i * k], i * k, (i + 1) * k});
            rows += k;
        }
        for (U q = n - d + 1; q < n; ++q) {
            U L = n - q; // < d
            U c1 = 0, c2 = 0, matched = 0;
            for (U i = 0; i < d; ++i) {
                U r = pos[i * k] - base;
                bool match = true;
                for (U t = 0; t < L; ++t)
                    if (text[(r + t) % n] != text[q + t]) { match = false; break; }
                if (match) { if (!matched) c1 = i; c2 = i + 1; ++matched; }
            }
            require(matched == c2 - c1, "periodic class run not contiguous");
            if (matched && (c2 - c1) * k >= 2) {
                out.push_back({q + base, c1 * k, c2 * k});
                rows += (c2 - c1) * k;
                ++steps;
            }
        }
        anchors.resize(n);
        for (U q = 0; q < n; ++q) anchors[q] = uint32_t(q + base);
        steps += n;
        if (getenv("CROSS_DEBUG")) {
            for (auto& b : out)
                std::fprintf(stderr, "DBG block q=%llu lo=%llu hi=%llu\n",
                             (unsigned long long)b.q, (unsigned long long)b.lo,
                             (unsigned long long)b.hi);
        }
        return;
    }
    std::array<U, 256> freq{}, C{};
    for (U i = 0; i < n; ++i) ++freq[bwt[i]];
    U acc = 0; for (unsigned c = 0; c < 256; ++c) { C[c] = acc; acc += freq[c]; }
    RankIndex rank; rank.build(bwt, n);
    if (!n) return;
    U q = n - 1;
    U lo = C[text[q]], hi = lo + freq[text[q]];
    for (;;) {
        if (hi - lo >= 2) {
            out.push_back({q + base, lo, hi});
            anchors.push_back(uint32_t(q + base));
            rows += hi - lo;
        }
        // The backward step cannot enlarge the interval (rank differences over
        // an interval are bounded by its length), so once fewer than two rows
        // share the suffix, no later suffix can be an anchor.
        if (q == 0 || hi - lo < 2) break;
        uint8_t c = text[q - 1];
        U nlo = C[c] + rank(c, lo), nhi = C[c] + rank(c, hi);
        lo = nlo; hi = nhi; --q; ++steps;
    }
    if (getenv("CROSS_DEBUG"))
        std::fprintf(stderr, "DBG aperiodic walk base=%llu steps=%llu blocks=%zu rows=%llu\n",
                     (unsigned long long)base, (unsigned long long)steps, out.size(),
                     (unsigned long long)rows);
}

// Sort every anchor block's members by merged key, in place. W holds GLOBAL
// positions (left chunk base 0, right chunk base nA); anchor bits are global.
static void repair(std::vector<uint32_t>& W, const std::vector<BlockRec>& blocks,
                   const std::vector<uint32_t>& anchors, U base, U n_local, const Keys& keys) {
    if (blocks.empty()) return;
    std::vector<uint8_t> anchor((base + n_local + 7) / 8, 0);
    auto setbit = [&](U p) { anchor[p >> 3] |= uint8_t(1u << (p & 7)); };
    auto getbit = [&](U p) { return bool(anchor[p >> 3] & (1u << (p & 7))); };
    for (U p : anchors) setbit(p);
    std::vector<uint32_t> sk, an;
    for (auto& b : blocks) {
        sk.clear(); an.clear();
        for (U r = b.lo; r < b.hi; ++r) {
            U p = W[r];
            (getbit(p) ? an : sk).push_back(uint32_t(p));
        }
        if (an.empty()) continue;
        std::sort(an.begin(), an.end(), [&](uint32_t x, uint32_t y) { return keys.cmp(x, y) < 0; });
        U i = 0, j = 0, r = b.lo;
        while (r < b.hi) {
            bool take_sk = j >= an.size() || (i < sk.size() && keys.cmp(sk[i], an[j]) < 0);
            W[r++] = take_sk ? sk[i++] : an[j++];
        }
    }
}

// ---------------------------------------------------------------- core merge
struct CoreStats { U anchorsA = 0, anchorsB = 0, rowsA = 0, rowsB = 0, stepsA = 0, stepsB = 0; };

// posA/posB are consumed (B is offset by nA in place). texts are consumed.
static void core_merge(std::vector<uint8_t>& textA, std::vector<uint32_t>& posA,
                       std::vector<uint8_t>& textB, std::vector<uint32_t>& posB,
                       U periodA, U periodB, bool dollar, U threads, Budget& bud, CoreStats& st,
                       std::vector<uint8_t>& M2, std::vector<U>& H, std::vector<uint32_t>& WM) {
    double last = now_sec();
    U nA = textA.size(), nB = textB.size(), n = nA + nB;
    M2.assign(2 * n, 0);
    std::memcpy(M2.data(), textA.data(), nA);
    std::memcpy(M2.data() + nA, textB.data(), nB);
    std::memcpy(M2.data() + n, M2.data(), n);
    phase("m2-build", last);
    H.assign(2 * n + 1, 0);
    build_hash(M2.data(), 2 * n, H.data(), threads);
    phase("hash-build", last);
    Keys keys; keys.init(M2.data(), H.data(), n, dollar, &bud);
    // Right side becomes global.
    for (U i = 0; i < nB; ++i) posB[i] = uint32_t(posB[i] + nA);
    // Anchor blocks per side, then repair.
    std::vector<BlockRec> blocks;
    std::vector<uint32_t> anchors;
    std::vector<uint8_t> bwt(nA);
    for (U i = 0; i < nA; ++i) bwt[i] = textA[(posA[i] + nA - 1) % nA];
    anchor_blocks(textA, bwt, posA, nA, 0, periodA, blocks, anchors, st.stepsA, st.rowsA);
    st.anchorsA = anchors.size();
    repair(posA, blocks, anchors, 0, nA, keys);
    bwt.assign(nB, 0); blocks.clear(); anchors.clear();
    for (U i = 0; i < nB; ++i) bwt[i] = textB[(posB[i] - nA + nB - 1) % nB];
    anchor_blocks(textB, bwt, posB, nB, nA, periodB, blocks, anchors, st.stepsB, st.rowsB);
    st.anchorsB = anchors.size();
    repair(posB, blocks, anchors, nA, nB, keys);
    phase("anchor+repair", last);
    textA.clear(); textA.shrink_to_fit();
    textB.clear(); textB.shrink_to_fit();
    bwt.clear(); bwt.shrink_to_fit();
    // Linear cross merge under the total order.
    WM.assign(n, 0);
    U i = 0, j = 0, k = 0;
    while (i < nA && j < nB)
        WM[k++] = keys.cmp(posA[i], posB[j]) < 0 ? posA[i++] : posB[j++];
    while (i < nA) WM[k++] = posA[i++];
    while (j < nB) WM[k++] = posB[j++];
    require(k == n, "merge coverage");
    phase("cross-merge", last);
}

// ---------------------------------------------------------------- emitters
// Final four files: rows = padding rotations n..n+9 then the $-order of M.
// Row char/sample: pad row t (0..9): sample n+t, char (t==0 ? M[n-1] : 0x02);
// suffix row p: sample p, char (p==0 ? 0x02 : M[p-1]).
static U emit_four(const std::string& prefix, const std::vector<uint8_t>& M2,
                   const std::vector<uint32_t>& WM, U n) {
    double last = now_sec();
    U total = n + 10;
    auto row_char_sample = [&](U t, uint8_t& c, U& sample) {
        if (t < 10) { sample = n + t; c = t == 0 ? M2[n - 1] : uint8_t(2); }
        else { U p = WM[t - 10]; sample = p; c = p == 0 ? uint8_t(2) : M2[p - 1]; }
    };
    std::array<U, 256> counts{}, rc{};
    U final_runs = 0;
    {
        uint8_t prev = 0xff; bool any = false;
        for (U t = 0; t < total; ++t) {
            uint8_t c; U s; row_char_sample(t, c, s);
            ++counts[c];
            if (!any || c != prev) { ++final_runs; ++rc[c]; prev = c; any = true; }
        }
    }
    require(counts[2] == 10, "padding count mismatch");
    phase("emit-count", last);
    {
        OutFile b(prefix + ".rlebwt"), m(prefix + ".rlebwt.meta"),
               h(prefix + ".ssa"), tl(prefix + ".ssa_t");
        m.w64(total); m.w64(final_runs);
        for (U c : counts) m.w64(c);
        for (U c : rc) m.w64(c);
        h.w64(final_runs); tl.w64(final_runs);
        uint8_t cur = 0; U runlen = 0, head = 0, tail = 0; bool any = false;
        auto flush = [&]() {
            if (!any) return;
            U left = runlen;
            while (left) {
                U take = std::min<U>(left, 0x7fffff); left -= take;
                b.w32(uint32_t(cur) | uint32_t(take << 8) | (left ? 0x80000000u : 0u));
            }
            h.w64(head); tl.w64(tail);
        };
        for (U t = 0; t < total; ++t) {
            uint8_t c; U s; row_char_sample(t, c, s);
            if (!any || c != cur) { flush(); cur = c; runlen = 0; head = s; any = true; }
            ++runlen; tail = s;
        }
        flush();
    }
    phase("emit-write", last);
    return final_runs;
}

// Intermediate: merged cyclic order as an SXCR chunk + validated sidecar.
static U emit_sxcr(const std::string& path, const std::vector<uint8_t>& M2,
                   const std::vector<uint32_t>& WM, U n, U offset) {
    double last = now_sec();
    U runs = 0;
    for (U t = 0; t < n; ++t) {
        uint8_t c = M2[WM[t] + n - 1];
        if (t == 0 || c != M2[WM[t - 1] + n - 1]) ++runs;
    }
    phase("sxcr-count", last);
    {
        OutFile f(path);
        char magic[4]{'S','X','C','R'}; f.raw(magic, 4);
        f.w32(1); f.w64(offset); f.w64(n); f.w64(runs);
        uint8_t cur = 0; U runlen = 0, head = 0, tail = 0; bool any = false;
        auto flush = [&]() {
            if (!any) return;
            f.w8(cur); f.w32(uint32_t(runlen)); f.w64(head); f.w64(tail);
        };
        for (U t = 0; t < n; ++t) {
            U p = WM[t]; uint8_t c = M2[p + n - 1];
            if (!any || c != cur) { flush(); cur = c; runlen = 0; head = p; any = true; }
            ++runlen; tail = p;
        }
        flush();
    }
    phase("sxcr-write", last);
    {
        OutFile f(path + ".sxs");
        char magic[4]{'S','X','S','1'}; f.raw(magic, 4);
        f.w64(offset); f.w64(n);
        f.raw(M2.data(), n);
        f.raw(WM.data(), 4 * n);
    }
    phase("sidecar-write", last);
    return runs;
}

// ---------------------------------------------------------------- pair merge
static U merge_pair_files(const std::string& leftPath, const std::string& rightPath,
                          bool dollar, const std::string& out, U threads) {
    if (dollar)
        for (const char* ext : {".rlebwt", ".rlebwt.meta", ".ssa", ".ssa_t"})
            require(!fs::exists(out + ext), "output exists; refusing to clobber");
    else
        require(!fs::exists(out) && !fs::exists(out + ".sxs"), "output exists; refusing to clobber");
    double t0 = now_sec();
    Budget bud; CoreStats st;
    Side A, B;
    load_side(leftPath, threads, A);
    load_side(rightPath, threads, B);
    require(A.offset + A.n == B.offset, "pair chunks do not tile");
    std::vector<uint8_t> M2; std::vector<U> H; std::vector<uint32_t> WM;
    core_merge(A.text, A.pos, B.text, B.pos, A.period, B.period, dollar, threads,
               bud, st, M2, H, WM);
    U n = A.n + B.n;
    H.clear(); H.shrink_to_fit();
    U out_runs = dollar ? emit_four(out, M2, WM, n)
                        : emit_sxcr(out, M2, WM, n, A.offset);
    double total = now_sec() - t0;
    std::printf("CROSS_PAIR mode=%s left=%s right=%s nA=%llu nB=%llu out=%s out_runs=%llu "
                "wall_total=%.3f comparisons=%llu symbols_compared=%llu probes=%llu max_lce=%llu "
                "fast_decided=%llu probe_decided=%llu cap_decided=%llu tie_decided=%llu "
                "lce_over_10k=%llu lce_over_100k=%llu lce_over_1m=%llu "
                "anchors_A=%llu anchors_B=%llu walk_steps_A=%llu walk_steps_B=%llu "
                "block_rows_A=%llu block_rows_B=%llu peak_rss_kib=%llu\n",
                dollar ? "dollar" : "cyclic", leftPath.c_str(), rightPath.c_str(),
                (unsigned long long)A.n, (unsigned long long)B.n, out.c_str(),
                (unsigned long long)out_runs, total,
                (unsigned long long)bud.comparisons, (unsigned long long)bud.symbols,
                (unsigned long long)bud.probes, (unsigned long long)bud.max_lce,
                (unsigned long long)bud.fast_decided, (unsigned long long)bud.probe_decided,
                (unsigned long long)bud.cap_decided, (unsigned long long)bud.tie_decided,
                (unsigned long long)bud.lce_over_10k, (unsigned long long)bud.lce_over_100k,
                (unsigned long long)bud.lce_over_1m,
                (unsigned long long)st.anchorsA, (unsigned long long)st.anchorsB,
                (unsigned long long)st.stepsA, (unsigned long long)st.stepsB,
                (unsigned long long)st.rowsA, (unsigned long long)st.rowsB,
                (unsigned long long)peak_rss_kib());
    std::fflush(stdout);
    return out_runs;
}

// Single-chunk finalize: cyclic order -> $ order, then the four files.
static U finalize_file(const std::string& path, const std::string& prefix, U threads) {
    if (1) for (const char* ext : {".rlebwt", ".rlebwt.meta", ".ssa", ".ssa_t"})
        require(!fs::exists(prefix + ext), "output exists; refusing to clobber");
    double t0 = now_sec(), last = t0;
    Budget bud; CoreStats st;
    Side A;
    load_side(path, threads, A);
    phase("load", last);
    std::vector<uint8_t> M2(2 * A.n);
    std::memcpy(M2.data(), A.text.data(), A.n);
    std::memcpy(M2.data() + A.n, A.text.data(), A.n);
    std::vector<U> H(2 * A.n + 1);
    build_hash(M2.data(), 2 * A.n, H.data(), threads);
    phase("hash-build", last);
    Keys keys; keys.init(M2.data(), H.data(), A.n, true, &bud);
    std::vector<uint8_t> bwt(A.n);
    for (U i = 0; i < A.n; ++i) bwt[i] = A.text[(A.pos[i] + A.n - 1) % A.n];
    std::vector<BlockRec> blocks;
    std::vector<uint32_t> anchors;
    anchor_blocks(A.text, bwt, A.pos, A.n, 0, A.period, blocks, anchors, st.stepsA, st.rowsA);
    st.anchorsA = anchors.size();
    repair(A.pos, blocks, anchors, 0, A.n, keys);
    phase("anchor+repair", last);
    std::vector<uint32_t> WM(A.pos.begin(), A.pos.end());
    U out_runs = emit_four(prefix, M2, WM, A.n);
    double total = now_sec() - t0;
    std::printf("CROSS_PAIR mode=dollar-single left=%s right=- nA=%llu nB=0 out=%s out_runs=%llu "
                "wall_total=%.3f comparisons=%llu symbols_compared=%llu probes=%llu max_lce=%llu "
                "fast_decided=%llu probe_decided=%llu cap_decided=%llu tie_decided=%llu "
                "lce_over_10k=%llu lce_over_100k=%llu lce_over_1m=%llu "
                "anchors_A=%llu anchors_B=0 walk_steps_A=%llu walk_steps_B=0 "
                "block_rows_A=%llu block_rows_B=0 peak_rss_kib=%llu\n",
                path.c_str(), (unsigned long long)A.n, prefix.c_str(), (unsigned long long)out_runs,
                total, (unsigned long long)bud.comparisons, (unsigned long long)bud.symbols,
                (unsigned long long)bud.probes, (unsigned long long)bud.max_lce,
                (unsigned long long)bud.fast_decided, (unsigned long long)bud.probe_decided,
                (unsigned long long)bud.cap_decided, (unsigned long long)bud.tie_decided,
                (unsigned long long)bud.lce_over_10k, (unsigned long long)bud.lce_over_100k,
                (unsigned long long)bud.lce_over_1m, (unsigned long long)st.anchorsA,
                (unsigned long long)st.stepsA, (unsigned long long)st.rowsA,
                (unsigned long long)peak_rss_kib());
    std::fflush(stdout);
    return out_runs;
}

// ---------------------------------------------------------------- selftest
static std::vector<uint32_t> brute_cyclic(const std::string& T) {
    U n = T.size();
    std::vector<std::pair<std::string, uint32_t>> v;
    v.reserve(n);
    for (U p = 0; p < n; ++p) v.push_back({T.substr(p) + T.substr(0, p), uint32_t(p)});
    std::sort(v.begin(), v.end());
    std::vector<uint32_t> o(n);
    for (U i = 0; i < n; ++i) o[i] = v[i].second;
    return o;
}
static std::vector<uint32_t> brute_dollar(const std::string& T) {
    U n = T.size();
    std::vector<std::pair<std::string, uint32_t>> v;
    v.reserve(n);
    for (U p = 0; p < n; ++p)
        v.push_back({T.substr(p) + std::string(p, '\x00'), uint32_t(p)});
    std::sort(v.begin(), v.end());
    std::vector<uint32_t> o(n);
    for (U i = 0; i < n; ++i) o[i] = v[i].second;
    return o;
}

static int selftest(U cases, uint64_t seed, U threads) {
    uint64_t s = seed ? seed : 1;
    auto next = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    const std::vector<std::string> alphabets = {
        {'\x06'}, {'\x06', '\x07'}, {'\x06', '\x07', '\x08'},
        {'\x06', '\x07', '\x08', '\x1e'}, {'\x06', '\x07', '\x08', '\x09', '\x0a', '\x1e'},
    };
    for (U cs = 0; cs < cases; ++cs) {
        auto gen = [&](U n, bool periodic) {
            const std::string& ab = alphabets[next() % alphabets.size()];
            std::string t;
            if (periodic) {
                U per = 1 + next() % 3;
                std::string pat;
                for (U i = 0; i < per; ++i) pat += ab[next() % ab.size()];
                for (U i = 0; i < n; ++i) t += pat[i % per];
            } else {
                for (U i = 0; i < n; ++i) t += ab[next() % ab.size()];
                // Force repeating suffixes so anchor repairs are exercised.
                if (n > 8 && (next() & 1)) {
                    U w = 1 + next() % std::min<U>(n / 2, 6);
                    for (U i = 0; i < w && i + w < n; ++i) t[i] = t[n - w + i];
                }
            }
            return t;
        };
        std::string A = gen(1 + next() % 60, bool(next() & 1));
        std::string B = gen(1 + next() % 60, bool(next() & 1));
        std::string M = A + B;
        std::vector<uint32_t> posA = brute_cyclic(A), posB = brute_cyclic(B);
        for (int mode = 0; mode < 2; ++mode) {
            bool dollar = mode == 1;
            std::vector<uint8_t> tA(A.begin(), A.end()), tB(B.begin(), B.end());
            std::vector<uint32_t> pA = posA, pB = posB;
            Budget bud; CoreStats st;
            std::vector<uint8_t> M2; std::vector<U> H; std::vector<uint32_t> WM;
            core_merge(tA, pA, tB, pB, min_period(tA, tA.size()), min_period(tB, tB.size()),
                       dollar, threads, bud, st, M2, H, WM);
            std::vector<uint32_t> want = dollar ? brute_dollar(M) : brute_cyclic(M);
            if (WM != want) {
                std::fprintf(stderr, "SELFTEST_FAIL case=%llu mode=%s nA=%zu nB=%zu\n",
                             (unsigned long long)cs, dollar ? "dollar" : "cyclic",
                             A.size(), B.size());
                auto hex = [](const std::string& s) {
                    std::string o;
                    char buf[8];
                    for (unsigned char ch : s) { std::snprintf(buf, sizeof buf, "%02x", ch); o += buf; }
                    return o;
                };
                std::fprintf(stderr, "A_hex=%s B_hex=%s\n", hex(A).c_str(), hex(B).c_str());
                std::fprintf(stderr, "got :");
                for (auto p : WM) std::fprintf(stderr, " %u", p);
                std::fprintf(stderr, "\nwant:");
                for (auto p : want) std::fprintf(stderr, " %u", p);
                std::fprintf(stderr, "\n");
                return 1;
            }
        }
        if ((cs + 1) % 200 == 0) {
            std::fprintf(stderr, "SELFTEST_PROGRESS done=%llu/%llu\n",
                         (unsigned long long)(cs + 1), (unsigned long long)cases);
            std::fflush(stderr);
        }
    }
    std::printf("SELFTEST_PASS count=%llu seed=%llu\n",
                (unsigned long long)cases, (unsigned long long)seed);
    return 0;
}

// ---------------------------------------------------------------- driver
int main_impl(int argc, char** argv) {
    G_T0 = now_sec();
    U threads = 8;
    std::vector<std::string> args(argv + 1, argv + argc);
    auto flag = [&](const char* name) {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == name) return args[i + 1];
        return std::string();
    };
    auto has = [&](const char* name) {
        for (auto& a : args) if (a == name) return true;
        return false;
    };
    if (!flag("--threads").empty()) threads = std::stoull(flag("--threads"));
    if (has("--selftest")) {
        U cases = flag("--selftest").empty() ? 2000 : std::stoull(flag("--selftest"));
        uint64_t seed = flag("--seed").empty() ? 20261002 : std::stoull(flag("--seed"));
        return selftest(cases, seed, threads);
    }
    if (has("--pair")) {
        std::string L = flag("--pair"), R = flag("--right");
        require(!L.empty() && !R.empty(), "--pair needs LEFT and --right RIGHT");
        if (has("--sxcr")) {
            require(!fs::exists(flag("--sxcr")) && !fs::exists(flag("--sxcr") + ".sxs"),
                    "output exists; refusing to clobber");
            merge_pair_files(L, R, false, flag("--sxcr"), threads);
        } else {
            require(has("--out-prefix"), "--pair needs --sxcr OUT or --out-prefix PREFIX");
            merge_pair_files(L, R, true, flag("--out-prefix"), threads);
        }
        return 0;
    }
    if (has("--finalize")) {
        require(has("--out-prefix"), "--finalize needs --out-prefix PREFIX");
        finalize_file(flag("--finalize"), flag("--out-prefix"), threads);
        return 0;
    }
    // Driver: CHUNK_DIR COUNT TOTAL_BYTES OUT_PREFIX [--work DIR] [--serial]
    std::string dir, prefix, work;
    U count = 0, total = 0;
    bool serial = has("--serial");
    auto valued = [&](const std::string& a) {
        return a == "--threads" || a == "--work" || a == "--seed" || a == "--pair" ||
               a == "--right" || a == "--sxcr" || a == "--out-prefix" || a == "--finalize" ||
               a == "--selftest";
    };
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i].rfind("--", 0) == 0) { if (valued(args[i])) ++i; continue; }
        if (dir.empty()) dir = args[i];
        else if (!count) count = std::stoull(args[i]);
        else if (!total) total = std::stoull(args[i]);
        else if (prefix.empty()) prefix = args[i];
        else fail("unexpected extra positional argument");
    }
    require(!dir.empty() && count && total && !prefix.empty(),
            "usage: cross_lcp_merge CHUNK_DIR COUNT TOTAL_BYTES OUT_PREFIX [--work DIR] [--serial] [--threads N]\n"
            "       cross_lcp_merge --pair LEFT --right RIGHT (--sxcr OUT | --out-prefix PREFIX)\n"
            "       cross_lcp_merge --finalize ONE --out-prefix PREFIX\n"
            "       cross_lcp_merge --selftest CASES [--seed N]");
    struct Part { std::string file; U offset, n; };
    std::vector<Part> parts;
    for (U i = 0; i < count; ++i) {
        Part p{dir + "/chunk-" + std::to_string(i) + ".crle", 0, 0};
        U runs = 0; sxcr_header(p.file, p.offset, p.n, runs);
        if (i) require(parts[i - 1].offset + parts[i - 1].n == p.offset, "chunks do not tile");
        parts.push_back(p);
    }
    require(parts[0].offset == 0 && parts.back().offset + parts.back().n == total,
            "chunk coverage does not match total");
    work = flag("--work");
    if (work.empty()) work = (fs::path(prefix).parent_path() / "cross-lcp-work").string();
    fs::create_directories(work);
    if (count == 1) {
        finalize_file(parts[0].file, prefix, threads);
        return 0;
    }
    if (serial) {
        Part acc = parts[0];
        for (U i = 1; i < count; ++i) {
            bool final = i + 1 == count;
            std::string out = final ? prefix : work + "/serial-" + std::to_string(i) + ".crle";
            merge_pair_files(acc.file, parts[i].file, final, out, threads);
            acc = {out, acc.offset, acc.n + parts[i].n};
        }
        return 0;
    }
    U level = 0;
    while (parts.size() > 1) {
        std::vector<Part> next;
        for (size_t j = 0; j < parts.size(); j += 2) {
            if (j + 1 == parts.size()) { next.push_back(parts[j]); continue; }
            bool final = parts.size() == 2 && j == 0;
            std::string out = final ? prefix
                : work + "/L" + std::to_string(level) + "-" + std::to_string(j / 2) + ".crle";
            merge_pair_files(parts[j].file, parts[j + 1].file, final, out, threads);
            next.push_back({out, parts[j].offset, parts[j].n + parts[j + 1].n});
        }
        parts = std::move(next);
        ++level;
    }
    return 0;
}

int main(int argc, char** argv) {
    try { return main_impl(argc, argv); }
    catch (const std::exception& e) { std::fprintf(stderr, "CROSS_LCP_FATAL %s\n", e.what()); return 2; }
}
