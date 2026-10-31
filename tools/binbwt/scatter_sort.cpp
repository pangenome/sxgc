// scatter_sort - THE TAPE MACHINE, prototype.
//
// Replaces "sort positions with random-access LCE at every level" with
// "scatter rows into key-ordered bands once, then sort each band in memory".
//
//   scatter_sort --corpus C --out SA [--ram-gb G] [--threads T]
//                [--sample N] [--scratch DIR] [--keep-rows]
//
// Pipeline:
//   1. SAMPLE   positional stride sample -> recursive 24-byte windows ->
//               prefix BOUNDARIES. A sample suffices: bands only have to be
//               balanced, and every row is routed by a key COMPARISON, so a
//               sampling error can only unbalance a band, never misorder a row.
//   2. SCATTER  ONE sequential pass over the tape (T threads over contiguous
//               text ranges). Each row's 24-byte key is compared against the
//               boundaries and the row (key, position) is appended to that
//               band's file. Text is read exactly once and never again during
//               sorting; the sort is pure in-memory work on fixed-width rows.
//   3. SORT     per band, in RAM: compare keys as two big-endian u64s; only
//               rows whose 24-byte keys are EQUAL fall through to a real
//               suffix comparison. On web text that tie set is ~1% (and 97% of
//               that is uniform-byte runs) - see tools/binbwt/kgram_survey.
//   4. EMIT     bands are key ranges, so emitting them in boundary order IS
//               suffix order: the output is a plain concatenation.
//
// Output: the SA stream in the emitted-SSA format (48-byte header + u64/rank),
// which is exactly what --sa-sample-k 1 writes, so `sa_check` and the banked
// containers can verify it directly.
//
// THE LCP COLUMN IS A BAND STREAM, NOT A FILE. On the tape route (--tape-bands)
// every band carries its own LCP slice next to its SA stream - band_<i>.lcp,
// written before band_<i>.done - with the same lifecycle as the band itself:
// slice published -> consumer folds it -> slice deleted -> band_<i>.lcp.consumed.
// The merge that sorts a band already knows every adjacent LCP (it IS the
// comparison), so the whole column costs one store per row on top of the sort it
// was doing anyway, and residency stays K bands of slice, never n: a single
// 1.34n column file cannot be opened by a streaming consumer that only needs its
// own band (and at 10 GB that file was 13.39 GB resident against a 2n law).
//
// Convention: dollar (plain suffix order, shorter suffix smaller) - established
// empirically against the bin machine's own emitted SA on a 4 MB slice.

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <fstream>
#include <sstream>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

static void die(const std::string& m) {
    std::fprintf(stderr, "scatter_sort: %s\n", m.c_str());
    std::exit(2);
}
static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}
static inline u64 bswap64(u64 x) { return __builtin_bswap64(x); }

// One keyed row carries 24 real bytes and a packed position/symbol.
// Finite tails are held separately and never receive a padded key.
struct Row {
    u64 klo, khi, kq, pos;   // 24-byte key (3 big-endian words) + packed pos/sym
};

static const u8* T = nullptr;
// N includes one structural END suffix at FILE_N. Its realization is chosen
// by preflight; comparisons exhaust the finite corpus before accessing END.
// Byte transport cells remain eight bits; position zero identifies the END BWT row.
static u64 N = 0, FILE_N = 0;
static u64 KEY_N = 0; // only positions with 24 real bytes may enter the key sorter
static constexpr u8 SENTINEL_SYMBOL = 0;
static const u8* REMAP = nullptr;   // optional: raw byte -> remapped symbol
// Two raw bytes -> their two remapped bytes, packed big-endian. A tie comparison
// then consumes 2 bytes per table lookup instead of 1.
static uint16_t PAIRMAP[65536];
static bool PAIRMAP_READY = false;
static inline void build_pairmap() {
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned b = 0; b < 256; ++b) {
            const unsigned ma = REMAP ? REMAP[a] : a;
            const unsigned mb = REMAP ? REMAP[b] : b;
            PAIRMAP[(a << 8) | b] = (uint16_t)((ma << 8) | mb);
        }
    PAIRMAP_READY = true;
}
static inline u64 pack_sym(u64 p, u8 sym) { return p | (u64(sym) << 48); }
static inline u64 unpack_pos(u64 v) { return v & ((1ull << 48) - 1); }

// A complete 24-byte key, big-endian packed. Tail positions are forbidden.
static inline void key_at(u64 p, u64& klo, u64& khi, u64& kq) {
    if (p >= KEY_N) die("tail position passed to key_at");
    u8 b[24];
    std::memcpy(b, T + p, 24);
    if (REMAP) for (u64 i = 0; i < 24; ++i) b[i] = REMAP[b[i]];
    u64 lo, hi, q;
    std::memcpy(&lo, b, 8);
    std::memcpy(&hi, b + 8, 8);
    std::memcpy(&q, b + 16, 8);
    klo = bswap64(lo);
    khi = bswap64(hi);
    kq = bswap64(q);
}

// Full suffix comparison in dollar mode (shorter is smaller).
// `from` is safe to pass as 24: the comparator only reaches here when the first
// 24 key bytes are EQUAL, so comparing them again is pure waste (and it is the
// dominant cost of the tie set).
// The same comparison, but it also reports HOW FAR the two suffixes agree. The
// caller is the merge sort, where that number IS the adjacent-rank LCP - the value
// the finish otherwise re-derives later with 1.28e11 random text reads.
static inline int suf_cmp_lcp(u64 a, u64 b, u64 from, u64& lcp) {
    const u64 cap = std::min(FILE_N - a, FILE_N - b);
    lcp = from;
    u64 i = from;
    if (PAIRMAP_READY) {
        while (i + 2 <= cap) {
            const uint16_t x = PAIRMAP[(unsigned(T[a + i]) << 8) | T[a + i + 1]];
            const uint16_t y = PAIRMAP[(unsigned(T[b + i]) << 8) | T[b + i + 1]];
            if (x != y) {
                lcp = i + ((x >> 8) == (y >> 8) ? 1 : 0);
                return ((x >> 8) == (y >> 8)) ? (((x & 0xff) < (y & 0xff)) ? -1 : 1)
                                              : ((x < y) ? -1 : 1);
            }
            i += 2;
        }
        for (; i < cap; ++i) {
            const u8 xb = REMAP ? REMAP[T[a + i]] : T[a + i];
            const u8 yb = REMAP ? REMAP[T[b + i]] : T[b + i];
            if (xb != yb) { lcp = i; return xb < yb ? -1 : 1; }
        }
        lcp = cap;
        if (a == b) return 0;
        return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
    }
    for (; i < cap; ++i) {
        const u8 xb = REMAP ? REMAP[T[a + i]] : T[a + i];
        const u8 yb = REMAP ? REMAP[T[b + i]] : T[b + i];
        if (xb != yb) { lcp = i; return xb < yb ? -1 : 1; }
    }
    lcp = cap;
    if (a == b) return 0;
    return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
}

static inline int suf_cmp_from(u64 a, u64 b, u64 from) {
    const u64 cap = std::min(FILE_N - a, FILE_N - b);
    if (from >= cap) {
        if (a == b) return 0;
        return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
    }
    if (PAIRMAP_READY) {
        u64 i = from;
        while (i + 2 <= cap) {
            const uint16_t x = PAIRMAP[(unsigned(T[a + i]) << 8) | T[a + i + 1]];
            const uint16_t y = PAIRMAP[(unsigned(T[b + i]) << 8) | T[b + i + 1]];
            if (x != y) return x < y ? -1 : 1;
            i += 2;
        }
        for (; i < cap; ++i) {
            const u8 xb = REMAP ? REMAP[T[a + i]] : T[a + i];
            const u8 yb = REMAP ? REMAP[T[b + i]] : T[b + i];
            if (xb != yb) return xb < yb ? -1 : 1;
        }
        if (a == b) return 0;
        return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
    }
    const u64 cap0 = cap;
    if (!REMAP) {   // fast path: identity alphabet, 8 bytes at a time
        u64 i = from;
        while (i + 8 <= cap) {
            u64 x, y;
            std::memcpy(&x, T + a + i, 8);
            std::memcpy(&y, T + b + i, 8);
            if (x != y) {
                u8 xs[8], ys[8];
                std::memcpy(xs, &x, 8);
                std::memcpy(ys, &y, 8);
                for (int j = 0; j < 8; ++j)
                    if (xs[j] != ys[j]) return xs[j] < ys[j] ? -1 : 1;
            }
            i += 8;
        }
        for (; i < cap; ++i)
            if (T[a + i] != T[b + i]) return T[a + i] < T[b + i] ? -1 : 1;
    } else {        // product alphabet: compare remapped symbols
        for (u64 i = from; i < cap0; ++i) {
            const u8 xb = REMAP[T[a + i]], yb = REMAP[T[b + i]];
            if (xb != yb) return xb < yb ? -1 : 1;
        }
    }
    if (a == b) return 0;
    return (FILE_N - a) < (FILE_N - b) ? -1 : 1;
}



// ---------------------------------------------------------------------------
// THE POOL: threads and buffers live for the WHOLE run.
//
// The first wave implementation spawned `threads` threads per wave and allocated
// `nparts x nbands` staging buffers per wave. With ~20 waves at the 2n cap that is
// 20 thread-spawn storms and 20 rounds of allocator churn - measured 34% of the
// wall as sys time. A wave should cost its work and nothing else.
// ---------------------------------------------------------------------------
struct Pool {
    std::vector<std::thread> ts;
    std::mutex mu;
    std::condition_variable cv, done;
    std::function<void(unsigned)> fn;
    u64 gen = 0;
    unsigned pending = 0;
    bool stop = false;
    explicit Pool(unsigned n) {
        for (unsigned i = 0; i < n; ++i) ts.emplace_back([this, i] { loop(i); });
    }
    ~Pool() {
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cv.notify_all();
        for (auto& t : ts) t.join();
    }
    void run(const std::function<void(unsigned)>& f) {
        {
            std::unique_lock<std::mutex> lk(mu);
            fn = f;
            ++gen;
            pending = (unsigned)ts.size();
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lk(mu);
        done.wait(lk, [&] { return pending == 0; });
    }
    void loop(unsigned i) {
        u64 seen = 0;
        for (;;) {
            std::function<void(unsigned)> f;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return stop || gen != seen; });
                if (stop) return;
                seen = gen;
                f = fn;
            }
            f(i);
            {
                std::lock_guard<std::mutex> lk(mu);
                if (--pending == 0) done.notify_all();
            }
        }
    }
};

