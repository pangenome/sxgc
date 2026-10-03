// VLB state-valid anchor sampling prototype (vlb-eval lane, Phase 2).
//
// Question (from arXiv:2602.17201, Sec 5): the lite container's member-11
// anchors (one tail-SA per 1024th run) serve "locate-from-anywhere". VLB's
// insight is that rank/succ correctness is only needed along states that
// backward search can reach. Transplanted to our anchor walk, locate only
// ever STARTS at rows of backward-search intervals, so an anchor set that
// bounds the walk from those rows may need fewer samples.
//
// IMPORTANT honest framing (see paper-notes.md): the set of interval states
// for ALL occurring patterns is the set of ALL rows (every row is the
// singleton interval of its own context). The non-trivial restriction is a
// workload assumption: MEMs of minimal length L, implemented exactly as
// V_L = rows sharing an L-char context with a SA-adjacent suffix
// ( = rows inside intervals of size >= 2 of L-mers, = rows whose L-mer
// occurs >= 2 times). Singleton-interval matches (occurring once) are NOT
// in V_L; the benchmark measures them separately as the uncovered hazard.
//
// Reads the retained SXI2 artifacts READ-ONLY. Nothing is written to the
// containers. The heavy passes are offline MEASUREMENT walks at yeast /
// pile-frag scale in the lane tradition (the skiplist lane's sweep walks
// the full LF cycle); they are NOT proposed build steps. A production
// placement would need an r-space method (open problem, journaled).
//
// Anchor semantics (member 11, codec 111): one raw u64 tail-SA for every
// 1024th run; a located walk terminates on the TAIL row of an anchored
// run: SA[origin] = (anchor_value + steps) mod n (LF decrements SA by 1).
//
// Modes:
//   info    container summary
//   full    [--Ls 20,39,100] [--Ds 1024,...] [--target-count N] [--queries K]
//           [--rowcap N] [--threads T<=31] [--text-cache F] [--src-rl F]
//           [--src-fa F] [--out-anchors F]
//
// 1. decode runs (codec 101), idx + per-symbol rank checkpoints
// 2. text reconstruction by inverse BWT (anchor-segment walks), optional
//    byte-exact source compare (one forward pass, THE-LAW style)
// 3. SA[row] by parallel anchor-segment walks; gates: all member-11 values
//    == SA[tailrow]; sum(SA) == n(n-1)/2
// 4. per L: lcpge bitmap (LCP(SA[i], SA[i+1]) >= L, cyclic)
// 5. one parallel orbit walk: gaps between consecutive run tails in
//    LF-orbit order + per-L valid aggregates + member-11 uniform walk
//    stats (cross-check against the banked skiplist-lane numbers)
// 6. per L, per D: greedy state-valid placement (min count, walk from V_L
//    bounded by D) + all-rows variant (pure orbit-aware placement) +
//    bisection of D* at the member-11 anchor count
// 7. benchmark per L: qlen=L patterns from the text, backward search,
//    locate every sampled interval row with (a) member-11 and (b) the
//    state-valid set at D*; every position gated against SA[startrow]
//    and the pattern context; latency measured.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static void die(const char* msg) { fprintf(stderr, "FATAL: %s\n", msg); exit(1); }

struct Map {
    uint8_t* p = nullptr; size_t len = 0;
    void open_readonly(const char* path) {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) die("open container");
        struct stat st; if (fstat(fd, &st)) die("stat container");
        len = st.st_size;
        p = (uint8_t*)mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) die("mmap container");
        close(fd);
    }
};

struct Member { uint32_t id, codec; uint64_t off, bytes, count; };

struct Container {
    Map map;
    uint64_t n = 0, k = 0, r = 0;
    uint32_t memcount = 0, flags = 0;
    std::vector<Member> members;
    const Member* member(uint32_t id) const {
        for (auto& m : members) if (m.id == id) return &m;
        return nullptr;
    }
    void load(const char* path) {
        map.open_readonly(path);
        const uint8_t* b = map.p;
        if (memcmp(b, "SXI2", 4)) die("magic");
        uint32_t ver; memcpy(&ver, b + 4, 4);
        if (ver < 3 || ver > 6) fprintf(stderr, "note: container version %u\n", ver);
        memcpy(&n, b + 8, 8); memcpy(&k, b + 16, 8); memcpy(&r, b + 24, 8);
        memcpy(&memcount, b + 32, 4); uint32_t dirbytes; memcpy(&dirbytes, b + 36, 4);
        memcpy(&flags, b + 48, 4);
        const uint8_t* d = b + 64;
        for (uint32_t i = 0; i < memcount; i++, d += 40) {
            Member m; memcpy(&m.id, d, 4); memcpy(&m.codec, d + 4, 4);
            memcpy(&m.off, d + 8, 8); memcpy(&m.bytes, d + 16, 8); memcpy(&m.count, d + 24, 8);
            members.push_back(m);
        }
        if (dirbytes != 64 + 40ull * memcount) die("dir bytes");
    }
};

static uint64_t rd_u64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint8_t bit_at(const uint8_t* d, uint64_t i) { return (d[i >> 3] >> (i & 7)) & 1; }

struct Runs {
    std::vector<uint32_t> len;
    std::vector<uint8_t> sym;
    std::vector<uint64_t> C;
};

