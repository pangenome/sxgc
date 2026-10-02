// Skiplist-over-anchors locate lane: baseline walk-length measurement,
// safe-jump skiplist prototype, and text verification for the lite
// container plan (member 11 sparse anchors + derived LF starts).
//
// Reads retained SXI2 artifacts READ-ONLY. No writes to the container.
//
// Modes:
//   info                     container summary
//   sweep   [--stride 1024] [--threads T] [--out dist.bin]
//            walk every anchor->next-anchor segment; exact walk-length
//            distribution over ALL n rows (segments partition the LF cycle)
//   entry   [--samples K] [--threads T]
//            sampled distance to first entry into an anchored RUN
//            (the "~1024" banked model) - context only
//   verify  [--samples K] [--threads T] [--window W] [--fa F] [--rl F]
//            [--no-jumps] [--cap N]
//            reconstructs the indexed text via inverse BWT, byte-compares
//            it against the source corpus, then samples rows: flat walk vs
//            skiplist walk; every recovered position is text-verified
//   jumps   [--cap N] [--threads T]
//            build safe-jump pointer statistics (space + level counts)
//
// Anchor semantics (member 11, codec 111): one raw u64 tail-SA value for
// every 1024th run (run id divisible by 1024); locate terminates when the
// walk lands exactly on the tail row of an anchored run:
//   SA[origin] = (anchor_value + steps) mod n
// (each LF step decrements SA by 1 in the cyclic domain).

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
        if (ver < 3 || ver > 6)
            fprintf(stderr, "note: container version %u\n", ver);
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

// ---------- codec 101 runs: C table + Huffman heads + gamma lengths ----------
struct Runs {
    std::vector<uint32_t> len;    // per run
    std::vector<uint8_t> sym;     // per run
    std::vector<uint64_t> C;     // 256 cumulative-before
};

