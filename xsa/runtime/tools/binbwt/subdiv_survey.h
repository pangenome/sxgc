// subdiv_survey.h - SURVEY and PLAN for the wave subdivision (WAVE_SUBDIVISION.md).
//
// This header is included by scatter_sort.cpp only (one translation unit, one
// build rule). It carries exactly the two things the spec's first two boxes
// name, and nothing that reads the corpus except the survey's own ONE pass:
//
//   SURVEY  one deterministic sweep: exact byte counts per chunk per top-level
//           bin, a prefix cache (P bytes at each sampled position), a capped u32
//           run-length column, the 1 MiB page column. sha256 over the canonical
//           serialization of the blob; nothing is written to disc.
//   PLAN    in memory, zero corpus reads: rows(class), children(class, depth),
//           coverage(class) answerable from the survey for any depth <= P.
//
// Both are labelled ESTIMATES. The exactness comes from the descent's own
// collection of the class's positions (see scatter_sort.cpp).

#pragma once

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). ~60 lines, no dependency, streaming so the blob is
// never materialized: the digest is computed while the columns are serialized.
// ---------------------------------------------------------------------------
struct Sha256 {
    u32 h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    u8 buf[64];
    u64 bufLen = 0, total = 0;

    static inline u32 ror(u32 x, unsigned n) { return (x >> n) | (x << (32 - n)); }