static void load_runs(const Container& c, Runs& out) {
    const Member* m = c.member(1);
    if (!m) die("member 1");
    if (m->codec == 1) {
        const uint8_t* p = c.map.p + m->off;
        out.C.resize(256);
        for (int i = 0; i < 256; i++) out.C[i] = rd_u64(p + 8ull * i);
        const uint8_t* syms = p + 256 * 8;
        const uint8_t* lens = syms + c.r;
        out.sym.resize(c.r); out.len.resize(c.r);
        memcpy(out.sym.data(), syms, c.r);
        for (uint64_t i = 0; i < c.r; i++) { uint32_t v; memcpy(&v, lens + 4ull * i, 4); out.len[i] = v; }
        return;
    }
    if (m->codec != 101) die("unsupported member-1 codec (need 1 or 101)");
    const uint8_t* p = c.map.p + m->off;
    out.C.resize(256);
    for (int i = 0; i < 256; i++) out.C[i] = rd_u64(p + 8ull * i);
    uint8_t hlen[256]; memcpy(hlen, p + 256 * 8, 256);
    uint64_t hbits = rd_u64(p + 256 * 8 + 256);
    uint64_t lbits = rd_u64(p + 256 * 8 + 264);
    uint64_t hbytes = (hbits + 7) / 8, lbytes = (lbits + 7) / 8;
    if (m->bytes != 2320 + hbytes + lbytes || hbits < c.r || lbits < c.r) die("run member size");
    const uint8_t* hd = p + 2320;
    const uint8_t* ld = hd + hbytes;
    int order[256]; int nord = 0;
    for (int i = 0; i < 256; i++) if (hlen[i]) order[nord++] = i;
    std::stable_sort(order, order + nord, [&](int a, int b) {
        return hlen[a] < hlen[b] || (hlen[a] == hlen[b] && a < b); });
    if (!nord || hlen[order[nord - 1]] > 63) die("huffman lengths");
    uint64_t codes[256], rcodes[256];
    uint64_t code = 0; int prev = 0;
    for (int i = 0; i < nord; i++) {
        int s = order[i], w = hlen[s];
        code <<= (w - prev); prev = w;
        if (code >= (1ull << w)) die("huffman code overflow");
        codes[s] = code; code += 1;
        uint64_t rv = 0; for (int b = 0; b < w; b++) rv |= ((codes[s] >> b) & 1) << (w - 1 - b);
        rcodes[s] = rv;
    }
    out.sym.resize(c.r); out.len.resize(c.r);
    uint64_t hp = 0, lp = 0, sum = 0;
    uint64_t counts[256]; memset(counts, 0, sizeof counts);
    for (uint64_t i = 0; i < c.r; i++) {
        const uint8_t* q = hd + (hp >> 3);
        uint64_t buf = 0; memcpy(&buf, q, 8);
        uint64_t acc = (buf >> (hp & 7));
        int sym = -1, nb = 0;
        for (int oi = 0; oi < nord; oi++) {
            int s = order[oi]; int ww = hlen[s];
            if ((acc & ((1ull << ww) - 1)) == rcodes[s]) { sym = s; nb = ww; break; }
        }
        if (sym < 0) die("huffman decode");
        hp += nb;
        uint32_t zeros = 0;
        while (lp < lbits && !bit_at(ld, lp)) { zeros++; lp++; if (zeros >= 32) die("gamma overflow"); }
        uint64_t value = 0;
        for (uint32_t j = 0; j <= zeros; j++) { value = (value << 1) | bit_at(ld, lp); lp++; }
        if (value == 0 || value > 0xffffffffull || value > c.n - sum) die("run length");
        sum += value; counts[sym] += value;
        out.sym[i] = (uint8_t)sym; out.len[i] = (uint32_t)value;
    }
    if (hp != hbits || lp != lbits || sum != c.n) die("run stream counts");
    uint64_t acc2 = 0;
    for (int i = 0; i < 256; i++) { if (out.C[i] != acc2) die("C table mismatch"); acc2 += counts[i]; }
}

struct Idx {
    const Container* c = nullptr;
    Runs runs;
    std::vector<uint32_t> runstart;  // r+1
    std::vector<uint32_t> lfstart;   // r
    std::vector<uint32_t> ckpt;      // per 1024 rows: run containing that row
    std::vector<uint64_t> anchor_val;// member 11 tail SA per anchored run
    std::vector<uint32_t> symcnt;    // per 1024-row boundary x compact symbol
    int nsym = 0;
    uint8_t symmap[256];

    void build(const Container& cont) {
        c = &cont;
        load_runs(cont, runs);
        uint64_t r = cont.r, n = cont.n;
        runstart.resize(r + 1);
        lfstart.resize(r);
        uint64_t rs = 0;
        uint64_t cnt[256]; memset(cnt, 0, sizeof cnt);
        for (uint64_t i = 0; i < r; i++) {
            runstart[i] = (uint32_t)rs;
            lfstart[i] = (uint32_t)(runs.C[runs.sym[i]] + cnt[runs.sym[i]]);
            cnt[runs.sym[i]] += runs.len[i];
            rs += runs.len[i];
        }
        runstart[r] = (uint32_t)n;
        if (rs != n) die("runstart sum");
        uint64_t nc = (n + 1023) / 1024;
        ckpt.resize(nc);
        uint64_t ci = 0;
        for (uint64_t i = 0; i < r && ci < nc; i++)
            while (ci < nc && (ci * 1024) < runstart[i + 1]) ckpt[ci++] = (uint32_t)i;
        if (ci != nc) die("checkpoint build");
        const Member* m = cont.member(11);
        if (!m) die("member 11 missing");
        const uint8_t* p = cont.map.p + m->off;
        if (m->codec != 111) die("member 11 codec");
        if (rd_u64(p) != 10) die("stride exponent != 10");
        uint64_t cnt11 = rd_u64(p + 8);
        if (cnt11 != m->count || m->bytes != 16 + 8 * cnt11) die("member 11 size");
        if (cnt11 != (r + 1023) / 1024) die("member 11 count vs stride 1024");
        anchor_val.resize(cnt11);
        for (uint64_t i = 0; i < cnt11; i++) anchor_val[i] = rd_u64(p + 16 + 8ull * i);
        // per-symbol rank checkpoints at 1024-row boundaries
        memset(symmap, 0xff, 256);
        for (int s = 0; s < 256; s++) if (cnt[s]) symmap[s] = (uint8_t)nsym++;
        symcnt.assign(nc * nsym, 0);
        uint64_t acc[256]; memset(acc, 0, sizeof acc);
        uint64_t b = 0;
        for (uint64_t i = 0; i < r; i++) {
            while (b < nc && b * 1024 < runstart[i + 1]) {
                uint64_t within = (b * 1024 > runstart[i]) ? b * 1024 - runstart[i] : 0;
                for (int s = 0; s < 256; s++) if (cnt[s]) {
                    uint64_t v = acc[s];
                    if (runs.sym[i] == s) v += within;
                    symcnt[b * nsym + symmap[s]] = (uint32_t)v;
                }
                b++;
            }
            acc[runs.sym[i]] += runs.len[i];
        }
        if (b != nc) die("rank ckpt build");
    }

    inline uint64_t run_of(uint64_t row) const {
        uint64_t ci = row >> 10;
        uint64_t rr = ckpt[ci];
        while (runstart[rr + 1] <= row) rr++;
        return rr;
    }
    inline bool at_anchor_tail(uint64_t run, uint64_t off) const {
        return run % 1024 == 0 && off + 1 == runs.len[run];
    }
    inline void step(uint64_t& run, uint64_t& off) const {
        uint64_t row = lfstart[run] + off;
        uint64_t rr = run_of(row);
        run = rr; off = row - runstart[rr];
    }
    // count of symbol sorig in rows [0, i)
    inline uint64_t rank_byte(uint8_t sorig, uint64_t i) const {
        if (!i || symmap[sorig] == 0xff) return 0;
        uint64_t ci = i >> 10;         // boundary at ci*1024 <= i, within same 1024 block
        uint64_t bpos = ci * 1024;
        if (i == bpos) return symcnt[ci * nsym + symmap[sorig]];
        uint64_t v = symcnt[ci * nsym + symmap[sorig]];
        uint64_t rr = ckpt[ci];
        while (runstart[rr] < i) {
            uint64_t lo = runstart[rr] > bpos ? runstart[rr] : bpos;
            uint64_t hi = runstart[rr + 1] < i ? runstart[rr + 1] : i;
            if (hi > lo && runs.sym[rr] == sorig) v += hi - lo;
            rr++;
        }
        return v;
    }
};

// ---------- text ----------
static bool load_text_cache(const char* path, uint64_t n, std::vector<uint8_t>& txt) {
    if (!path || !*path) return false;
    struct stat st;
    if (::stat(path, &st) != 0 || (uint64_t)st.st_size != n) return false;
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    txt.resize(n);
    uint64_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, txt.data() + got, std::min<uint64_t>(1 << 30, n - got));
        if (r <= 0) { close(fd); return false; }
        got += r;
    }
    close(fd);
    return true;
}