// ---------------------------------------------------------------------------
// WAVES: the scratch cap is a DESIGN CONSTRAINT, not a hope.
//
// The first version wrote every band to disc before sorting any of them, so peak
// scratch was the whole intermediate: 32 B x n = 42 TB at pile. Wrong by design.
// Now the work runs in WAVES: each wave owns a contiguous KEY RANGE big enough
// that (rows in wave) x (row bytes) <= scratch_cap, and each wave's rows are
// written, sorted, emitted and DELETED before the next wave is written. Peak
// scratch is therefore one wave, i.e. the cap, and the corpus is re-read once
// per wave (190 s at pile - nothing against the win).
//
// Order is preserved because a wave is a key range and waves run in ascending key
// order, so concatenating the waves' outputs IS suffix order.
// ---------------------------------------------------------------------------
// =========================== THE LCP-CARRYING MERGE ==========================
// Sorting a band with a comparison sort throws away the one thing the finish
// needs: how far each adjacent pair agreed. A bottom-up merge does not - the
// comparison that decides who goes next IS the cross-run LCP, and the classic
// merge identity finishes the job:
//
//   emitting e from run A, having last emitted from B:
//       lcp(e) = min( internal_lcp_of_B_head , cross_lcp )
//
// so the whole adjacent-LCP array costs ONE STORE per element on top of the
// comparisons the sort was making anyway. No inversion, no random scatter, no
// re-derivation: the finish's 1.28e11 random reads become a sequential stream.
//
// v[] is sorted in place; out_lcp[k] = LCP(v[k-1], v[k]) (out_lcp[0] = 0).
// Values >= 255 are recorded in `esc` as (rank, value).
// Exact row comparison that also yields the LCP. The three key words are
// big-endian, so an integer compare IS the byte order, and clz of the xor gives
// the number of leading EQUAL bytes for free. Only a full 24-byte tie falls
// through to the text.
static inline int row_cmp_lcp(const Row& a, const Row& b, u64& lcp) {
    if (a.klo != b.klo) {
        lcp = u64(__builtin_clzll(a.klo ^ b.klo)) >> 3;
        return a.klo < b.klo ? -1 : 1;
    }
    if (a.khi != b.khi) {
        lcp = 8 + (u64(__builtin_clzll(a.khi ^ b.khi)) >> 3);
        return a.khi < b.khi ? -1 : 1;
    }
    if (a.kq != b.kq) {
        lcp = 16 + (u64(__builtin_clzll(a.kq ^ b.kq)) >> 3);
        return a.kq < b.kq ? -1 : 1;
    }
    return suf_cmp_lcp(unpack_pos(a.pos), unpack_pos(b.pos), 24, lcp);
}

static void merge_sort_lcp(std::vector<Row>& v, std::vector<uint8_t>& out_lcp,
                           std::vector<std::pair<u64, u64>>& esc, u64 rank_base) {
    const size_t n = v.size();
    if (n < 2) { out_lcp.assign(n ? n : 0, 0); return; }
    std::vector<Row> buf(n);
    // FULL u64 values are carried through the levels: an element's index in an
    // intermediate buffer is NOT its final rank, so recording escapes at that point
    // attached them to the wrong ranks (measured: rank 279 with an escape of 297
    // against a true value of 14). Convert once, at the end, by final rank.
    std::vector<u64> lcp_a(n, 0), lcp_b(n, 0);
    Row* a = v.data();
    Row* b = buf.data();
    u64* la = lcp_a.data();
    u64* lb = lcp_b.data();
    for (size_t w = 1; w < n; w <<= 1) {
        for (size_t i0 = 0; i0 < n; i0 += 2 * w) {
            const size_t i1 = std::min(i0 + w, n), i2 = std::min(i0 + 2 * w, n);
            if (i1 >= i2) { for (size_t k = i0; k < i2; ++k) { b[k] = a[k]; lb[k] = la[k]; } continue; }
            size_t i = i0, j = i1, k = i0;
            int prev = 0;
            u64 l = 0;   // LCP(last emitted, other run's current head) == last cross
            while (i < i1 && j < i2) {
                u64 cross = 0;
                const int c = row_cmp_lcp(a[i], a[j], cross);
                if (c < 0) {
                    b[k] = a[i];
                    lb[k] = (k == i0 || prev == 1) ? la[i] : l;
                    prev = 1; ++i;
                } else {
                    b[k] = a[j];
                    lb[k] = (k == i0 || prev == 2) ? la[j] : l;
                    prev = 2; ++j;
                }
                l = cross;
                ++k;
            }
            while (i < i1) { b[k] = a[i]; lb[k] = (prev == 1) ? la[i] : l; prev = 1; ++i; ++k; }
            while (j < i2) { b[k] = a[j]; lb[k] = (prev == 2) ? la[j] : l; prev = 2; ++j; ++k; }
        }
        std::swap(a, b);
        std::swap(la, lb);
    }
    if (a != v.data()) {
        std::memcpy(v.data(), a, n * sizeof(Row));
        std::memcpy(lcp_a.data(), la, n * sizeof(u64));
    }
    out_lcp.assign(n, 0);
    for (size_t k = 0; k < n; ++k) {
        const u64 val = lcp_a[k];
        if (val >= 255) { esc.emplace_back(rank_base + k, val); out_lcp[k] = 255; }
        else out_lcp[k] = uint8_t(val);
    }
}

// A fence is a finite prefix, never padded with a corpus symbol.
struct TapeFence { u64 pos, depth; bool after=false; };
static int prefix_cmp(u64 a, u64 b, u64 depth) {
    const u64 cap = std::min({depth, FILE_N-a, FILE_N-b});
    for (u64 i=0; i<cap; ++i) {
        unsigned x=REMAP ? REMAP[T[a+i]] : T[a+i];
        unsigned y=REMAP ? REMAP[T[b+i]] : T[b+i];
        if (x != y) return x < y ? -1 : 1;
    }
    if (cap == depth || a == b) return 0;
    return FILE_N-a < FILE_N-b ? -1 : 1;
}
struct TapeLeaf { TapeFence first; u64 samples; bool analytic; };
// Only oversized equal-prefix nodes descend. Multiplicity is retained: every
// entry is a sampled POSITION, not a distinct key. No runtime subdivision.
static std::vector<TapeLeaf> plan_tape(std::vector<u64> positions, u64 stride,
                                     u64 target, u64 wave, u64 trailing) {
    std::vector<TapeLeaf> leaves;
    const u64 goal=std::max<u64>(1, target*4/5);
    std::function<void(size_t,size_t,u64)> descend;
    descend = [&](size_t lo, size_t hi, u64 depth) {
        std::sort(positions.begin()+lo, positions.begin()+hi,
                  [=](u64 a,u64 b){return prefix_cmp(a,b,depth)<0;});
        size_t pending=lo;
        auto emit = [&](size_t a,size_t b,bool analytic=false) {
            if (a<b) leaves.push_back({{positions[a],depth},u64(b-a),analytic});
        };
        for (size_t a=lo; a<hi;) {
            size_t b=a+1;
            while (b<hi && prefix_cmp(positions[a],positions[b],depth)==0) ++b;
            if ((b-a)*stride > goal) {
                emit(pending,a); pending=b;
                // Prove the whole prefix class from the CORPUS, not samples.
                // A matching internal run disqualifies analytic termination.
                bool analytic=true;
                for (size_t j=a;j<b;++j) analytic &= positions[j]>=trailing;
                if (analytic) {
                    for (u64 p=0;p<trailing;++p) {
                        if (prefix_cmp(p,positions[a],depth)==0) { analytic=false; break; }
                    }
                }
                if (analytic) {
                    emit(a,b,true);
                    // Close this exact prefix class, including any unsampled
                    // gap before the next sampled key in a normal empty leaf.
                    leaves.push_back({{positions[a],depth,true},0,false});
                }
                else if (depth==192) {
                    if ((b-a)*stride>target) std::fprintf(stderr,"TAPE_LEAF_AT_CAP rows=%llu wave=%llu depth=192\n",
                        (unsigned long long)((b-a)*stride),(unsigned long long)wave);
                    emit(a,b);
                } else descend(a,b,depth+24);
            } else if ((b-pending)*stride > goal) {
                emit(pending,a); pending=a;
            }
            a=b;
        }
        emit(pending,hi);
    };
    if (!positions.empty()) descend(0,positions.size(),24);
    // The first range normally extends below its sampled key. An analytic
    // range must instead start at its proved prefix, leaving unsampled lower
    // keys (and short finite tails) in an ordinary zero-estimate range.
    if (!leaves.empty() && leaves.front().analytic)
        leaves.insert(leaves.begin(),{{0,24},0,false});
    if (leaves.empty()) leaves.push_back({{0,24},0,false});
    u64 histogram[8]={}, largest=0;
    for (const auto& leaf:leaves) {
        ++histogram[leaf.first.depth/24-1];
        largest=std::max(largest,leaf.samples*stride);
    }
    std::fprintf(stderr,"TAPE_DEPTH_HISTOGRAM wave=%llu",(unsigned long long)wave);
    for (unsigned i=0;i<8;++i) std::fprintf(stderr," depth%u=%llu",24*(i+1),(unsigned long long)histogram[i]);
    std::fprintf(stderr," max_estimated_rows=%llu target=%llu plan_target=%llu\n",
        (unsigned long long)largest,(unsigned long long)target,(unsigned long long)goal);
    return leaves;
}
static void sort_tape_keys(std::vector<Row>& rows, u64) {
    std::sort(rows.begin(),rows.end(),[](const Row& a,const Row& b) {
        if (a.klo!=b.klo) return a.klo<b.klo;
        if (a.khi!=b.khi) return a.khi<b.khi;
        if (a.kq!=b.kq) return a.kq<b.kq;
        return suf_cmp_from(unpack_pos(a.pos),unpack_pos(b.pos),24)<0;
    });
}