static void load_runs(const Container& c, Runs& out) {
    const Member* m = c.member(1);
    if (!m) die("member 1");
    if (m->codec == 1) { // SXI1 raw: 256 C, R u8 syms, R u32 lens
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

// ---------- index arrays ----------
struct Idx {
    const Container* c = nullptr;
    Runs runs;
    std::vector<uint32_t> runstart;  // r+1 entries
    std::vector<uint32_t> lfstart;   // LF output block start per run
    std::vector<uint32_t> ckpt;      // per 1024 rows: run id containing that row
    uint64_t stride = 1024;          // anchored-run stride (positions)
    std::vector<uint64_t> anchor_val;// member 11 tail SA per anchored run (stride 1024 only)

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
    }

    inline uint64_t run_of(uint64_t row) const {
        uint64_t ci = row >> 10;
        uint64_t rr = ckpt[ci];
        while (runstart[rr + 1] <= row) rr++;
        return rr;
    }
    // anchored-tail check from known (run, off): valid iff off == len-1 and run%stride==0
    inline bool at_anchor_tail(uint64_t run, uint64_t off) const {
        return run % stride == 0 && off + 1 == runs.len[run];
    }
    inline void step(uint64_t& run, uint64_t& off) const {
        uint64_t row = lfstart[run] + off;
        uint64_t rr = run_of(row);
        run = rr; off = row - runstart[rr];
    }
};

// ---------- xorshift RNG ----------
struct Rng { uint64_t s; explicit Rng(uint64_t seed) : s(seed ? seed : 88172645463325252ull) {}
    inline uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    inline uint64_t below(uint64_t m) { return next() % m; }
};

static double now_s() {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

static const uint64_t LITE_BYTES = 177179983; // yeast235 v6-3 minus member 8

static void mode_info(const char* path) {
    Container c; c.load(path);
    printf("n=%llu k=%llu R=%llu members=%u flags=%u\n",
           (unsigned long long)c.n, (unsigned long long)c.k, (unsigned long long)c.r,
           c.memcount, c.flags);
    for (auto& m : c.members)
        printf("  id=%u codec=%u off=%llu bytes=%llu count=%llu\n",
               m.id, m.codec, (unsigned long long)m.off, (unsigned long long)m.bytes,
               (unsigned long long)m.count);
    const Member* m8 = c.member(8);
    uint64_t total = 0; for (auto& m : c.members) total += m.bytes;
    if (m8) printf("lite (all members minus member 8) = %llu bytes; member8 = %llu bytes\n",
                   (unsigned long long)(total - m8->bytes), (unsigned long long)m8->bytes);
}

// exact row-weighted stats from anchor-segment lengths delta[]:
// segment a covers rows at distances {0..delta_a-1}; S(m)=sum_{d>m}(d-m)/n
struct Dist {
    std::vector<uint64_t> sd; // sorted segment lengths
    uint64_t n;
    std::vector<uint64_t> suf; // suffix sums, suf[i] = sum(sd[i..])
    void prep(std::vector<uint64_t>& delta, uint64_t n_) {
        n = n_; sd = delta; std::sort(sd.begin(), sd.end());
        size_t na = sd.size(); suf.assign(na + 1, 0);
        for (size_t i = na; i-- > 0;) suf[i] = suf[i + 1] + sd[i];
    }
    double surv(uint64_t m) const { // P(dist >= m), m >= 1
        size_t ub = std::upper_bound(sd.begin(), sd.end(), m) - sd.begin();
        uint64_t cnt = sd.size() - ub;
        long double s = (long double)suf[ub] - (long double)cnt * (long double)m;
        return (double)(s / (long double)n);
    }
    double mean() const {
        long double acc = 0;
        for (uint64_t d : sd) acc += (long double)d * (long double)(d - 1) / 2.0;
        return (double)(acc / (long double)n);
    }
    uint64_t quantile(double p) const { // smallest m with P(dist>=m) <= 1-p
        uint64_t lo = 0, hi = sd.empty() ? 0 : sd.back() + 1;
        while (lo < hi) { uint64_t mid = lo + (hi - lo) / 2;
            if (surv(mid) <= 1.0 - p) hi = mid; else lo = mid + 1; }
        return lo;
    }
};

static void print_dist(const Dist& D, const char* tag) {
    printf("%s: mean=%.1f p50=%llu p90=%llu p99=%llu p999=%llu p9999=%llu max=%llu\n",
           tag, D.mean(), (unsigned long long)D.quantile(0.5),
           (unsigned long long)D.quantile(0.9), (unsigned long long)D.quantile(0.99),
           (unsigned long long)D.quantile(0.999), (unsigned long long)D.quantile(0.9999),
           (unsigned long long)(D.sd.empty() ? 0 : D.sd.back()));
}

static void mode_sweep(const char* path, uint64_t stride, int threads, const char* outfile,
                       uint64_t derive, const char* valsfile) {
    Container c; c.load(path);
    Idx idx; idx.build(c);
    if (stride != 1024 && derive == 0)
        fprintf(stderr, "note: non-1024 stride sweeps positions only (values unused)\n");
    idx.stride = stride;
    uint64_t na = (c.r + stride - 1) / stride;
    std::vector<uint64_t> delta(na);
    // optional derivation of denser anchor values: the stride-1024 segments
    // visit every row exactly once, so every stride-S anchored tail row is
    // seen and its SA = (v_a - steps) is recorded. Sentinel-checked at the end.
    std::vector<uint64_t> dvals;
    uint64_t dn = 0;
    if (derive) { dn = (c.r + derive - 1) / derive; dvals.assign(dn, ~0ull); }
    std::atomic<uint64_t> overflow{0}, derived{0}, dcheck{0}, dbad{0};
    double t0 = now_s();
    auto worker = [&](uint64_t lo, uint64_t hi) {
        for (uint64_t a = lo; a < hi; a++) {
            uint64_t run = a * stride;
            uint64_t v = idx.anchor_val.size() ? idx.anchor_val[a] : 0;
            if (stride != 1024) v = 0;
            uint64_t crun = run, coff = idx.runs.len[run] - 1;
            uint64_t steps = 0;
            while (true) {
                idx.step(crun, coff);
                steps++;
                if (derive && crun % derive == 0 && coff + 1 == idx.runs.len[crun]) {
                    uint64_t val = (v + c.n - (steps % c.n)) % c.n;
                    dvals[crun / derive] = val; derived++;
                }
                if (idx.at_anchor_tail(crun, coff)) break;
                if (steps > c.n) { overflow++; break; }
            }
            delta[a] = steps;
        }
    };
    std::vector<std::thread> th;
    uint64_t per = (na + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
        uint64_t lo = (uint64_t)t * per, hi = std::min(na, lo + per);
        if (lo < hi) th.emplace_back(worker, lo, hi);
    }
    for (auto& t : th) t.join();
    double wall = now_s() - t0;
    uint64_t sum = 0; for (auto d : delta) sum += d;
    printf("stride=%llu anchors=%llu sum(delta)=%llu n=%llu %s wall=%.1fs threads=%d\n",
           (unsigned long long)stride, (unsigned long long)na, (unsigned long long)sum,
           (unsigned long long)c.n, (sum == c.n && !overflow) ? "CYCLE-OK" : "CYCLE-BROKEN",
           wall, threads);
    if (overflow) printf("overflow segments: %llu\n", (unsigned long long)overflow.load());
    if (derive) {
        // every slot must be filled; slots at stride-1024 runs must equal member 11
        uint64_t holes = 0;
        for (uint64_t i = 0; i < dn; i++) if (dvals[i] == ~0ull) holes++;
        for (uint64_t a = 0; a * 1024 < c.r && a < idx.anchor_val.size(); a++) {
            if ((a * 1024) % derive == 0) {
                dcheck++;
                if (dvals[(a * 1024) / derive] != idx.anchor_val[a]) dbad++;
            }
        }
        printf("derive stride=%llu: values=%llu holes=%llu member11-crosscheck=%llu mismatches=%llu\n",
               (unsigned long long)derive, (unsigned long long)derived.load(),
               (unsigned long long)holes, (unsigned long long)dcheck,
               (unsigned long long)dbad.load());
        if (valsfile) {
            FILE* f = fopen(valsfile, "wb");
            if (!f) die("open vals out");
            uint64_t hdr[3] = {derive, dn, c.n};
            fwrite(hdr, 8, 3, f); fwrite(dvals.data(), 8, dn, f);
            fclose(f);
            printf("  wrote %s\n", valsfile);
        }
    }
    Dist D; D.prep(delta, c.n);
    char tag[128];
    snprintf(tag, sizeof tag, "flat walk stride=%lu (tail-row termination, all rows)", (unsigned long)stride);
    print_dist(D, tag);
    printf("  segment-length histogram (anchor-side, unweighted):\n");
    const uint64_t hb[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192,
                           16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152,
                           4194304, 8388608, 1ull << 62};
    for (int i = 0; hb[i] != (1ull << 62); i++) {
        auto it1 = std::lower_bound(D.sd.begin(), D.sd.end(), hb[i]);
        auto it2 = std::lower_bound(D.sd.begin(), D.sd.end(), hb[i + 1]);
        if (it2 > it1) printf("    [%6llu,%6llu): %llu segments\n",
                              (unsigned long long)hb[i], (unsigned long long)hb[i + 1],
                              (unsigned long long)(it2 - it1));
    }
    if (outfile) {
        FILE* f = fopen(outfile, "wb");
        if (!f) die("open out");
        uint64_t hdr[3] = {stride, na, c.n};
        fwrite(hdr, 8, 3, f); fwrite(delta.data(), 8, na, f);
        fclose(f);
        printf("  wrote %s\n", outfile);
    }
}

static void mode_entry(const char* path, uint64_t samples, int threads) {
    Container c; c.load(path);
    Idx idx; idx.build(c);
    std::vector<uint64_t> dist(samples);
    auto worker = [&](uint64_t lo, uint64_t hi) {
        Rng rng(12345 + lo);
        for (uint64_t s = lo; s < hi; s++) {
            uint64_t row = rng.below(c.n);
            uint64_t run = idx.run_of(row), off = row - idx.runstart[run];
            uint64_t steps = 0;
            while (run % 1024 != 0) { idx.step(run, off); steps++; if (steps > c.n) break; }
            dist[s] = steps;
        }
    };
    std::vector<std::thread> th; uint64_t per = (samples + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
        uint64_t lo = (uint64_t)t * per, hi = std::min(samples, lo + per);
        if (lo < hi) th.emplace_back(worker, lo, hi);
    }
    for (auto& t : th) t.join();
    std::sort(dist.begin(), dist.end());
    double mean = 0; for (auto d : dist) mean += d; mean /= samples;
    printf("entry distance to anchored RUN (sampled %llu rows; the '~1024 steps' banked model):\n",
           (unsigned long long)samples);
    printf("  mean=%.1f p50=%llu p90=%llu p99=%llu max=%llu\n", mean,
           (unsigned long long)dist[samples / 2], (unsigned long long)dist[samples * 9 / 10],
           (unsigned long long)dist[samples - std::max<uint64_t>(1, samples / 100)],
           (unsigned long long)dist.back());
    printf("  NOTE: entry into an anchored run does NOT resolve SA (within-run SA values\n"
           "  are not consecutive; e.g. BWT \"annb$aa\" run {a,a} has SA {4,2}); correct\n"
           "  termination requires the anchored TAIL row.\n");
}

// ---------- text reconstruction via anchor-segment walks (parallel) ----------
static void reconstruct_text(const Container& c, const Idx& idx, std::vector<uint8_t>& txt,
                             const char* cache) {
    if (cache && *cache) {
        struct stat st;
        if (::stat(cache, &st) == 0 && (uint64_t)st.st_size == c.n) {
            int fd = ::open(cache, O_RDONLY);
            txt.resize(c.n);
            uint64_t got = 0;
            while (got < c.n) {
                ssize_t r = read(fd, txt.data() + got, std::min<uint64_t>(1 << 30, c.n - got));
                if (r <= 0) die("text cache read");
                got += r;
            }
            close(fd);
            printf("  loaded text cache %s\n", cache);
            return;
        }
    }
    txt.assign(c.n, 0);
    std::atomic<uint64_t> bad{0};
    auto worker = [&](uint64_t lo, uint64_t hi) {
        for (uint64_t a = lo; a < hi; a++) {
            uint64_t run = a * 1024;
            uint64_t crun = run, coff = idx.runs.len[run] - 1;
            uint64_t v = idx.anchor_val[a];
            uint64_t k = 0;
            while (true) {
                // row r has SA = (v - k) mod n ; T[(SA-1) mod n] = BWT[row]
                txt[(v + c.n - 1 - k) % c.n] = idx.runs.sym[crun];
                idx.step(crun, coff); k++;
                if (idx.at_anchor_tail(crun, coff)) break;  // next anchor owns its own segment
                if (k > c.n) { bad++; break; }
            }
        }
    };
    std::vector<std::thread> th; uint64_t na = idx.anchor_val.size();
    uint64_t per = (na + 31) / 32;
    for (int t = 0; t < 32; t++) {
        uint64_t lo = (uint64_t)t * per, hi = std::min(na, lo + per);
        if (lo < hi) th.emplace_back(worker, lo, hi);
    }
    for (auto& t : th) t.join();
    if (bad) die("reconstruct overflow");
    // every position written exactly once: verify no zeros beyond separators
    uint64_t z = 0;
    for (uint64_t i = 0; i < c.n; i++) if (!txt[i]) { z++; if (z > 10) break; }
    // separators are 0x1e (nonzero); ACGT nonzero -> any zero = hole
    if (z) die("reconstruct holes");
    if (cache && *cache) {
        FILE* f = fopen(cache, "wb");
        if (!f) die("text cache open");
        if (fwrite(txt.data(), 1, c.n, f) != c.n) die("text cache write");
        fclose(f);
        printf("  wrote text cache %s\n", cache);
    }
}

static bool fill_from_fasta(const char* path, std::vector<uint8_t>& t, bool revrec, uint8_t sep) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint64_t w = 0; bool inrec = false, inhdr = false; int c;
    std::vector<uint8_t> rec;
    auto flush = [&]() {
        if (rec.empty()) return;
        if (revrec) std::reverse(rec.begin(), rec.end());
        for (auto b : rec) { if (w >= t.size()) die("fasta overfill"); t[w++] = b; }
        if (w >= t.size()) die("fasta overfill sep");
        t[w++] = sep;
        rec.clear();
    };
    while ((c = fgetc(f)) != EOF) {
        if (c == '>') { if (inrec) flush(); inrec = true; inhdr = true; continue; }
        if (c == '\n') { inhdr = false; continue; }
        if (c == '\r') continue;
        if (!inrec || inhdr) continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        rec.push_back((uint8_t)c);
    }
    flush(); fclose(f);
    return w == t.size();
}
static bool fill_from_lines(const char* path, std::vector<uint8_t>& t, bool revline, uint8_t sep) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint64_t w = 0; int c; bool any = false;
    std::vector<uint8_t> line;
    auto flush = [&]() {
        if (!any) return;
        if (revline) std::reverse(line.begin(), line.end());
        for (auto b : line) { if (w >= t.size()) die("lines overfill"); t[w++] = b; }
        if (w >= t.size()) die("lines overfill sep");
        t[w++] = sep; line.clear(); any = false;
    };
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') { flush(); continue; }
        line.push_back((uint8_t)c); any = true;
    }
    flush(); fclose(f);
    return w == t.size();
}