static void save_text_cache(const char* path, const std::vector<uint8_t>& txt) {
    if (!path || !*path) return;
    FILE* f = fopen(path, "wb");
    if (!f) die("text cache open");
    if (fwrite(txt.data(), 1, txt.size(), f) != txt.size()) die("text cache write");
    fclose(f);
}

static void reconstruct_text(const Container& c, const Idx& idx, std::vector<uint8_t>& txt, int threads) {
    txt.assign(c.n, 0);
    std::atomic<uint64_t> bad{0};
    auto worker = [&](uint64_t lo, uint64_t hi) {
        for (uint64_t a = lo; a < hi; a++) {
            uint64_t run = a * 1024;
            uint64_t crun = run, coff = idx.runs.len[run] - 1;
            uint64_t v = idx.anchor_val[a];
            uint64_t k = 0;
            while (true) {
                txt[(v + c.n - 1 - k) % c.n] = idx.runs.sym[crun];
                idx.step(crun, coff); k++;
                if (idx.at_anchor_tail(crun, coff)) break;
                if (k > c.n) { bad++; break; }
            }
        }
    };
    std::vector<std::thread> th; uint64_t na = idx.anchor_val.size();
    uint64_t per = (na + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
        uint64_t lo = (uint64_t)t * per, hi = std::min(na, lo + per);
        if (lo < hi) th.emplace_back(worker, lo, hi);
    }
    for (auto& t : th) t.join();
    if (bad) die("reconstruct overflow");
    uint64_t z = 0;
    for (uint64_t i = 0; i < c.n; i++) if (!txt[i]) { z++; if (z > 10) break; }
    if (z) die("reconstruct holes");
}

static bool compare_flat(const char* path, const std::vector<uint8_t>& txt) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    static const uint64_t CH = 1 << 24;
    std::vector<uint8_t> buf(CH);
    uint64_t pos = 0; bool ok = true; size_t got;
    while ((got = fread(buf.data(), 1, CH, f)) > 0) {
        if (pos + got > txt.size() || memcmp(buf.data(), txt.data() + pos, got) != 0) { ok = false; break; }
        pos += got;
    }
    if (ok && pos != txt.size()) ok = false;
    fclose(f);
    return ok;
}

static bool compare_fasta_revrec(const char* path, const std::vector<uint8_t>& txt) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> rec; rec.reserve(1 << 20);
    uint64_t w = 0; bool ok = true, inhdr = false, inrec = false; int c;
    auto flush = [&]() {
        if (rec.empty()) return;
        std::reverse(rec.begin(), rec.end());
        if (w + rec.size() + 1 > txt.size()) { ok = false; return; }
        if (memcmp(rec.data(), txt.data() + w, rec.size()) != 0 || txt[w + rec.size()] != 0x1e) { ok = false; return; }
        w += rec.size() + 1; rec.clear();
    };
    while (ok && (c = fgetc(f)) != EOF) {
        if (c == '>') { if (inrec) flush(); inrec = true; inhdr = true; continue; }
        if (c == '\n') { inhdr = false; continue; }
        if (c == '\r') continue;
        if (!inrec || inhdr) continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        rec.push_back((uint8_t)c);
    }
    if (ok) flush();
    if (ok && w != txt.size()) ok = false;
    fclose(f);
    return ok;
}

// ---------- SA reconstruction ----------
static void build_sa(const Container& c, const Idx& idx, std::vector<uint32_t>& SA, int threads) {
    SA.assign(c.n, 0);
    std::atomic<uint64_t> bad{0};
    auto worker = [&](uint64_t lo, uint64_t hi) {
        for (uint64_t a = lo; a < hi; a++) {
            uint64_t run = a * 1024;
            uint64_t crun = run, coff = idx.runs.len[run] - 1;
            uint64_t v = idx.anchor_val[a];
            uint64_t k = 0;
            SA[idx.runstart[crun] + coff] = (uint32_t)v;
            while (true) {
                idx.step(crun, coff); k++;
                uint64_t row = idx.runstart[crun] + coff;
                SA[row] = (uint32_t)((v + c.n - k) % c.n);
                if (idx.at_anchor_tail(crun, coff)) break;
                if (k > c.n) { bad++; break; }
            }
        }
    };
    std::vector<std::thread> th; uint64_t na = idx.anchor_val.size();
    uint64_t per = (na + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
        uint64_t lo = (uint64_t)t * per, hi = std::min(na, lo + per);
        if (lo < hi) th.emplace_back(worker, lo, hi);
    }
    for (auto& t : th) t.join();
    if (bad) die("SA reconstruct overflow");
}

struct Bits {
    std::vector<uint8_t> b;
    void init(uint64_t n) { b.assign((n + 7) / 8, 0); }
    inline int get(uint64_t i) const { return (b[i >> 3] >> (i & 7)) & 1; }
    inline void set(uint64_t i) { b[i >> 3] |= (uint8_t)(1 << (i & 7)); }
};