int main(int argc, char** argv) {
    std::string corpus, out, scratch = "/tmp/ss_scratch", lcpOut, isaOut;
    u64 isaK = 1024;
    double ram_gb = 1.0;
    double scratch_cap_gb = 0.0;      // 0 = 2n (the operator's law)
    u64 threads = 32, par_bands = 1, sample_n = 4000000;
    bool keep_rows = false;
    std::string selftestOracle;
    std::string remapPath, sort_mode = "std", tapeBands, orderSample;
    bool noLcpSlices = false;   // A/B arm: the finish walks text (XSA_LCE_VERIFY=1)
    u64 sampleLo = 0, sampleCount = 4096;
    u64 pendingBands = 8;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() { if (i + 1 >= argc) die("missing value for " + a); return std::string(argv[++i]); };
        if (a == "--order-sample-out") orderSample = nxt();
        else if (a == "--order-sample-lo") sampleLo = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--order-sample-count") sampleCount = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--tape-bands") tapeBands = nxt();
        else if (a == "--pending-bands") pendingBands = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--selftest") selftestOracle = nxt();
        else if (a == "--corpus") corpus = nxt();
        else if (a == "--out") out = nxt();
        else if (a == "--scratch") scratch = nxt();
        else if (a == "--ram-gb") ram_gb = std::atof(nxt().c_str());
        else if (a == "--scratch-cap-gb") scratch_cap_gb = std::atof(nxt().c_str());
        else if (a == "--threads") threads = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--par-bands") par_bands = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--sample") sample_n = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--keep-rows") keep_rows = true;
        else if (a == "--sort") sort_mode = nxt();
        else if (a == "--remap") remapPath = nxt();
        else if (a == "--lcp-out") lcpOut = nxt();
        else if (a == "--no-lcp-slices") noLcpSlices = true;
        else if (a == "--isa-sample-out") isaOut = nxt();
        else if (a == "--isa-sample-k") isaK = std::strtoull(nxt().c_str(), nullptr, 10);
        else die("unknown arg " + a);
    }
    (void)sort_mode;
    if (corpus.empty() || (out.empty() && tapeBands.empty())) die("--corpus and --out required");

    if (!selftestOracle.empty() && !tapeBands.empty()) die("--selftest requires --out");
    if (!tapeBands.empty() && (!pendingBands || keep_rows))
        die("tape bands require K>0 and no keep-rows");
    int fd = ::open(corpus.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("cannot open/stat corpus");
    FILE_N = (u64)st.st_size;
    if (!FILE_N) die("empty corpus");
    N = FILE_N + 1; // the END suffix exists in both realizations
    KEY_N = FILE_N >= 24 ? FILE_N - 23 : 0;
    T = (const u8*)::mmap(nullptr, (size_t)FILE_N, PROT_READ, MAP_PRIVATE, fd, 0);
    if (T == MAP_FAILED) die("mmap corpus");
    ::madvise((void*)T, (size_t)FILE_N, MADV_WILLNEED);
    std::vector<u8> remapBuf;
    if (!remapPath.empty()) {
        int rf = ::open(remapPath.c_str(), O_RDONLY);
        if (rf < 0) die("cannot open remap");
        remapBuf.resize(256);
        if (::read(rf, remapBuf.data(), 256) != 256) die("remap short read");
        ::close(rf);
        REMAP = remapBuf.data();
    }
    // END is virtual: byte cells carry only corpus bytes. SA position zero
    // identifies the exceptional BWT row, independently of its transport cell.
    bool used[256]{};
    for (u64 p = 0; p < FILE_N; ++p) used[T[p]] = true;
    unsigned present = 0;
    for (unsigned c = 0; c < 256; ++c) present += used[c];
    if (REMAP) {
        bool seen[256]{};
        int previous = -1;
        for (unsigned c = 0; c < 256; ++c) {
            if (seen[REMAP[c]]) die("remap must be a permutation");
            seen[REMAP[c]] = true;
            if (used[c]) {
                if (int(REMAP[c]) <= previous) die("remap does not preserve byte order");
                previous = REMAP[c];
            }
        }
    }
    // The inherited finisher still interprets cell zero as END. Refuse an
    // ambiguous tape until that consumer supports the structural END row.
    if (!tapeBands.empty())
        for (unsigned c = 0; c < 256; ++c)
            if (used[c] && (REMAP ? REMAP[c] : c) == 0)
                die("tape consumer reserves cell zero; virtual END consumer required");
    if (!tapeBands.empty() && !lcpOut.empty())
        die("--lcp-out is the one-file column (non-tape); tape bands emit per-band slices");
    // THE DEFAULT PATH: the tape route always produces the LCP column, as one
    // slice per band. --no-lcp-slices is the A/B arm that reproduces the old
    // LCE-verify route (the finish then walks the corpus for every endpoint).
    const bool lcpOn = tapeBands.empty() ? !lcpOut.empty() : !noLcpSlices;
    std::fprintf(stderr, "SS_LCP_ARM lcp_source=%s slices=%d\n",
                 lcpOn ? (tapeBands.empty() ? "column-file" : "sorter-bands") : "none",
                 int(lcpOn && !tapeBands.empty()));
    std::fprintf(stderr, "END_SYMBOL mode=virtual position=%" PRIu64 " multiplicity=1 rank=0\n", FILE_N);
    std::fprintf(stderr, "END_FORMAT k=%u ends_in_sentinel=1 end_code=none\nTAIL_GROUP rows=%" PRIu64 "\n", present, N - KEY_N);
    build_pairmap();
    // SYMBOL ACCESS ON THE SENTINEL-TERMINATED TEXT: the corpus bytes through the
    // product remap, and index FILE_N = the sentinel symbol 0.
    auto sym_at = [](u64 p) -> unsigned {
        if (p >= FILE_N) return SENTINEL_SYMBOL;
        return REMAP ? REMAP[T[p]] : T[p];
    };

    const u64 row_bytes = sizeof(Row);            // the WIRE row (band part files)
    // THE IN-RAM FOOTPRINT OF ONE SORTED ROW. The LCP-carrying merge sorts with a
    // ping-pong buffer and carries two u64 LCP arrays, and the band holds its u8
    // column as well: 2*32 + 16 + 1 = 81 bytes per row, not the 32 the wire row
    // suggests. Sizing bands from the wire row alone would make the LCP arm use
    // 2.5x the arena the plan priced - the exact way the 2n law gets broken.
    const u64 sort_row_bytes = lcpOn ? (2 * row_bytes + 16 + 1) : row_bytes;
    const u64 ram_bytes = (u64)(ram_gb * 1e9);
    if (par_bands < 1) par_bands = 1;
    u64 rows_per_band = std::max<u64>(1, ram_bytes / (sort_row_bytes * par_bands));
    // THE CAP CAN BE THE BINDING CONSTRAINT: with a 2n scratch cap a wave is often
    // SMALLER than one RAM-sized band, which left each wave with a single band and
    // therefore a single-threaded sort - 20 waves of serial sorting, measured at
    // 370 s for a 1 GB corpus against 40 s unwaved. Bands exist to bound RAM, so
    // size them from whichever budget is smaller: RAM/par or the wave/par.
    // THE SCRATCH LAW: default cap is 2n bytes.
    const u64 cap_bytes = scratch_cap_gb > 0.0 ? (u64)(scratch_cap_gb * 1e9) : 2 * N;
    if (!tapeBands.empty() && (cap_bytes > 2 * N || N >= (1ull << 48)))
        die("tape bands require wave cap <= 2n and n < 2^48");
    // Waves are filled greedily by byte class, so a wave can overshoot the target
    // by one class (measured 2.06n against a 2.0n cap). Fill to 95% of the cap and
    // the overshoot stays inside the law instead of breaking it.
    u64 rows_per_wave = std::max<u64>(1, (cap_bytes - cap_bytes / 20) / row_bytes);
    // BANDS MUST OUTNUMBER WORKERS. With bands == par_bands the work-stealing
    // queue is a 1:1 map: each worker gets exactly one band and the wall is set by
    // the fattest one (measured: 2.43 s of sort summed in a 1.14 s wave = ~2 cores
    // busy of 8). Oversubscribe 4x so stealing can balance the fat bands.
    if (rows_per_wave / (4 * par_bands) < rows_per_band)
        rows_per_band = std::max<u64>(1, rows_per_wave / (4 * par_bands));
    u64 waves = (N + rows_per_wave - 1) / rows_per_wave;
    if (waves < 1) waves = 1;

    std::fprintf(stderr,
        "SS_START n=%" PRIu64 " row_bytes=%zu sort_row_bytes=%" PRIu64 " cap_bytes=%" PRIu64
        " (%.2fn) rows_per_wave=%" PRIu64 " waves=%" PRIu64 " ram_gb=%.2f rows_per_band=%" PRIu64
        " par_bands=%" PRIu64 " threads=%" PRIu64 " in_band_arena_bytes=%" PRIu64 " (%.3fn)\n",
        N, row_bytes, sort_row_bytes, cap_bytes, double(cap_bytes) / double(N), rows_per_wave,
        waves, ram_gb, rows_per_band, par_bands, threads,
        rows_per_band * par_bands * sort_row_bytes,
        double(rows_per_band * par_bands * sort_row_bytes) / double(N));

    // ---- exact first-byte histogram: one parallel pass, 256 counters. This is
    // what makes the per-wave FILTER cheap: a rejected position costs one load
    // instead of a 24-byte key computation (16 waves x n key computations was
    // 4.6x the whole sort at 150 MB).
    std::vector<u64> hist(256, 0);
    {
        const u64 nt = std::max<u64>(1, threads);
        std::vector<std::vector<u64>> lh(nt, std::vector<u64>(256, 0));
        std::vector<std::thread> ts;
        for (u64 t = 0; t < nt; ++t)
            ts.emplace_back([&, t]() {
                const u64 lo = N * t / nt, hi = N * (t + 1) / nt;
                auto& h = lh[t];
                for (u64 p = lo; p < hi; ++p) ++h[sym_at(p)];
            });
        for (auto& th : ts) th.join();
        for (u64 t = 0; t < nt; ++t)
            for (int i = 0; i < 256; ++i) hist[i] += lh[t][i];
    }
    // ---- wave list: greedy byte ranges, each <= rows_per_wave rows. A byte whose
    // class ALONE exceeds the cap is split by its second byte (2-level wave).
    struct Wave { unsigned lo, hi; bool lvl2; unsigned b; unsigned lo2, hi2; };
    std::vector<Wave> wl;
    {
        unsigned lo = 0;
        u64 acc = 0;
        for (unsigned c = 0; c < 256; ++c) {
            if (hist[c] > rows_per_wave) {
                if (lo < c) { wl.push_back({lo, c, false, 0, 0, 0}); lo = c; acc = 0; }
                // split this byte by its second byte
                std::vector<u64> h2(256, 0);
                for (u64 p = 0; p < N; ++p) {
                    if (sym_at(p) != c) continue;
                    h2[sym_at(p + 1)]++;
                }
                unsigned l2 = 0; u64 a2 = 0;
                for (unsigned d = 0; d < 256; ++d) {
                    if (a2 && a2 + h2[d] > rows_per_wave) {
                        wl.push_back({c, c + 1, true, c, l2, d}); l2 = d; a2 = 0;
                    }
                    if (!tapeBands.empty() && h2[d] > rows_per_wave) die("byte-pair class exceeds wave cap");
                    a2 += h2[d];
                }
                if (l2 < 256) wl.push_back({c, c + 1, true, c, l2, 256});
                lo = c + 1; acc = 0;
                continue;
            }
            if (acc && acc + hist[c] > rows_per_wave) {
                wl.push_back({lo, c, false, 0, 0, 0}); lo = c; acc = 0;
            }
            acc += hist[c];
        }
        if (lo < 256) wl.push_back({lo, 256, false, 0, 0, 0});
    }
    waves = wl.size();
    std::fprintf(stderr, "SS_WAVES count=%" PRIu64 " (byte-range waves)\n", waves);

    // ---------------- SAMPLE once: band edges inside each wave ----------------
    const double t0 = now_sec();
    const u64 stride = std::max<u64>(1, N / std::max<u64>(1, sample_n));
    std::vector<u64> sk;
    sk.reserve(N / stride + 2);
    for (u64 p=0; p<KEY_N; p+=stride) sk.push_back(p);
    // Direct corpus proof of the maximal single-symbol suffix. Samples cannot
    // establish this property ("az" and "aa" agree on their first byte).
    u64 trailing=FILE_N;
    if (FILE_N) {
        const unsigned last=REMAP ? REMAP[T[FILE_N-1]] : T[FILE_N-1];
        while (trailing && unsigned(REMAP ? REMAP[T[trailing-1]] : T[trailing-1])==last) --trailing;
    }
    const double t_sample = now_sec() - t0;
    std::fprintf(stderr, "SS_SAMPLE samples=%zu stride=%" PRIu64 " wall=%.2f\n",
                 sk.size(), stride, t_sample);

    std::unique_ptr<Pool> pool(new Pool((unsigned)std::max<u64>(1, threads)));
    std::string cmd = "mkdir -p '" + scratch + "'";
    if (std::system(cmd.c_str()) != 0) die("scratch prep failed");

    // SAMPLED INVERSE: ISA[p] for every k-th POSITION. Anchors are keyed by
    // position, which is invariant under the finish's repair (run indices are not,
    // and run lengths change), so an anchor lets the finish RESYNC its rank cursor.
    // 10 GB at pile with k=1024, against 10.5 TB for the full inverse.
    int ifd = -1;
    if (!isaOut.empty()) {
        ifd = ::open(isaOut.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (ifd < 0) die("create sampled ISA");
        u64 ih[4] = {1363761235ull /*'SXIQ'*/, 1ull, N, isaK};
        if (::write(ifd, ih, 32) != 32) die("write ISA header");
        const u64 ne = (N + isaK - 1) / isaK;
        if (::ftruncate(ifd, off_t(32 + ne * 8)) != 0) die("size sampled ISA");
    }
    int sampleFd = -1;
    if (!orderSample.empty()) {
        if (!sampleCount || sampleCount > 1048576 || sampleLo >= N || sampleCount > N - sampleLo)
            die("order sample range invalid (maximum 1048576 rows)");
        sampleFd = ::open(orderSample.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (sampleFd < 0) die("create bounded order sample");
        u64 sh[6] = {0x0000000141535853ull, N, 1, sampleCount, N, 10};
        if (::write(sampleFd, sh, 48) != 48) die("write order sample header");
    }
    u64 globalBand = 0;
    auto bandPath = [&](u64 b, const char* suffix) {
        return tapeBands + "/band_" + std::to_string(b) + suffix;
    };
    auto producerState = [&](u64 band, const char* phase) {
        if (tapeBands.empty()) return;
        const std::string path = bandPath(band, ".producer-state");
        { std::ofstream out(path + ".tmp"); out << phase << "\n";
          if (!out.good()) die("write producer state " + path); }
        if (::rename((path + ".tmp").c_str(), path.c_str())) die("publish producer state " + path);
    };
    const u64 reorderRowCapacity = std::max<u64>(1, ram_bytes / 8);
    auto marker = [&](const std::string& path) {
        int f = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (f < 0) die("create tape marker " + path);
        ::close(f);
    };
    // ============ THE TWO CONSUMERS, AND WHY THEIR WINDOWS DIFFER ============
    // band_<i>.sa has ONE consumer: cross_lcp_merge's tape emit absorbs the band,
    // unlinks it and stamps band_<i>.consumed - a per-band handshake at the
    // sorter's own pace, so K=pending_bands bounds it.
    // band_<i>.lcp has a DIFFERENT consumer: xsa_finish's rank-cursor fold. The
    // fold stages ONE PIECE BAND at a time and a piece band is 65536 BWT RUNS -
    // not rows - so it cannot retire its first slice until it owns 65536 runs
    // worth of slice, i.e. 65536 x (rows per run) ROWS. Holding the slice to the
    // SA's K-band handshake deadlocked exactly when K x rows_per_band was smaller
    // than that (measured at 4 MB: 8 x 7421 = 59,368 rows < one fold band): the
    // sorter waited for band_0.lcp.consumed while the finish waited for the piece
    // of a band the sorter had not published. The credit below is MEASURED from
    // the run structure this producer already emits, never assumed.
    std::atomic<u64> lcpRowsSeen{0}, lcpRunsSeen{0};
    auto lcpFoldRows = [&]() -> u64 {                 // ROWS spanned by one 65536-RUN piece band
        const u64 seenRows = lcpRowsSeen.load(std::memory_order_relaxed);
        const u64 seenRuns = std::max<u64>(1, lcpRunsSeen.load(std::memory_order_relaxed));
        const double rowsPerRun = seenRows ? double(seenRows) / double(seenRuns) : 1.0;
        return u64(65536.0 * rowsPerRun) + 1;
    };
    // THE CREDIT IS A ROW BUDGET OF UNCONSUMED SLICE, and it is set from the
    // measured fold-band span. Measured at the 4 MB fixture (rows_per_run 2.48,
    // band 7421 rows): 400,000 rows stalls the ring, 672,000 completes - i.e. the
    // required slack is several fold bands, not one, because the emit absorbs rows
    // into ITS current piece band before the finish can fold and retire. The rule
    // below is 8 fold bands; if that is ever too small the wait GROWS it (doubling,
    // journaled) instead of hanging - the ring always breaks, and LCP_SLICE still
    // reports what was actually live.
    std::atomic<u64> lcpCreditRows{0};
    lcpCreditRows.store([&] {
        const char* e = ::getenv("SS_LCP_CREDIT_ROWS");
        const u64 want = (e && *e) ? std::strtoull(e, nullptr, 10) : 0;
        if (want && want < std::max<u64>(u64(65536), rows_per_band))
            die("SS_LCP_CREDIT_ROWS below one fold band (65536 runs) would deadlock the fold");
        return want ? want : 8 * lcpFoldRows();
    }());
    auto lcpWindowNow = [&]() -> u64 {
        const u64 rows = lcpCreditRows.load(std::memory_order_relaxed);
        const u64 need = (rows + rows_per_band - 1) / rows_per_band + 1;
        return std::max<u64>(pendingBands, need);
    };
    // Band waits diagnose ownership periodically. Diagnostics neither return
    // credits nor terminate the producer; external process ownership handles
    // failed peers. The legacy slice-credit arm below is not covered by this
    // protocol's progress argument (see DEADLOCK_FREEDOM.md).
    const double bandWaitDeadlineS = [] {
        const char* e = ::getenv("XSA_TAPE_WAIT_S");
        if (!e || !*e) e = ::getenv("SS_BAND_WAIT_TIMEOUT_S");
        if (!e || !*e) return 1800.0;
        const double v = std::strtod(e, nullptr);
        if (!(v > 0)) die("XSA_TAPE_WAIT_S / SS_BAND_WAIT_TIMEOUT_S must be positive");
        return v;
    }();
    std::mutex lcpLiveMu;
    std::deque<u64> lcpLive;                       // bytes of each PUBLISHED slice not yet retired
    u64 lcpPublished = 0, lcpLiveBytes = 0, lcpPeakLiveBytes = 0, lcpPeakLiveSlices = 0;
    // Retirement is in RANK ORDER (the finish unlinks slice j only after it folded
    // it), so once band_<back>.lcp.consumed exists every slice <= back is gone:
    // that makes the live figure below exact at the sampling instant, and between
    // two waits live only grows - so the max over waits IS the peak.
    auto retireUpTo = [&](u64 back) {
        std::lock_guard<std::mutex> lk(lcpLiveMu);
        while (lcpPublished - u64(lcpLive.size()) <= back && !lcpLive.empty()) {
            lcpLiveBytes -= lcpLive.front();
            lcpLive.pop_front();
        }
        lcpPeakLiveBytes = std::max(lcpPeakLiveBytes, lcpLiveBytes);
        lcpPeakLiveSlices = std::max<u64>(lcpPeakLiveSlices, u64(lcpLive.size()));
    };
    // Name an engaged wait after two seconds; report every completed duration
    // and periodically dump state without changing scheduling or capacity.
    auto waitMarker = [&](const std::string& path, const char* what, const char* who) {
        const double t0 = now_sec();
        bool said = false;
        double nextDiagnostic = bandWaitDeadlineS;
        while (::access(path.c_str(), F_OK) != 0) {
            const double waitedS = now_sec() - t0;
            if (!said && waitedS > 2.0) {
                said = true;
                std::fprintf(stderr,
                    "SS_WAIT producer=scatter_sort blocks_on=%s path=%s writer=%s wall_s=%.1f\n",
                    what, path.c_str(), who, waitedS);
            }
            if (waitedS > nextDiagnostic) {
                nextDiagnostic += bandWaitDeadlineS;
                const std::string stem = path.substr(0, path.size() - std::strlen(".consumed"));
                struct stat st{};
                const bool exists = ::stat((stem + ".sa").c_str(), &st) == 0;
                std::ifstream stateFile(tapeBands + "/merge.state");
                std::string state; std::getline(stateFile, state);
                const bool opened = ::access((stem + ".opened").c_str(), F_OK) == 0;
                std::ifstream producerFile(stem + ".producer-state");
                std::string producerPhase; std::getline(producerFile, producerPhase);
                std::fprintf(stderr, "SS_DIAGNOSTIC path=%s writer=%s band_exists=%d band_bytes=%llu merge_opened=%d producer_phase=%s merge_state={%s}\n",
                    path.c_str(), who, int(exists), (unsigned long long)(exists ? st.st_size : 0),
                    int(opened), producerPhase.empty() ? "unclaimed" : producerPhase.c_str(), state.empty() ? "unavailable" : state.c_str());

            }
            ::usleep(10000);
        }
        std::fprintf(stderr, "SS_WAIT_DONE path=%s wall_s=%.3f\n", path.c_str(), now_sec() - t0);
    };
    // .consumed      <- cross_lcp_merge's tape emit, at READ time (payload in its buffer)
    // .lcp.consumed  <- xsa_finish, immediately after copying a slice into private memory
    auto waitBandConsumed = [&](u64 back) {
        waitMarker(bandPath(back, ".consumed"), "band.consumed", "emit-tape(cross_lcp_merge)");
    };
    // Reservation covers sorting plus publication. Return ANY read credit;
    // serial band-index credits would idle all workers behind one slow sort.
    // Called only while claim_mu is held; workers never need that mutex to
    // finish sorting, publish, or release their arena.
    std::deque<u64> bandReservations;
    auto reserveBand = [&](u64 band) {
        const double started = now_sec();
        double diagnostic = bandWaitDeadlineS;
        bool waiting = false;
        while (bandReservations.size() >= pendingBands) {
            for (auto it = bandReservations.begin(); it != bandReservations.end();) {
                if (::access(bandPath(*it, ".consumed").c_str(), F_OK) == 0)
                    it = bandReservations.erase(it);
                else ++it;
            }
            if (bandReservations.size() < pendingBands) break;
            const double elapsed = now_sec() - started;
            if (!waiting && elapsed >= 2) {
                waiting = true;
                std::fprintf(stderr, "SS_WAIT producer=scatter_sort blocks_on=band.queue band=%llu reservations=%zu wall_s=%.3f\n",
                    (unsigned long long)band, bandReservations.size(), elapsed);
            }
            if (elapsed >= diagnostic) {
                diagnostic += bandWaitDeadlineS;
                std::fprintf(stderr, "SS_DIAGNOSTIC queue=band capacity=%llu reservations=%zu next=%llu wall_s=%.3f\n",
                    (unsigned long long)pendingBands, bandReservations.size(), (unsigned long long)band, elapsed);
                for (u64 held : bandReservations) {
                    std::ifstream pf(bandPath(held, ".producer-state"));
                    std::string phase; std::getline(pf, phase);
                    std::fprintf(stderr, "SS_DIAGNOSTIC_BAND band=%llu phase=%s done=%d consumed=%d\n",
                        (unsigned long long)held, phase.c_str(),
                        int(::access(bandPath(held, ".done").c_str(), F_OK) == 0),
                        int(::access(bandPath(held, ".consumed").c_str(), F_OK) == 0));
                }
                std::ifstream mf(tapeBands + "/merge.state");
                std::string state; std::getline(mf, state);
                std::fprintf(stderr, "SS_DIAGNOSTIC_MERGE state={%s}\n", state.c_str());
            }
            ::usleep(10000);
        }
        std::fprintf(stderr, "SS_WAIT_DONE queue=band band=%llu wall_s=%.6f\n",
            (unsigned long long)band, now_sec() - started);
        bandReservations.push_back(band);
    };
    // The slice credit, with GROWTH instead of a hang: if the fold has not retired
    // the slice this band needs for SS_LCP_GROW_S (default 30 s), the credit
    // doubles and the wait re-targets a later slice. Every growth is journaled.
    const double lcpGrowS = [] {
        const char* e = ::getenv("SS_LCP_GROW_S");
        if (!e || !*e) return 30.0;
        const double v = std::strtod(e, nullptr);
        if (!(v > 0)) die("SS_LCP_GROW_S must be positive");
        return v;
    }();
    auto waitSliceCredit = [&](u64 tapeBand) {
        const double t0 = now_sec();
        for (;;) {
            const u64 lw = lcpWindowNow();
            if (tapeBand < lw) return;                       // inside the credit: go
            const u64 back = tapeBand - lw;
            if (::access(bandPath(back, ".lcp.consumed").c_str(), F_OK) == 0) {
                retireUpTo(back);
                return;
            }
            const double waitedS = now_sec() - t0;
            if (waitedS > lcpGrowS) {
                const u64 was = lcpCreditRows.load();
                if (was < N) {
                    lcpCreditRows.store(std::min<u64>(N, was * 2 + 1));
                    std::fprintf(stderr,
                        "SS_LCP_CREDIT_GROW rows=%llu -> %llu window=%llu waited_s=%.1f "
                        "blocked_on=band_%llu.lcp.consumed writer=finish(xsa_finish fold)\n",
                        (unsigned long long)was, (unsigned long long)lcpCreditRows.load(),
                        (unsigned long long)lcpWindowNow(), waitedS, (unsigned long long)back);
                }
            }
            if (waitedS > bandWaitDeadlineS)
                die("no consumer progress on slice.lcp.consumed band_" + std::to_string(back) +
                    " for " + std::to_string(int(waitedS)) + " s (finish); credit_rows=" +
                    std::to_string(lcpCreditRows.load()) + " of n=" + std::to_string(N));
            ::usleep(10000);
        }
    };
    if (!tapeBands.empty()) {
        struct stat ts{};
        if (::stat(tapeBands.c_str(), &ts) != 0 || !S_ISDIR(ts.st_mode)) die("tape directory must exist and be fresh");
        if (::access((tapeBands + "/manifest").c_str(), F_OK) == 0) die("stale tape manifest");
        int mf = ::open((tapeBands + "/manifest").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (mf < 0) die("create tape manifest");
        std::string meta = std::to_string(N) + (lcpOn ? " 1\n" : " 2 " + std::to_string(pendingBands) + " " + std::to_string(reorderRowCapacity) + "\n");
        if (::write(mf, meta.data(), meta.size()) != (ssize_t)meta.size()) die("write tape manifest");
        ::close(mf);
        marker(tapeBands + "/ready");
        std::fprintf(stderr, "SS_TAPE K=%llu packed_row_bytes=8 wave_cap=%llu\n", (unsigned long long)pendingBands, (unsigned long long)cap_bytes);
        std::fprintf(stderr,
                     "SS_TAPE_CREDIT sa_window=%llu lcp_window0=%llu lcp_credit_rows0=%llu "
                     "rows_per_band=%llu wait_timeout_s=%.0f grow_s=%.0f\n",
                     (unsigned long long)pendingBands, (unsigned long long)lcpWindowNow(),
                     (unsigned long long)lcpCreditRows.load(), (unsigned long long)rows_per_band,
                     bandWaitDeadlineS, lcpGrowS);
    }
    int ofd = tapeBands.empty() ? ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (tapeBands.empty() && ofd < 0) die("create out");
    // THE STREAM IS IN THE PRODUCT'S OWN SAMPLED-SA ANCHOR FORMAT ('SXSA'), so
    // kasai_column and every existing tool can consume it directly. The old header
    // here was 'SXLC' (the COLUMN magic) with n at the wrong offset, which made the
    // LCP column unbuildable from a tape SA - i.e. it silently disabled the Kasai
    // arm of the finish, the one thing that removes the finish's LCE wall.
    {
        u64 h[6];
        const uint32_t magic = 0x41535853u, ver = 1;   // 'SXSA', version 1
        std::memcpy(h, &magic, 4);
        std::memcpy((char*)h + 4, &ver, 4);
        h[1] = N;    // n
        h[2] = 1;    // k (full SA)
        h[3] = N;    // count
        h[4] = N;    // rows
        h[5] = 0;    // SA_ANCHOR_ROW_PAD: no transport frame (sentinel text)
        if (ofd >= 0 && ::write(ofd, h, 48) != 48) die("write header");
    }
    const std::string symPath = out + ".sym";
    int sfd = tapeBands.empty() ? ::open(symPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (tapeBands.empty() && sfd < 0) die("create sym stream");

    // the LCP column: n bytes of adjacent-rank LCP + escapes, in the SXLC format
    // the finish already reads via --lcp-column.
    int lfd = -1;
    if (lcpOn && tapeBands.empty()) {
        lfd = ::open(lcpOut.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (lfd < 0) die("create lcp column");
        std::vector<u8> lh(48, 0);
        const uint32_t magic = 0x434C5853u, ver = 1;
        std::memcpy(lh.data() + 0, &magic, 4);
        std::memcpy(lh.data() + 4, &ver, 4);
        const u64 rows = N, zero = 0, flags = 1;
        std::memcpy(lh.data() + 8, &rows, 8);
        std::memcpy(lh.data() + 16, &rows, 8);
        std::memcpy(lh.data() + 24, &zero, 8);
        std::memcpy(lh.data() + 32, &zero, 8);
        std::memcpy(lh.data() + 40, &flags, 8);
        if (::write(lfd, lh.data(), 48) != 48) die("write lcp header");
        if (::ftruncate(lfd, off_t(48 + N)) != 0) die("size lcp column");
    }
    std::vector<std::pair<u64,u64>> gEsc;
    std::mutex escMu;
    // THE BAND-STREAM RECEIPT: how much LCP slice this run produced and the
    // largest one it ever held (the residency the 2n law is checked against).
    u64 lcpSlices = 0, lcpSliceBytes = 0, lcpRows = 0, lcpEscapes = 0, lcpMaxSliceBytes = 0;
    std::vector<u64> bandFirstPos, bandLastPos;
    std::vector<u8> cat(1u << 22), symcat(1u << 22);
    u64 rows_done = 0, ties_total = 0, emitted = 0, wave_rows = 0;
    u64 prev_wave_last = ~0ull;   // last row position emitted by the previous wave
    const double t1 = now_sec();

    for (u64 w = 0; w < waves; ++w) {
        const double tw = now_sec();
        const Wave& W = wl[w];
        std::vector<u64> waveSample;
        for (u64 p:sk) {
            const unsigned c=sym_at(p);
            if (c<W.lo || c>=W.hi) continue;
            if (W.lvl2 && (sym_at(p+1)<W.lo2 || sym_at(p+1)>=W.hi2)) continue;
            waveSample.push_back(p);
        }
        const auto plan=plan_tape(std::move(waveSample),stride,rows_per_band,w,trailing);
        std::vector<TapeFence> bounds;
        for (size_t i=1;i<plan.size();++i) bounds.push_back(plan[i].first);
        const u64 nbands=plan.size();
        auto route = [&](u64 p) -> u64 {
            return std::upper_bound(bounds.begin(),bounds.end(),p,
                [](u64 pos,const TapeFence& f){const int c=prefix_cmp(pos,f.pos,f.depth); return c<0 || (c==0 && f.after);})-bounds.begin();
        };

        // ---- scatter this wave only ----
        const u64 nparts = std::max<u64>(1, threads);
        std::vector<std::vector<int>> fds(nbands, std::vector<int>(nparts, -1));
        u64 bufsz = 1u << 20;
        while (nbands * nparts * bufsz > (1ull << 31) && bufsz > (1u << 12)) bufsz >>= 1;
        std::vector<std::vector<std::vector<u8>>> bufs(nparts, std::vector<std::vector<u8>>(nbands));
        for (u64 t = 0; t < nparts; ++t)
            for (u64 b = 0; b < nbands; ++b) bufs[t][b].reserve(bufsz);
        auto flush_buf = [&](u64 t, u64 b) {
            auto& v = bufs[t][b];
            if (v.empty()) return;
            if (fds[b][t] < 0) {
                char name[512];
                std::snprintf(name, sizeof name, "%s/w%" PRIu64 ".b%05" PRIu64 ".p%05" PRIu64,
                              scratch.c_str(), w, b, t);
                int f = ::open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (f < 0) die("create band part");
                fds[b][t] = f;
            }
            size_t off = 0;
            while (off < v.size()) {
                ssize_t wr = ::write(fds[b][t], v.data() + off, v.size() - off);
                if (wr <= 0) die("write band part");
                off += size_t(wr);
            }
            v.clear();
        };
        // THE SCAN USES pread INTO A REUSABLE BUFFER, not a faulting mmap walk.
        // Measured: the mmap scan cost ~19 ns per position per wave (a page fault
        // every 4 KB and a TLB walk), against 6.89 GB/s for buffered sequential
        // reads. The mmap stays for the RANDOM tie comparisons in suf_cmp, where
        // it is the right tool.
        auto worker = [&](u64 t) {
            const u64 lo = N * t / nparts, hi = N * (t + 1) / nparts;
            const u64 CH = 4u << 20;
            std::vector<u8> buf(CH + 64);
            for (u64 base = lo; base < hi;) {
                const u64 chunk = std::min<u64>(CH, hi - base);
                // one byte of run-up (the symbol byte at p-1) plus 32 bytes of
                // look-ahead so a 24-byte key and the symbol are always in range
                const u64 from = (base ? base - 1 : base);
                // Read real bytes only. Tail positions are handled separately
                // and cannot inspect the unfilled part of this buffer.
                const u64 want = std::min<u64>(chunk + 1 + 48, FILE_N > from ? FILE_N - from : 0);
                size_t got = 0;
                while (got < want) {
                    ssize_t r = ::pread(fd, buf.data() + got, want - got, off_t(from + got));
                    if (r <= 0) die("pread scan");
                    got += size_t(r);
                }
                // REMAP THE CHUNK ONCE, in place, instead of 24 lookups per row.
                // Per-row key building with byte-by-byte REMAP[] was the real cost:
                // ~400 ns/row for the included rows (~43M per wave at 1 GB), which
                // dominated the whole scan. One sequential pass over the chunk
                // makes every complete key a plain big-endian load.
                if (REMAP)
                    for (u64 i = 0; i < got; ++i) buf[i] = REMAP[buf[i]];
                const u64 off = base - from;   // index in buf of position `base`
                for (u64 j = 0; j < chunk; ++j) {
                    const u64 p = base + j;
                    if (p >= KEY_N) continue; // finite tails never receive a key
                    const u64 bj = off + j;
                    const unsigned c0 = buf[bj];
                    if (c0 < W.lo || c0 >= W.hi) continue;
                    if (W.lvl2) {
                        const unsigned c1 = buf[bj + 1];   // 0 past the corpus end
                        if (c1 < W.lo2 || c1 >= W.hi2) continue;
                    }
                    // THE WHOLE 24-BYTE KEY. Leaving kq unset here was a real bug:
                    // the comparator's third word then read uninitialised memory,
                    // and it still passed sa_check once - the verifier catches order
                    // errors, not undefined values that happen to agree.
                    u64 a1, a2, a3;
                    std::memcpy(&a1, buf.data() + bj, 8);
                    std::memcpy(&a2, buf.data() + bj + 8, 8);
                    std::memcpy(&a3, buf.data() + bj + 16, 8);
                    a1 = bswap64(a1); a2 = bswap64(a2); a3 = bswap64(a3);
                    // The carried BWT symbol. Position 0's predecessor is the
                    // appended end-of-text byte: symbol 0, exactly as the
                    // successor rule says (no marker, no rewrite downstream).
                    const u8 sy = p == 0 ? SENTINEL_SYMBOL : buf[bj - 1];
                    Row r{};
                    r.klo = a1;
                    r.khi = a2;
                    r.kq = a3;
                    r.pos = pack_sym(p, sy);
                    u64 b = route(p);
                    if (b >= nbands) b = nbands - 1;
                    auto& v = bufs[t][b];
                    v.insert(v.end(), (const u8*)&r, (const u8*)&r + sizeof(Row));
                    if (v.size() >= bufsz) flush_buf(t, b);
                }
                base += chunk;
            }
            for (u64 b = 0; b < nbands; ++b) flush_buf(t, b);
            for (u64 b = 0; b < nbands; ++b)
                if (fds[b][t] >= 0) { ::close(fds[b][t]); fds[b][t] = -1; }
        };
        pool->run([&](unsigned t) { worker(t); });
        const double tScan = now_sec() - tw;

        std::vector<std::vector<Row>> tailBands(nbands);
        for (u64 p = KEY_N; p < N; ++p) {
            const unsigned c0 = sym_at(p);
            if (c0 < W.lo || c0 >= W.hi) continue;
            if (W.lvl2 && (sym_at(p + 1) < W.lo2 || sym_at(p + 1) >= W.hi2)) continue;
            const u64 b=route(p);
            Row r{};
            r.pos = pack_sym(p, p == 0 ? SENTINEL_SYMBOL : u8(sym_at(p - 1)));
            tailBands[b].push_back(r);
        }
        for (auto& tail : tailBands)
            std::sort(tail.begin(), tail.end(), [](const Row& a, const Row& b) {
                return suf_cmp_from(unpack_pos(a.pos), unpack_pos(b.pos), 0) < 0;
            });

        // band row counts are known BEFORE sorting (file sizes / row bytes), so
        // every band's offset in the global streams is computable up front and
        // each band can write DIRECTLY there. No append copy, no serial tail.
        std::vector<u64> band_rows(nbands, 0), band_off(nbands, 0);
        for (u64 b = 0; b < nbands; ++b) {
            for (u64 t = 0; t < nparts; ++t) {
                char name[512];
                std::snprintf(name, sizeof name, "%s/w%" PRIu64 ".b%05" PRIu64 ".p%05" PRIu64,
                              scratch.c_str(), w, b, t);
                struct stat bs{};
                if (::stat(name, &bs) == 0) band_rows[b] += u64(bs.st_size) / row_bytes;
            }
        }
        {
            for (u64 b = 0; b < nbands; ++b) band_rows[b] += tailBands[b].size();
            for (u64 b=0;b<nbands;++b) {
                const bool ok=band_rows[b]<=rows_per_band+rows_per_band/10;
                std::fprintf(stderr,"TAPE_ROW_BOUND wave=%llu band=%llu rows=%llu target=%llu ok=%d\n",
                    (unsigned long long)w,(unsigned long long)b,(unsigned long long)band_rows[b],
                    (unsigned long long)rows_per_band,int(ok));
                if (!ok) die("planner bug: completed band exceeds target x 1.1");
            }
            u64 acc = emitted;
            for (u64 b = 0; b < nbands; ++b) { band_off[b] = acc; acc += band_rows[b]; }
            wave_rows = acc - emitted;
        }
        // ---- sort this wave's bands (parallel), writing at their own offsets ----
        std::atomic<u64> next_band{0};
        std::atomic<u64> ties_w{0};
        // Published AFTER a band is sorted: 0 = not published, 1 = empty band,
        // else last-row position + 2. This is what a band needs to compute its own
        // seam without a post-wave patch racing the consumer.
        std::unique_ptr<std::atomic<u64>[]> bandPub;
        if (lcpOn) {
            bandPub.reset(new std::atomic<u64>[nbands]);
            for (u64 b = 0; b < nbands; ++b) bandPub[b].store(0, std::memory_order_relaxed);
        }
        const u64 prevWaveLastLocal = prev_wave_last;
        std::vector<double> tRead(nbands, 0.0), tSort(nbands, 0.0), tWrite(nbands, 0.0);
        bandFirstPos.assign(nbands, 0);
        bandLastPos.assign(nbands, 0);
        std::mutex claim_mu;
        auto band_worker = [&]() {
            std::vector<Row> rows;
            for (;;) {
                u64 b = 0;
                {
                    // Only the next claim waits for disk credit. Future claims
                    // must not time out before their prerequisite is produced,
                    // or reserve arena while blocked behind downstream work.
                    std::lock_guard<std::mutex> lk(claim_mu);
                    b = next_band.fetch_add(1);
                    if (b >= nbands) return;
                    const u64 nextTapeBand = globalBand + b;
                    producerState(nextTapeBand, "credit");
                    if (!tapeBands.empty() && !lcpOn) {
                        reserveBand(nextTapeBand);
                    } else if (!tapeBands.empty() && nextTapeBand >= pendingBands) {
                        waitBandConsumed(nextTapeBand - pendingBands);
                        if (lcpOn) waitSliceCredit(nextTapeBand);
                    }
                }
                const u64 tapeBand = globalBand + b;
                producerState(tapeBand, "read");
                rows.clear();
                const double br0 = now_sec();
                for (u64 t = 0; t < nparts; ++t) {
                    char name[512];
                    std::snprintf(name, sizeof name, "%s/w%" PRIu64 ".b%05" PRIu64 ".p%05" PRIu64,
                                  scratch.c_str(), w, b, t);
                    int f = ::open(name, O_RDONLY);
                    if (f < 0) continue;
                    struct stat fs{};
                    ::fstat(f, &fs);
                    const u64 bytes = (u64)fs.st_size;
                    if (bytes) {
                        const size_t at = rows.size();
                        rows.resize(at + bytes / sizeof(Row));
                        size_t off = 0;
                        while (off < bytes) {
                            ssize_t r = ::read(f, ((u8*)rows.data()) + at * sizeof(Row) + off, bytes - off);
                            if (r <= 0) die("read band part");
                            off += size_t(r);
                        }
                    }
                    ::close(f);
                    if (!keep_rows) ::unlink(name);
                }
                tRead[b] = now_sec() - br0;
                const double bs0 = now_sec();
                producerState(tapeBand, "sort");
                std::vector<u8> bandLcp;
                std::vector<std::pair<u64,u64>> bandEsc;
                if (plan[b].analytic) {
                    for (const Row& r:rows)
                        if (unpack_pos(r.pos)<trailing) die("analytic candidate not proved from corpus");
                    std::reverse(rows.begin(),rows.end());
                    if (lcpOn) {
                        bandLcp.assign(rows.size(),0);
                        for (size_t i=1;i<rows.size();++i) {
                            const u64 lcp=FILE_N-unpack_pos(rows[i-1].pos);
                            bandLcp[i]=u8(std::min<u64>(255,lcp));
                            if (lcp>=255) bandEsc.emplace_back(band_off[b]+i,lcp);
                        }
                    }
                    std::fprintf(stderr,"TAPE_LEAF band=%llu order=analytic rows=%zu\n",
                        (unsigned long long)tapeBand,rows.size());
                } else if (lcpOn) {
                    merge_sort_lcp(rows, bandLcp, bandEsc, band_off[b]);
                } else {
                    // product path: the plain comparison sort. The merge-with-LCP
                    // is opt-in until its propagation is verified exact.
                    sort_tape_keys(rows, tapeBand);
                }
                // Insert the <=24 separately sorted finite suffixes. Only this
                // cold path uses a full comparison against keyed rows.
                // LCP values of unaffected adjacent rows remain unchanged.
                if (!tailBands[b].empty()) {
                    std::vector<u64> lcps(rows.size(), 0);
                    if (lcpOn) {
                        for (size_t i = 0; i < rows.size(); ++i) lcps[i] = bandLcp[i];
                        for (const auto& e : bandEsc) lcps[e.first - band_off[b]] = e.second;
                    }
                    for (const Row& tail : tailBands[b]) {
                        auto it = std::lower_bound(rows.begin(), rows.end(), tail,
                            [](const Row& a, const Row& t) {
                                return suf_cmp_from(unpack_pos(a.pos), unpack_pos(t.pos), 0) < 0;
                            });
                        const size_t i = size_t(it - rows.begin());
                        rows.insert(it, tail);
                        if (lcpOn) {
                            u64 lcp = 0;
                            if (i) suf_cmp_lcp(unpack_pos(rows[i-1].pos), unpack_pos(tail.pos), 0, lcp);
                            lcps.insert(lcps.begin() + i, lcp);
                            if (i + 1 < rows.size()) {
                                suf_cmp_lcp(unpack_pos(tail.pos), unpack_pos(rows[i+1].pos), 0, lcp);
                                lcps[i+1] = lcp;
                            }
                        }
                    }
                    if (lcpOn) {
                        bandLcp.resize(rows.size()); bandEsc.clear();
                        for (size_t i = 0; i < lcps.size(); ++i) {
                            bandLcp[i] = u8(std::min<u64>(lcps[i], 255));
                            if (lcps[i] >= 255) bandEsc.emplace_back(band_off[b] + i, lcps[i]);
                        }
                    }
                }
                if (lcpOn) {
                    // ---- THE SEAM --------------------------------------------------
                    // Byte 0 of a band is the LCP with the PREVIOUS row in rank
                    // order, i.e. the last row of the previous non-empty band - a
                    // pair that belongs to neither band's own merge. It is computed
                    // HERE, before the slice is published, because a post-wave patch
                    // would race the consumer that already holds the band.
                    //
                    // The wait is on an EARLIER band (claimed earlier by the ordered
                    // work queue), so the dependency graph is acyclic: with W workers
                    // band b only ever waits for bands < b, and every band's worker
                    // finishes without waiting on a band >= b.
                    if (!rows.empty()) {
                        const u64 firstPos = unpack_pos(rows.front().pos);
                        u64 prevLast = ~0ull;
                        for (u64 j = b; j-- > 0;) {
                            u64 v = 0;
                            for (;;) {
                                v = bandPub[j].load(std::memory_order_acquire);
                                if (v) break;
                                ::usleep(50);
                            }
                            if (v > 1) { prevLast = v - 2; break; }
                        }
                        if (prevLast == ~0ull) prevLast = prevWaveLastLocal;
                        if (prevLast != ~0ull) {
                            u64 cross = 0;
                            suf_cmp_lcp(prevLast, firstPos, 0, cross);
                            // the band's own block-start value is replaced, never
                            // duplicated: the escape list must stay rank-strict.
                            for (size_t e = bandEsc.size(); e-- > 0;)
                                if (bandEsc[e].first == band_off[b]) bandEsc.erase(bandEsc.begin() + e);
                            bandLcp[0] = u8(std::min<u64>(cross, 255));
                            if (cross >= 255) bandEsc.emplace_back(band_off[b], cross);
                        }
                    }
                    std::sort(bandEsc.begin(), bandEsc.end());
                    if (tapeBands.empty()) {
                        // the one-file arm: the band's LCP stream lands at its own
                        // offset (rank order is band order).
                        const u64 at = 48 + band_off[b];
                        size_t off = 0;
                        while (off < bandLcp.size()) {
                            ssize_t wr = ::pwrite(lfd, bandLcp.data() + off, bandLcp.size() - off,
                                                  off_t(at + off));
                            if (wr <= 0) die("pwrite lcp");
                            off += size_t(wr);
                        }
                        if (!bandEsc.empty()) {
                            std::lock_guard<std::mutex> lk(escMu);
                            for (auto& e : bandEsc) gEsc.push_back(e);
                        }
                    } else {
                        // ---- THE SLICE -------------------------------------------------
                        // band_<i>.lcp: 48-byte header {rows, escapes, max_lcp,
                        // rank_base, flags} + rows bytes + escapes*16. Published by
                        // rename, then .lcp.done, and it is consumed (deleted) by the
                        // fold that reads it -> .lcp.consumed, which is this
                        // producer's flow control for the NEXT K bands.
                        u64 maxL = 0;
                        for (u64 i = 0; i < bandLcp.size(); ++i) maxL = std::max<u64>(maxL, bandLcp[i]);
                        for (const auto& e : bandEsc) maxL = std::max(maxL, e.second);
                        const std::string tmp = bandPath(tapeBand, ".lcp.tmp");
                        const std::string dst = bandPath(tapeBand, ".lcp");
                        int lf = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                        if (lf < 0) die("create lcp slice " + tmp);
                        u8 h[48];
                        std::memset(h, 0, sizeof h);
                        const uint32_t magic = 0x534C5853u, ver = 1;   // 'SXLS', v1
                        std::memcpy(h + 0, &magic, 4);
                        std::memcpy(h + 4, &ver, 4);
                        const u64 rowsN = bandLcp.size(), escN = bandEsc.size(), flags = 1;
                        std::memcpy(h + 8, &rowsN, 8);
                        std::memcpy(h + 16, &escN, 8);
                        std::memcpy(h + 24, &maxL, 8);
                        std::memcpy(h + 32, &band_off[b], 8);
                        std::memcpy(h + 40, &flags, 8);
                        auto wf = [&](const void* p, size_t n2) {
                            const char* c = (const char*)p;
                            size_t gone = 0;
                            while (gone < n2) {
                                ssize_t wr = ::write(lf, c + gone, n2 - gone);
                                if (wr <= 0) die("write lcp slice");
                                gone += size_t(wr);
                            }
                        };
                        wf(h, 48);
                        if (!bandLcp.empty()) wf(bandLcp.data(), bandLcp.size());
                        for (const auto& e : bandEsc) {
                            const u64 pair[2] = {e.first, e.second};
                            wf(pair, 16);
                        }
                        if (::close(lf)) die("close lcp slice");
                        if (::rename(tmp.c_str(), dst.c_str()) != 0) die("publish lcp slice");
                        marker(dst + ".done");
                        const u64 sliceBytes = bandLcp.size() + bandEsc.size() * 16;
                        {
                            std::lock_guard<std::mutex> lk(escMu);
                            lcpSlices += 1;
                            lcpSliceBytes += sliceBytes;
                            lcpRows += bandLcp.size();
                            lcpEscapes += bandEsc.size();
                            if (sliceBytes > lcpMaxSliceBytes) lcpMaxSliceBytes = sliceBytes;
                        }
                        {
                            // live = published - retired; the peak is sampled at every
                            // credit wait (see retireUpTo). Exact bytes, not a ratio.
                            std::lock_guard<std::mutex> lk(lcpLiveMu);
                            ++lcpPublished;
                            lcpLive.push_back(sliceBytes);
                            lcpLiveBytes += sliceBytes;
                            lcpPeakLiveBytes = std::max(lcpPeakLiveBytes, lcpLiveBytes);
                            lcpPeakLiveSlices = std::max<u64>(lcpPeakLiveSlices, u64(lcpLive.size()));
                        }
                    }
                    bandFirstPos[b] = rows.empty() ? ~0ull : unpack_pos(rows.front().pos);
                    bandLastPos[b] = rows.empty() ? ~0ull : unpack_pos(rows.back().pos);
                }
                // PUBLISH the band's last row for the next band's seam. 1 = empty
                // band, else lastPos + 2 (0 stays "not published").
                if (lcpOn) bandPub[b].store(rows.empty() ? 1ull : (unpack_pos(rows.back().pos) + 2),
                                            std::memory_order_release);
                tSort[b] = now_sec() - bs0;
                const double bw0 = now_sec();
                producerState(tapeBand, "publish");
                u64 ties = 0;
                for (size_t i = 1; i < rows.size(); ++i)
                    if (rows[i].klo == rows[i-1].klo && rows[i].khi == rows[i-1].khi &&
                        rows[i].kq == rows[i-1].kq) ++ties;
                ties_w.fetch_add(ties);
                // ordered output: positions to the SA stream, symbols alongside
                std::vector<u64> ob;
                std::vector<u8> sb;
                ob.reserve(rows.size());
                sb.reserve(rows.size());
                for (size_t ri = 0; ri < rows.size(); ++ri) {
                    const u64 pp = unpack_pos(rows[ri].pos);
                    ob.push_back(pp);
                    sb.push_back((u8)(rows[ri].pos >> 48));
                    if (ifd >= 0 && pp % isaK == 0) {
                        const u64 rk = band_off[b] + (u64)ri;   // this row's global rank
                        if (::pwrite(ifd, &rk, 8, off_t(32 + (pp / isaK) * 8)) != 8)
                            die("pwrite ISA anchor");
                    }
                }
                if (lcpOn && !tapeBands.empty()) {
                    // The slice credit needs rows per BWT run (lcpWindowNow): count
                    // the symbol changes this band publishes. The boundary symbol
                    // between two bands is not counted, so runs are UNDER-counted
                    // and rows/run over-estimated - the safe direction.
                    u64 runsHere = sb.empty() ? 0 : 1;
                    for (size_t i = 1; i < sb.size(); ++i) runsHere += sb[i] != sb[i - 1];
                    lcpRowsSeen.fetch_add(u64(sb.size()), std::memory_order_relaxed);
                    lcpRunsSeen.fetch_add(runsHere, std::memory_order_relaxed);
                }
                if (sampleFd >= 0) {
                    const u64 lo = std::max(sampleLo, band_off[b]);
                    const u64 hi = std::min(sampleLo + sampleCount, band_off[b] + u64(ob.size()));
                    if (lo < hi && ::pwrite(sampleFd, ob.data() + (lo - band_off[b]), (hi - lo) * 8,
                                           off_t(48 + (lo - sampleLo) * 8)) != ssize_t((hi - lo) * 8))
                        die("write bounded order sample");
                }
                if (!tapeBands.empty()) {
                    // Compact wire: low 48 bits position, next 8 bits carried symbol.
                    for (size_t i = 0; i < ob.size(); ++i) ob[i] |= u64(sb[i]) << 48;
                    int bf = ::open(bandPath(tapeBand, ".sa").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                    if (bf < 0) die("create tape band");
                    size_t off = 0, bytes = ob.size() * 8;
                    while (off < bytes) {
                        ssize_t wr = ::write(bf, (const char*)ob.data() + off, bytes - off);
                        if (wr <= 0) die("write tape band");
                        off += size_t(wr);
                    }
                    if (::close(bf)) die("close tape band");
                    marker(bandPath(tapeBand, ".done"));
                } else {
                // direct write at this band's offset in the global streams
                    const off_t at = off_t(48 + band_off[b] * 8);
                    size_t off = 0;
                    const size_t bytes = ob.size() * 8;
                    while (off < bytes) {
                        ssize_t wr = ::pwrite(ofd, ((const u8*)ob.data()) + off, bytes - off,
                                             at + off_t(off));
                        if (wr <= 0) die("pwrite band out");
                        off += size_t(wr);
                    }
                    size_t soff = 0;
                    while (soff < sb.size()) {
                        ssize_t wr = ::pwrite(sfd, sb.data() + soff, sb.size() - soff,
                                              off_t(band_off[b] + soff));
                        if (wr <= 0) die("pwrite band sym");
                        soff += size_t(wr);
                    }
                }
                tWrite[b] = now_sec() - bw0;
                std::fprintf(stderr, "SS_BAND band=%llu rows=%zu read_s=%.6f sort_s=%.6f write_s=%.6f\n",
                    (unsigned long long)tapeBand, rows.size(), tRead[b], tSort[b], tWrite[b]);
                producerState(tapeBand, "published");
                rows.clear();
                rows.shrink_to_fit();
            }
        };
        if (!tapeBands.empty()) {
            if (w + 1 == waves) {
                int ef = ::open((tapeBands + "/end").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
                if (ef < 0) die("create tape end");
                std::string count = std::to_string(globalBand + nbands) + "\n";
                if (::write(ef, count.data(), count.size()) != (ssize_t)count.size()) die("write tape end");
                ::close(ef);
                marker(tapeBands + "/end.ready");
            }
        }
        if (!tapeBands.empty() && !lcpOn) {
            const u64 rowLimit = reorderRowCapacity;
            for (u64 first = 0; first < nbands;) {
                u64 count = 0, rows = 0;
                while (first + count < nbands && count < pendingBands) {
                    const u64 next = band_rows[first + count];
                    if (next > rowLimit) die("band exceeds bounded reorder row budget; reduce work unit");
                    if (next > rowLimit - rows) break;
                    rows += next; ++count;
                }
                const std::string path = tapeBands + "/window_" + std::to_string(globalBand + first);
                { std::ofstream f(path + ".tmp");
                  f << count << "\n";
                  if (!f.good()) die("write tape window"); }
                if (::rename((path + ".tmp").c_str(), path.c_str())) die("publish tape window");
                std::fprintf(stderr, "SS_REORDER_WINDOW first=%llu bands=%llu rows=%llu row_limit=%llu bytes=%llu ratio=%.6f\n",
                    (unsigned long long)(globalBand + first), (unsigned long long)count,
                    (unsigned long long)rows, (unsigned long long)rowLimit,
                    (unsigned long long)(rows * 8), double(rows) / double(rowLimit));
                first += count;
            }
        }
        pool->run([&](unsigned) { band_worker(); });
        // BAND BOUNDARIES are computed INSIDE the band that owns them (each slice
        // is published complete, so no post-wave patch can race a consumer). The
        // only thing the wave must still carry forward is the LAST row position of
        // its last non-empty band: the next wave's first band seams against it
        // (wave seams were not written at all in an earlier version, and 4 of the
        // 22 seams at 150 MB then had a true LCP of 1).
        if (lcpOn)
            for (u64 b = 0; b < nbands; ++b)
                if (bandLastPos[b] != ~0ull) prev_wave_last = bandLastPos[b];
        if (!tapeBands.empty()) {
            // Drain the SA stream before scattering another wave: no cross-wave disk
            // overlap. The LCP slices are NOT drained here: their consumer folds a
            // 65536-RUN piece band, which can be LARGER than one wave of rows (4 MB:
            // 32 x 7421 = 237,500 rows < one fold band's run span), so a wave-aligned
            // drain is the same chicken-and-egg one level up - measured as a full
            // wait cycle with all three tools asleep. The slice stream is bounded by
            // lcpWindowNow() instead, which is measured in rows and is what LCP_SLICE
            // reports.
            for (u64 b = 0; b < nbands; ++b) waitBandConsumed(globalBand + b);
            globalBand += nbands;
        }
        emitted += wave_rows;
        if (false) for (u64 b = 0; b < nbands; ++b) {
            char oname[512];
            std::snprintf(oname, sizeof oname, "%s/w%" PRIu64 ".out%05" PRIu64, scratch.c_str(), w, b);
            int f = ::open(oname, O_RDONLY);
            if (f < 0) die("open band out");
            for (;;) {
                ssize_t r = ::read(f, cat.data(), cat.size());
                if (r < 0) die("read band out");
                if (r == 0) break;
                size_t off = 0;
                while (off < size_t(r)) {
                    ssize_t wr = ::write(ofd, cat.data() + off, size_t(r) - off);
                    if (wr <= 0) die("write out");
                    off += size_t(wr);
                }
            }
            ::close(f);
            ::unlink(oname);
            std::snprintf(oname, sizeof oname, "%s/w%" PRIu64 ".sym%05" PRIu64, scratch.c_str(), w, b);
            int sf = ::open(oname, O_RDONLY);
            if (sf < 0) die("open band sym");
            for (;;) {
                ssize_t r = ::read(sf, symcat.data(), symcat.size());
                if (r < 0) die("read band sym");
                if (r == 0) break;
                size_t off = 0;
                while (off < size_t(r)) {
                    ssize_t wr = ::write(sfd, symcat.data() + off, size_t(r) - off);
                    if (wr <= 0) die("write sym");
                    off += size_t(wr);
                }
            }
            ::close(sf);
            ::unlink(oname);
        }
        rows_done += 0;
        ties_total += ties_w.load();
        {
            double rd = 0, so = 0, wr = 0, rdm = 0, som = 0, wrm = 0;
            for (u64 b = 0; b < nbands; ++b) {
                rd += tRead[b]; so += tSort[b]; wr += tWrite[b];
                rdm = std::max(rdm, tRead[b]); som = std::max(som, tSort[b]); wrm = std::max(wrm, tWrite[b]);
            }
            std::fprintf(stderr,
                "SS_WAVE w=%" PRIu64 "/%" PRIu64 " bands=%" PRIu64 " wall=%.2f"
                " scan=%.2f | band_sum read=%.2f sort=%.2f write=%.2f"
                " | band_max read=%.2f sort=%.2f write=%.2f\n",
                w + 1, waves, nbands, now_sec() - tw, tScan,
                rd, so, wr, rdm, som, wrm);
        }
    }

    if (emitted != N) die("scatter row coverage != n");
    if (lfd >= 0) {
        // DEDUPE BY RANK, keeping the LAST value written for a rank: a band's own
        // merge can escape the rank that the seam also escapes (the block-start
        // inheritance), and the finish requires a STRICTLY ordered escape list.
        std::sort(gEsc.begin(), gEsc.end(),
                  [](const std::pair<u64,u64>& a, const std::pair<u64,u64>& b) {
                      return a.first != b.first ? a.first < b.first : a.second < b.second;
                  });
        gEsc.erase(std::unique(gEsc.begin(), gEsc.end(),
                               [](const std::pair<u64,u64>& a, const std::pair<u64,u64>& b) {
                                   return a.first == b.first;
                               }), gEsc.end());
        for (auto& e : gEsc) {
            u64 pair[2] = {e.first, e.second};
            if (::pwrite(lfd, pair, 16, off_t(48 + off_t(N) + off_t(&e - gEsc.data()) * 16)) != 16)
                die("write lcp escape");
        }
        const u64 ne = gEsc.size();
        u64 maxL = 0;
        for (auto& e : gEsc) maxL = std::max(maxL, e.second);
        if (::pwrite(lfd, &ne, 8, 24) != 8) die("patch escapes");
        if (::pwrite(lfd, &maxL, 8, 32) != 8) die("patch maxlcp");
        ::fsync(lfd); ::close(lfd);
    }
    if (sampleFd >= 0) { ::fsync(sampleFd); ::close(sampleFd); }
    if (ofd >= 0) { ::fsync(ofd); ::close(ofd); }
    if (sfd >= 0) { ::fsync(sfd); ::close(sfd); }
    if (!tapeBands.empty()) marker(tapeBands + "/producer.done");
    if (lcpOn && !tapeBands.empty()) {
        // THE RESIDENCY RECEIPT: the largest slice this producer ever held is one
        // band's, and rows must sum to n. Both are gates, not telemetry.
        if (lcpRows != N) die("lcp slices do not cover n");
        std::fprintf(stderr,
            "SS_LCP_STREAM mode=sorter-bands slices=%" PRIu64 " rows=%" PRIu64
            " escapes=%" PRIu64 " bytes=%" PRIu64 " max_live_lcp_slice_bytes=%" PRIu64
            " slice_bytes_per_row=%.6f K=%" PRIu64 "\n",
            lcpSlices, lcpRows, lcpEscapes, lcpSliceBytes, lcpMaxSliceBytes,
            double(lcpSliceBytes) / double(N), pendingBands);
        // THE LCP SLICE FOOTPRINT, in the units the residency law is stated in.
        // live = published-but-not-yet-retired SLICE bytes on disc (the finish
        // unlinks each slice as its rank cursor passes it), sampled at every
        // credit wait - the peak is exact because between two waits live only
        // grows. This is the number that may never approach n: one K-band window
        // of slices, never a resident 1.34n column.
        std::fprintf(stderr,
            "LCP_SLICE max_live_bytes=%" PRIu64 " slices=%" PRIu64 " peak_over_n=%.6f"
            " live_slices_peak=%" PRIu64 " slice_bytes_total=%" PRIu64 " rows=%" PRIu64
            " escapes=%" PRIu64 " rows_per_run=%.4f lcp_window_final=%" PRIu64
            " max_slice_bytes=%" PRIu64 "\n",
            lcpPeakLiveBytes, lcpSlices, double(lcpPeakLiveBytes) / double(N),
            lcpPeakLiveSlices, lcpSliceBytes, lcpRows, lcpEscapes,
            lcpRowsSeen.load() ? double(lcpRowsSeen.load()) / double(std::max<u64>(1, lcpRunsSeen.load())) : 0.0,
            lcpWindowNow(), lcpMaxSliceBytes);
    }
    std::fprintf(stderr,
        "SS_DONE n=%" PRIu64 " waves=%" PRIu64 " ties=%" PRIu64
        " sample_s=%.2f waves_s=%.2f total_s=%.2f\n",
        N, waves, ties_total, t_sample, now_sec() - t1, now_sec() - t0);
    if (!selftestOracle.empty()) {
        std::ifstream oracle(selftestOracle), sa(out, std::ios::binary), symbols(symPath, std::ios::binary);
        if (!oracle || !sa || !symbols) die("selftest open oracle/output");
        sa.seekg(48);
        u8 inverse[256];
        for (unsigned c = 0; c < 256; ++c) inverse[REMAP ? REMAP[c] : c] = u8(c);
        std::string line;
        u64 rank = 0;
        while (std::getline(oracle, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            u64 expectedRank, expectedPos, expectedSymbol, p;
            unsigned char carried;
            if (!(fields >> expectedRank >> expectedPos >> std::hex >> expectedSymbol))
                die("selftest malformed oracle");
            if (!sa.read(reinterpret_cast<char*>(&p), 8) ||
                !symbols.read(reinterpret_cast<char*>(&carried), 1)) die("selftest truncated output");
            if (p == 0 && carried != SENTINEL_SYMBOL) die("selftest END carry");
            const unsigned symbol = p == 0 ? 0 : inverse[carried];
            if (expectedRank != rank || expectedPos != p || expectedSymbol != symbol)
                die("G6 disagreement rank=" + std::to_string(rank) + " position=" + std::to_string(p));
            ++rank;
        }
        if (rank != N || sa.peek() != EOF || symbols.peek() != EOF) die("selftest row coverage");
        std::printf("G6_PASS mode=%s rows=%" PRIu64 " mismatches=0\n",
                    "virtual", rank);
    }
    return 0;
}