// ---------- safe-jump skiplist ----------
// J(r) = (d, dest): from ANY row of run r, LF^d(row) = dest + off, valid
// because the parallel band [LF^k(rs), LF^k(rs)+L_r) stays inside one run
// for all k<=d and contains no anchored tail row (termination can never
// be skipped). Build: phase-1 cap 256 for all band-filter candidates,
// phase-2 cap `cap` for runs that reached the phase-1 cap.
struct Jumps {
    std::vector<uint32_t> dest, dist, runs_q;
    std::vector<uint8_t> has;
    uint64_t cap = 0, censored = 0;
    inline bool get(uint64_t run, uint32_t* d, uint32_t* dst) const {
        if (!(has[run >> 3] & (1 << (run & 7)))) return false;
        auto it = std::lower_bound(runs_q.begin(), runs_q.end(), (uint32_t)run);
        uint64_t i = it - runs_q.begin();
        *d = dist[i]; *dst = dest[i];
        return true;
    }
};

static uint64_t safe_walk(const Idx& idx, uint64_t run, uint64_t cap, uint32_t* dest_out) {
    uint64_t L = idx.runs.len[run];
    uint64_t x = idx.lfstart[run];            // x_1
    uint64_t m = idx.run_of(x), o = x - idx.runstart[m];
    uint64_t d = 0;
    while (d < cap) {
        if (o + L > idx.runs.len[m]) break;
        if (m % idx.stride == 0) {            // band may contain an anchored tail
            uint64_t tail = idx.runstart[m] + idx.runs.len[m] - 1;
            if (tail >= x && tail < x + L) break;
        }
        d++; *dest_out = (uint32_t)x;         // LF^d(rs) = x, safe through step d
        if (d == cap) break;
        uint64_t row = idx.lfstart[m] + o;     // advance to x_{d+1}
        m = idx.run_of(row); o = row - idx.runstart[m];
        x = row;
    }
    return d;
}