static double now_s() {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

struct Rng { uint64_t s; explicit Rng(uint64_t seed) : s(seed ? seed : 88172645463325252ull) {}
    inline uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    inline uint64_t below(uint64_t m) { return next() % m; }
};

// ---------- phase A ----------
// One parallel walk over member-11 anchor segments (they partition the LF
// orbit; every segment starts and ends at an anchored TAIL, so gaps between
// consecutive run tails never straddle segment boundaries). Output: gaps in
// orbit order + per-L valid-point aggregates per gap + member-11 uniform
// walk stats. Orbit index of a row with SA value v: (A0 - v) mod n, where
// A0 = anchor_val[0]; LF decrements SA by 1 per step.
struct OrbitGaps {
    std::vector<uint32_t> tidx;   // orbit idx of closing tail (last record = n, run 0)
    std::vector<uint32_t> trun;   // run of the closing tail
    std::vector<std::vector<uint32_t>> tfirst; // per L: first valid point in gap (~0 = none)
    std::vector<std::vector<uint32_t>> tcnt;   // per L: valid points in gap
    std::vector<std::vector<uint64_t>> tsum;   // per L: sum of valid orbit idxs in gap
    uint64_t maxseg = 0;
};

struct UniformStats {
    uint64_t cnt = 0, sumdist = 0, maxdist = 0;
};

static void phase_a(const Container& c, const Idx& idx,
                    const std::vector<Bits>& lcpge, const std::vector<uint64_t>& Ls,
                    OrbitGaps& G, std::vector<UniformStats>& uall, std::vector<UniformStats>& uvalid,
                    int threads) {
    uint64_t n = c.n, r = c.r;
    size_t nL = Ls.size();
    G.tidx.resize(r); G.trun.resize(r);
    G.tfirst.assign(nL, std::vector<uint32_t>(r, ~0u));
    G.tcnt.assign(nL, std::vector<uint32_t>(r, 0));
    G.tsum.assign(nL, std::vector<uint64_t>(r, 0));
    std::vector<UniformStats> uallL(threads);
    std::atomic<uint64_t> bad{0};
    double t0 = now_s();
    uint64_t na = idx.anchor_val.size();

    std::vector<std::vector<uint32_t>> th_tidx(threads), th_trun(threads);
    std::vector<std::vector<std::vector<uint32_t>>> th_first(threads, std::vector<std::vector<uint32_t>>(nL));
    std::vector<std::vector<std::vector<uint32_t>>> th_cnt(threads, std::vector<std::vector<uint32_t>>(nL));
    std::vector<std::vector<std::vector<uint64_t>>> th_sum(threads, std::vector<std::vector<uint64_t>>(nL));
    std::vector<uint64_t> th_maxseg(threads, 0);
    std::vector<std::vector<UniformStats>> th_uv(threads, std::vector<UniformStats>(nL));

    std::vector<std::thread> th;
    // CRITICAL: anchor (segment) order by run index is NOT orbit order.
    // The walk from anchored tail a ends at whichever anchored tail comes
    // next ON THE ORBIT, which need not be run 1024(a+1). Sort segments by
    // their start's orbit idx = (A0 - anchor_val[a]) mod n, then split that
    // orbit order across threads: per-thread buffers concatenated in thread
    // order are exactly orbit order.
    std::vector<uint32_t> segOrder(na);
    {
        std::vector<std::pair<uint64_t, uint32_t>> kv(na);
        for (uint64_t a = 0; a < na; a++)
            kv[a] = {(idx.anchor_val[0] + n - idx.anchor_val[a]) % n, (uint32_t)a};
        std::sort(kv.begin(), kv.end());
        for (uint64_t i = 0; i < na; i++) segOrder[i] = kv[i].second;
    }

    auto worker2 = [&](int tid, uint64_t lo, uint64_t hi) {
        auto& ltidx = th_tidx[tid]; auto& ltrun = th_trun[tid];
        auto& ltfirst = th_first[tid]; auto& ltcnt = th_cnt[tid]; auto& ltsum = th_sum[tid];
        uint64_t localMaxSeg = 0;
        std::vector<uint32_t> gfirst(nL, ~0u), gcnt(nL, 0);
        std::vector<uint64_t> gsum(nL, 0);
        for (uint64_t oi = lo; oi < hi; oi++) {
            uint64_t a = segOrder[oi];
            uint64_t run = a * 1024;
            uint64_t crun = run, coff = idx.runs.len[run] - 1;
            uint64_t v = idx.anchor_val[a];
            uint64_t startIdx = (idx.anchor_val[0] + n - v) % n;
            for (size_t li = 0; li < nL; li++) { gfirst[li] = ~0u; gcnt[li] = 0; gsum[li] = 0; }
            std::vector<uint64_t> uvsum(nL, 0), uvcnt(nL, 0);
            uint64_t uacnt = 0, uasum = 0;
            uint64_t k = 0;
            while (true) {
                idx.step(crun, coff); k++;
                uint64_t row = idx.runstart[crun] + coff;
                uint64_t idxp = startIdx + k;
                for (size_t li = 0; li < nL; li++) {
                    if (lcpge[li].get(row ? row - 1 : n - 1) | lcpge[li].get(row)) {
                        if (!gcnt[li]) gfirst[li] = (uint32_t)idxp;
                        gcnt[li]++; gsum[li] += idxp;
                        uvcnt[li]++; uvsum[li] += idxp;
                    }
                }
                uacnt++; uasum += idxp;
                if (coff + 1 == idx.runs.len[crun]) { // tail closes the gap
                    ltidx.push_back((uint32_t)idxp);
                    ltrun.push_back((uint32_t)crun);
                    for (size_t li = 0; li < nL; li++) {
                        ltfirst[li].push_back(gfirst[li]);
                        ltcnt[li].push_back(gcnt[li]);
                        ltsum[li].push_back(gsum[li]);
                        gfirst[li] = ~0u; gcnt[li] = 0; gsum[li] = 0;
                    }
                }
                if (idx.at_anchor_tail(crun, coff)) break;
                if (k > n) { bad++; break; }
            }
            uint64_t delta = k;
            if (delta > localMaxSeg) localMaxSeg = delta;
            uint64_t endIdx = startIdx + delta;
            uallL[tid].cnt += uacnt;
            uallL[tid].sumdist += uacnt * endIdx - uasum;
            if (delta > 1 && (delta - 1) > uallL[tid].maxdist) uallL[tid].maxdist = delta - 1;
            for (size_t li = 0; li < nL; li++) {
                th_uv[tid][li].cnt += uvcnt[li];
                th_uv[tid][li].sumdist += uvcnt[li] * endIdx - uvsum[li];
            }
        }
        th_maxseg[tid] = localMaxSeg;
    };

    uint64_t per = (na + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
        uint64_t lo = (uint64_t)t * per, hi = std::min(na, lo + per);
        if (lo < hi) th.emplace_back(worker2, t, lo, hi);
    }
    for (auto& t : th) t.join();
    if (bad) die("phase A overflow");
    size_t g = 0;
    for (int t = 0; t < threads; t++) {
        for (size_t i = 0; i < th_tidx[t].size(); i++) {
            G.tidx[g] = th_tidx[t][i]; G.trun[g] = th_trun[t][i];
            for (size_t li = 0; li < nL; li++) {
                G.tfirst[li][g] = th_first[t][li][i];
                G.tcnt[li][g] = th_cnt[t][li][i];
                G.tsum[li][g] = th_sum[t][li][i];
            }
            g++;
        }
        if (th_maxseg[t] > G.maxseg) G.maxseg = th_maxseg[t];
        uall[0].cnt += uallL[t].cnt; uall[0].sumdist += uallL[t].sumdist;
        if (uallL[t].maxdist > uall[0].maxdist) uall[0].maxdist = uallL[t].maxdist;
        for (size_t li = 0; li < nL; li++) {
            uvalid[li].cnt += th_uv[t][li].cnt;
            uvalid[li].sumdist += th_uv[t][li].sumdist;
        }
    }
    if (g != r) { fprintf(stderr, "FATAL: gaps=%zu != R=%llu\n", g, (unsigned long long)r); exit(1); }
    if (uall[0].cnt != n) { fprintf(stderr, "FATAL: orbit coverage %llu != n\n", (unsigned long long)uall[0].cnt); exit(1); }
    // orbit-order sanity: tidx strictly increasing, last = n, last run = 0
    for (uint64_t m = 1; m < r; m++)
        if (G.tidx[m] <= G.tidx[m - 1]) { fprintf(stderr, "FATAL: gap order at %llu\n", (unsigned long long)m); exit(1); }
    if (G.tidx[r - 1] != n || G.trun[r - 1] != 0) { fprintf(stderr, "FATAL: wrap record\n"); exit(1); }
    // per-gap valid counts must partition |V_L| exactly
    for (size_t li = 0; li < nL; li++) {
        uint64_t s = 0;
        for (uint64_t m = 0; m < r; m++) {
            s += G.tcnt[li][m];
            if (G.tfirst[li][m] != ~0u && (uint64_t)G.tfirst[li][m] > (uint64_t)G.tidx[m])
                { fprintf(stderr, "FATAL: gap first beyond close\n"); exit(1); }
        }
        if (s != uvalid[li].cnt) { fprintf(stderr, "FATAL: V_L partition %llu != %llu\n",
                                            (unsigned long long)s, (unsigned long long)uvalid[li].cnt); exit(1); }
    }
    // max valid walk under member-11, exactly: segments close at anchored
    // tails (gap with trun % 1024 == 0); dist = endIdx - firstValidInSegment.
    {
        std::vector<uint32_t> segFirst(nL, ~0u);
        std::vector<uint64_t> maxv(nL, 0);
        for (uint64_t gi = 0; gi < r; gi++) {
            for (size_t li = 0; li < nL; li++)
                if (G.tfirst[li][gi] != ~0u && segFirst[li] == ~0u) segFirst[li] = G.tfirst[li][gi];
            if (G.trun[gi] % 1024 == 0) {
                uint64_t endIdx = G.tidx[gi];
                for (size_t li = 0; li < nL; li++) {
                    if (segFirst[li] != ~0u) {
                        uint64_t d = endIdx - segFirst[li];
                        if (d > maxv[li]) maxv[li] = d;
                    }
                    segFirst[li] = ~0u;
                }
            }
        }
        for (size_t li = 0; li < nL; li++) uvalid[li].maxdist = maxv[li];
    }
    fprintf(stderr, "phase A done in %.1fs (max segment %llu)\n", now_s() - t0,
            (unsigned long long)G.maxseg);
}

// ---------- phase B: greedy placement ----------
// Facility = anchored run tail; valid point x needs a facility in [x, x+D]
// (the walk runs forward along the orbit). Process gaps in orbit order; for
// the first pending valid point fp, place at the LAST tail in [fp, fp+D].
// Gap m spans (tidx[m-1], tidx[m]] (tidx[-1] := 0); its closing tail
// tidx[m] is the (m+1)-th tail on the orbit; the last record has tidx = n
// and is run 0's tail, which is the forced anchor at orbit idx 0.
struct GreedyResult {
    uint64_t count = 0;
    uint64_t cntPts = 0, sumDist = 0, maxDist = 0;
    uint64_t forced = 0, forcedExcess = 0;
};

static void greedy(const OrbitGaps& G, size_t li, uint64_t n, uint64_t D, bool allRows,
                   GreedyResult& out, std::vector<uint32_t>* collectRuns) {
    const uint32_t NONE = ~0u;
    const std::vector<uint32_t>& tidx = G.tidx;
    const std::vector<uint32_t>& trun = G.trun;
    uint64_t R = tidx.size();
    uint64_t NOFP = ~(uint64_t)0;
    out = GreedyResult{};
    auto gapFirst = [&](uint64_t m) -> uint64_t {
        if (allRows) return m == 0 ? 1 : (uint64_t)tidx[m - 1] + 1;
        uint32_t f = G.tfirst[li][m];
        return f == NONE ? NOFP : (uint64_t)f;
    };
    auto gapCnt = [&](uint64_t m) -> uint64_t {
        if (allRows) return (uint64_t)tidx[m] - (m == 0 ? 0 : tidx[m - 1]);
        return G.tcnt[li][m];
    };
    auto gapSum = [&](uint64_t m) -> uint64_t {
        if (allRows) {
            uint64_t lo = (m == 0 ? 0 : tidx[m - 1]) + 1, hi = tidx[m];
            return (lo + hi) * (hi - lo + 1) / 2;
        }
        return G.tsum[li][m];
    };
    out.count = 1; // forced anchor: tail of run 0 at orbit idx 0
    if (collectRuns) collectRuns->push_back(trun[R - 1]);
    uint64_t curGap = 0, candPtr = 0;
    while (curGap < R) {
        uint64_t fp = NOFP, fpGap = R;
        for (uint64_t m = curGap; m < R; m++) {
            uint64_t f = gapFirst(m);
            if (f != NOFP) { fp = f; fpGap = m; break; }
        }
        if (fpGap == R) break;
        if (candPtr < fpGap) candPtr = fpGap;
        uint64_t best = NOFP, bestK = R;
        while (candPtr < R && tidx[candPtr] <= fp + D) { best = tidx[candPtr]; bestK = candPtr; candPtr++; }
        uint64_t f, fK;
        if (best == NOFP) { // no tail in [fp, fp+D]: forced at first tail >= fp
            f = tidx[fpGap]; fK = fpGap;
            if (f > fp + D) { out.forced++; uint64_t ex = f - fp - D; if (ex > out.forcedExcess) out.forcedExcess = ex; }
            if (candPtr <= fK) candPtr = fK + 1;
        } else { f = best; fK = bestK; }
        bool isWrap = (fK == R - 1); // tidx[R-1] = n is run 0's tail = forced anchor
        for (uint64_t m = curGap; m <= fK; m++) {
            uint64_t cnt = gapCnt(m);
            if (!cnt) continue;
            out.cntPts += cnt;
            out.sumDist += cnt * f - gapSum(m);
            uint64_t d = f - gapFirst(m);
            if (d > out.maxDist) out.maxDist = d;
        }
        if (!isWrap) {
            out.count++;
            if (collectRuns) collectRuns->push_back(trun[fK]);
        }
        curGap = fK + 1;
        if (isWrap) break;
    }
}

// ---------- benchmark ----------
static void benchmark(const Container& c, const Idx& idx, const std::vector<uint8_t>& txt,
                      const std::vector<uint32_t>& SA, const std::vector<Bits>& lcpge,
                      const std::vector<uint64_t>& Ls, size_t li,
                      const std::vector<uint32_t>& placedRuns, uint64_t queries,
                      uint64_t rowCap, int threads) {
    uint64_t n = c.n, L = Ls[li], qlen = L;
    Bits abit; abit.init(c.r);
    for (auto rn : placedRuns) abit.set(rn);
    std::atomic<uint64_t> occTotal{0}, rowsTotal{0}, mismSA{0}, mismCtx{0}, uncovered{0};
    std::atomic<uint64_t> steps11{0}, stepsSV{0}, maxs11{0}, maxsSV{0}, uncStepsSV{0}, uncCnt{0};
    std::atomic<double> ns11{0}, nsSV{0};
    auto addns = [](std::atomic<double>& a, double v) { double cur = a.load(); while (!a.compare_exchange_weak(cur, cur + v)) {} };
    auto worker = [&](uint64_t lo, uint64_t hi) {
        Rng rng(0xBEEF0000 + lo * 131 + li);
        for (uint64_t s = lo; s < hi; s++) {
            uint64_t p0 = 0;
            for (;;) {
                p0 = rng.below(n);
                bool ok = true;
                for (uint64_t j = 0; j < qlen; j++) if (txt[(p0 + j) % n] == 0x1e) { ok = false; break; }
                if (ok) break;
            }
            uint8_t P[512];
            for (uint64_t j = 0; j < qlen; j++) P[j] = txt[(p0 + j) % n];
            uint64_t sp = 0, ep = n;
            bool found = true;
            for (int64_t i = (int64_t)qlen - 1; i >= 0; i--) {
                uint8_t ch = P[i];
                uint64_t nsp = idx.runs.C[ch] + idx.rank_byte(ch, sp);
                uint64_t nep = idx.runs.C[ch] + idx.rank_byte(ch, ep);
                if (nsp >= nep) { found = false; break; }
                sp = nsp; ep = nep;
            }
            if (!found) { mismSA++; continue; }
            uint64_t occ = ep - sp;
            occTotal += occ;
            std::vector<uint64_t> rows;
            rows.push_back(sp);
            if (occ > 1) {
                rows.push_back(ep - 1);
                for (uint64_t j = 1; j + 1 < rowCap && rows.size() < rowCap; j++) {
                    uint64_t rr = sp + (occ - 1) * j / (rowCap - 1);
                    if (rr == sp || rr == ep - 1) continue;
                    rows.push_back(rr);
                }
                std::sort(rows.begin(), rows.end());
                rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
            }
            for (uint64_t x : rows) {
                uint64_t run = idx.run_of(x), off = x - idx.runstart[run];
                int covered = lcpge[li].get(x ? x - 1 : n - 1) | lcpge[li].get(x);
                double t0 = now_s();
                uint64_t cr = run, co = off, st = 0;
                while (!idx.at_anchor_tail(cr, co)) { idx.step(cr, co); st++; if (st > n) break; }
                uint64_t p11 = (idx.anchor_val[cr / 1024] + st) % n;
                double t1 = now_s();
                uint64_t sr2 = run, so = off, st2 = 0;
                while (!(so + 1 == idx.runs.len[sr2] && abit.get(sr2))) { idx.step(sr2, so); st2++; if (st2 > n) break; }
                uint64_t psv = (SA[idx.runstart[sr2] + so] + st2) % n;
                double t2 = now_s();
                if (p11 != SA[x] || psv != SA[x]) mismSA++;
                else {
                    bool ctxok = true;
                    for (uint64_t j = 0; j < qlen; j++) if (txt[(SA[x] + j) % n] != P[j]) { ctxok = false; break; }
                    if (!ctxok) {
                        mismCtx++;
                        static std::atomic<uint64_t> dumped{0};
                        if (dumped.fetch_add(1) < 3) {
                            printf("CTXMISS x=%llu sp=%llu ep=%llu occ=%llu SA[x]=%llu P=", \
 (unsigned long long)x, (unsigned long long)sp, (unsigned long long)ep, \
 (unsigned long long)occ, (unsigned long long)SA[x]);
                            for (uint64_t j = 0; j < qlen; j++) printf("%02x ", P[j]);
                            printf(" ctx=");
                            for (uint64_t j = 0; j < qlen; j++) printf("%02x ", txt[(SA[x] + j) % n]);
                            printf("\n");
                        }
                    }
                }
                rowsTotal++;
                steps11 += st; stepsSV += st2;
                if (st > maxs11.load()) maxs11 = st;
                if (st2 > maxsSV.load()) maxsSV = st2;
                if (!covered) { uncovered++; uncStepsSV += st2; uncCnt++; }
                addns(ns11, (t1 - t0) * 1e9);
                addns(nsSV, (t2 - t1) * 1e9);
            }
        }
    };
    std::vector<std::thread> th;
    uint64_t per = (queries + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
        uint64_t lo = (uint64_t)t * per, hi = std::min(queries, lo + per);
        if (lo < hi) th.emplace_back(worker, lo, hi);
    }
    for (auto& t : th) t.join();
    printf("BENCH L=%llu qlen=%llu: patterns=%llu occurrencesTotal=%llu locatedRows=%llu\n",
           (unsigned long long)L, (unsigned long long)qlen, (unsigned long long)queries,
           (unsigned long long)occTotal.load(), (unsigned long long)rowsTotal.load());
    printf("  gates: SA-parity mismatches=%llu context mismatches=%llu uncoveredStarts=%llu (mean stateval steps on uncovered=%.1f)\n",
           (unsigned long long)mismSA.load(), (unsigned long long)mismCtx.load(),
           (unsigned long long)uncovered.load(),
           uncCnt.load() ? (double)uncStepsSV.load() / uncCnt.load() : 0.0);
    printf("  member11: meanSteps=%.1f maxSteps=%llu meanLatency=%.0f ns/locate\n",
           rowsTotal.load() ? (double)steps11.load() / rowsTotal.load() : 0.0,
           (unsigned long long)maxs11.load(),
           rowsTotal.load() ? ns11.load() / rowsTotal.load() : 0.0);
    printf("  stateval: meanSteps=%.1f maxSteps=%llu meanLatency=%.0f ns/locate (%.2fx member11 latency, %.2fx steps)\n",
           rowsTotal.load() ? (double)stepsSV.load() / rowsTotal.load() : 0.0,
           (unsigned long long)maxsSV.load(),
           rowsTotal.load() ? nsSV.load() / rowsTotal.load() : 0.0,
           ns11.load() / std::max(1e-9, nsSV.load()),
           (double)steps11.load() / std::max(1.0, (double)stepsSV.load()));
}

// ---------- full mode ----------
static void mode_full(const char* path, std::vector<uint64_t> Ls, std::vector<uint64_t> Ds,
                      uint64_t targetCount, uint64_t queries, uint64_t rowCap, int threads,
                      const char* textCache, const char* srcRl, const char* srcFa,
                      const char* outAnchors) {
    Container c; c.load(path);
    uint64_t n = c.n, r = c.r;
    printf("container: n=%llu k=%llu R=%llu flags=%u\n", (unsigned long long)n,
           (unsigned long long)c.k, (unsigned long long)r, c.flags);
    double T0 = now_s();
    Idx idx; idx.build(c);
    printf("idx built (%.1fs): %d symbols, member11 anchors=%llu\n", now_s() - T0,
           idx.nsym, (unsigned long long)idx.anchor_val.size());
    if (targetCount == 0) targetCount = idx.anchor_val.size();
    std::vector<uint8_t> txt;
    if (!load_text_cache(textCache, n, txt)) {
        double t = now_s();
        reconstruct_text(c, idx, txt, threads);
        printf("text reconstructed (%.1fs)\n", now_s() - t);
        save_text_cache(textCache, txt);
    } else printf("text cache loaded\n");
    if (srcRl && *srcRl) {
        double t = now_s();
        bool ok = compare_flat(srcRl, txt);
        printf("source compare (flat) %s: %s (%.1fs)\n", srcRl, ok ? "BYTE-EXACT" : "MISMATCH", now_s() - t);
        if (!ok) die("source compare failed");
    }
    if (srcFa && *srcFa) {
        double t = now_s();
        bool ok = compare_fasta_revrec(srcFa, txt);
        printf("source compare (fasta revrec) %s: %s (%.1fs)\n", srcFa, ok ? "BYTE-EXACT" : "MISMATCH", now_s() - t);
        if (!ok) die("source compare failed");
    }
    std::vector<uint32_t> SA;
    {
        double t = now_s();
        build_sa(c, idx, SA, threads);
        printf("SA reconstructed (%.1fs)\n", now_s() - t);
    }
    {   // gates
        double t = now_s();
        uint64_t mism = 0;
        for (uint64_t a = 0; a < idx.anchor_val.size(); a++) {
            uint64_t run = a * 1024;
            uint64_t row = idx.runstart[run] + idx.runs.len[run] - 1;
            if (SA[row] != idx.anchor_val[a]) mism++;
        }
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; i++) sum += SA[i];
        uint64_t expect = (n * (n - 1)) / 2;
        printf("gates: member11 SA mismatches=%llu / %llu ; sum(SA)=%s (%.1fs)\n",
               (unsigned long long)mism, (unsigned long long)idx.anchor_val.size(),
               sum == expect ? "OK" : "BAD", now_s() - t);
        if (mism) die("member-11 crosscheck failed");
        if (sum != expect) die("SA sum check failed");
    }
    // per-L lcpge bitmaps
    size_t nL = Ls.size();
    std::vector<Bits> lcpge(nL);
    for (size_t li = 0; li < nL; li++) {
        double t = now_s();
        uint64_t L = Ls[li];
        Bits& B = lcpge[li]; B.init(n);
        std::atomic<uint64_t> pairs{0};
        auto worker = [&](uint64_t lo, uint64_t hi) {
            uint64_t lp = 0;
            for (uint64_t i = lo; i < hi; i++) {
                uint64_t p1 = SA[i], p2 = SA[(i + 1) % n];
                bool ge = true;
                for (uint64_t j = 0; j < L; j++) {
                    if (txt[(p1 + j) % n] != txt[(p2 + j) % n]) { ge = false; break; }
                }
                if (ge) { B.set(i); lp++; }
            }
            pairs += lp;
        };
        std::vector<std::thread> th;
        uint64_t per = (n + threads - 1) / threads;
        for (int t2 = 0; t2 < threads; t2++) {
            uint64_t lo = (uint64_t)t2 * per, hi = std::min(n, lo + per);
            if (lo < hi) th.emplace_back(worker, lo, hi);
        }
        for (auto& t2 : th) t2.join();
        std::atomic<uint64_t> vc{0};
        auto vworker = [&](uint64_t lo, uint64_t hi) {
            uint64_t lvc = 0;
            for (uint64_t i = lo; i < hi; i++)
                if (B.get(i ? i - 1 : n - 1) | B.get(i)) lvc++;
            vc += lvc;
        };
        th.clear();
        for (int t2 = 0; t2 < threads; t2++) {
            uint64_t lo = (uint64_t)t2 * per, hi = std::min(n, lo + per);
            if (lo < hi) th.emplace_back(vworker, lo, hi);
        }
        for (auto& t2 : th) t2.join();
        printf("LCPGE L=%llu: adjacent pairs with LCP>=%llu: %llu (%.4f%% of n); |V_L|=%llu (%.2f%% of n) (%.1fs)\n",
               (unsigned long long)L, (unsigned long long)L, (unsigned long long)pairs.load(),
               100.0 * (double)pairs.load() / (double)n, (unsigned long long)vc.load(),
               100.0 * (double)vc.load() / (double)n, now_s() - t);
    }
    // phase A
    OrbitGaps G;
    std::vector<UniformStats> uall(1), uvalid(nL);
    phase_a(c, idx, lcpge, Ls, G, uall, uvalid, threads);
    printf("orbit gaps: R=%llu records; member-11 uniform walk over ALL rows: mean=%.1f max=%llu (banked lane: yeast 221573.0/6516845, pile 3050.6/134859)\n",
           (unsigned long long)r, uall[0].cnt ? (double)uall[0].sumdist / uall[0].cnt : 0.0,
           (unsigned long long)uall[0].maxdist);
    for (size_t li = 0; li < nL; li++)
        printf("member-11 uniform walk over V_L (L=%llu): cnt=%llu mean=%.1f max=%llu\n",
               (unsigned long long)Ls[li], (unsigned long long)uvalid[li].cnt,
               uvalid[li].cnt ? (double)uvalid[li].sumdist / uvalid[li].cnt : 0.0,
               (unsigned long long)uvalid[li].maxdist);
    {   // inter-tail orbit gap stats + valid-to-next-tail floor
        uint64_t maxg = 0; long double sumg = 0; uint64_t cnt1k = 0;
        for (uint64_t m = 0; m < r; m++) {
            uint64_t lo2 = m ? G.tidx[m - 1] : 0;
            uint64_t g = G.tidx[m] - lo2;
            sumg += g; if (g > maxg) maxg = g; if (g >= 1024) cnt1k++;
        }
        printf("orbit inter-tail gaps: mean=%.1f max=%llu gaps>=1024=%llu\n",
               (double)(sumg / r), (unsigned long long)maxg, (unsigned long long)cnt1k);
        for (size_t li = 0; li < nL; li++) {
            uint64_t maxf = 0;
            for (uint64_t m = 0; m < r; m++) {
                if (G.tfirst[li][m] != ~0u) {
                    uint64_t d = G.tidx[m] - G.tfirst[li][m];
                    if (d > maxf) maxf = d;
                }
            }
            printf("floor L=%llu: max distance from a V_L point to the next run tail on the orbit = %llu\n",
                   (unsigned long long)Ls[li], (unsigned long long)maxf);
        }
    }
    // phase B
    printf("\n=== state-valid greedy placement (D = walk bound from V_L) ===\n");
    printf("%-10s %-12s | %11s %12s %11s %8s\n", "L", "D", "count", "meanwalk", "maxwalk", "forced");
    std::vector<std::vector<uint32_t>> placedPerL(nL);
    std::vector<uint64_t> Dstar(nL, 0);
    for (size_t li = 0; li < nL; li++) {
        for (uint64_t D : Ds) {
            GreedyResult res;
            greedy(G, li, n, D, false, res, nullptr);
            printf("%-10llu %-12llu | %11llu %12.1f %11llu %8llu\n",
                   (unsigned long long)Ls[li], (unsigned long long)D,
                   (unsigned long long)res.count,
                   res.cntPts ? (double)res.sumDist / res.cntPts : 0.0,
                   (unsigned long long)res.maxDist, (unsigned long long)res.forced);
            GreedyResult resA;
            greedy(G, li, n, D, true, resA, nullptr);
            printf("%-10s %-12llu | %11llu %12.1f %11llu %8llu   (all-rows)\n",
                   "ALL", (unsigned long long)D,
                   (unsigned long long)resA.count,
                   resA.cntPts ? (double)resA.sumDist / resA.cntPts : 0.0,
                   (unsigned long long)resA.maxDist, (unsigned long long)resA.forced);
        }
        {   // all-rows variant summary at D=4096 dropped (per-D all-rows now printed)
        }
        uint64_t lo = 1, hi = n;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            GreedyResult res;
            greedy(G, li, n, mid, false, res, nullptr);
            if (res.count <= targetCount) hi = mid; else lo = mid + 1;
        }
        Dstar[li] = lo;
        GreedyResult res;
        std::vector<uint32_t> runs;
        greedy(G, li, n, lo, false, res, &runs);
        printf("L=%llu: D*=%llu (min D with count<=%llu) -> count=%llu meanwalk=%.1f maxwalk=%llu forced=%llu forcedExcess=%llu\n",
               (unsigned long long)Ls[li], (unsigned long long)lo, (unsigned long long)targetCount,
               (unsigned long long)res.count,
               res.cntPts ? (double)res.sumDist / res.cntPts : 0.0,
               (unsigned long long)res.maxDist, (unsigned long long)res.forced,
               (unsigned long long)res.forcedExcess);
        placedPerL[li] = std::move(runs);
        uint64_t alo = 1, ahi = n;
        while (alo < ahi) {
            uint64_t mid = alo + (ahi - alo) / 2;
            GreedyResult res2;
            greedy(G, li, n, mid, true, res2, nullptr);
            if (res2.count <= targetCount) ahi = mid; else alo = mid + 1;
        }
        GreedyResult res2;
        greedy(G, li, n, alo, true, res2, nullptr);
        printf("  all-rows D*=%llu -> count=%llu meanwalk=%.1f maxwalk=%llu (orbit-aware, workload-independent)\n",
               (unsigned long long)alo, (unsigned long long)res2.count,
               res2.cntPts ? (double)res2.sumDist / res2.cntPts : 0.0,
               (unsigned long long)res2.maxDist);
    }
    if (outAnchors && *outAnchors) {
        FILE* f = fopen(outAnchors, "wb");
        if (!f) die("anchor out");
        for (size_t li = 0; li < nL; li++) {
            uint64_t hdr[3] = {Ls[li], Dstar[li], placedPerL[li].size()};
            fwrite(hdr, 8, 3, f);
            fwrite(placedPerL[li].data(), 4, placedPerL[li].size(), f);
        }
        fclose(f);
        printf("anchor sets written to %s\n", outAnchors);
    }
    printf("\n=== locate benchmark (patterns of length L from the indexed text) ===\n");
    for (size_t li = 0; li < nL; li++)
        benchmark(c, idx, txt, SA, lcpge, Ls, li, placedPerL[li], queries, rowCap, threads);
    printf("TOTAL wall %.1fs\n", now_s() - T0);
}