    void block(const u8* p) {
        static const u32 K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
            0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
            0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
            0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
            0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
            0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
            0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
            0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
            0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        u32 w[64];
        for (unsigned i = 0; i < 16; ++i)
            w[i] = (u32(p[4*i]) << 24) | (u32(p[4*i+1]) << 16) | (u32(p[4*i+2]) << 8) | u32(p[4*i+3]);
        for (unsigned i = 16; i < 64; ++i) {
            const u32 s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3);
            const u32 s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (unsigned i = 0; i < 64; ++i) {
            const u32 S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 t1 = hh + S1 + ch + K[i] + w[i];
            const u32 S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            const u32 t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void* data, size_t len) {
        const u8* p = (const u8*)data;
        total += len;
        if (bufLen) {
            const size_t take = std::min<size_t>(64 - bufLen, len);
            std::memcpy(buf + bufLen, p, take);
            bufLen += take; p += take; len -= take;
            if (bufLen < 64) return;
            block(buf); bufLen = 0;
        }
        while (len >= 64) { block(p); p += 64; len -= 64; }
        if (len) { std::memcpy(buf, p, len); bufLen = len; }
    }
    void update_u64(u64 v) {
        u8 b[8];
        for (unsigned i = 0; i < 8; ++i) b[i] = u8((v >> (8 * i)) & 0xff);  // LE, fixed
        update(b, 8);
    }
    void update_u32(u32 v) {
        u8 b[4];
        for (unsigned i = 0; i < 4; ++i) b[i] = u8((v >> (8 * i)) & 0xff);
        update(b, 4);
    }
    std::string final_hex() {
        const u64 bits = total * 8;
        u8 pad[72];
        size_t n = 0;
        pad[n++] = 0x80;
        while ((bufLen + n) % 64 != 56) pad[n++] = 0;
        for (int i = 7; i >= 0; --i) pad[n++] = u8((bits >> (8 * i)) & 0xff);
        update(pad, n);
        char out[65];
        for (unsigned i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

// ---------------------------------------------------------------------------
// THE SURVEY. One pass, deterministic, journaled.
//
// SIZING (all bounded by the SAMPLE COUNT, never by n):
//   prefix cache   samples x P            (P = 128 -> 4 M x 128 B = 512 MB)
//   run column     samples x u32          (16 MB, capped at SS_SURVEY_RUN_CAP)
//   page column    samples x u32          (16 MB, 1 MiB page index)
//   chunk counts   SS_SURVEY_CHUNKS x 256 x u64 (512 KB, EXACT per chunk)
// The sample POSITIONS are not stored: position i is i * stride, by
// construction (the stride walk that builds the band plan's sample).
// ---------------------------------------------------------------------------
static constexpr u32 SS_SURVEY_P = 128;                       // prefix cache width
static constexpr u64 SS_SURVEY_PREFIX_BUDGET = 512ull << 20;  // the blob's prefix share
static constexpr u32 SS_SURVEY_RUN_CAP = 255;                 // run-length column cap
static constexpr u64 SS_SURVEY_CHUNKS = 256;                  // exact counts: chunks x bins
static constexpr u64 SS_SURVEY_PAGE = 1ull << 20;             // the page column's unit

struct Survey {
    u64 n = 0;              // FILE_N (the corpus bytes the text owns)
    u64 stride = 1;
    u64 samples = 0;
    u32 P = 0;              // prefix bytes cached at each sample (<= SS_SURVEY_P)
    u32 run_cap = 0;
    u64 chunks = 0, chunk_bytes = 0;
    std::vector<u8>  pre;   // samples x P
    std::vector<u32> run;   // samples
    std::vector<u32> page;  // samples
    std::vector<u64> cbin;  // chunks x 256, exact
    std::string sha;        // over the canonical serialization
    u64 blob_bytes = 0;
    double wall = 0.0;

    inline u64 pos(u64 i) const { return i * stride; }
    // The cached symbol at offset k of sample i: the END sentinel past the text.
    inline unsigned sym(u64 i, u64 k) const { return pre[size_t(i) * P + k]; }
};

// The survey's own pass. `fd` is the corpus; the text is read through
// SS_SCAN_CHUNK windows (never a faulting mmap walk) and remapped in place, so
// the cached symbols are in the SAME space sym_at() returns.
static void survey_run(Survey& S, int fd, u64 fileN, u64 sample_n, u64 threads) {
    const double t0 = now_sec();
    S.n = fileN;
    S.stride = std::max<u64>(1, (fileN + 1) / std::max<u64>(1, sample_n));
    S.samples = (fileN + S.stride - 1) / S.stride;      // positions 0,stride,... < fileN
    S.run_cap = SS_SURVEY_RUN_CAP;
    // The prefix width is the blob's only large term: cap it, then keep P whole.
    u32 P = SS_SURVEY_P;
    if (S.samples) {
        const u64 max_slots = SS_SURVEY_PREFIX_BUDGET / SS_SURVEY_P;
        if (S.samples > max_slots) P = u32(std::max<u64>(8, SS_SURVEY_PREFIX_BUDGET / S.samples));
    }
    S.P = P;
    S.chunks = std::min<u64>(SS_SURVEY_CHUNKS, std::max<u64>(1, fileN));
    S.chunk_bytes = (fileN + S.chunks - 1) / S.chunks;
    S.pre.assign(size_t(S.samples) * P, 0);
    S.run.assign(size_t(S.samples), 0);
    S.page.assign(size_t(S.samples), 0);
    S.cbin.assign(size_t(S.chunks) * 256, 0);
    S.blob_bytes = S.pre.size() + S.run.size() * 4 + S.page.size() * 4 +
                   S.cbin.size() * 8;
    const u64 nt = std::max<u64>(1, std::min<u64>(threads, std::max<u64>(1, fileN / (1u << 20) + 1)));
    std::vector<std::string> err(nt);
    std::vector<std::thread> ts;
    for (u64 t = 0; t < nt; ++t) {
        ts.emplace_back([&, t]() {
            // Chunk range owned by this thread: whole chunks, so every (chunk,
            // bin) cell is written by exactly one thread (determinism).
            const u64 c_lo = S.chunks * t / nt, c_hi = S.chunks * (t + 1) / nt;
            const u64 lo = std::min<u64>(fileN, c_lo * S.chunk_bytes);
            const u64 hi = std::min<u64>(fileN, c_hi * S.chunk_bytes);
            std::vector<u8> buf(SS_SCAN_CHUNK + 4096);
            u64 base = lo;
            while (base < hi) {
                const u64 chunk = std::min<u64>(SS_SCAN_CHUNK, hi - base);
                read_window(fd, base, chunk, buf);   // remapped in place
                // ---- exact counts, per chunk ----
                u64 pos = base;
                const u64 end = base + chunk;
                while (pos < end) {
                    const u64 c = pos / S.chunk_bytes;
                    const u64 seg_end = std::min<u64>(end, (c + 1) * S.chunk_bytes);
                    u64* row = &S.cbin[size_t(c) * 256];
                    const u8* p = buf.data() + (pos - base);
                    for (u64 i = 0; i < seg_end - pos; ++i) ++row[p[i]];
                    pos = seg_end;
                }
                // ---- the sample columns inside this window ----
                const u64 i_lo = (base + S.stride - 1) / S.stride;
                const u64 i_hi = (chunk ? (end - 1) / S.stride + 1 : i_lo);
                for (u64 i = i_lo; i < i_hi && i < S.samples; ++i) {
                    const u64 p = i * S.stride;
                    const u64 off = p - base;
                    S.page[size_t(i)] = u32(p / SS_SURVEY_PAGE);
                    // prefix cache: the next P symbols; past the text is END (0)
                    const u64 avail = fileN - p;
                    const u64 take = std::min<u64>(P, avail);
                    u8* dst = &S.pre[size_t(i) * P];
                    std::memcpy(dst, buf.data() + off, size_t(take));
                    if (take < P) std::memset(dst + take, SENTINEL_SYMBOL, size_t(P - take));
                    // run length of the symbol at p, capped; a run crossing the
                    // window edge is completed by a small direct read.
                    const u8 s0 = buf[off];
                    u64 r = 1;
                    while (r < u64(S.run_cap) && p + r < fileN) {
                        if (off + r < chunk) {
                            if (buf[off + r] != s0) break;
                        } else {
                            u8 tmp[1];
                            ssize_t got = ::pread(fd, tmp, 1, off_t(p + r));
                            if (got != 1) { err[t] = "survey run probe"; return; }
                            if ((REMAP ? REMAP[tmp[0]] : tmp[0]) != s0) break;
                        }
                        ++r;
                    }
                    S.run[size_t(i)] = u32(r);
                }
                base += chunk;
            }
        });
    }
    for (auto& th : ts) th.join();
    for (u64 t = 0; t < nt; ++t) if (!err[t].empty()) die(err[t]);

    // ---- the blob's sha256, over a canonical (LE) serialization ----
    Sha256 h;
    h.update_u64(S.n); h.update_u64(S.samples); h.update_u64(S.stride);
    h.update_u32(S.P); h.update_u32(S.run_cap);
    h.update_u64(S.chunks); h.update_u64(S.chunk_bytes);
    for (u64 v : S.cbin) h.update_u64(v);
    h.update(S.pre.data(), S.pre.size());
    for (u32 v : S.run) h.update_u32(v);
    for (u32 v : S.page) h.update_u32(v);
    S.sha = h.final_hex();
    S.wall = now_sec() - t0;
    // THE LINE IS A FUNCTION OF THE CORPUS AND THE SAMPLE SIZE ONLY: no timing, no
    // pid, no thread count. G7 compares two of these strings AND the blob sha, and
    // a wall clock inside the line would make that comparison a coin toss. The
    // timing is journaled on its own line.
    std::fprintf(stderr,
        "SS_SURVEY samples=%" PRIu64 " stride=%" PRIu64 " prefix_bytes=%u run_cap=%u pages=%zu"
        " chunks=%" PRIu64 " chunk_bytes=%" PRIu64 " blob_bytes=%" PRIu64
        " sha256=%s bytes_read=%" PRIu64 " rule=one_pass_all_top_level_bins\n",
        S.samples, S.stride, S.P, S.run_cap, S.page.size(), S.chunks, S.chunk_bytes,
        S.blob_bytes, S.sha.c_str(), S.n);
    std::fprintf(stderr, "SS_SURVEY_TIME wall=%.2f threads_note=does_not_enter_the_line\n", S.wall);
}

// ---------------------------------------------------------------------------
// THE PLAN. In memory. ZERO corpus reads: every class query is answered from
// the survey's prefix cache (depths <= P), which is what makes a census free.
//
// A class is a key PREFIX (off,len into the arena) and its members are the
// sample positions whose cached prefix matches, held as a contiguous subrange
// of a permutation of sample indices, so the plan's own descent is a partition
// walk and every level costs O(samples) once.
// ---------------------------------------------------------------------------
static constexpr double SS_COVERAGE_LOCAL_BELOW = 0.25;   // the strategy line (spec)
static constexpr unsigned SS_STRAT_LOCAL = 0, SS_STRAT_GLOBAL = 1;

struct Plan {
    const Survey* s = nullptr;
    std::vector<u8> arena;                // the digit paths (dynamic: no depth cap)
    std::vector<u32> perm;                // sample indices, partitioned by the descent
    u64 cache_misses = 0, nodes = 0, nodes_leaf = 0;

    // rows(class) for the class the sample subrange [lo,hi) describes, scaled by
    // the stride. `exact_rows` (root only) comes from the survey's chunk counts.
    inline u64 est_rows(u64 sample_count, u64 stride) const { return sample_count * stride; }

    // coverage(class) = #distinct 1 MiB pages among sampled class positions x stride / n
    double coverage(size_t lo, size_t hi) const;
    // children(class, depth): the 256 child counts at the class's next offset.
    void children(size_t lo, size_t hi, u64 off, u64 stride, u64* counts) const;
    // The cached digit of sample `i` at offset `off`; false past the cache.
    inline bool digit(u32 i, u64 off, unsigned& c) const {
        if (off >= s->P) return false;
        c = s->sym(i, off);
        return true;
    }
};

double Plan::coverage(size_t lo, size_t hi) const {
    if (lo >= hi || !s->n) return 0.0;
    // The subrange is a range of perm (sample INDICES); the page column is keyed
    // by sample index, so the gather goes through the permutation.
    std::vector<u32> pg;
    pg.reserve(hi - lo);
    for (size_t i = lo; i < hi; ++i) pg.push_back(s->page[size_t(perm[i])]);
    std::sort(pg.begin(), pg.end());
    u64 distinct = 0;
    for (size_t i = 0; i < pg.size(); ++i)
        if (i == 0 || pg[i] != pg[i - 1]) ++distinct;
    return double(distinct) * double(s->stride) / double(s->n);
}

void Plan::children(size_t lo, size_t hi, u64 off, u64 stride, u64* counts) const {
    for (unsigned c = 0; c < 256; ++c) counts[c] = 0;
    for (size_t i = lo; i < hi; ++i) {
        unsigned c = 0;
        if (!digit(perm[i], off, c)) continue;   // past the cache: uncounted, journaled
        counts[c] += stride;
    }
}