static void build_jumps(const Container& c, const Idx& idx, uint64_t cap, int threads, Jumps& J) {
    double t0 = now_s();
    std::atomic<uint64_t> cand{0};
    // phase 1: all runs, cap 256
    std::vector<std::vector<uint32_t>> q1(threads), d1(threads), s1(threads);
    auto phase1 = [&](int tid, uint64_t lo, uint64_t hi) {
        for (uint64_t run = lo; run < hi; run++) {
            uint32_t L = idx.runs.len[run];
            if (L < 2) continue;
            uint64_t x = idx.lfstart[run];
            uint64_t m = idx.run_of(x), o = x - idx.runstart[m];
            if (o + L > idx.runs.len[m]) continue;
            cand++;
            uint32_t dest; uint64_t d = safe_walk(idx, run, 256, &dest);
            if (d >= 2) { q1[tid].push_back((uint32_t)run); d1[tid].push_back((uint32_t)d); s1[tid].push_back(dest); }
        }
    };
    std::vector<std::thread> th;
    uint64_t per = (c.r + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
        uint64_t lo = (uint64_t)t * per, hi = std::min(c.r, lo + per);
        if (lo < hi) th.emplace_back(phase1, t, lo, hi);
    }
    for (auto& t : th) t.join();
    // phase 2: extend runs that reached the phase-1 cap (satellite class)
    std::vector<uint32_t> ids;
    for (int t = 0; t < threads; t++)
        for (size_t i = 0; i < q1[t].size(); i++)
            if ((uint64_t)d1[t][i] >= 255) ids.push_back(q1[t][i]);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::atomic<uint64_t> censored{0};
    std::vector<uint32_t> xrun(ids.size()), xdist(ids.size()), xdest(ids.size());
    auto phase2 = [&](int tid) {
        for (uint64_t j = tid; j < ids.size(); j += threads) {
            uint64_t run = ids[j];
            uint32_t dest = 0; uint64_t d = safe_walk(idx, run, cap, &dest);
            if (d >= cap) censored++;
            xrun[j] = (uint32_t)run; xdist[j] = (uint32_t)d; xdest[j] = dest;
        }
    };
    th.clear();
    for (int t = 0; t < threads; t++) th.emplace_back(phase2, t);
    for (auto& t : th) t.join();
    // merge: phase-1 pointers, with extended values replacing censored ones
    std::vector<std::pair<uint32_t, uint32_t>> xt; // run -> index into x* arrays
    for (size_t j = 0; j < ids.size(); j++) xt.push_back({xrun[j], (uint32_t)j});
    std::sort(xt.begin(), xt.end());
    J.runs_q.clear(); J.dist.clear(); J.dest.clear();
    size_t ei = 0;
    for (int t = 0; t < threads; t++) {
        for (size_t i = 0; i < q1[t].size(); i++) {
            uint32_t run = q1[t][i];
            uint32_t d = d1[t][i], dst = s1[t][i];
            if (ei < xt.size() && xt[ei].first == run) {
                d = xdist[xt[ei].second]; dst = xdest[xt[ei].second]; ei++;
            }
            J.runs_q.push_back(run); J.dist.push_back(d); J.dest.push_back(dst);
        }
    }
    q1.clear(); d1.clear(); s1.clear();
    J.cap = cap; J.censored = censored;
    J.has.assign((c.r + 7) / 8, 0);
    for (auto run : J.runs_q) J.has[run >> 3] |= 1 << (run & 7);
    size_t total = J.runs_q.size();
    double mean_d = 0; uint64_t max_d = 0; for (auto d : J.dist) { mean_d += d; if (d > max_d) max_d = d; }
    if (total) mean_d /= total;
    printf("jump build: band-filter candidates=%llu pointers=%llu cap=%llu censored=%llu wall=%.1fs\n",
           (unsigned long long)cand.load(), (unsigned long long)total,
           (unsigned long long)cap, (unsigned long long)censored.load(), now_s() - t0);
    for (uint64_t lv = 2; lv <= cap; lv <<= 1) {
        uint64_t cnt = 0; for (auto d : J.dist) if ((uint64_t)d >= lv) cnt++;
        if (cnt)
            printf("  level>=%-6llu : %8llu runs (%5.2f%% of R)  space@8B+bitmap=%llu B (%.2f%% of R*8)\n",
                   (unsigned long long)lv, (unsigned long long)cnt, 100.0 * cnt / c.r,
                   (unsigned long long)(cnt * 8 + (c.r + 7) / 8), 100.0 * (cnt * 8.0 + (c.r + 7) / 8.0) / (c.r * 8.0));
    }
    printf("  pointer distance: mean=%.1f max=%llu\n", mean_d, (unsigned long long)max_d);
}