static void mode_selftest(const char* path, int threads) {
    Container c; c.load(path);
    Idx idx; idx.build(c);
    uint64_t n = c.n;
    std::vector<uint8_t> L(n);
    for (uint64_t r2 = 0; r2 < c.r; r2++)
        for (uint64_t j = 0; j < idx.runs.len[r2]; j++) L[idx.runstart[r2] + j] = idx.runs.sym[r2];
    // brute rank check
    uint64_t bad = 0;
    for (uint64_t i = 0; i <= n; i += (n > 20000 ? 997 : 1)) {
        for (int s = 0; s < 256; s++) {
            if (idx.symmap[s] == 0xff) continue;
            uint64_t brute = 0;
            for (uint64_t j = 0; j < i; j++) if (L[j] == s) brute++;
            uint64_t got = idx.rank_byte((uint8_t)s, i);
            if (got != brute) {
                if (bad < 10) printf("RANKBAD s=%d i=%llu got=%llu brute=%llu\n", s,
                                    (unsigned long long)i, (unsigned long long)got, (unsigned long long)brute);
                bad++;
            }
        }
    }
    printf("rank selftest: bad=%llu\n", (unsigned long long)bad);
    // LF bijection check: LF(i) = C[L[i]] + rank_{L[i]}(i)
    std::vector<uint8_t> seen(n, 0);
    uint64_t lfbad = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t lf = idx.runs.C[L[i]] + idx.rank_byte(L[i], i);
        if (lf >= n) { lfbad++; continue; }
        if (seen[lf]) lfbad++;
        seen[lf] = 1;
    }
    printf("LF bijection: bad=%llu\n", (unsigned long long)lfbad);
    // BWT fundamental property with the reconstructed text + SA array:
    // L[i] = T[(SA[i]-1) mod n] for every row i. Catches any inconsistency
    // between anchor values, the walks, and the BWT.
    std::vector<uint8_t> txt;
    reconstruct_text(c, idx, txt, 1);
    std::vector<uint32_t> SA;
    build_sa(c, idx, SA, 1);
    uint64_t bwtbad = 0; uint64_t firstbad = ~0ull, lastbad = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t expect = txt[(SA[i] + n - 1) % n];
        if (L[i] != expect) {
            bwtbad++;
            if (i < firstbad) firstbad = i;
            if (i > lastbad) lastbad = i;
        }
    }
    printf("BWT property L[i]==T[SA[i]-1]: bad=%llu", (unsigned long long)bwtbad);
    if (bwtbad) printf(" (rows %llu..%llu)", (unsigned long long)firstbad, (unsigned long long)lastbad);
    printf("\n");
    (void)threads;
}