static void save_jumps(const char* path, const Jumps& J) {
    FILE* f = fopen(path, "wb");
    if (!f) die("open jumps out");
    uint64_t hdr[3] = {J.runs_q.size(), J.cap, J.censored};
    fwrite(hdr, 8, 3, f);
    fwrite(J.runs_q.data(), 4, J.runs_q.size(), f);
    fwrite(J.dest.data(), 4, J.runs_q.size(), f);
    fwrite(J.dist.data(), 4, J.runs_q.size(), f);
    fclose(f);
    printf("saved %llu pointers to %s\n", (unsigned long long)J.runs_q.size(), path);
}
static void load_jumps(const char* path, uint64_t r, Jumps& J) {
    FILE* f = fopen(path, "rb");
    if (!f) die("open jumps in");
    uint64_t hdr[3]; if (fread(hdr, 8, 3, f) != 3) die("jumps hdr");
    uint64_t cnt = hdr[0]; J.cap = hdr[1]; J.censored = hdr[2];
    J.runs_q.resize(cnt); J.dest.resize(cnt); J.dist.resize(cnt);
    if (fread(J.runs_q.data(), 4, cnt, f) != cnt) die("jumps read");
    if (fread(J.dest.data(), 4, cnt, f) != cnt) die("jumps read");
    if (fread(J.dist.data(), 4, cnt, f) != cnt) die("jumps read");
    fclose(f);
    J.has.assign((r + 7) / 8, 0);
    for (auto run : J.runs_q) J.has[run >> 3] |= 1 << (run & 7);
    printf("loaded %llu pointers from %s (cap=%llu censored=%llu)\n",
           (unsigned long long)cnt, path, (unsigned long long)J.cap,
           (unsigned long long)J.censored);
}

// ---------- verify mode ----------
static void mode_verify(const char* path, uint64_t samples, int threads, uint64_t window,
                        const char* fa, const char* rl, bool no_jumps, uint64_t cap,
                        const char* jumps_file, uint64_t minlevel, const char* text_cache,
                        uint64_t vstride, const char* valsfile) {
    Container c; c.load(path);
    Idx idx; idx.build(c);
    printf("reconstructing text via anchor-segment inverse BWT (%llu rows)...\n",
           (unsigned long long)c.n);
    std::vector<uint8_t> txt;
    double t0 = now_s();
    reconstruct_text(c, idx, txt, text_cache);
    printf("  reconstructed in %.1fs\n", now_s() - t0);
    struct Cand { const char* name; const char* path; bool rev; bool lines; };
    Cand cands[] = {
        {"fasta forward", fa, false, false},
        {"fasta revrec", fa, true, false},
        {"rl lines forward", rl, false, true},
        {"rl lines reversed", rl, true, true},
    };
    bool matched = false; const char* matched_name = "none";
    for (auto& cd : cands) {
        if (!cd.path || !*cd.path) continue;
        std::vector<uint8_t> t(c.n);
        bool ok = cd.lines ? fill_from_lines(cd.path, t, cd.rev, 0x1e)
                          : fill_from_fasta(cd.path, t, cd.rev, 0x1e);
        if (!ok) { printf("  candidate %-18s: length mismatch\n", cd.name); continue; }
        if (memcmp(t.data(), txt.data(), c.n) == 0) {
            matched = true; matched_name = cd.name;
            printf("  SOURCE MATCH: %s (byte-exact, all %llu bytes)\n",
                   cd.name, (unsigned long long)c.n);
            break;
        } else printf("  candidate %-18s: mismatch\n", cd.name);
    }
    if (!matched) die("no source candidate matched; refusing to verify");
    // optional denser anchor values (derived by sweep --derive)
    std::vector<uint64_t> vals;
    if (valsfile && *valsfile) {
        FILE* f = fopen(valsfile, "rb");
        if (!f) die("open vals");
        uint64_t hdr[3]; if (fread(hdr, 8, 3, f) != 3) die("vals hdr");
        if (hdr[0] != vstride) die("vals stride mismatch");
        uint64_t dn = hdr[1];
        if (dn != (c.r + vstride - 1) / vstride) die("vals count");
        vals.resize(dn);
        if (fread(vals.data(), 8, dn, f) != dn) die("vals read");
        fclose(f);
        idx.stride = vstride;
        printf("using derived anchor values: stride=%llu count=%llu (+%llu bytes vs member 11)\n",
               (unsigned long long)vstride, (unsigned long long)dn,
               (unsigned long long)(dn * 8));
    }
    Jumps J; bool use_jumps = !no_jumps;
    if (use_jumps) {
        if (jumps_file) load_jumps(jumps_file, c.r, J);
        else { printf("building safe-jump skiplist...\n"); build_jumps(c, idx, cap, threads, J); }
    }
    if (use_jumps) {
        uint64_t cnt = 0; for (auto d : J.dist) if ((uint64_t)d >= minlevel) cnt++;
        double sp = cnt * 8.0 + (double)((c.r + 7) / 8);
        printf("active pointers (d>=%llu): %llu  space: %.1f MB raw(8B+bitmap) = %.2f%% of the %llu B lite artifact\n",
               (unsigned long long)minlevel, (unsigned long long)cnt, sp / 1e6,
               100.0 * sp / (double)LITE_BYTES, (unsigned long long)LITE_BYTES);
    }
    std::vector<uint64_t> flat_steps(samples), jump_steps(samples), jump_hops(samples);
    std::atomic<uint64_t> mism{0}, sa_mism{0};
    std::atomic<double> flat_ns{0}, jump_ns{0};
    auto add_ns = [](std::atomic<double>& a, double v) { double cur = a.load(); while (!a.compare_exchange_weak(cur, cur + v)) {} };
    t0 = now_s();
    auto worker = [&](uint64_t lo, uint64_t hi) {
        Rng rng(0xC0FFEE + lo * 7919);
        for (uint64_t s = lo; s < hi; s++) {
            uint64_t row0 = rng.below(c.n);
            double tflat0 = now_s();
            // flat walk
            uint64_t run = idx.run_of(row0), off = row0 - idx.runstart[run];
            uint64_t steps = 0, anch = 0;
            while (true) {
                if (idx.at_anchor_tail(run, off)) { anch = run; break; }
                if (steps > c.n) die("flat walk runaway");
                idx.step(run, off); steps++;
            }
            uint64_t p;
            if (vals.empty()) p = (idx.anchor_val[anch / 1024] + steps) % c.n;
            else p = (vals[anch / vstride] + steps) % c.n;
            flat_steps[s] = steps;
            add_ns(flat_ns, (now_s() - tflat0) * 1e9);
            // text verify: BWT[LF^i(row0)] == T[(p-1-i) mod n]
            {
                uint64_t r2 = idx.run_of(row0), o2 = row0 - idx.runstart[r2];
                uint64_t W = std::min(steps, window);
                for (uint64_t i = 0; i < W; i++) {
                    if (idx.runs.sym[r2] != txt[(p + c.n - 1 - i) % c.n]) { mism++; break; }
                    idx.step(r2, o2);
                }
            }
            if (use_jumps) {
                double tjump0 = now_s();
                uint64_t run = idx.run_of(row0), off = row0 - idx.runstart[run];
                uint64_t steps2 = 0, hops = 0;
                while (true) {
                    if (idx.at_anchor_tail(run, off)) break;
                    if (steps2 > c.n) die("jump walk runaway");
                    uint32_t d, dst;
                    if (J.get(run, &d, &dst) && d >= minlevel) {
                        uint64_t nrow = (uint64_t)dst + off;
                        run = idx.run_of(nrow); off = nrow - idx.runstart[run];
                        steps2 += d; hops++;
                    } else { idx.step(run, off); steps2++; }
                }
                if (run != anch || steps2 != steps) {
                    printf("JUMP MISMATCH row %llu: flat steps=%llu anchor=%llu | jump steps=%llu anchor=%llu\n",
                           (unsigned long long)row0, (unsigned long long)steps,
                           (unsigned long long)anch, (unsigned long long)steps2,
                           (unsigned long long)run);
                    sa_mism++;
                }
                jump_steps[s] = steps2; jump_hops[s] = hops;
                add_ns(jump_ns, (now_s() - tjump0) * 1e9);
            }
        }
    };
    std::vector<std::thread> th; uint64_t per = (samples + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
        uint64_t lo = (uint64_t)t * per, hi = std::min(samples, lo + per);
        if (lo < hi) th.emplace_back(worker, lo, hi);
    }
    for (auto& t : th) t.join();
    double wall = now_s() - t0;
    auto q = [&](std::vector<uint64_t>& v, double p) {
        std::vector<uint64_t> s = v; std::sort(s.begin(), s.end());
        return s[std::min((size_t)(p * (double)samples), (size_t)samples - 1)];
    };
    double mf = 0; for (auto d : flat_steps) mf += d; mf /= samples;
    printf("verify: %llu sampled rows, window=%llu, text mismatches=%llu, anchor mismatches=%llu, wall=%.2fs (source=%s)\n",
           (unsigned long long)samples, (unsigned long long)window,
           (unsigned long long)mism.load(), (unsigned long long)sa_mism.load(), wall, matched_name);
    printf("  flat : mean=%.1f p50=%llu p99=%llu max=%llu | locate latency mean=%.0f ns (=%.1f ns/step)\n",
           mf, (unsigned long long)q(flat_steps, 0.5), (unsigned long long)q(flat_steps, 0.99),
           (unsigned long long)q(flat_steps, 1.0), flat_ns / (double)samples,
           (flat_ns / (double)samples) / mf);
    if (use_jumps) {
        double mj = 0, mh = 0;
        for (auto d : jump_steps) mj += d;
        for (auto d : jump_hops) mh += d;
        printf("  skiplist: mean=%.1f p50=%llu p99=%llu max=%llu (mean hops=%.2f) | locate latency mean=%.0f ns = %.2fx flat\n",
               mj / samples, (unsigned long long)q(jump_steps, 0.5),
               (unsigned long long)q(jump_steps, 0.99), (unsigned long long)q(jump_steps, 1.0),
               mh / samples, jump_ns / (double)samples,
               (flat_ns / std::max(1e-9, jump_ns.load())));
    }
}