static void mode_info(const char* path) {
    Container c; c.load(path);
    printf("n=%llu k=%llu R=%llu members=%u flags=%u\n",
           (unsigned long long)c.n, (unsigned long long)c.k, (unsigned long long)c.r,
           c.memcount, c.flags);
    for (auto& m : c.members)
        printf("  id=%u codec=%u off=%llu bytes=%llu count=%llu\n",
               m.id, m.codec, (unsigned long long)m.off, (unsigned long long)m.bytes,
               (unsigned long long)m.count);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s CONTAINER info|full [opts]\n"
                        "  full [--Ls 20,39,100] [--Ds 1024,4096,...] [--target-count N]\n"
                        "       [--queries K] [--rowcap N] [--threads T<=31] [--text-cache F]\n"
                        "       [--src-rl F] [--src-fa F] [--out-anchors F]\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];
    std::string mode = argv[2];
    std::vector<uint64_t> Ls = {20, 39, 100}, Ds = {1024, 4096, 16384, 65536, 262144, 1048576};
    uint64_t targetCount = 0, queries = 1000, rowCap = 8;
    int threads = 31;
    const char* textCache = nullptr, *srcRl = nullptr, *srcFa = nullptr, *outAnchors = nullptr;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { if (i + 1 >= argc) die("missing arg"); return argv[++i]; };
        auto nums = [&](std::vector<uint64_t>& v) {
            v.clear(); char* s = strdup(next());
            for (char* t = strtok(s, ","); t; t = strtok(nullptr, ","))
                v.push_back(strtoull(t, 0, 0));
            free(s); };
        if (a == "--Ls") nums(Ls);
        else if (a == "--Ds") nums(Ds);
        else if (a == "--target-count") targetCount = strtoull(next(), 0, 0);
        else if (a == "--queries") queries = strtoull(next(), 0, 0);
        else if (a == "--rowcap") rowCap = strtoull(next(), 0, 0);
        else if (a == "--threads") threads = atoi(next());
        else if (a == "--text-cache") textCache = next();
        else if (a == "--src-rl") srcRl = next();
        else if (a == "--src-fa") srcFa = next();
        else if (a == "--out-anchors") outAnchors = next();
        else die("bad opt");
    }
    if (threads > 31) { fprintf(stderr, "clamping threads to 31\n"); threads = 31; }
    if (mode == "info") mode_info(path);
    else if (mode == "selftest") mode_selftest(path, threads);
    else if (mode == "full") mode_full(path, Ls, Ds, targetCount, queries, rowCap, threads,
                                        textCache, srcRl, srcFa, outAnchors);
    else die("mode");
    return 0;
}