static void mode_jumps(const char* path, uint64_t cap, int threads, const char* outfile,
                       uint64_t vstride) {
    Container c; c.load(path);
    Idx idx; idx.build(c);
    idx.stride = vstride;   // band-anchor safety checked against THIS termination stride
    printf("building jumps with band safety for stride=%llu termination\n",
           (unsigned long long)vstride);
    Jumps J; build_jumps(c, idx, cap, threads, J);
    if (outfile) save_jumps(outfile, J);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s CONTAINER info|sweep|entry|verify|jumps [opts]\n"
                        "  sweep [--stride N] [--threads T] [--out FILE]\n"
                        "  entry [--samples K] [--threads T]\n"
                        "  verify [--samples K] [--threads T] [--window W] [--fa F] [--rl F] [--no-jumps]"
                        " [--cap N] [--jumps FILE] [--minlevel M]\n"
                        "  jumps [--cap N] [--threads T] [--out FILE]\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];
    std::string mode = argv[2];
    uint64_t stride = 1024, samples = 4096, cap = 65536, window = 128, minlevel = 2;
    uint64_t vstride = 1024, derive = 0;
    int threads = 32;
    const char* out = nullptr, *jumps_file = nullptr, *text_cache = nullptr, *valsfile = nullptr;
    const char* fa = "/mnt/nvme3n1/erikg/sxgc-yeast/syng/yeast.fa";
    const char* rl = "/mnt/nvme3n1/erikg/sxgc-yeast/grl/yeast235.rl.txt";
    bool no_jumps = false;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { if (i + 1 >= argc) die("missing arg"); return argv[++i]; };
        if (a == "--stride") stride = strtoull(next(), 0, 0);
        else if (a == "--samples") samples = strtoull(next(), 0, 0);
        else if (a == "--threads") threads = atoi(next());
        else if (a == "--window") window = strtoull(next(), 0, 0);
        else if (a == "--out") out = next();
        else if (a == "--fa") fa = next();
        else if (a == "--rl") rl = next();
        else if (a == "--cap") cap = strtoull(next(), 0, 0);
        else if (a == "--minlevel") minlevel = strtoull(next(), 0, 0);
        else if (a == "--vstride") vstride = strtoull(next(), 0, 0);
        else if (a == "--derive") derive = strtoull(next(), 0, 0);
        else if (a == "--vals") valsfile = next();
        else if (a == "--jumps") jumps_file = next();
        else if (a == "--text-cache") text_cache = next();
        else if (a == "--no-jumps") no_jumps = true;
        else die("bad opt");
    }
    if (threads > 63) { fprintf(stderr, "clamping threads to 63\n"); threads = 63; }
    if (mode == "info") mode_info(path);
    else if (mode == "sweep") mode_sweep(path, stride, threads, out, derive, valsfile);
    else if (mode == "entry") mode_entry(path, samples, threads);
    else if (mode == "verify") mode_verify(path, samples, threads, window, fa, rl, no_jumps, cap,
                                            jumps_file, minlevel, text_cache, vstride, valsfile);
    else if (mode == "jumps") mode_jumps(path, cap, threads, out, vstride);
    else die("mode");
    return 0;
}
