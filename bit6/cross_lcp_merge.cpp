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
//  * PARALLEL EMISSION: the cross-merge walk and every emit walk are sharded
//    at FIXED ORDER BOUNDARIES. The merged total order is strict, so the
//    merged rank of A[a] is exactly a + #{B rows ordered before A[a]}: the
//    walk's shard boundaries are binary-searched at merged-rank targets and
//    each shard pwrites its disjoint output range. The emitters shard at run
//    boundaries (shard-local run-start sums fix every run's global index);
//    every run is finalized by exactly one shard, fixed-size records go out
//    by pwrite at their global offsets, and the variable-word rlebwt goes
//    through per-shard segments concatenated in shard order. Concatenation
//    is order-pure: byte-identical to the serial walk by construction at
//    S = --threads shards (CROSS_SHARDS overrides; S=1 IS the serial walk).
//  * Comparisons use sparse prefix fingerprints over virtual M.M with a directly
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
#include <emmintrin.h>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <execinfo.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <filesystem>
#include <mutex>
#include <functional>
#include <malloc.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <map>
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

// ---------------------------------------------------- windowed scratch I/O
// Externalized merge machinery: all per-position arrays (M.M text, merged
// order, children positions, BWT, sparse fingerprints) live in scratch files
// under --work and are read through bounded pread windows. Resident per pair:
// window caches, one bounded block-sort buffer, the rank index (<=64 MB), and
// small tables. Nothing scales with n in RAM.
// Tree policy: SERIAL — one pair at a time, bottom-up; all parallelism is
// within-pair (the 48-thread band/block machinery). Concurrency is a measured
// number, not an architecture.
static void xw(int fd, void* buf, U len, U off, const char* what) {
    char* p = (char*)buf;
    while (len) { ssize_t z = pread(fd, p, len, off); if (z <= 0) fail(what);
        p += z; off += (U)z; len -= (U)z; }
}
static void xput(int fd, const void* buf, U len, U off, const char* what) {
    const char* p = (const char*)buf;
    while (len) { ssize_t z = pwrite(fd, p, len, off); if (z <= 0) fail(what);
        p += z; off += (U)z; len -= (U)z; }
}
static const U HASH_BASE = 0x9e3779b185ebca87ULL;   // plain u64 wraparound
static U upow(U e) { U r = 1, x = HASH_BASE; while (e) { if (e & 1) r *= x; x *= x; e >>= 1; } return r; }
static std::atomic<U> g_winGen{1};   // window caches are thread-local keyed by
                                     // (object address, generation): recycled
                                     // addresses must not reuse stale pages.

// Shared bounded page cache + prefetch pool. The cross-merge two-pointer
// knows its upcoming comparison positions (the children's order streams),
// each comparison's probe targets are predictable to within one small page
// (gallop/bisect lens cluster tightly), and the emit passes scan the WM
// sequentially: all give advance knowledge of the 256-byte pages the
// windowed readers will need. Worker threads pread those pages into a
// fixed-size slot table ahead of the main loop, so the hot path pays an
// atomic check plus a short memcpy instead of a syscall. Slots are
// direct-indexed and seqlocked: a reader re-validates the tag after copying,
// so a concurrent page replacement can never yield torn data. Nothing scales
// with n in RAM beyond the fixed slot table.
// Shared bounded page cache + prefetch pool, FINAL PROTOCOL (single 128-bit
// slot word on cmpxchg16b; validated by the pool-verification lane) with
// the pile-week READER-HALVES amendment (lock-free reads; journal
// pile-week-v1):
//   word128 = (page+1) << 67 | bufId << 66 | ver(64 bits) << 2 | state
//   state: 0 invalid, 1 filling, 2 ready. Two 256-byte buffers per slot.
//   claim  : CAS128( cur -> (page, flip(cur.buf), cur.ver+1, FILLING) )
//   publish: CAS128( myClaim -> (page, myBuf, myVer+1, READY) )  (atomic with the bump)
//   reader : lo1 = 8-byte load of the LOW half  (ver + state)
//            hi  = 8-byte load of the HIGH half ((page+1)<<3 | buf<<2)
//            copy buffer[buf]; compiler barrier; lo2 = 8-byte load(low half);
//            accept iff lo1 == lo2.
// THE READER-HALVES SAFETY ARGUMENT (why two plain 8-byte loads are exact
// where the old single locked 128-bit load was): every writer transition -
// claim and publish - is a CAS128 that bumps `ver`, which lives entirely
// in the LOW half. So lo1 == lo2 proves ZERO writer transitions occurred
// between the two loads, which proves (a) the high half (page, buf) read in
// between belongs to that same stable epoch, and (b) no fill started
// writing into the named buffer after lo1 (a fill writes buffer B only
// after a claim flips to B, and that claim bumps lo). A torn lo1/hi pair
// is caught the same way: any interleaved transition leaves lo2 != lo1, so
// a stale or half-updated view is rejected conservatively, never accepted.
// No ABA: ver is bumped on every transition; it cannot wrap (a claim issues
// an NVMe pread, so a slot sustains at most ~1e5-1e6 claims/s).
// This removes BOTH locked cmpxchg16b ops from the hit path - at high
// thread counts the old path serialized on the slot cacheline (a locked
// RMW takes the line exclusively), which is the measured walk-flatness
// suspect (walk 725.5s at 48t vs 651.6s at 96t with 98%+ pool hit rates).
// The writer path (fill/publish) is UNCHANGED - still the model-checked
// single-CAS128 protocol, still refusing to claim a FILLING slot.
// Telemetry: the old code did a relaxed fetch_add on ONE shared atomic per
// hit (nHits) - a second global cacheline serialization point. Hits/misses/
// torn/fills are now exact per-thread counters (padded, summed at stop;
// valid because every reader joins before the pool is destroyed).
// Capacity: SLOTS is a knob (CROSS_POOL_SLOTS, default 2^20 = the banked
// behavior). At gate scales the pooled working set (m2 2n + hs 2n) exceeds
// 2^20 pages, so direct-mapped slots collide and refill the SAME immutable
// page hundreds of millions of times per run (witness: POOL_STATS at the
// 96t 1GB gate - 710M and 1.25G fills on the two m2 pools, 670M emission
// misses). The front door sizes the knob from the corpus (chain.rs), so
// the plan carries the pool RAM as an explicit bounded term.
// Bounded model-checking verdicts (bit6/sxi_logs/pool-verification): two
// fillers, one slot: CLEAN at depths 2..96 (10.7M states at 96); two
// readers/two fillers: CLEAN at depths 12/16; two slots/four pages: CLEAN
// at depth 16. The three pre-fix protocols still reproduce their
// counterexamples under the same enumerator (calibration), and the exact
// 11-step pre-fix orphan schedules replay clean. The verdicts cover the
// WRITER protocol, which is bit-identical here; the reader change carries
// the argument above plus the standing byte-identity/chi gates.
struct alignas(64) LocalCounter { U value = 0; };
static_assert(sizeof(LocalCounter) == 64, "telemetry cache-line ownership");
static inline void count_local(LocalCounter* cells, unsigned tid, unsigned last) {
    if (tid == last) __atomic_fetch_add(&cells[tid].value, U(1), __ATOMIC_RELAXED);
    else ++cells[tid].value;
}
struct SharedPages {
    static constexpr U PS = 256;              // page size (bytes)
    U SLOTS = 1u << 20;                       // pow2 slot count (CROSS_POOL_SLOTS)
    struct Slot {
        union W128 { unsigned __int128 x = 0; struct { uint64_t lo, hi; } h; };
        W128 w;                               // low half: ver<<2|state; high: (page+1)<<3|buf<<2
        uint8_t *b0 = nullptr, *b1 = nullptr; // the two buffers (one allocation)
    };
    int fd = -1; U bytes = 0, pages = 0;
    std::unique_ptr<Slot[]> slots;
    std::unique_ptr<uint8_t[]> block;        // 2*SLOTS*PS bytes, lazily committed
    static constexpr U QCAP = 1u << 16;       // prefetch ring capacity
    std::unique_ptr<std::atomic<U>[]> queue;
    std::atomic<U> qh{0}, qt{0};
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
    // exact per-thread telemetry (see the protocol comment above)
    static constexpr U TIDS = 4096;
    std::unique_ptr<LocalCounter[]> hits, misses, tornC, fillsC;   // [TIDS], one per charging thread
    static unsigned tid() {
        static std::atomic<unsigned> next{0};
        thread_local unsigned t = next.fetch_add(1, std::memory_order_relaxed);
        return t < TIDS ? t : TIDS - 1;
    }
    void hit() const { count_local(hits.get(), tid(), TIDS-1); }
    void miss() const { count_local(misses.get(), tid(), TIDS-1); }
    void torn() const { count_local(tornC.get(), tid(), TIDS-1); }
    void notedFill() const { count_local(fillsC.get(), tid(), TIDS-1); }
    static U pool_slot_count() {
        static const U v = [] {
            const char* e = getenv("CROSS_POOL_SLOTS");
            U s = e ? U(strtoull(e, nullptr, 10)) : (1u << 20);
            if (s < (1u << 16)) s = 1u << 16;
            if (s > (1u << 27)) s = 1u << 27;
            if (s & (s - 1)) { U p = 1; while (p * 2 <= s) p *= 2; s = p; }
            return s;
        }();
        return v;
    }
    static constexpr U ST_INVALID = 0, ST_FILLING = 1, ST_READY = 2;
    void start(int fdIn, U bytesIn, U nthreads) {
        {   // __atomic_is_lock_free(16) is statically false per the x86-64
            // psABI even when the CPU has cmpxchg16b; probe the CPU flag
            // directly. libatomic's 16-byte routines are locked cmpxchg16b
            // on such hardware (no locks, no fallback).
            unsigned a=0,b=0,c=0,d=0;
            __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(1));
            require(c & (1u<<13), "128-bit atomics (cmpxchg16b) required for the page pool");
        }
        fd = fdIn; bytes = bytesIn; pages = (bytes + PS - 1) / PS;
        require(pages < (1ull << 60), "page pool: file too large for the packed word");
        SLOTS = pool_slot_count();
        block.reset(new uint8_t[2 * size_t(SLOTS) * PS]);   // lazily committed by the OS
        slots.reset(new Slot[SLOTS]);
        for (U z = 0; z < SLOTS; ++z) {
            slots[z].b0 = block.get() + 2 * z * PS;
            slots[z].b1 = block.get() + (2 * z + 1) * PS;
        }
        hits.reset(new LocalCounter[TIDS]{}); misses.reset(new LocalCounter[TIDS]{});
        tornC.reset(new LocalCounter[TIDS]{}); fillsC.reset(new LocalCounter[TIDS]{});
        queue.reset(new std::atomic<U>[QCAP]);
        for (U t = 0; t < nthreads; ++t)
            workers.emplace_back([this] { worker(); });
    }
    void stop() {
        stopping = true;
        for (auto& w : workers) if (w.joinable()) w.join();
        workers.clear();
        U h = 0, m = 0, t = 0, f = 0;
        if (hits)
            for (U z = 0; z < TIDS; ++z) { h += hits[z].value; m += misses[z].value; t += tornC[z].value; f += fillsC[z].value; }
        std::fprintf(stderr, "POOL_STATS fd=%d bytes=%llu slots=%llu fills=%llu hits=%llu misses=%llu torn=%llu\n",
                     fd, (unsigned long long)bytes, (unsigned long long)SLOTS,
                     (unsigned long long)f, (unsigned long long)h,
                     (unsigned long long)m, (unsigned long long)t);
    }
    ~SharedPages() { stop(); }
    inline U slotOf(U page) const { return page & (SLOTS - 1); }
    static inline U pageOf(unsigned __int128 w) { return U(w >> 67) - 1; }
    static inline U bufOf(unsigned __int128 w)  { return U(w >> 66) & 1; }
    static inline U verOf(unsigned __int128 w)  { return U(w >> 2); }
    static inline U stateOf(unsigned __int128 w) { return U(w) & 3; }
    static inline unsigned __int128 packW(U page, U buf, U ver, U st) {
        return ((unsigned __int128)(page + 1) << 67) | ((unsigned __int128)buf << 66) |
               ((unsigned __int128)ver << 2) | st;
    }
    void fill(U page) {
        if (page >= pages) return;
        Slot& sl = slots[slotOf(page)];
        unsigned __int128 cur = __atomic_load_n(&sl.w.x, __ATOMIC_ACQUIRE);
        // THE POOL FIX (residual corruption #3, slot-collision contamination).
        // NEVER claim a slot another filler is mid-fill on. The old rule
        // claimed any non-ready slot, which disowned the owner's in-flight
        // pread; a later claim flips the buffer table BACK to the buffer the
        // disowned pread is still writing into, so two preads race in one
        // buffer and the final publish can name a page whose buffer holds
        // the slot-collision PARTNER page's bytes -- the reader's seqlock
        // recheck passes because the word never changed after the publish.
        // Live witness: pj2 served from partner hash page 251770 for
        // requested page 1300346 (grid dump, kill pairs 3+). Bounded checker
        // (pool-fix lane, faithful half-latch): STALE READ at depth 16 with
        // 2 fillers. Refusing to claim while FILLING makes every disowned
        // write impossible: only the owner writes, only the owner publishes,
        // and the owner's publish CAS can never fail (nobody can move the
        // word while it owns it), so FILLING is transient (one pread) and a
        // refused filler simply skips: the owner publishes and the reader
        // falls back to its synchronous thread-local page on the miss path.
        if (stateOf(cur) == ST_FILLING) return;   // an in-flight owner owns the slot
        if (pageOf(cur) == page && stateOf(cur) == ST_READY) return;   // advisory: ready
        // Claim: CAS128 from the latched word. The expectation carries the
        // full 64-bit version (a stale latch can never re-match) and the
        // buffer flip is part of the same atomic transition.
        unsigned __int128 mine = packW(page, 1 - bufOf(cur), verOf(cur) + 1, ST_FILLING);
        if (__atomic_compare_exchange_n(&sl.w.x, &cur, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            U myBuf = bufOf(mine);
            U off = page * PS;
            U need = std::min(PS, bytes - off);
            xw(fd, myBuf ? sl.b1 : sl.b0, need, off, "shared page fill");
            notedFill();
            // Publish: CAS128 from OUR claim (atomic with the ver bump).
            unsigned __int128 ready = packW(page, myBuf, verOf(mine) + 1, ST_READY);
            if (__atomic_compare_exchange_n(&sl.w.x, &mine, ready, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                // our page is live
            } else {
                torn();   // stolen mid-fill: disowned
            }
        }
    }
    // Reader halves (see the protocol comment above): two plain 8-byte
    // loads, acceptance on the low half alone. The compiler barrier in the
    // recheck pins the caller's copy between the two loads; x86 TSO needs
    // nothing else (loads are not reordered with loads).
    inline bool try_page(U page, const uint8_t*& out, uint64_t& loStamp) const {
        if (page >= pages) return false;
        const Slot& sl = slots[slotOf(page)];
        U lo1 = __atomic_load_n(&sl.w.h.lo, __ATOMIC_ACQUIRE);
        if ((lo1 & 3) != ST_READY) return false;
        U hi = __atomic_load_n(&sl.w.h.hi, __ATOMIC_ACQUIRE);
        if (((hi >> 3) - 1) != page) return false;
        out = (hi & 4) ? sl.b1 : sl.b0;
        loStamp = lo1;
        return true;
    }
    inline bool try_page_recheck(U page, uint64_t loStamp) const {
        if (page >= pages) return false;
        const Slot& sl = slots[slotOf(page)];
        asm volatile("" ::: "memory");   // the caller's copy stays above the recheck load
        return __atomic_load_n(&sl.w.h.lo, __ATOMIC_ACQUIRE) == loStamp;
    }
    inline void prefetch(U page) {
        if (page >= pages) return;
        // Advisory fast check with the reader halves (plain loads; a torn
        // view only enqueues a redundant hint - the fill path re-checks).
        U lo1 = __atomic_load_n(&slots[slotOf(page)].w.h.lo, __ATOMIC_ACQUIRE);
        U hi = 0;
        if ((lo1 & 3) == ST_READY) {
            hi = __atomic_load_n(&slots[slotOf(page)].w.h.hi, __ATOMIC_ACQUIRE);
            if (((hi >> 3) - 1) == page) return;
        }
        U t = qt.load(std::memory_order_relaxed);
        U h = qh.load(std::memory_order_acquire);
        if (t - h >= QCAP - 2) return;             // ring full: drop
        queue[t & (QCAP - 1)].store(page, std::memory_order_relaxed);
        // POOL-FIX RING RULE (pool-fix lane; stress-rig teardown-hang
        // witness): publish by monotonic CAS. A plain store of a stale t+1
        // could REGRESS qt below qh; the emptiness test h==t then never
        // holds again, the unsigned fullness check t-h inverts (producers
        // silently drop everything), and the workers spin past the stopping
        // check forever (pool teardown join hang). A lost CAS is just a
        // dropped hint; qt never regresses.
        U exp = t;
        qt.compare_exchange_strong(exp, t + 1, std::memory_order_release, std::memory_order_relaxed);
    }
    void worker() {
        unsigned idle = 0;
        for (;;) {
            U h = qh.load(std::memory_order_acquire);
            U t = qt.load(std::memory_order_acquire);   // pairs the producer's release CAS
            if (h == t) {
                if (stopping.load(std::memory_order_relaxed)) return;
                // Idle: brief pause-spin (no syscalls), then sleep; an idle
                // worker must not burn sched_yield.
                if (++idle < 200) { for (volatile int z = 0; z < 2000; ++z) {} continue; }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                idle = 0;
                continue;
            }
            idle = 0;
            U page = queue[h & (QCAP - 1)].load(std::memory_order_relaxed);
            if (!qh.compare_exchange_strong(h, h + 1, std::memory_order_acquire, std::memory_order_acquire))
                continue;   // taken by another worker: retry (pool-fix ring rule)
            fill(page);
        }
    }
};

// Grid-cell cache telemetry: exact per-thread counters (same sharding
// discipline as the pool telemetry - the old single-word atomics serialized
// every grid-cell materialize on one cacheline at high thread counts).
static constexpr unsigned CELL_TIDS = 4096;
static LocalCounter g_cellHitsArr[CELL_TIDS], g_cellMissArr[CELL_TIDS];   // zero-init BSS
static unsigned cell_tid() {
    static std::atomic<unsigned> next{0};
    thread_local unsigned t = next.fetch_add(1, std::memory_order_relaxed);
    return t < CELL_TIDS ? t : CELL_TIDS - 1;
}
static U cell_sum(const LocalCounter (&arr)[CELL_TIDS]) {
    U v = 0;
    for (unsigned z = 0; z < CELL_TIDS; ++z) v += arr[z].value;
    return v;
}

// Hoisted env checks: the hot reader paths used to call getenv() on every
// pool miss (hundreds of millions of libc calls per run at gate scales).
static bool async_fill() {
    static const bool v = getenv("CROSS_ASYNC_FILL") != nullptr;
    return v;
}
static bool no_pool() {
    static const bool v = getenv("CROSS_NO_POOL") != nullptr;
    return v;
}

// Cached windowed byte reader. Hot objects (the M2 text and the sparse hash
// column) attach a SharedPages pool; readers go: shared pool (seqlocked) ->
// per-thread 4-page fallback (synchronous pread, 16 KiB pages for bulk
// passes).
struct WinBytes {
    int fd = -1; U size = 0, base = 0, gen = 0;
    std::shared_ptr<SharedPages> pool;
    ~WinBytes() { pool.reset(); if (fd >= 0) ::close(fd); }
    void open_ro(const std::string& path, U bytes, U byteBase = 0, bool pooled = false) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) fail("open windowed byte file");
        size = bytes; base = byteBase; gen = g_winGen.fetch_add(1);
        if (pooled && !no_pool()) {
            pool = std::make_shared<SharedPages>();
            pool->start(fd, base + bytes, 8);
        }
    }
    struct TL { const void* o = nullptr; U gen = 0; U tag[4]{UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
                std::unique_ptr<uint8_t[]> d[4]; unsigned nxt = 0; };
    // The thread-local cache is a small table keyed by (object, generation):
    // several reader objects alternate on one thread (e.g. the two merged
    // children's position streams), and a single-slot cache would reset on
    // every alternation — one syscall per access.
    struct TLS4 { TL e[4]; unsigned nxt = 0; };
    inline TL& tlEntry() const {
        thread_local TLS4 tls;
        for (unsigned z = 0; z < 4; ++z)
            if (tls.e[z].o == (const void*)this && tls.e[z].gen == gen) return tls.e[z];
        TL& e = tls.e[tls.nxt++ & 3];
        e.o = (const void*)this; e.gen = gen;
        e.tag[0]=e.tag[1]=e.tag[2]=e.tag[3]=UINT64_MAX; e.nxt = 0;
        return e;
    }
    inline uint8_t at(U i) const {
        if (i >= size) fail("windowed byte bounds");
        if (pool) {
            U off = base + i;
            U page = off / SharedPages::PS;
            const uint8_t* p; uint64_t stamp;
            if (pool->try_page(page, p, stamp)) {
                U z = off & (SharedPages::PS - 1);
                uint8_t v = p[z];
                if (pool->try_page_recheck(page, stamp)) {
                    pool->hit();
                    return v;   // seqlock clean
                }
                pool->torn();
                // torn: fall through to the synchronous path
            }
            pool->miss();
            if (!async_fill()) pool->fill(page);   // default: synchronous fill (deterministic mode)
        }
        constexpr U B = 16384;
        TL& tls = tlEntry();
        U page = i / B;
        for (unsigned k = 0; k < 4; ++k) if (tls.tag[k] == page) return tls.d[k][i % B];
        unsigned slot = tls.nxt++ & 3;
        if (!tls.d[slot]) tls.d[slot].reset(new uint8_t[B]);
        U start = page * B, need = std::min(B, size - start);
        xw(fd, tls.d[slot].get(), need, base + start, "windowed byte page");
        tls.tag[slot] = page;
        return tls.d[slot][i % B];
    }
    // Short-span read: one seqlock for up to 128 bytes within one pool page
    // (the comparator's fast path, tail folds, and verification scans).
    inline void at_span(U i, void* out, U len) const {
        if (i + len > size) fail("windowed span bounds");
        if (pool && len <= 128 && (i & (SharedPages::PS - 1)) + len <= SharedPages::PS) {
            U off = base + i;
            U page = off / SharedPages::PS;
            const uint8_t* p; uint64_t stamp;
            if (pool->try_page(page, p, stamp)) {
                U z = off & (SharedPages::PS - 1);
                std::memcpy(out, p + z, len);
                if (pool->try_page_recheck(page, stamp)) { pool->hit(); return; }   // seqlock clean
                pool->torn();
            } else pool->miss();
        }
        // fallback: per-byte through at() (thread-local pages)
        uint8_t* q = (uint8_t*)out;
        for (U z = 0; z < len; ++z) q[z] = at(i + z);
    }
    void read(U i, void* buf, U len) const {   // bulk banded read
        if (i + len > size) fail("windowed byte bulk bounds");
        xw(fd, buf, len, base + i, "windowed byte bulk");
    }
    // Prefetch a logical range into the shared pool (probe/front speculation).
    void prefetch(U i, U len) const {
        if (!pool) return;
        U off = base + i;
        U p0 = off / SharedPages::PS, p1 = (off + len - 1) / SharedPages::PS;
        for (U pg = p0; pg <= p1 && pg - p0 < 8; ++pg) pool->prefetch(pg);
    }
};

static bool wm_pack_enabled() {
    const char* e = getenv("CROSS_WM_PACK");
    return e && e[0] == '1' && e[1] == '\0';
}
static unsigned wm_value_bits(U n) {
    return unsigned(std::max<U>(1, 64 - __builtin_clzll(n + 9)));
}
struct WmHeader {
    uint32_t magic;       // 0x31574D50 = 'PWM1' LE
    uint32_t wbits;
    uint64_t count;
    uint32_t blockVals;   // 4096
    uint32_t blockBytes;
    uint64_t reserved;    // 0
};
static constexpr U WM_BLOCK_VALS = 4096;
static U wm_block_bytes(unsigned w) { return (WM_BLOCK_VALS * U(w) + 7) / 8; }
static U wm_packed_size(U n, unsigned w) {
    return sizeof(WmHeader) + ((n + WM_BLOCK_VALS - 1) / WM_BLOCK_VALS) * wm_block_bytes(w);
}

// Cached u64-column reader with byte base and optional constant shift.
struct WinU64 {
    int fd = -1; U base = 0, count = 0, shift = 0, gen = 0;
    unsigned wbits = 0;   // packed-WM mode (open_ro_auto on a PWM1 file); 0 = legacy u64 column
    U blockBytes = 0;
    std::shared_ptr<SharedPages> pool;
    ~WinU64() { pool.reset(); if (fd >= 0) ::close(fd); }
    void open_ro(const std::string& path, U elems, U byteBase, U shiftIn = 0, bool pooled = false) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) fail("open windowed u64 file");
        base = byteBase; count = elems; shift = shiftIn; gen = g_winGen.fetch_add(1);
        if (pooled && !no_pool()) {
            pool = std::make_shared<SharedPages>();
            pool->start(fd, base + 8 * elems, 8);
        }
    }
    // Auto-detect the WM scratch layout: legacy u64-per-row (size == 8*elems)
    // or the packed PWM1 block format. Readers stay layout-agnostic.
    void open_ro_auto(const std::string& path, U elems) {
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) fail("stat wm scratch");
        if (uint64_t(st.st_size) == 8 * elems) {
            open_ro(path, elems, 0);
            return;
        }
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) fail("open packed wm");
        count = elems; base = 0; shift = 0; gen = g_winGen.fetch_add(1);
        WmHeader hdr{};
        xw(fd, &hdr, sizeof hdr, 0, "packed wm header");
        if (hdr.magic != 0x31574D50) fail("packed wm magic");
        if (hdr.count != elems) fail("packed wm count");
        if (hdr.blockVals != WM_BLOCK_VALS) fail("packed wm block shape");
        if (hdr.wbits < 1 || hdr.wbits >= 64) fail("packed wm width");
        if (hdr.blockBytes != wm_block_bytes(hdr.wbits)) fail("packed wm block bytes");
        if (uint64_t(st.st_size) != wm_packed_size(elems, hdr.wbits)) fail("packed wm size");
        wbits = hdr.wbits;
        blockBytes = hdr.blockBytes;
    }
    struct TL { const void* o = nullptr; U gen = 0; U tag[2]{UINT64_MAX,UINT64_MAX};
               U d[2][4096]; unsigned nxt = 0; };
    struct TLS4 { TL e[4]; unsigned nxt = 0; };
    inline TL& tlEntry() const {
        thread_local TLS4 tls;
        for (unsigned z = 0; z < 4; ++z)
            if (tls.e[z].o == (const void*)this && tls.e[z].gen == gen) return tls.e[z];
        TL& e = tls.e[tls.nxt++ & 3];
        e.o = (const void*)this; e.gen = gen;
        e.tag[0]=e.tag[1]=UINT64_MAX; e.nxt = 0;
        return e;
    }
    inline U at(U i) const {
        if (i >= count) fail("windowed u64 bounds");
        U val;
        if (wbits) {   // packed WM: 4096-value blocks behind the header
            TL& tls = tlEntry();
            U page = i / 4096;
            for (unsigned k = 0; k < 2; ++k) if (tls.tag[k] == page) return tls.d[k][i % 4096] + shift;
            unsigned slot = tls.nxt++ & 1;
            loadPackedPage(page, tls.d[slot]);
            tls.tag[slot] = page;
            return tls.d[slot][i % 4096] + shift;
        }
        if (pool) {
            U off = base + 8 * i;
            U page = off / SharedPages::PS;
            const uint8_t* p; uint64_t stamp;
            if (pool->try_page(page, p, stamp)) {
                U z = off & (SharedPages::PS - 1);
                std::memcpy(&val, p + z, 8);
                if (pool->try_page_recheck(page, stamp)) {
                    pool->hit();
                    return val + shift;
                }
                pool->torn();
            } else pool->miss();
            if (!async_fill()) pool->fill(page);   // default: synchronous fill (deterministic mode)
        }
        TL& tls = tlEntry();
        U page = i / 4096;
        for (unsigned k = 0; k < 2; ++k) if (tls.tag[k] == page) return tls.d[k][i % 4096] + shift;
        unsigned slot = tls.nxt++ & 1;
        U start = page * 4096;
        U need = std::min<U>(4096, count - start);
        xw(fd, tls.d[slot], 8 * need, base + 8 * start, "windowed u64 page");
        tls.tag[slot] = page;
        return tls.d[slot][i % 4096] + shift;
    }
    // Unpack one 4096-value packed block into dst (the thread-local page
    // cache serves it; the WM read pattern is the emission's mostly-
    // sequential row scan plus pile_finish's small class-window preads).
    void loadPackedPage(U page, U* dst) const {
        thread_local std::vector<uint8_t> staging;
        if (staging.size() != blockBytes) staging.resize(blockBytes);
        xw(fd, staging.data(), blockBytes, sizeof(WmHeader) + page * blockBytes, "packed wm page");
        const unsigned w = wbits;
        __uint128_t res = 0; unsigned avail = 0; U bi = 0;
        const U mask = (U(1) << w) - 1;
        for (U k = 0; k < WM_BLOCK_VALS; ++k) {
            while (avail < w) { res |= __uint128_t(staging[bi]) << avail; ++bi; avail += 8; }
            dst[k] = U(res) & mask;
            res >>= w;
            avail -= w;
        }
    }
    void read(U i, U* buf, U len) const {
        if (i + len > count) fail("windowed u64 bulk bounds");
        if (wbits) {
            U done = 0;
            while (done < len) {
                U page = (i + done) / 4096;
                U in = (i + done) % 4096;
                U take = std::min<U>(len - done, 4096 - in);
                take = std::min<U>(take, count - page * 4096 - in);
                U tmp[4096];
                loadPackedPage(page, tmp);
                std::memcpy(buf + done, tmp + in, 8 * take);
                done += take;
            }
            if (shift) for (U k = 0; k < len; ++k) buf[k] += shift;
            return;
        }
        xw(fd, buf, 8 * len, base + 8 * i, "windowed u64 bulk");
        if (shift) for (U k = 0; k < len; ++k) buf[k] += shift;
    }
    void prefetch(U i) const {
        if (!pool) return;
        pool->prefetch((base + 8 * i) / SharedPages::PS);
    }
};
// ------------------------------------------------------------ packed WM
// CROSS_WM_PACK=1: wm.bin - the merged SA in ROW order, the walk->emission
// pipe consumed by emit_four_ext/emit_sxcr_ext and pile_finish's seam
// machinery - is stored w-bits-per-value LSB-first (w = bits(n+9), the
// pieces' packed-SD convention) in fixed 4096-value blocks behind a
// 32-byte PWM1 header, instead of u64-per-row: 8n -> ~w/8 n (3.75n at
// 1GB, 5.125n at pile scale) off the merge transient. The WM is scratch
// (deleted at pair end, never byte-gated directly); the arbiter stays
// rlebwt/meta/pieces/witness/chi byte-identity. Block layout (values are
// arbitrary SA positions - w-packing is their information-theoretic
// floor; blocks are fixed-size so random access is O(1)):
//   bytes [0,32):  magic u32 'PWM1', wbits u32, count u64,
//                   blockVals u32 (=4096), blockBytes u32, reserved u64
//   block b at byte 32 + b*blockBytes: values [b*4096, min(n,(b+1)*4096))
//   packed LSB-first, zero-padded to blockBytes.
// Parallel block packer: walk shards produce disjoint row ranges; a block
// is packed and written exactly when its last value arrives (the completing
// thread does the I/O - no block's bytes are ever written by two threads).
// Staging stays bounded by the in-flight blocks (<= one boundary block per
// shard boundary).
struct WmPacker {
    int fd = -1;
    std::string path;
    unsigned w = 0;
    U n = 0, blockBytes = 0;
    struct Staging {
        std::vector<U> vals;
        std::atomic<U> filled{0};
    };
    std::mutex mu;
    std::unordered_map<U, std::shared_ptr<Staging>> live;
    void open(const std::string& wmPath, U count) {
        path = wmPath;
        w = wm_value_bits(count);
        require(w >= 1 && w < 64, "packed wm width");
        n = count;
        blockBytes = wm_block_bytes(w);
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        require(fd >= 0, "create packed wm scratch");
        WmHeader hdr{0x31574D50, w, count, uint32_t(WM_BLOCK_VALS), uint32_t(blockBytes), 0};
        xput(fd, &hdr, sizeof hdr, 0, "packed wm header");
    }
    void put(U idx, U v) {
        require(idx < n, "packed wm bounds");
        U b = idx / WM_BLOCK_VALS;
        thread_local std::unordered_map<const WmPacker*, std::pair<U, std::shared_ptr<Staging>>> cache;
        auto& c = cache[this];
        std::shared_ptr<Staging> st = c.second;
        if (!st || c.first != b) {
            std::lock_guard<std::mutex> g(mu);
            auto& p = live[b];
            if (!p) {
                p = std::make_shared<Staging>();
                p->vals.assign(WM_BLOCK_VALS, 0);
            }
            st = p;
            c.first = b;
            c.second = st;
        }
        U blockSize = std::min<U>(WM_BLOCK_VALS, n - b * WM_BLOCK_VALS);
        require(st->filled.load(std::memory_order_relaxed) < blockSize,
                "late put to a completed packed-WM block");
        require(v < (U(1) << w), "packed wm value overflows wbits");
        st->vals[idx - b * WM_BLOCK_VALS] = v;
        if (st->filled.fetch_add(1, std::memory_order_acq_rel) + 1 == blockSize) {
            pack_block(b, *st);
            std::lock_guard<std::mutex> g(mu);
            live.erase(b);
            c.second.reset();
        }
    }
    void pack_block(U b, Staging& st) {
        U valsInBlock = std::min<U>(WM_BLOCK_VALS, n - b * WM_BLOCK_VALS);
        // Fixed-size blocks (512*w bytes, word-aligned) so random access is
        // O(1); the final partial block is zero-padded to blockBytes.
        U words = blockBytes / 8;
        require(words * 8 == blockBytes, "packed wm block alignment");
        std::vector<U> wout(words, 0);
        __uint128_t res = 0; unsigned avail = 0; U wi = 0;
        for (U i = 0; i < valsInBlock; ++i) {
            res |= __uint128_t(st.vals[i]) << avail; avail += w;
            if (avail >= 64) { wout[wi++] = U(res); res >>= 64; avail -= 64; }
        }
        if (avail) wout[wi] = U(res);
        xput(fd, wout.data(), blockBytes, sizeof(WmHeader) + b * blockBytes, "packed wm block");
    }
    void close() {
        require(fd >= 0, "packed wm close");
        {
            std::lock_guard<std::mutex> g(mu);
            require(live.empty(), "packed wm closed with incomplete blocks");
        }
        require(::close(fd) == 0, "packed wm close fd");
        fd = -1;
        // Completion sentinel, rename-published AFTER the last block: the
        // packed blocks land in arbitrary order, so file size alone does
        // not imply completeness (the legacy u64 layout kept the banked
        // size==8n approximation; the packed layout gets the exact gate).
        std::string tmp = path + ".done.tmp", done = path + ".done";
        int sfd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        require(sfd >= 0, "create packed wm done tmp");
        require(::write(sfd, "1\n", 2) == 2, "write packed wm done");
        require(::close(sfd) == 0, "close packed wm done");
        require(::rename(tmp.c_str(), done.c_str()) == 0, "publish packed wm done");
    }
};
// Corpus-referenced side text. A side's text is either embedded in its
// SXS2 sidecar (intermediates; legacy raw-chunk walks) or REFERENCED: chunk
// artifacts carry (corpus path, offset, n) + the 256-byte remap, and the
// view is remap[corpus[offset+i]] — the text is never materialized (it
// already exists in the corpus; the disc discipline kills the per-side text
// copies). One API in two backing modes; consumers are bulk passes and
// per-row random reads, so a small thread-local page cache suffices (the
// merge comparison uses the corpus-backed virtual view below).
// One referenced text segment: bytes [off, off+len) of `path`, mapped
// through `remap`. A side's text is a CONCATENATION of segments (its refs):
// a corpus window is one segment; a merged intermediate's text is its input
// sides' segments in order - no merged text is ever copied (the progressive
// shape's invariant: level N's output is a valid level N+1 input).
struct TextSeg {
    std::string path;
    U off = 0, len = 0;
    std::array<uint8_t, 256> remap{};
};
// Varint (LEB128) for the compact run stream: every absolute column stays
// 64-bit (positions live in the persisted order column); the varint is
// only the run LENGTH, bounded by the chunk size, checked on decode.
static void put_varint(std::vector<char>& out, U v) {
    while (v >= 0x80) { out.push_back(char(uint8_t(v) | 0x80)); v >>= 7; }
    out.push_back(char(uint8_t(v)));
}
struct RunCursor {   // streams run records from a v2 (25B) or v3 (varint) file
    std::ifstream f;
    U version = 0;
    char vbuf[16]; size_t vlen = 0, vused = 0;   // varint record buffer
    explicit RunCursor(const std::string& path, U ver) : f(path, std::ios::binary), version(ver) {
        require(bool(f), "reopen chunk runs");
        f.seekg(32);
        if (ver == 3) { f.read(vbuf, 16); vlen = size_t(f.gcount()); }
    }
    void refill() {
        require(vused == vlen, "varint refill alignment");
        f.read(vbuf, 16); vlen = size_t(f.gcount()); vused = 0;
        require(vlen > 0, "truncated varint run stream");
    }
    uint8_t byte() {
        if (version == 2) { char b; f.read(&b, 1); require(bool(f), "truncated run record"); return uint8_t(b); }
        if (vused == vlen) refill();
        return uint8_t(vbuf[vused++]);
    }
    U fixed(U bytes) {   // v2 little-endian field
        U v = 0;
        for (U i = 0; i < bytes; ++i) { char b; f.read(&b, 1); require(bool(f), "truncated run record"); v |= U(uint8_t(b)) << (8 * i); }
        return v;
    }
    U varint() {
        U v = 0;
        for (unsigned shift = 0; ; shift += 7) {
            uint8_t b = byte();
            require(shift < 63, "varint overflow");
            v |= U(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
    }
    // next record; for v2 fills h/t from the file, for v3 leaves them 0
    // (samples come from the persisted order column).
    void next(uint8_t& c, U& len, U& h, U& t) {
        c = byte();
        if (version == 2) {
            len = fixed(8); h = fixed(8); t = fixed(8);
        } else {
            len = varint(); h = 0; t = 0;
        }
    }
};
struct SideText {
    int fd = -1;
    U n = 0, base = 0, gen = 0;   // base: text byte offset (sidecar 20 / corpus range start)
    bool remapped = false;
    std::array<uint8_t, 256> map{};
    std::vector<TextSeg> segs;           // segment mode (referenced text)
    std::vector<U> segBase;              // cumulative segment starts
    static std::atomic<U> g_gen;
    void open_sidecar(const std::string& path, U elems, U byteBase) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) fail("open sidecar text");
        n = elems; base = byteBase; remapped = false; gen = g_gen.fetch_add(1);
    }
    void open_corpus(const std::string& path, U elems, U corpusOff,
                     const std::array<uint8_t, 256>& remap) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) fail("open corpus (referenced chunk text)");
        struct stat st{};
        require(fstat(fd, &st) == 0 && U(st.st_size) >= corpusOff + elems,
                "corpus range out of bounds (referenced chunk)");
        n = elems; base = corpusOff; remapped = true; map = remap; gen = g_gen.fetch_add(1);
    }
    void open_segments(std::vector<TextSeg> list, U elems) {
        segs = std::move(list);
        segBase.assign(segs.size() + 1, 0);
        for (size_t z = 0; z < segs.size(); ++z) segBase[z + 1] = segBase[z] + segs[z].len;
        require(segBase.back() == elems, "referenced text segments do not cover the side");
        n = elems; gen = g_gen.fetch_add(1);
    }
    inline size_t segOf(U i) const {
        return size_t(std::upper_bound(segBase.begin(), segBase.end() - 1, i) - segBase.begin()) - 1;
    }
    ~SideText() { if (fd >= 0) ::close(fd); }
    SideText() = default;
    SideText(const SideText&) = delete;
    SideText& operator=(const SideText&) = delete;
    struct TL { const void* o = nullptr; U gen = 0; U tag[2]{UINT64_MAX, UINT64_MAX};
                U segOfTag[2]{}; std::unique_ptr<uint8_t[]> d[2]; unsigned nxt = 0; };
    struct TLS4 { TL e[4]; unsigned nxt = 0; };
    inline TL& tlEntry() const {
        thread_local TLS4 tls;
        for (unsigned z = 0; z < 4; ++z)
            if (tls.e[z].o == (const void*)this && tls.e[z].gen == gen) return tls.e[z];
        TL& e = tls.e[tls.nxt++ & 3];
        e.o = (const void*)this; e.gen = gen;
        e.tag[0] = e.tag[1] = UINT64_MAX; e.nxt = 0;
        return e;
    }
    inline uint8_t at(U i) const {
        if (i >= n) fail("side text bounds");
        if (!segs.empty()) {   // segment mode: 2-page cache keyed (segment, page)
            size_t z = segOf(i);
            const TextSeg& s = segs[z];
            U within = i - segBase[z];
            constexpr U B = 16384;
            U page = within / B;
            TL& tls = tlEntry();
            for (unsigned k = 0; k < 2; ++k)
                if (tls.tag[k] == page && tls.segOfTag[k] == U(z)) {
                    uint8_t raw = tls.d[k][within % B];
                    return s.remap[raw];
                }
            unsigned slot = tls.nxt++ & 1;
            if (!tls.d[slot]) tls.d[slot].reset(new uint8_t[B]);
            U start = page * B, need = std::min<U>(B, s.len - start);
            int f = ::open(s.path.c_str(), O_RDONLY);
            if (f < 0) fail("open referenced text segment");
            xw(f, tls.d[slot].get(), need, s.off + start, "referenced text page");
            ::close(f);
            tls.tag[slot] = page; tls.segOfTag[slot] = U(z);
            uint8_t raw = tls.d[slot][within % B];
            return s.remap[raw];
        }
        constexpr U B = 16384;
        TL& tls = tlEntry();
        U page = i / B;
        for (unsigned k = 0; k < 2; ++k)
            if (tls.tag[k] == page) {
                uint8_t raw = tls.d[k][i % B];
                return remapped ? map[raw] : raw;
            }
        unsigned slot = tls.nxt++ & 1;
        if (!tls.d[slot]) tls.d[slot].reset(new uint8_t[B]);
        U start = page * B, need = std::min(B, n - start);
        xw(fd, tls.d[slot].get(), need, base + start, "side text page");
        tls.tag[slot] = page;
        uint8_t raw = tls.d[slot][i % B];
        return remapped ? map[raw] : raw;
    }
    void read(U i, void* buf, U len) const {   // bulk banded read (+per-seg remap)
        if (i + len > n) fail("side text bulk bounds");
        uint8_t* q = (uint8_t*)buf;
        if (!segs.empty()) {
            U done = 0;
            while (done < len) {
                size_t z = segOf(i + done);
                const TextSeg& s = segs[z];
                U within = i + done - segBase[z];
                U take = std::min<U>(len - done, s.len - within);
                int f = ::open(s.path.c_str(), O_RDONLY);
                if (f < 0) fail("open referenced text segment");
                xw(f, q + done, take, s.off + within, "referenced text bulk");
                ::close(f);
                for (U t = 0; t < take; ++t) q[done + t] = s.remap[q[done + t]];
                done += take;
            }
            return;
        }
        constexpr U W = 1u << 22;
        U done = 0;
        while (done < len) {
            U take = std::min<U>(W, len - done);
            xw(fd, q + done, take, base + i + done, "side text bulk");
            if (remapped) for (U z = 0; z < take; ++z) q[done + z] = map[q[done + z]];
            done += take;
        }
    }
};
std::atomic<U> SideText::g_gen{1};
// Virtual doubled text: coordinates [0,2n) wrap into the original
// referenced corpus segments. A fitting corpus is remapped once into RAM;
// larger corpora retain bounded WinBytes pages and remap on return.
// The old path DID use its corpus pool: the banked sol5i 1GB run records
// 30,567,717,347 hits / 2,992,893 misses. Its cost was repeated short-span
// copies/remapping (and per-byte pool probes on page-crossing spans), not
// absent hits. The 5-fill POOL_STATS printed during hash-build belongs to
// that builder's temporary boundary-check hash reader, not the walk.
struct CorpusText {
    U n = 0;
    bool pool = false;
    std::vector<TextSeg> segs;
    std::vector<U> starts, fileIndex;
    std::vector<std::string> paths;
    std::vector<std::unique_ptr<WinBytes>> files;
    // Immutable, remapped corpus RAM replaces (never supplements) this view's
    // pool when it fits the same buffer budget. No doubled text is stored.
    std::unique_ptr<uint8_t[]> cached;
    // A private counter must also own its cache line. Adjacent U counters
    // serialize P borrowing producers on each span despite immutable text.
    using CacheHit = LocalCounter;
    static_assert(sizeof(CacheHit) == 64, "cache telemetry line ownership");
    mutable std::array<CacheHit, SharedPages::TIDS> cacheHits{};
    ~CorpusText() {
        if (cached) {
            U hits = 0; for (const auto& h : cacheHits) hits += h.value;
            std::fprintf(stderr, "CORPUS_CACHE_STATS bytes=%llu hits=%llu demand_misses=0 preload_bytes=%llu\n",
                (unsigned long long)n, (unsigned long long)hits, (unsigned long long)n);
        }
    }
    // The data are immutable after open(), so a span can be borrowed without
    // pool probes, seqlocks, copies, or per-symbol remapping. Wraps retain the
    // general reader. Each charging thread owns its telemetry entry.
    inline const uint8_t* span(U i, uint8_t* fallback, U len) const {
        if (cached && i < 2 * n) {
            U p = i >= n ? i - n : i;
            if (len <= n - p) {
                unsigned t = SharedPages::tid();
                // tid() saturates in long-lived selftests; overflow threads
                // share only the last counter and must charge it atomically.
                if (t == SharedPages::TIDS - 1)
                    __atomic_fetch_add(&cacheHits[t].value, U(1), __ATOMIC_RELAXED);
                else ++cacheHits[t].value;
                return cached.get() + p;
            }
        }
        at_span(i, fallback, len);
        return fallback;
    }
    void open(std::vector<TextSeg> list, bool pooled = false) {
        for (auto& s : list) {
            if (!s.len) continue;
            if (!segs.empty() && segs.back().path == s.path &&
                segs.back().off + segs.back().len == s.off && segs.back().remap == s.remap)
                segs.back().len += s.len;
            else segs.push_back(std::move(s));
        }
        U cacheBytes = 0;
        for (const auto& s : segs) cacheBytes += s.len;
        const bool cacheFits = pooled && !no_pool() &&
            cacheBytes <= 2 * SharedPages::pool_slot_count() * SharedPages::PS;
        for (const auto& s : segs) {
            starts.push_back(n); n += s.len;
            auto it = std::find(paths.begin(), paths.end(), s.path);
            U f = U(it - paths.begin());
            if (it == paths.end()) {
                paths.push_back(s.path);
                auto r = std::make_unique<WinBytes>();
                U bytes = fs::file_size(s.path);
                r->open_ro(s.path, bytes, 0, pooled && !cacheFits && !pool && bytes >= (1u << 20));
                pool = pool || bool(r->pool);
                files.push_back(std::move(r));
            }
            require(s.off <= files[f]->size && s.len <= files[f]->size - s.off,
                    "virtual corpus segment bounds");
            fileIndex.push_back(f);
        }
        require(n && n <= UINT64_MAX / 2, "virtual corpus length");
        if (cacheFits) {
            cached.reset(new uint8_t[n]);
            // read() remaps each segment exactly once, before publication to
            // comparator threads. It deliberately bypasses the page pools.
            read(0, cached.get(), n);
            std::fprintf(stderr, "CORPUS_RAM_CACHE bytes=%llu budget_bytes=%llu immutable=1 remapped=1 doubled=0\n",
                (unsigned long long)n,
                (unsigned long long)(2 * SharedPages::pool_slot_count() * SharedPages::PS));
        }
    }
    inline size_t segment(U i) const {
        if (segs.size() == 1) return 0;
        return size_t(std::upper_bound(starts.begin(), starts.end(), i) - starts.begin() - 1);
    }
    inline uint8_t at(U i) const {
        if (i >= 2 * n) fail("virtual corpus byte bounds");
        if (i >= n) i -= n;
        if (cached) return cached[i];
        size_t z = segment(i);
        const auto& s = segs[z];
        return s.remap[files[fileIndex[z]]->at(s.off + i - starts[z])];
    }
    void at_span(U i, void* out, U len) const {
        if (i > 2 * n || len > 2 * n - i) fail("virtual corpus span bounds");
        auto* q = static_cast<uint8_t*>(out);
        if (cached) {
            U p = i >= n ? i - n : i;
            U take = std::min(len, n - p);
            std::memcpy(q, cached.get() + p, take);
            if (take < len) std::memcpy(q + take, cached.get(), len - take);
            return;
        }
        while (len) {
            U p = i >= n ? i - n : i;
            size_t z = segment(p);
            const auto& s = segs[z];
            U take = std::min(len, s.len - (p - starts[z]));
            files[fileIndex[z]]->at_span(s.off + p - starts[z], q, take);
            for (U t = 0; t < take; ++t) q[t] = s.remap[q[t]];
            q += take; i += take; len -= take;
        }
    }
    void read(U i, void* out, U len) const {
        if (i > 2 * n || len > 2 * n - i) fail("virtual corpus bulk bounds");
        auto* q = static_cast<uint8_t*>(out);
        constexpr U W = 1u << 22;
        while (len) {
            U p = i >= n ? i - n : i;
            size_t z = segment(p);
            const auto& s = segs[z];
            U take = std::min({len, s.len - (p - starts[z]), W});
            files[fileIndex[z]]->read(s.off + p - starts[z], q, take);
            for (U t = 0; t < take; ++t) q[t] = s.remap[q[t]];
            q += take; i += take; len -= take;
        }
    }
    void prefetch(U i, U len) const {
        if (cached && len && i < 2 * n) {
            U p = i >= n ? i - n : i;
            __builtin_prefetch(cached.get() + p, 0, 3);
            if (len > 1 && len <= n - p)
                __builtin_prefetch(cached.get() + p + len - 1, 0, 3);
            return;
        }
        if (!pool || !len || i >= 2 * n) return;
        len = std::min(len, 2 * n - i);
        while (len) {
            U p = i >= n ? i - n : i;
            size_t z = segment(p);
            const auto& s = segs[z];
            U take = std::min(len, s.len - (p - starts[z]));
            files[fileIndex[z]]->prefetch(s.off + p - starts[z], take);
            i += take; len -= take;
        }
    }
};
// Overlay of repaired slices over an immutable position column: the repair
// writes each sorted block slice sequentially and registers [lo,hi) with its
// file offset. Anchor blocks NEST (the backward-search walk shrinks the row
// interval as the suffix grows), so a later region overrides an earlier one
// for shared rows — matching the resident in-place semantics exactly.
// Region count per side is bounded by the walk depth (small); lookups scan
// backward, last pushed first.
// SXP4 order-column WRITER: "SXP4" | u32 ver | u64 offset | u64 n |
// u64 period | u64 rsvd | u64 groups | per group: startRow, byteOff,
// basePos (3 x u64) | the zigzag-varint delta stream (4096-row groups).
// Encoder takes a pull function to stay independent of the row source.
static void write_pos_sxp4(const std::string& path, U offset, U n, U period,
                            const std::function<U(U)>& rowPos) {
    constexpr U GRP = 4096;
    U groups = (n + GRP - 1) / GRP;
    std::vector<U> tab(3 * groups);
    std::vector<uint8_t> stream;
    stream.reserve(n * 4 + 64);
    std::vector<uint8_t> rec;
    auto putVar = [&](U v) {
        while (v >= 0x80) { stream.push_back(uint8_t(v) | 0x80); v >>= 7; }
        stream.push_back(uint8_t(v));
    };
    std::vector<U> pos(GRP);
    for (U g = 0; g < groups; ++g) {
        U b0 = g * GRP, len = std::min<U>(GRP, n - b0);
        tab[3 * g] = b0;
        tab[3 * g + 1] = 0;   // fixed after the sizes are known
        for (U i = 0; i < len; ++i) pos[i] = rowPos(b0 + i);
        tab[3 * g + 2] = len ? pos[0] : 0;
        tab[3 * g + 1] = stream.size();
        for (U i = 1; i < len; ++i) {
            U d = U(pos[i] >= pos[i - 1]) * 2 * (pos[i] - pos[i - 1]) + U(pos[i] < pos[i - 1]) * (2 * (pos[i - 1] - pos[i]) - 1);   // standard zigzag
            putVar(d);
        }
    }
    int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
    require(fd >= 0, "create SXP4 pos column");
    char hdr[56];
    std::memset(hdr, 0, sizeof hdr);
    std::memcpy(hdr, "SXP4", 4);
    auto w32 = [&](char* b, uint32_t v) { for (int i = 0; i < 4; ++i) b[i] = char(v >> (8 * i)); };
    auto w64 = [&](char* b, U v) { for (int i = 0; i < 8; ++i) b[i] = char(v >> (8 * i)); };
    w32(hdr + 4, 1); w64(hdr + 8, offset); w64(hdr + 16, n); w64(hdr + 24, period);
    w64(hdr + 32, 0); w64(hdr + 40, groups);
    xput(fd, hdr, 56, 0, "sxp4 header");
    std::vector<char> tb(24 * groups);
    for (U g = 0; g < groups; ++g) {
        w64(tb.data() + 24 * g, tab[3 * g]);
        w64(tb.data() + 24 * g + 8, 56 + 24 * groups + tab[3 * g + 1]);
        w64(tb.data() + 24 * g + 16, tab[3 * g + 2]);
    }
    xput(fd, tb.data(), tb.size(), 56, "sxp4 group table");
    if (!stream.empty()) xput(fd, stream.data(), stream.size(), 56 + 24 * groups, "sxp4 delta stream");
    require(::close(fd) == 0, "close SXP4 pos column");
}
// Order-column reader for a side: DENSE (SXP2/SXP3: 8-byte positions after
// a 20/40-byte header) or COMPACT (SXP4: per-row-group base position +
// zigzag varint deltas - measured 3.13 B/position on pile-like 100MB text,
// the 8n -> ~3.1n chunk-disc lever; absolute positions stay 64-bit in the
// bases, only the bounded within-group deltas are narrow). Random access:
// binary search the group table (group = 4096 rows), decode to the row.
// Same surface PosOverlay consumed from WinU64 (at / bulk read, + shift).
struct PosColumn {
    int fd = -1;
    U count = 0, byteBase = 0, shift = 0, fileSize = 0;
    bool compact = false;
    U headerBytes = 0, groups = 0;
    std::unique_ptr<U[]> tab;   // 3 u64 per group: startRow, byteOff, basePos
    struct TL { int fd = -1; U tag[2]{UINT64_MAX,UINT64_MAX}; std::unique_ptr<uint8_t[]> d[2];
                U dv[2][4096]; unsigned nxt = 0; U gen = 0; };   // dense cache
    struct TLS4 { TL e[4]; unsigned nxt = 0; };
    static std::atomic<U> g_gen;
    U gen = 0;
    bool entropy = false;
    std::array<std::array<U,33>,5> modelFirstCode{}, modelFirstSymbol{}, modelCodeCounts{};
    std::array<std::vector<uint8_t>,5> modelAlphabet;
    void open_ro(const std::string& path, U elems, U posByteBase, U shiftIn) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) fail("open pos column");
        count = elems; byteBase = posByteBase; shift = shiftIn; gen = g_gen.fetch_add(1);
        char magic[4]{};
        pread_exact(magic, 4, 0);
        if (!std::memcmp(magic, "SXP4", 4)) {
            compact = true;
            char hdr[56];
            pread_exact(hdr, 56, 0);
            // "SXP4" | u32 ver | u64 offset | u64 n | u64 period | u64 rsvd | u64 groups
            U n2 = le64(hdr + 16);
            require(n2 == count, "SXP4 header n mismatch");
            require(le64(hdr + 8) == le64(hdr + 8) || true, "");
            groups = le64(hdr + 40);
            headerBytes = 56 + 24 * groups;
            require(groups && groups < (1ull << 32), "SXP4 group count");
            struct stat st{};
            require(fstat(fd, &st) == 0, "stat SXP4");
            fileSize = U(st.st_size);
            tab.reset(new U[3 * groups]);
            char* tb = new char[24 * groups];
            pread_exact(tb, 24 * groups, 56);
            for (U g = 0; g < groups; ++g) {
                tab[3 * g] = le64(tb + 24 * g);          // startRow
                tab[3 * g + 1] = le64(tb + 24 * g + 8);   // byteOff
                tab[3 * g + 2] = le64(tb + 24 * g + 16);  // basePos
            }
            delete[] tb;
            const unsigned version=uint8_t(hdr[4]) | (unsigned(uint8_t(hdr[5]))<<8);
            require(version==1 || version==2,"unsupported SXP4 version");
            entropy=version==2;
            if (entropy) {
                for (unsigned context=0;context<5;++context) {
                auto& firstCode=modelFirstCode[context]; auto& firstSymbol=modelFirstSymbol[context];
                auto& codeCounts=modelCodeCounts[context]; auto& alphabet=modelAlphabet[context];
                uint8_t lengths[256]; pread_exact(lengths,256,headerBytes+256*context);
                std::vector<std::pair<unsigned,unsigned>> sorted;
                for (unsigned c=0;c<256;++c) if (lengths[c]) {
                    require(lengths[c]<=32,"SXP4 Huffman length");
                    sorted.push_back({lengths[c],c}); ++codeCounts[lengths[c]];
                }
                require(!sorted.empty(),"SXP4 empty Huffman model");
                std::sort(sorted.begin(),sorted.end());
                for (auto item:sorted) alphabet.push_back(uint8_t(item.second));
                U code=0, base=0;
                for (unsigned len=1;len<=32;++len) {
                    code=(code+codeCounts[len-1])<<1;
                    firstCode[len]=code; firstSymbol[len]=base; base+=codeCounts[len];
                    require(code+codeCounts[len]<=(1ull<<len),"SXP4 oversubscribed model");
                }
                }
                require(fileSize>=headerBytes+1280,"SXP4 model truncated");
            }
        } else {
            compact = false;
            headerBytes = byteBase;
        }
    }
    ~PosColumn() { if (fd >= 0) ::close(fd); }
    PosColumn() = default;
    PosColumn(const PosColumn&) = delete;
    PosColumn& operator=(const PosColumn&) = delete;
    void pread_exact(void* buf, U len, U off) {
        char* q = (char*)buf;
        while (len) { ssize_t z = pread(fd, q, len, off); if (z <= 0) fail("pos column read");
            q += z; off += U(z); len -= U(z); }
    }
    static U le64(const char* b) { U v = 0; for (int i = 0; i < 8; ++i) v |= U(uint8_t(b[i])) << (8 * i); return v; }
    inline U groupOf(U i, U& g) const {   // largest g with tab[3g] <= i
        U lo = 0, hi = groups;
        while (lo + 1 < hi) { U mid = (lo + hi) / 2; if (tab[3 * mid] <= i) lo = mid; else hi = mid; }
        g = lo;
        return tab[3 * g];
    }
    // decode positions [start, end) of group g into out (SXP4)
    void decodeGroup(U g, U start, U end, U* out) {
        U b0 = tab[3 * g], boff = tab[3 * g + 1], base = tab[3 * g + 2];
        U spanEnd = (g + 1 < groups) ? tab[3 * (g + 1) + 1] : fileSize;
        require(spanEnd >= boff, "SXP4 group span");   // a 1-row group has no deltas
        std::vector<uint8_t> buf(spanEnd - boff);
        if (!buf.empty()) pread_exact(buf.data(), spanEnd - boff, boff);
        U pos = base, idx = b0, p = 0, bit=0;
        auto nextByte = [&](unsigned context) -> uint8_t {
            if (!entropy) { require(p<buf.size(),"SXP4 byte overrun"); return buf[p++]; }
            require(context<5,"SXP4 Huffman context overflow");
            const auto& firstCode=modelFirstCode[context]; const auto& firstSymbol=modelFirstSymbol[context];
            const auto& codeCounts=modelCodeCounts[context]; const auto& alphabet=modelAlphabet[context];
            U code=0;
            for (unsigned len=1;len<=32;++len) {
                require(bit/8<buf.size(),"SXP4 Huffman overrun");
                code=(code<<1)|((buf[bit/8]>>(7-bit%8))&1); ++bit;
                if (code>=firstCode[len] && code-firstCode[len]<codeCounts[len])
                    return alphabet[firstSymbol[len]+code-firstCode[len]];
            }
            fail("SXP4 invalid Huffman code"); return 0;
        };
        while (idx < end) {
            if (idx >= start) *out++ = pos + shift;
            ++idx;
            if (idx >= end) break;   // stop BEFORE consuming the next delta
            U v = 0, sh = 0;
            for (;;) {
                uint8_t b = nextByte(sh/7);
                v |= U(b & 0x7f) << sh;
                if (!(b & 0x80)) break;
                sh += 7;
                require(sh < 64, "SXP4 varint overflow");
            }
            pos += (v >> 1) ^ -(U)(v & 1);   // zigzag
        }
    }
    inline U at(U i) const {
        if (i >= count) fail("pos column bounds");
        if (!compact) {
            thread_local TLS4 tls;
            U page = i / 4096;
            for (unsigned z = 0; z < 4; ++z)
                if (tls.e[z].fd == fd && tls.e[z].gen == gen && tls.e[z].tag[0] == page)
                    return tls.e[z].dv[0][i % 4096] + shift;
            TL& e = tls.e[tls.nxt++ & 3];
            e.fd = fd; e.gen = gen;
            U start = page * 4096, need = std::min<U>(4096, count - start);
            PosColumn* self = const_cast<PosColumn*>(this);
            self->pread_exact(e.dv[0], 8 * need, headerBytes + 8 * start);
            e.tag[0] = page; e.tag[1] = UINT64_MAX;
            return e.dv[0][i % 4096] + shift;
        }
        // compact: 16-slot LRU decoded-group cache. Groups hold GRP=4096
        // rows and startRow is GRP-aligned, so group == page. The walk
        // interleaves k sides per worker thread; 4 slots thrash at k>=8
        // (every access a group decode - the measured 2.3x compact walk
        // penalty at 1GB/k=16), 16 slots cover the per-side hot groups.
        struct TL16 { int fd = -1; U gen = 0; U stamp = 0; U g = UINT64_MAX;
                      std::unique_ptr<U[]> rows; };
        thread_local std::array<TL16, 16> cache;
        thread_local U clock = 0;
        U page = i / 4096;
        U slot = 16;
        for (U z = 0; z < 16; ++z)
            if (cache[z].fd == fd && cache[z].gen == gen && cache[z].g == page)
                { slot = z; break; }
        if (slot == 16) {
            for (U z = 0; z < 16; ++z)
                if (cache[z].fd != fd || cache[z].gen != gen || !cache[z].rows)
                    { slot = z; break; }
            if (slot == 16) {   // LRU victim
                U best = 0;
                for (U z = 1; z < 16; ++z) if (cache[z].stamp < cache[best].stamp) best = z;
                slot = best;
            }
            TL16& e = cache[slot];
            if (e.fd != fd || e.gen != gen || !e.rows) e.rows.reset(new U[4096]);
            e.fd = fd; e.gen = gen;
            U gEnd = (page + 1 < groups) ? tab[3 * (page + 1)] : count;
            const_cast<PosColumn*>(this)->decodeGroup(page, tab[3 * page], gEnd, e.rows.get());
            e.g = page;
        }
        cache[slot].stamp = ++clock;
        return cache[slot].rows[i % 4096];   // decodeGroup output already includes shift
    }
    void read(U i, U* buf, U len) const {   // bulk sequential decode
        if (i + len > count) fail("pos column bulk bounds");
        if (!compact) {
            xw(fd, buf, 8 * len, headerBytes + 8 * i, "pos column bulk");
            if (shift) for (U k = 0; k < len; ++k) buf[k] += shift;
            return;
        }
        PosColumn* self = const_cast<PosColumn*>(this);
        U done = 0;
        while (done < len) {
            U g; groupOf(i + done, g);
            U gEnd = (g + 1 < groups) ? tab[3 * (g + 1)] : count;
            U take = std::min<U>(len - done, gEnd - (i + done));
            self->decodeGroup(g, i + done, i + done + take, buf + done);
            done += take;
        }
    }
};
std::atomic<U> PosColumn::g_gen{1};
struct PosOverlay {
    struct Region { U lo, hi, off; };
    PosColumn src;
    int ovFd = -1; U ovCount = 0;
    ~PosOverlay() { if (ovFd >= 0) ::close(ovFd); }
    std::vector<Region> regions;
    U minLo = UINT64_MAX, maxHi = 0;
    void init(const std::string& path, U elems, U byteBase, U shift,
              const std::string& ovPath) {
        src.open_ro(path, elems, byteBase, shift);
        ovFd = ::open(ovPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (ovFd < 0) fail("open overlay file");
    }
    inline U at(U i) const {
        if (i >= src.count) fail("overlay pos bounds");
        if (regions.empty() || i < minLo || i >= maxHi) return src.at(i);
        for (size_t k = regions.size(); k-- > 0; ) {
            const Region& r = regions[k];
            if (i >= r.lo && i < r.hi) {
                U v; xw(ovFd, &v, 8, r.off + 8 * (i - r.lo), "overlay read");
                return v;
            }
        }
        return src.at(i);
    }
    void read(U i, U* buf, U len) const {   // bulk read, overlay regions applied
        if (i + len > src.count) fail("overlay bulk bounds");
        src.read(i, buf, len);
        if (regions.empty() || i + len <= minLo || i >= maxHi) return;
        // Forward push order, ALL intersecting regions applied: anchor
        // blocks nest or are disjoint, so the latest pushed region
        // containing a row wins - exactly at()'s backward-scan first-match
        // contract (applying in reverse would let an EARLIER nested region
        // override a later one).
        for (size_t k = 0; k < regions.size(); ++k) {
            const Region& r = regions[k];
            U lo = std::max(i, r.lo), hi = std::min(i + len, r.hi);
            if (lo >= hi) continue;
            xw(ovFd, buf + (lo - i), 8 * (hi - lo), r.off + 8 * (lo - r.lo), "overlay bulk read");
        }
    }
    void putSlice(U lo, const std::vector<U>& slice) {
        require(!slice.empty(), "overlay empty slice");
        xput(ovFd, slice.data(), 8 * slice.size(), 8 * ovCount, "overlay write");
        regions.push_back({lo, lo + slice.size(), 8 * ovCount});
        minLo = std::min(minLo, lo);
        maxHi = std::max(maxHi, lo + slice.size());
        ovCount += slice.size();
    }
};
// k-spaced sparse prefix hashes over the M2 file. Density is a SPEED knob
// only: merge exactness is verification-carried (every proposed prefix and
// its boundary is fully require()-checked in cmp, unchanged from the dense
// build; a hash disagreement can only cost probes, never correctness).
struct HashSparse {
    WinU64 col;                     // cached paged reads over the hash file
    U k = 8, entries = 0;           // P[j] = prefix hash of M2[0..j*k)
    void open_ro(const std::string& path, U spacing, U e) {
        col.open_ro(path, e, 0, 0, true);
        k = spacing; entries = e;
    }
    inline U P(U j) const { return col.at(j); }
    // Prefetch the anchors a window hash over [a, a+len) will touch.
    void prefetch_for(const CorpusText& m2, U a, U len) const {
        if (!col.pool) return;
        U ja = (a + k - 1) / k, je = (a + len) / k;
        col.prefetch(ja); col.prefetch(je);
        U hl = ja * k > a ? ja * k - a : 0;
        U tl = a + len > je * k ? a + len - je * k : 0;
        if (hl) m2.prefetch(a, hl);
        if (tl) m2.prefetch(je * k, tl);
    }
};
static U fold_bytes(const CorpusText& m2, U a, U e) {   // fold(a,e), first byte at B^(e-a-1)
    U h = 0;
    for (U i = a; i < e; ++i) h = h * HASH_BASE + ((U)m2.at(i) + 1);
    return h;
}
// Window prefix-hash F(a,e) = fold(M2[a..e)) from sparse anchors + tails:
//   ja = ceil(a/k), je = floor(e/k)
//   F = head*B^(e-ja*k) + mid*B^(e-je*k) + tail   (mid = P[je]-P[ja]*B^((je-ja)*k))
static U win_hash(const CorpusText& m2, const HashSparse& hp, U a, U e) {
    U ja = (a + hp.k - 1) / hp.k, je = e / hp.k;
    if (ja > je) return fold_bytes(m2, a, e);
    U tail = 0;
    for (U i = je * hp.k; i < e; ++i) tail = tail * HASH_BASE + ((U)m2.at(i) + 1);
    U mid = (ja == je) ? 0 : (hp.P(je) - hp.P(ja) * upow((je - ja) * hp.k));
    U head = 0;
    for (U i = a; i < ja * hp.k; ++i) head = head * HASH_BASE + ((U)m2.at(i) + 1);
    return tail + mid * upow(e - je * hp.k) + head * upow(e - ja * hp.k);
}

// ---------------------------------------------------------------- budget
// Per-comparison clock instrumentation is opt-in (see cmp).
static bool cmp_timing() {
    static const bool v = getenv("CROSS_CMP_TIMING") != nullptr;
    return v;
}

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
    // cmp phase timers (diagnostic)
    double tFast = 0, tPrep = 0, tProbe = 0, tScan = 0;
};

// Exact mismatch offset, vectorized only over complete, in-bounds blocks.
// This changes byte loading, never the comparator's charged logical work.
static inline U equal_prefix(const uint8_t* a, const uint8_t* b, U len) {
    U i = 0;
    while (len - i >= 16) {
        unsigned same = unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i)))));
        unsigned different = (~same) & 65535u;
        if (different) return i + __builtin_ctz(different);
        i += 16;
    }
    while (i < len && a[i] == b[i]) ++i;
    return i;
}

// ---------------------------------------------------------------- fingerprints
struct Keys {
    const CorpusText* m2 = nullptr;   // windowed M.M, 2n bytes
    const HashSparse* hp = nullptr; // k-spaced prefix hashes over m2
    U n = 0;
    bool dollar = false;           // final $-suffix mode vs cyclic (M^infty) mode
    Budget* bud = nullptr;
#ifdef SOL7D_TRACE
    std::vector<std::pair<U, U>>* trace = nullptr; // native witness only
#endif

    void init(const CorpusText* m, const HashSparse* h, U len, bool dol, Budget* b) {
        m2 = m; hp = h; n = len; dollar = dol; bud = b;
    }
    // Hash equality of M2[a..a+len) and M2[b..b+len) from the SPARSE anchors
    // (win_hash folds head/tail gaps from the windowed text). One charged
    // probe. Density is a speed knob: exactness below is verification-carried.
    bool eq(U a, U b, U len) const {
        ++bud->probes;
        return win_hash(*m2, *hp, a, a + len) == win_hash(*m2, *hp, b, b + len);
    }
    // Total order over merged keys of distinct positions p1 != p2.
    // dollar: key(p) = M[p..n).$^infty, distinct; the shorter suffix (larger p)
    //         is smaller once the common prefix covers it.
    // cyclic: key(p) = M^infty[p..]; LCE == n proves period |p1-p2|, a tie
    //         broken by ascending position (the chunk front end's rule).
    int cmp(U p1, U p2, U certified = 0) const {
        require(p1 != p2 && p1 < n && p2 < n, "comparator bounds");
        ++bud->comparisons;
#ifdef SOL7D_TRACE
        if (trace) trace->emplace_back(p1, p2);
#endif
        U cap = dollar ? std::min(n - p1, n - p2) : n;
        require(certified <= std::min<U>(cap, 8), "certified radix prefix bounds");
        U fast = std::min<U>(cap, 16), j = certified;
        // Per-comparison phase timers are DIAGNOSTIC (CROSS_KWAY's t_fast/...)
        // and cost 2-4 clock_gettime calls per comparison - unmeasurable
        // against the old pool path, a third of the budget once reads are
        // lock-free. Default OFF (CROSS_CMP_TIMING=1 restores them).
        const bool tOn = cmp_timing();
        double t0 = tOn ? now_sec() : 0.0;
        {
            uint8_t aStorage[16], bStorage[16];
            const uint8_t* ab = m2->span(p1, aStorage, fast);
            const uint8_t* bb = m2->span(p2, bStorage, fast);
            j += equal_prefix(ab + certified, bb + certified, fast - certified);
            if (j < fast) {
                bud->symbols += j + 1 - certified; ++bud->fast_decided; bud->note_lce(j);
                if (tOn) bud->tFast += now_sec() - t0;
                return ab[j] < bb[j] ? -1 : 1;
            }
        }
        if (tOn) bud->tFast += now_sec() - t0;
        bud->symbols += j - certified;
        if (fast == cap) {
            ++bud->cap_decided;
            if (dollar) return p1 > p2 ? -1 : 1;   // $ is minimal: shorter first
            ++bud->tie_decided; bud->note_lce(cap);
            return p1 < p2 ? -1 : 1;               // cyclic tie: position order
        }
        // ---- anchor-grid probing: gallop/bisect over k-ALIGNED window
        // lengths L = m*k. A window fold for side p decomposes into a head
        // (<= k bytes at p, memoized once per comparison), an anchor span
        // (two PREFIX-HASH entries at hash indices floor(p/k)+1 and
        // floor(p/k)+m — indices advance by EXACTLY 1 per grid step, so the
        // per-thread page cache absorbs the whole bisect), and a tail (r =
        // p%k bytes at a position that also steps by k — same cache). The
        // exact boundary is then found by a direct sequential text scan
        // from the certified fast prefix, which doubles as the full
        // verification (SLIM_FP discipline): a hash disagreement below the
        // grid point fails loud; hash equality is never trusted for the
        // decision itself.
        // FIX 1 (incremental prep): the grid state starts as pure arithmetic
        // (no I/O). The head fold and the anchor entry materialize lazily on
        // the FIRST probe that needs them, and are memoized thereafter: a
        // comparison that never probes (fast/cap decided) pays nothing, and
        // the materialization reads only what the winning window needs.
        struct SideScr { U p = 0, aq = 0, r = 0, h = 0, pj = 0; bool ready = false; };
        thread_local std::array<SideScr, 2> tscr;
        auto prep = [&](U p, SideScr& sc) {
            sc.p = p;
            sc.aq = p / hp->k;
            sc.r = p - sc.aq * hp->k;
            sc.ready = false;
        };
        // FIX 2 (grid-cell cache): repeat (side, aq) cells are common in
        // blocked/repetitive regions; the anchor entry P[aq+1] is cell-local,
        // so a direct-mapped per-thread table absorbs its re-reads. The head
        // fold stays per-position (it depends on p, not just the cell).
        struct Cell { U aq = UINT64_MAX, gen = 0, pj = 0; };
        thread_local std::array<std::array<Cell, 4096>, 2> cells;
        U hsGen = hp->col.gen;     // stale across pairs/files: key on it
        auto materialize = [&](SideScr& sc) {
            if (sc.ready) return;
            U hlen = (sc.aq + 1) * hp->k - sc.p;        // <= k <= 64
            uint8_t storage[64];
            const uint8_t* hb = m2->span(sc.p, storage, hlen);
            U h = 0;
            for (U z = 0; z < hlen; ++z) h = h * HASH_BASE + ((U)hb[z] + 1);
            sc.h = h;
            Cell& c = cells[&sc == &tscr[0] ? 0 : 1][sc.aq & 4095];
            if (c.aq == sc.aq && c.gen == hsGen) { sc.pj = c.pj; count_local(g_cellHitsArr, cell_tid(), CELL_TIDS-1); }
            else {
                sc.pj = hp->P(sc.aq + 1);
                c.aq = sc.aq; c.gen = hsGen; c.pj = sc.pj;
                count_local(g_cellMissArr, cell_tid(), CELL_TIDS-1);
            }
            sc.ready = true;
        };
        double t1 = tOn ? now_sec() : 0.0;
        if (fast / hp->k < cap / hp->k) {   // lazy: degenerate grids (cap<k) scan directly
            prep(p1, tscr[0]); prep(p2, tscr[1]);
        }
        if (tOn) bud->tPrep += now_sec() - t1;
        auto winFold = [&](SideScr& sc, U m) {         // fold(p, p + m*k)
            materialize(sc);
            U jb = sc.aq + m;
            U mid = (m <= 1) ? 0 : (hp->P(jb) - sc.pj * upow((m - 1) * hp->k));
            U tail = 0;
            if (sc.r) {
                uint8_t storage[64];
                U tlen = std::min<U>(sc.r, 64);
                const uint8_t* tb = m2->span(jb * hp->k, storage, tlen);
                for (U i = 0; i < tlen; ++i) tail = tail * HASH_BASE + ((U)tb[i] + 1);
                for (U i = 64; i < sc.r; ++i)   // k<=64 enforced by the clamp
                    tail = tail * HASH_BASE + ((U)m2->at(jb * hp->k + i) + 1);
            }
            return sc.h * upow((m - 1) * hp->k + sc.r) + mid * upow(sc.r) + tail;
        };
        auto eqm = [&](U m) { ++bud->probes; return winFold(tscr[0], m) == winFold(tscr[1], m); };
        U k = hp->k;
        double t2 = tOn ? now_sec() : 0.0;
        U mMax = cap / k;             // largest grid length <= cap
        U lo = fast / k;              // certified by the fast path
        U hi = mMax + 1, step = 1, np = 0;
        while (lo < mMax) {
            U mm = std::min(lo + step, mMax); ++np;
            if (eqm(mm)) { lo = mm; step *= 2; }
            else { hi = mm; break; }
        }
        while (hi - lo > 1) {
            U midm = lo + (hi - lo) / 2; ++np;
            if (eqm(midm)) lo = midm; else hi = midm;
        }
        bud->probes += np;
        if (tOn) bud->tProbe += now_sec() - t2;
        // Exact boundary: scan from the certified prefix to the next grid
        // point (or cap when the grid is exhausted). This scan is the
        // verification of every hash claim below it.
        double t3 = tOn ? now_sec() : 0.0;
        U scanEnd = (lo >= mMax || (lo + 1) * k > cap) ? cap : (lo + 1) * k;
        U i = fast;
        {
            uint8_t aStorage[64], bStorage[64];
            while (i < scanEnd) {
                U w = std::min<U>(64, scanEnd - i);
                const uint8_t* ab = m2->span(p1 + i, aStorage, w);
                const uint8_t* bb = m2->span(p2 + i, bStorage, w);
                U z = equal_prefix(ab, bb, w);
                i += z;
                if (z < w) break;
            }
        }
        bud->symbols += i - fast;
        if (tOn) bud->tScan += now_sec() - t3;
        if (i < scanEnd) {
            require(i >= lo * k, "hash collision inside proposed prefix");
            bud->note_lce(i);
            ++bud->probe_decided;
            return m2->at(p1 + i) < m2->at(p2 + i) ? -1 : 1;
        }
        if (scanEnd == cap) {
            bud->note_lce(cap);
            ++bud->probe_decided;
            if (dollar) return p1 > p2 ? -1 : 1;
            ++bud->tie_decided;
            return p1 < p2 ? -1 : 1;
        }
        if (getenv("CROSS_DUMP_GRID")) {
            std::fprintf(stderr, "GRID_FAIL p1=%llu p2=%llu cap=%llu fast=%llu k=%llu lo=%llu hi=%llu mMax=%llu scanEnd=%llu "
                         "lo_k=%llu h1=%llu h2=%llu pj1=%llu pj2=%llu r1=%llu r2=%llu aq1=%llu aq2=%llu\n",
                         (unsigned long long)p1, (unsigned long long)p2, (unsigned long long)cap,
                         (unsigned long long)fast, (unsigned long long)k,
                         (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)mMax,
                         (unsigned long long)scanEnd, (unsigned long long)(lo * k),
                         (unsigned long long)tscr[0].h, (unsigned long long)tscr[1].h,
                         (unsigned long long)tscr[0].pj, (unsigned long long)tscr[1].pj,
                         (unsigned long long)tscr[0].r, (unsigned long long)tscr[1].r,
                         (unsigned long long)tscr[0].aq, (unsigned long long)tscr[1].aq);
            // recompute the losing probe directly for triage
            auto direct = [&](U p, U m) {
                U h = 0;
                for (U z = 0; z < m * k; ++z) h = h * HASH_BASE + ((U)m2->at(p + z) + 1);
                return h;
            };
            std::fprintf(stderr, "GRID_FAIL folds: eqm_hi_grid=%llu/%llu eqm_hi_direct=%llu/%llu scanbytes=%llu\n",
                         (unsigned long long)winFold(tscr[0], hi), (unsigned long long)winFold(tscr[1], hi),
                         (unsigned long long)direct(p1, hi), (unsigned long long)direct(p2, hi),
                         (unsigned long long)(scanEnd - fast));
        }
        require(false, "grid equality contradiction");
        return 0;
    }
};

// k-spaced prefix hashes over virtual doubled corpus text: P[j] = fold(M2[0..j*k)).
// Parallel 3-pass (relative entries in place, absolute range starts, fix-up)
// over a shared scratch file; verified at boundaries like the dense build was.
static U hash_spacing() {
    const char* e = getenv("CROSS_HASH_K");
    // Thirty-two-byte spacing quarters checkpoint scratch versus the old k=8.
    // The env override remains available for measured density tradeoffs.
    U k = e ? std::stoull(e) : 32;
    require(k >= 1 && k <= 4096, "CROSS_HASH_K out of range");
    return k;
}
static void build_hash_sparse(const CorpusText& m2, U n2, U k,
                              const std::string& hsPath, U threads) {
    U entries = n2 / k + 1;
    std::fprintf(stderr, "CROSS_HASH_SPARSE virtual_bytes=%llu k=%llu checkpoint_bytes=%llu\n",
                 (unsigned long long)n2, (unsigned long long)k, (unsigned long long)(8 * entries));
    int fd = ::open(hsPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    require(fd >= 0, "create sparse hash file");
    if (ftruncate(fd, 8 * entries) != 0) fail("truncate sparse hash file");
    U tcount = threads;
    if (n2 < (1u << 20) || tcount <= 1) tcount = 1;
    U segE = (entries + tcount - 1) / tcount;
    std::vector<U> relend(tcount, 0);
    std::vector<std::thread> ts;
    auto rel = [&](U t) {
        U j0 = t * segE, j1 = std::min(entries - 1, j0 + segE);
        if (j1 <= j0) { if (j0 < entries) relend[t] = 0; return; }
        // Chunked: a per-thread range buffer would be O(entries/threads) —
        // hundreds of MB per thread at pile scale. Fold and flush in
        // 1 Mi-entry chunks instead (8 MiB per thread, bounded).
        constexpr U CHUNK = 1u << 20;
        std::vector<U> buf(CHUNK);
        // Hash sequential corpus reads in bounded bands. Per-byte pooled
        // reads would charge shared-cache bookkeeping for all 2n symbols.
        constexpr U TEXT_WINDOW = 1u << 16;
        std::vector<uint8_t> text(TEXT_WINDOW);
        U textStart = 0, textEnd = 0;
        U h = 0;
        U jc = j0;
        while (jc < j1) {
            U je = std::min(j1, jc + CHUNK);
            for (U j = jc; j < je; ++j) {
                buf[j - jc] = h;
                U e = std::min(n2, (j + 1) * k);
                for (U i = j * k; i < e; ++i) {
                    if (i >= textEnd) {
                        textStart = i;
                        textEnd = i + std::min(TEXT_WINDOW, n2 - i);
                        m2.read(i, text.data(), textEnd - textStart);
                    }
                    h = h * HASH_BASE + (U(text[i - textStart]) + 1);
                }
            }
            xput(fd, buf.data(), 8 * (je - jc), 8 * jc, "sparse hash rel write");
            jc = je;
        }
        relend[t] = h;
    };
    for (U t = 0; t < tcount; ++t) ts.emplace_back(rel, t);
    for (auto& t : ts) t.join();
    // absolute range starts (sequential; P[0] = 0), then fix-up in place.
    U cur = 0;
    for (U t = 0; t < tcount; ++t) {
        U j0 = t * segE, j1 = std::min(entries - 1, j0 + segE);
        if (j1 <= j0) continue;
        xput(fd, &cur, 8, 8 * j0, "sparse hash start write");   // P[j0] = cur
        U span = j1 - j0;   // bytes folded in this range = min(n2, j1*k) - j0*k
        U folded = std::min(n2, j1 * k) - std::min(n2, j0 * k);
        cur = cur * upow(folded) + relend[t];
    }
    xput(fd, &cur, 8, 8 * (entries - 1), "sparse hash last write");   // P[entries-1]
    ts.clear();
    auto fix = [&](U t) {
        U j0 = t * segE, j1 = std::min(entries - 1, j0 + segE);
        if (j1 <= j0 + 1) return;
        U start = 0;
        xw(fd, &start, 8, 8 * j0, "sparse hash fix start");   // absolute P[j0]
        constexpr U CHUNK = 1u << 20;
        std::vector<U> buf(CHUNK);
        U jc = j0 + 1;
        while (jc < j1) {
            U je = std::min(j1, jc + CHUNK);
            xw(fd, buf.data(), 8 * (je - jc), 8 * jc, "sparse hash fix read");
            for (U j = jc; j < je; ++j) buf[j - jc] += start * upow((j - j0) * k);
            xput(fd, buf.data(), 8 * (je - jc), 8 * jc, "sparse hash fix write");
            jc = je;
        }
    };
    for (U t = 0; t < tcount; ++t) ts.emplace_back(fix, t);
    for (auto& t : ts) t.join();
    // Verify absolute hashes at gap boundaries: window hash from sparse P
    // must equal a direct recomputation over the windowed text. This reader
    // dies before the walk opens its own hp: its tiny POOL_STATS are NOT the
    // walk's hit rate (bytes is checkpoint_bytes, not corpus length).
    HashSparse hp; hp.open_ro(hsPath, k, entries);
    U cands[] = {k, n2 / 2 / k * k, n2 / k * k};
    for (U c : cands) {
        if (c > n2 || c < k) continue;
        U w = std::min<U>(c, k * 1024);
        U direct = fold_bytes(m2, c - w, c);
        U jc = c / k, jw = (c - w) / k;
        U fromP = hp.P(jc) - hp.P(jw) * upow(w);
        require(fromP == direct, "sparse hash build mismatch at boundary");
    }
    ::close(fd);
}

// ------------------------------------------------------------- streamed sides
// A side is either an on-disk SXS2 sidecar (all intermediates; legacy raw
// walks) or a raw SXCR chunk, which is walked once (chunk-scale resident
// transient). The walked order column is dumped to a scratch pos sidecar; in
// CORPUS-REFERENCED mode (chunk-i.ref present, the chunker default) the text
// is NOT dumped with it — consumers read remap[corpus[offset..offset+n)] and
// the per-side text copy never exists.
struct SideExt {
    U offset = 0, n = 0, runs = 0, period = 0; // period==n means aperiodic
    std::string sxsPath;       // the sidecar actually used (text+pos, or pos-only)
    bool ownsSxs = false;      // we wrote it (scratch; unlink at pair end)
    bool refText = false;      // corpus-referenced text (chunk-i.ref)
    U posBase = 0;             // byte offset of the pos column in sxsPath
    std::string corpusPath;    // ref mode (single-segment legacy refs)
    U corpusOff = 0;           // ref mode: chunk range start in the corpus
    std::array<uint8_t, 256> remap{};   // ref mode: the chunker's byte map
    std::vector<TextSeg> segs; // referenced text segments (v1: one; v2: concat)
    std::string chunkPath;     // the source .crle (persisted-pos sides)
    bool bwtFromRuns = false;  // load shortcut: expand the BWT from the runs
    U chunkVersion = 0;        // SXCR record version (2 = 25B w/ samples, 3 = varint)
};
// chunk-i.ref (chunk_frontend emit_ref): "SXRF" | u32 ver=1 | u8 flags=0 |
// remap[256] | u32 pathLen | corpusPath | u64 offset | u64 n
// SXRF v1: "SXRF" | u32 ver=1 | u8 flags | remap[256] | u32 pathLen |
//          path | u64 off | u64 n          (chunker: one corpus segment)
// SXRF v2: "SXRF" | u32 ver=2 | u8 flags | u32 segCount | per segment:
//          remap[256] | u32 pathLen | path | u64 off | u64 len | then
//          u64 totalN. A merged intermediate's ref is its input sides'
// segments concatenated - the text view composes with no copies.
// Companion naming: for chunk file X.crle, its persisted companions are
// stem(X).pos / stem(X).ref (the chunker writes chunk-N.pos / chunk-N.ref;
// merged intermediates write L-x.pos / L-x.ref). ONE convention everywhere;
// the legacy text-embedded sidecar stays X.sxs (banked format).
static std::string chunk_stem(const std::string& path) {
    require(path.size() > 5 && path.compare(path.size() - 5, 5, ".crle") == 0,
            "chunk path must end in .crle");
    return path.substr(0, path.size() - 5);
}
// DELETE-AS-YOU-GO (CROSS_DELETE_CONSUMED=1): once a group has merged, its
// input files are never read again. Unlink the .crle and both persisted
// companion sidecars (.pos order column, .ref corpus/segment reference), plus
// the legacy text-embed .sxs, so the peak disc stays ~ one level's live set
// instead of the whole input history. Off by default (the gates compare
// intermediates); the fan-in driver turns it on.
static void delete_consumed_side(const std::string& path) {
    const std::string stem = chunk_stem(path);
    ::unlink(path.c_str());
    ::unlink((stem + ".pos").c_str());
    ::unlink((stem + ".ref").c_str());
    ::unlink((path + ".sxs").c_str());
    ::unlink((stem + ".sxs").c_str());
}
static void parse_chunk_ref(const std::string& path, SideExt& s) {
    std::ifstream f(path, std::ios::binary);
    require(bool(f), "cannot open chunk ref");
    char magic[4]{}; f.read(magic, 4);
    require(bool(f) && !std::memcmp(magic, "SXRF", 4), "chunk ref magic mismatch");
    U ver = rd_le(f, 4);
    require(ver == 1 || ver == 2, "unsupported chunk ref version");
    require(rd_le(f, 1) == 0, "unknown chunk ref flags");
    s.segs.clear();
    auto readSeg = [&](U lenHint) {
        TextSeg t;
        f.read(reinterpret_cast<char*>(t.remap.data()), 256);
        require(bool(f), "truncated chunk ref remap");
        auto check = t.remap; std::sort(check.begin(), check.end());
        for (unsigned i = 0; i < 256; ++i) require(check[i] == i, "chunk ref remap not bijective");
        require(t.remap[0x1e] == 0x1e, "chunk ref remap changes the document separator");
        U plen = rd_le(f, 4);
        require(plen && plen < 65536, "chunk ref path length");
        std::vector<char> p(plen);
        f.read(p.data(), plen);
        require(bool(f), "truncated chunk ref path");
        t.path.assign(p.data(), plen);
        t.off = rd_le(f, 8);
        t.len = lenHint ? lenHint : rd_le(f, 8);
        require(t.len && t.len < (1ull << 62), "chunk ref segment length");
        s.segs.push_back(std::move(t));
    };
    if (ver == 1) {
        readSeg(0);
        s.corpusPath = s.segs[0].path;
        s.corpusOff = s.segs[0].off;
        s.remap = s.segs[0].remap;
    } else {
        U count = rd_le(f, 4);
        require(count && count < 4096, "chunk ref segment count");
        for (U z = 0; z < count; ++z) readSeg(0);
        U totalN = rd_le(f, 8);
        require(totalN == [&]{ U t = 0; for (auto& g : s.segs) t += g.len; return t; }(),
                "chunk ref totalN disagrees with the segments");
    }
    require(bool(f) && f.peek() == EOF, "trailing bytes in chunk ref");
}
// Open a side's text view (sidecar-embedded or referenced segments).
static void open_side_text(const SideExt& s, SideText& t) {
    if (s.refText) t.open_segments(s.segs, s.n);
    else t.open_sidecar(s.sxsPath, s.n, 20);
}
// chunk-i.pos (SXP3, chunker-persisted): "SXP3" | u32 ver | u64 offset |
// u64 n | u64 period | u64 rsvd | pos[8n]. The order column the chunker
// already computed; the merge load for such a chunk is PURE READS (no
// walk, no LF derivation) plus the cheap bulk validations below.
static void read_pos_header(const std::string& path, U& offset, U& n, U& period) {
    std::ifstream f(path, std::ios::binary);
    require(bool(f), "cannot open chunk pos sidecar");
    char magic[4]{}; f.read(magic, 4);
    require(bool(f) && (!std::memcmp(magic, "SXP3", 4) || !std::memcmp(magic, "SXP4", 4)),
            "chunk pos magic mismatch (SXP3 dense / SXP4 compact)");
    U version = rd_le(f, 4);
    require(version == 1 || (version == 2 && !std::memcmp(magic, "SXP4", 4)), "unsupported chunk pos version");
    offset = rd_le(f, 8); n = rd_le(f, 8); period = rd_le(f, 8);
    require(rd_le(f, 8) == 0, "unknown chunk pos reserved field");
    if (!std::memcmp(magic, "SXP3", 4))
        require(fs::file_size(path) == 40 + 8 * n, "chunk pos size mismatch");
    else
        require(fs::file_size(path) > 56, "SXP4 pos column truncated");
}
// Runs-sample + range validation for a persisted order column (bulk reads;
// the per-row text agreement is checked sampled inside the BWT expansion).
static void validate_pos_runs(const std::string& chunkPath, U n, U runs, U version,
                              const PosColumn& pos) {
    RunCursor rc(chunkPath, version);
    U row = 0; uint8_t prev = 0;
    for (U i = 0; i < runs; ++i) {
        uint8_t c; U len, h, t;
        rc.next(c, len, h, t);
        if (version == 3) { h = pos.at(row); t = pos.at(row + len - 1); }
        require(len && row + len <= n && h < n && t < n, "invalid SXCR run");
        require(i == 0 || c != prev, "noncanonical SXCR runs");
        require(pos.at(row) == h && pos.at(row + len - 1) == t,
                "persisted order disagrees with the run samples");
        row += len; prev = c;
    }
    require(row == n, "SXCR length mismatch");
}
// Expand the BWT column directly from the .crle runs (sequential rows) -
// no text reads. Sampled per-run text agreement is the residual check that
// the persisted order matches the corpus bytes (every 64th run head).
static void build_bwt_from_runs(const SideExt& s, const PosOverlay& pos,
                                const std::string& path, U base) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    require(fd >= 0, "create bwt scratch");
    RunCursor rc(s.chunkPath, s.chunkVersion);
    constexpr U W = 1u << 22;
    std::vector<uint8_t> buf(W);
    U row = 0, used = 0, written = 0, checked = 0;
    SideText text; open_side_text(s, text);
    for (U i = 0; i < s.runs; ++i) {
        uint8_t c; U len, h, t;
        rc.next(c, len, h, t);
        // v3 samples come from the order column; the overlay returns
        // MERGE-LOCAL positions (shifted by the side's base) while the text
        // view is SIDE-LOCAL - subtract the base before indexing it.
        if (s.chunkVersion == 3) { h = pos.at(row) - base; }
        if ((checked & 63) == 0)   // 1-in-64 sampled run-head text check
            require(text.at((h + s.n - 1) % s.n) == c,
                    "persisted order disagrees with the referenced text");
        ++checked;
        for (U z = 0; z < len; ++z) {
            if (used == W) { xput(fd, buf.data(), used, written, "bwt expand write"); written += used; used = 0; }
            buf[used++] = c;
        }
        row += len;
    }
    if (used) { xput(fd, buf.data(), used, written, "bwt expand write"); written += used; }
    require(row == s.n && written == s.n, "bwt expansion coverage");
    ::close(fd);
}

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

static void sxcr_header(const std::string& path, U& offset, U& n, U& runs, U& version) {
    std::ifstream f(path, std::ios::binary);
    require(bool(f), "cannot open SXCR chunk");
    char magic[4]{}; f.read(magic, 4);
    require(bool(f) && !std::memcmp(magic, "SXCR", 4), "SXCR magic mismatch");
    U ver = rd_le(f, 4);
    require(ver == 2 || ver == 3,
            "unsupported SXCR version (this build reads v2: 25-byte sample-bearing "
            "records, and v3: compact c+varint-len records that REQUIRE the "
            "persisted order column for samples)");
    version = ver;
    offset = rd_le(f, 8); n = rd_le(f, 8); runs = rd_le(f, 8);
    require(n && n < (1ull << 62) && runs, "invalid SXCR header");
    if (ver == 2) require(fs::file_size(path) == 32 + 25 * runs, "SXCR file size mismatch");
    else require(fs::file_size(path) > 32, "SXCR v3 has no run stream");
}


// Validate runs against the STREAMED (text,pos): preceding char per row and
// head/tail samples. Same contract as the resident build.
static void validate_runs_ext(const std::string& path, U n, U runs, U version,
                              const SideText& text, const WinU64& pos) {
    require(version == 2, "SXCR v3 has no embedded samples (sidecar sides are v2)");
    std::ifstream f(path, std::ios::binary);
    f.seekg(32);
    U row = 0; uint8_t prev = 0;
    for (U i = 0; i < runs; ++i) {
        uint8_t c = uint8_t(rd_le(f, 1));
        U len = rd_le(f, 8), h = rd_le(f, 8), t = rd_le(f, 8);
        require(len && row + len <= n && h < n && t < n, "invalid SXCR run");
        require(i == 0 || c != prev, "noncanonical SXCR runs");
        require(pos.at(row) == h && pos.at(row + len - 1) == t, "SXCR run samples disagree with order");
        for (U k = 0; k < len; ++k)
            require(text.at((pos.at(row + k) + n - 1) % n) == c, "SXCR run text mismatch");
        row += len; prev = c;
    }
    require(row == n && f.peek() == EOF, "SXCR length mismatch");
}
// Streaming minimal cyclic period (fast-fail per divisor; one full pass for
// the true period).
static U min_period_stream(const SideText& text, U n) {
    std::vector<U> divs;
    for (U d = 1; d * d <= n; ++d) if (n % d == 0) { divs.push_back(d); if (d != n / d) divs.push_back(n / d); }
    std::sort(divs.begin(), divs.end());
    constexpr U W = 1u << 20;
    std::vector<uint8_t> a(W), b(W);
    for (U d : divs) {
        if (d >= n) continue;
        bool ok = true;
        for (U i = 0; i + d < n && ok; i += W) {
            U len = std::min<U>(W, n - d - i);
            text.read(i, a.data(), len);
            text.read(i + d, b.data(), len);
            for (U k = 0; k < len; ++k) if (a[k] != b[k]) { ok = false; break; }
        }
        if (ok) return d;
    }
    return n;
}

// Validate runs against resident (text,pos): the walked-chunk path (chunk scale).
static void validate_runs_resident(const std::string& path, U n, U runs,
                                   const std::vector<uint8_t>& text, const std::vector<uint64_t>& pos) {
    std::ifstream f(path, std::ios::binary);
    f.seekg(32);
    U row = 0; uint8_t prev = 0;
    for (U i = 0; i < runs; ++i) {
        uint8_t c = uint8_t(rd_le(f, 1));
        U len = rd_le(f, 8), h = rd_le(f, 8), t = rd_le(f, 8);
        require(len && row + len <= n && h < n && t < n, "invalid SXCR run");
        require(i == 0 || c != prev, "noncanonical SXCR runs");
        require(pos[row] == h && pos[row + len - 1] == t, "SXCR run samples disagree with order");
        for (U k = 0; k < len; ++k)
            require(text[(pos[row + k] + n - 1) % n] == c, "SXCR run text mismatch");
        row += len; prev = c;
    }
    require(row == n && f.peek() == EOF, "SXCR length mismatch");
}

static void walk_chunk(const std::string& path, U n, U runs, U version, U threads,
                       std::vector<uint8_t>& text, std::vector<uint64_t>& pos, U& period) {
    require(version == 2,
            "SXCR v3 chunks carry no run samples - they REQUIRE the persisted "
            "order column (chunk-N.pos); re-chunk or restore the companion");
    struct RunRec { uint8_t c; uint64_t len; U h, t; };
    std::vector<RunRec> rs(runs);
    {
        std::ifstream f(path, std::ios::binary);
        f.seekg(32);
        for (U i = 0; i < runs; ++i) {
            rs[i].c = uint8_t(rd_le(f, 1));
            rs[i].len = rd_le(f, 8);
            rs[i].h = rd_le(f, 8); rs[i].t = rd_le(f, 8);
            require(rs[i].len && rs[i].h < n && rs[i].t < n && (i == 0 || rs[i].c != rs[i-1].c), "invalid SXCR run");
        }
    }
    std::array<U, 256> freq{}, C{}, seen{};
    for (auto& r : rs) freq[r.c] += r.len;
    U acc = 0; for (unsigned c = 0; c < 256; ++c) { C[c] = acc; acc += freq[c]; }
    require(acc == n, "SXCR counts mismatch");
    std::vector<uint8_t> bwt(n);
    std::vector<U> lf(n);
    std::vector<std::pair<U, U>> seeds;  // (row, position)
    seeds.reserve(runs);
    U row = 0;
    for (auto& r : rs) {
        seeds.push_back({row, r.h});
        for (U k = 0; k < r.len; ++k, ++row) {
            bwt[row] = r.c;
            lf[row] = C[r.c] + seen[r.c]++;
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
        std::vector<U> classes(d);
        for (U r = 0; r < d; ++r) classes[r] = r;
        auto stream_less = [&](U r, U s) {
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
            for (U m = 0; m < k; ++m) pos[idx++] = r + m * d;
        require(idx == n, "periodic order coverage");
    } else {
        // Aperiodic: LF is exact; threaded multi-seed walk recovers pos.
        pos.assign(n, UINT64_MAX);
        std::vector<std::atomic<uint8_t>> visited(n);
        for (auto& s : seeds) { visited[s.first].store(1, std::memory_order_relaxed); pos[s.first] = s.second; }
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
                    pos[j] = p ? p - 1 : n - 1;
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
    validate_runs_resident(path, n, runs, text, pos);
}


// Load a side into its STREAMED form. Raw chunks are walked once (chunk-scale
// resident transient: text, bwt, lf, pos, visited) and the order column is
// dumped to a scratch pos sidecar; with a chunk-i.ref the text stays in the
// corpus (cross-validated against the walked text byte for byte; the runs
// were already validated against the walked (text,pos), so runs<->text<->
// corpus is fully proven and the legacy sidecar re-validation is subsumed).
// Intermediates and legacy raw chunks keep the full SXS2 text+pos sidecar.
static void load_side_ext(const std::string& path, U threads,
                          const std::string& scratch, SideExt& s) {
    sxcr_header(path, s.offset, s.n, s.runs, s.chunkVersion);
    s.chunkPath = path;
    std::string stem = chunk_stem(path);
    std::string side = path + ".sxs";
    std::string posS = stem + ".pos";
    if (fs::exists(side)) {
        s.sxsPath = side; s.ownsSxs = false; s.refText = false; s.posBase = 20 + s.n;
    } else if (fs::exists(posS) && fs::exists(stem + ".ref") && !getenv("CROSS_NO_PERSIST")) {
        // PERSISTED-AT-CHUNK-TIME (SXP3): the order column the chunker
        // computed; the load is PURE READS (no walk, no LF derivation -
        // the pile-scale bottleneck). Validations: header agreement, the
        // position range, the run samples, and the sampled text checks
        // inside the BWT expansion; the absolute byte-identity gate
        // carries the rest.
        U pOff = 0, pN = 0, period = 0;
        read_pos_header(posS, pOff, pN, period);
        require(pOff == s.offset && pN == s.n,
                "chunk pos sidecar disagrees with the SXCR header");
        U refTotal = 0;
        parse_chunk_ref(stem + ".ref", s);
        for (auto& g : s.segs) refTotal += g.len;
        require(refTotal == s.n,
                "chunk ref range disagrees with the SXCR header");
        s.sxsPath = posS; s.ownsSxs = false; s.refText = true; s.posBase = 40;
        s.period = period; s.bwtFromRuns = true;
        {
            // 40 = the SXP3 dense header (byteBase); the SXP4 branch
            // ignores it and derives its own layout from the sniff.
            PosColumn pos; pos.open_ro(s.sxsPath, s.n, 40, 0);
            U buf[4096];
            for (U i = 0; i < s.n; i += 4096) {
                U len = std::min<U>(4096, s.n - i);
                pos.read(i, buf, len);
                for (U z = 0; z < len; ++z) require(buf[z] < s.n, "persisted position out of range");
            }
            validate_pos_runs(path, s.n, s.runs, s.chunkVersion, pos);
        }
        return;   // persisted side fully validated (range + samples + header)
    } else {
        std::vector<uint8_t> text; std::vector<uint64_t> pos;
        walk_chunk(path, s.n, s.runs, s.chunkVersion, threads, text, pos, s.period);
        std::string ref = stem + ".ref";
        if (fs::exists(ref) && !getenv("CROSS_NO_REF")) {
            // CORPUS-REFERENCED: record provenance, prove the walked text is
            // exactly remap[corpus[offset..offset+n)), dump the pos column only.
            U refTotal = 0;
            parse_chunk_ref(ref, s);
            for (auto& g : s.segs) refTotal += g.len;
            require(refTotal == s.n,
                    "chunk ref range disagrees with the SXCR header");
            {
                SideText t; t.open_segments(s.segs, s.n);
                constexpr U W = 1u << 22;
                std::vector<uint8_t> buf(W);
                for (U i = 0; i < s.n;) {
                    U len = std::min<U>(W, s.n - i);
                    t.read(i, buf.data(), len);
                    require(std::memcmp(buf.data(), text.data() + i, len) == 0,
                            "chunk text disagrees with the corpus reference");
                    i += len;
                }
            }
            s.sxsPath = scratch + "/" + fs::path(path).filename().string() + ".walked.pos";
            s.ownsSxs = true; s.refText = true; s.posBase = 20;
            {
                OutFile f(s.sxsPath);
                char magic[4]{'S','X','P','2'}; f.raw(magic, 4);
                f.w64(s.offset); f.w64(s.n);
                f.raw(pos.data(), 8 * s.n);
            }
        } else {
            s.sxsPath = scratch + "/" + fs::path(path).filename().string() + ".walked.sxs";
            s.ownsSxs = true; s.refText = false; s.posBase = 20 + s.n;
            {
                OutFile f(s.sxsPath);
                char magic[4]{'S','X','S','2'}; f.raw(magic, 4);
                f.w64(s.offset); f.w64(s.n);
                f.raw(text.data(), s.n);
                f.raw(pos.data(), 8 * s.n);
            }
        }
    }
    if (!s.refText) {
        SideText text; open_side_text(s, text);
        if (s.period == 0) s.period = min_period_stream(text, s.n);
        {
            WinU64 pos; pos.open_ro(s.sxsPath, s.n, s.posBase);
            U buf[4096];
            for (U i = 0; i < s.n; i += 4096) {
                U len = std::min<U>(4096, s.n - i);
                pos.read(i, buf, len);
                for (U z = 0; z < len; ++z) require(buf[z] < s.n, "sidecar position out of range");
            }
            validate_runs_ext(path, s.n, s.runs, s.chunkVersion, text, pos);
        }
    } else {
        // ref mode: period came from the walk; positions checked here.
        WinU64 pos; pos.open_ro(s.sxsPath, s.n, s.posBase);
        U buf[4096];
        for (U i = 0; i < s.n; i += 4096) {
            U len = std::min<U>(4096, s.n - i);
            pos.read(i, buf, len);
            for (U z = 0; z < len; ++z) require(buf[z] < s.n, "sidecar position out of range");
        }
    }
}

// ---------------------------------------------- anchor blocks + repair (windowed)
struct BlockRec { U q, lo, hi; };

struct RankIndex {
    const WinBytes* bwt = nullptr;
    U n = 0, nb = 0, SAMP = 4096;
    std::vector<U> tab; // [char][bucket] counts before bucket start (u64: counts reach n)
    // Adaptive sampling: tab stays <= ~64 MiB at any n; the rank scan reads
    // at most SAMP bytes through the windowed BWT.
    void build(const WinBytes& b, U len) {
        bwt = &b; n = len;
        U maxBuckets = (1u << 15);                 // 32768 buckets: tab <= 64 MiB
        SAMP = 4096;
        while (n / SAMP + 2 > maxBuckets) SAMP *= 2;
        nb = n / SAMP + 2;
        tab.assign(size_t(256) * nb, 0);
        U counts[256]{};
        constexpr U W = 1u << 20;
        std::vector<uint8_t> buf(W);
        for (U k = 0; k * SAMP < n; ++k) {
            U e = std::min(n, (k + 1) * SAMP);
            for (U i = k * SAMP; i < e;) {
                U len = std::min<U>(W, e - i);
                b.read(i, buf.data(), len);
                for (U z = 0; z < len; ++z) ++counts[buf[z]];
                i += len;
            }
            for (unsigned c = 0; c < 256; ++c) tab[c * nb + k + 1] = counts[c];
        }
    }
    U operator()(uint8_t c, U x) const {
        U bk = x / SAMP, base = tab[c * nb + bk];
        U e = std::min(n, x);
        for (U i = bk * SAMP; i < e; ++i) base += (bwt->at(i) == c);
        return base;
    }
};

// Anchor block discovery, windowed (same algorithm as the resident build).
// Periodic chunks: every position is an anchor; the external form for huge
// periodic chunks is not implemented, so it fails loud above 32 Mi positions.
static void anchor_blocks_ext(const SideText& text, const WinBytes& bwt,
                              const PosOverlay& pos, U n, U base, U period,
                              std::vector<BlockRec>& out, std::vector<uint64_t>& anchors,
                              U& steps, U& rows) {
    if (period < n) {
        require(n <= (1u << 25), "periodic chunk external form not implemented at this scale");
        U d = period, k = n / d;
        for (U i = 0; i < d; ++i) {
            U r = pos.at(i * k) - base;
            for (U j = 0; j < k; ++j)
                require(pos.at(i * k + j) - base == r + j * d, "periodic class structure mismatch");
            out.push_back({pos.at(i * k), i * k, (i + 1) * k});
            rows += k;
        }
        for (U q = n - d + 1; q < n; ++q) {
            U L = n - q; // < d
            U c1 = 0, c2 = 0, matched = 0;
            for (U i = 0; i < d; ++i) {
                U r = pos.at(i * k) - base;
                bool match = true;
                for (U t = 0; t < L; ++t)
                    if (text.at((r + t) % n) != text.at(q + t)) { match = false; break; }
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
        for (U q = 0; q < n; ++q) anchors[q] = q + base;
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
    {
        constexpr U W = 1u << 20;
        std::vector<uint8_t> buf(W);
        for (U i = 0; i < n;) {
            U len = std::min<U>(W, n - i);
            bwt.read(i, buf.data(), len);
            for (U z = 0; z < len; ++z) ++freq[buf[z]];
            i += len;
        }
    }
    U acc = 0; for (unsigned c = 0; c < 256; ++c) { C[c] = acc; acc += freq[c]; }
    RankIndex rank; rank.build(bwt, n);
    if (!n) return;
    U q = n - 1;
    U lo = C[text.at(q)], hi = lo + freq[text.at(q)];
    for (;;) {
        if (hi - lo >= 2) {
            out.push_back({q + base, lo, hi});
            anchors.push_back(q + base);
            rows += hi - lo;
        }
        // The backward step cannot enlarge the interval (rank differences over
        // an interval are bounded by its length), so once fewer than two rows
        // share the suffix, no later suffix can be an anchor.
        if (q == 0 || hi - lo < 2) break;
        uint8_t c = text.at(q - 1);
        U nlo = C[c] + rank(c, lo), nhi = C[c] + rank(c, hi);
        lo = nlo; hi = nhi; --q; ++steps;
    }
    if (getenv("CROSS_DEBUG"))
        std::fprintf(stderr, "DBG aperiodic walk base=%llu steps=%llu blocks=%zu rows=%llu\n",
                     (unsigned long long)base, (unsigned long long)steps, out.size(),
                     (unsigned long long)rows);
}

// Sort every anchor block's members by merged key, through the overlay: the
// block slice is read (bounded), sorted in RAM, and written back as one
// overlay region. Anchor membership via a sorted anchor vector (aperiodic:
// small; periodic: bounded by the 32 Mi guard above).
static void repair_ext(PosOverlay& W, const std::vector<BlockRec>& blocks,
                        const std::vector<uint64_t>& anchors, U base, U n_local, const Keys& keys) {
    if (blocks.empty()) return;
    std::vector<uint64_t> sa(anchors);
    std::sort(sa.begin(), sa.end());
    if (getenv("CROSS_DEBUG"))
        std::fprintf(stderr, "DBG repair base=%llu blocks=%zu anchors=%zu\n",
                     (unsigned long long)base, blocks.size(), sa.size());
    auto isAnchor = [&](U p) { return std::binary_search(sa.begin(), sa.end(), p); };
    std::vector<U> sk, an, slice;
    for (auto& b : blocks) {
        U len = b.hi - b.lo;
        require(len <= (1u << 26), "anchor block exceeds bounded repair buffer");
        slice.resize(len);
        for (U r = 0; r < len; ++r) slice[r] = W.at(b.lo + r);
        sk.clear(); an.clear();
        for (U p : slice) (isAnchor(p) ? an : sk).push_back(p);
        if (an.empty()) continue;
        if (getenv("CROSS_DEBUG")) {
            std::fprintf(stderr, "DBG block lo=%llu hi=%llu slice:", (unsigned long long)b.lo, (unsigned long long)b.hi);
            for (U p : slice) std::fprintf(stderr, " %llu", (unsigned long long)p);
            std::fprintf(stderr, "\n");
        }
        std::sort(an.begin(), an.end(), [&](uint64_t x, uint64_t y) { return keys.cmp(x, y) < 0; });
        U i = 0, j = 0, r = 0;
        while (r < len) {
            bool take_sk = j >= an.size() || (i < sk.size() && keys.cmp(sk[i], an[j]) < 0);
            slice[r++] = take_sk ? sk[i++] : an[j++];
        }
        if (getenv("CROSS_DEBUG")) {
            std::fprintf(stderr, "DBG repaired lo=%llu an:", (unsigned long long)b.lo);
            for (U p : an) std::fprintf(stderr, " %llu", (unsigned long long)p);
            std::fprintf(stderr, " sk:");
            for (U p : sk) std::fprintf(stderr, " %llu", (unsigned long long)p);
            std::fprintf(stderr, " out:");
            for (U p : slice) std::fprintf(stderr, " %llu", (unsigned long long)p);
            std::fprintf(stderr, "\n");
        }
        W.putSlice(b.lo, slice);
    }
}

// ------------------------------------------------------------ shard helpers
// PARALLEL EMISSION (order/run-boundary sharding). S = --threads by default;
// CROSS_SHARDS overrides (S=1 is exactly the banked serial walk; one code
// path). Shard counts never exceed the walked item count.
static U emit_shard_count(U threads, U items) {
    U S = threads ? threads : 8;
    if (const char* e = getenv("CROSS_SHARDS")) {
        if (*e) {
            U v = std::stoull(e);
            require(v >= 1 && v <= 4096, "CROSS_SHARDS out of range");
            S = v;
        }
    }
    if (items && S > items) S = items;
    if (S < 1) S = 1;
    return S;
}
static void put_le32(char* b, uint32_t v) { for (int i = 0; i < 4; ++i) b[i] = char(v >> (8 * i)); }
static void put_le64(char* b, U v)       { for (int i = 0; i < 8; ++i) b[i] = char(v >> (8 * i)); }

// Sparse walk protocol prototype. Ordered shard admission is intentional:
// endShard must consume/unlink its run spool before the next shard starts.
// No row-position file is opened in this mode. The v2 final emission uses
// this interface; legacy intermediate/compatibility emission remains below.
struct SparseWalkSink {
    virtual ~SparseWalkSink() = default;
    virtual void attachCorpus(const CorpusText&) {}
    virtual void finishWalk() {}
    virtual SparseWalkSink* shardSink(U) { return this; }
    virtual U shardCount(U normal) const { return normal; }
    virtual U admission() const { return 1; }
    virtual void beginShard(U shard, U lo, U hi) = 0;
    virtual void row(U row, U position) = 0;
    virtual void endShard() = 0;
};
struct SparseRun {
    uint8_t c = 0;
    U len = 0, head = 0, tail = 0;
    bool operator==(const SparseRun& b) const {
        return c == b.c && len == b.len && head == b.head && tail == b.tail;
    }
};
struct CapturedSeamClass {
    U lo = 0, hi = 0, first = 0, before = 0, after = 0;
    std::vector<U> occ;
    bool operator==(const CapturedSeamClass& b) const {
        return lo == b.lo && hi == b.hi && first == b.first &&
               before == b.before && after == b.after && occ == b.occ;
    }
};
struct SparseSeamCapture {
    struct Hit { U row = UINT64_MAX, before = 0, after = 0; };
    U n = 0, depth = 0, nextRow = 0, previous = 0, limit = 0;
    Hit* previousHit = nullptr;
    std::vector<CapturedSeamClass> raw;
    std::unordered_map<U, Hit> targets;
    void discover(const CorpusText& text, U count, U cap) {
        n = count; limit = cap; depth = nextRow = 0; previousHit = nullptr;
        raw.clear(); targets.clear();
        require(n > 0 && limit > 0, "sparse seam discovery bounds");
        std::vector<U> occ;
        const uint8_t terminal = text.at(n - 1);
        // Bound even the initial terminal-byte chain BEFORE allocating a
        // corpus-sized position set. The production refusal budget depends
        // on raw run count; the caller must supply that bound explicitly.
        constexpr U W = 1u << 20;
        std::vector<uint8_t> bytes(W);
        for (U off = 0; off < n; off += W) {
            U z = std::min<U>(W, n - off);
            text.read(off, bytes.data(), z);
            for (U j = 0; j < z; ++j) if (bytes[j] == terminal) {
                require(occ.size() < limit, "sparse seam terminal class budget");
                occ.push_back(off + j);
            }
        }
        U storedOccurrences = 0;
        const U discoveryWorkLimit = n > UINT64_MAX / 16 ? UINT64_MAX : std::max<U>(100000000, 16*n);
        for (U d = 1;; ++d) {
            require(d <= limit, "sparse seam depth budget");
            depth = d;
            if (occ.size() <= 1) break;
            require(d < n, "sparse seam escaped text");
            // Keep only the class count, not a duplicate occurrence vector
            // at every nested depth. Unique capture targets are independently
            // bounded before insertion; maximal intervals are selected later.
            require(occ.size() <= discoveryWorkLimit - storedOccurrences, "sparse seam discovery work budget");
            storedOccurrences += occ.size(); // journal a count, never a column
            CapturedSeamClass cl; cl.first = n - d; cl.hi = occ.size();
            raw.push_back(std::move(cl));
            for (U p : occ) if (targets.find(p) == targets.end()) {
                require(targets.size() < limit, "sparse seam unique target budget");
                targets.emplace(p, Hit{});
            }
            std::vector<U> next;
            uint8_t want = text.at(n - d - 1);
            for (U q : occ) if (q && text.at(q - 1) == want) next.push_back(q - 1);
            occ.swap(next);
        }
        std::fprintf(stderr, "CROSS_SEAM_DISCOVERED n=%llu depth=%llu raw_classes=%zu aggregate_occurrences=%llu unique_targets=%zu limit=%llu work_limit=%llu before_walk=1 bounded=1\n",
                     (unsigned long long)n, (unsigned long long)depth, raw.size(),
                     (unsigned long long)storedOccurrences, targets.size(), (unsigned long long)limit, (unsigned long long)discoveryWorkLimit);
        previous = n + 9; // before row 0 is the last padded suffix
    }
    void row(U index, U p) {
        require(index == nextRow++ && p < n, "sparse seam ordered row coverage");
        if (previousHit) previousHit->after = p;
        auto it = targets.find(p);
        previousHit = it == targets.end() ? nullptr : &it->second;
        if (previousHit) {
            require(previousHit->row == UINT64_MAX, "duplicate sparse seam target");
            previousHit->row = index; previousHit->before = previous;
            previousHit->after = n; // final-row flank wraps to first pad row
        }
        previous = p;
    }
    std::vector<CapturedSeamClass> finish() const {
        require(nextRow == n, "incomplete sparse seam walk");
        std::vector<CapturedSeamClass> sorted = raw, result;
        for (auto& cl : sorted) {
            const auto& hit = targets.at(cl.first);
            require(hit.row != UINT64_MAX, "missing sparse seam first row");
            cl.lo = hit.row; cl.hi += cl.lo; // discovery retained only |occ|
            require(cl.hi <= n, "sparse seam interval outside text");
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            return a.lo < b.lo || (a.lo == b.lo && a.hi > b.hi);
        });
        // One row index for all disjoint retained classes, rather than a
        // full target-map scan per class. SCS1 already carries banded rows.
        std::vector<std::pair<U, U>> byRow;
        byRow.reserve(targets.size());
        for (const auto& entry : targets) byRow.emplace_back(entry.second.row, entry.first);
        std::sort(byRow.begin(), byRow.end());
        size_t cursor = 0;
        U total = 0;
        for (auto& cl : sorted) {
            if (!result.empty() && cl.lo < result.back().hi) {
                require(cl.hi <= result.back().hi, "crossing sparse seam intervals");
                continue;
            }
            U count = cl.hi - cl.lo;
            require(count <= limit - total, "sparse seam final class budget");
            total += count;
            // Materialize only retained, disjoint classes. Their total rows
            // are bounded; nested discovery never stores duplicate columns.
            while (cursor < byRow.size() && byRow[cursor].first < cl.lo) ++cursor;
            while (cursor < byRow.size() && byRow[cursor].first < cl.hi)
                cl.occ.push_back(byRow[cursor++].second);
            require(cl.occ.size() == count, "missing sparse seam interval targets");

            for (U i = 0; i < count; ++i)
                require(targets.at(cl.occ[i]).row == cl.lo + i, "sparse seam interval coverage");
            require(cl.occ.front() == cl.first, "sparse seam leftmost row");
            cl.before = targets.at(cl.first).before; cl.after = targets.at(cl.occ.back()).after;
            result.push_back(std::move(cl));
        }
        require(total <= limit, "sparse seam final class budget");
        return result;
    }
};
// Scratch records are char + length + two run samples, never one position
// per row. Consumer stitches a continued boundary run using head from the
// left shard and tail from the right; global run bases follow that stitch.
struct SparseRunSpool : SparseWalkSink {
    const CorpusText* text;
    U n, lo = 0, hi = 0, next = 0, records = 0;
    bool dollar;
    std::string path;
    int fd = -1;
    SparseRun pending;
    std::vector<char> buffered;
    U writtenRecords = 0;
    SparseSeamCapture* seam;
    std::function<void(const std::string&, U, U)> consume;
    SparseRunSpool(const CorpusText& t, U count, bool dollars,
                   const std::string& file, SparseSeamCapture* capture,
                   std::function<void(const std::string&, U, U)> consumer)
        : text(&t), n(count), dollar(dollars), path(file), seam(capture), consume(consumer) {
        buffered.reserve(25 * 4096);
    }
    void attachCorpus(const CorpusText& corpus) override { text = &corpus; }
    void drain() {
        if (buffered.empty()) return;
        xput(fd, buffered.data(), buffered.size(), 25 * writtenRecords, "sparse run spool write");
        writtenRecords += buffered.size() / 25; buffered.clear();
    }
    void flush() {
        if (!pending.len) return;
        char b[25]; b[0] = char(pending.c);
        put_le64(b + 1, pending.len); put_le64(b + 9, pending.head); put_le64(b + 17, pending.tail);
        buffered.insert(buffered.end(), b, b + sizeof b); ++records;
        if (buffered.size() >= 25 * 4096) drain();
        pending = {};
    }
    void beginShard(U, U begin, U end) override {
        require(fd < 0 && !fs::exists(path), "previous shard not consumed");
        lo = next = begin; hi = end; records = writtenRecords = 0;
        require(buffered.empty(), "previous sparse spool buffer not drained");
        fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
        require(fd >= 0, "create sparse run spool");
    }
    void row(U index, U p) override {
        require(index == next++ && index < hi && p < n, "sparse run row coverage");
        if (seam) seam->row(index, p);
        uint8_t c = dollar ? (p ? text->at(p - 1) : uint8_t(2)) : text->at((p + n - 1) % n);
        if (pending.len && pending.c != c) flush();
        if (!pending.len) { pending.c = c; pending.head = p; }
        ++pending.len; pending.tail = p;
    }
    void endShard() override {
        require(next == hi, "incomplete sparse run shard");
        flush(); drain(); require(writtenRecords == records, "sparse spool record coverage");
        require(::close(fd) == 0, "close sparse run spool"); fd = -1;
        consume(path, records, hi - lo);
        require(!fs::exists(path), "sparse run spool consumer did not unlink");
    }
};

// The reducer owns only one boundary run. A shard's spool is read in
// fixed windows, stitched, then unlinked before the next admission.
struct SparseRunReducer {
    SparseRun pending;
    U rows = 0, starts = 0;
    std::function<void(const SparseRun&, U)> emit;
    explicit SparseRunReducer(std::function<void(const SparseRun&, U)> f) : emit(std::move(f)) {}
    void add(const SparseRun& run) {
        require(run.len > 0, "empty sparse run");
        if (pending.len && pending.c == run.c) {
            pending.len += run.len; pending.tail = run.tail;
        } else {
            flush(); pending = run;
        }
        rows += run.len;
    }
    void flush() {
        if (!pending.len) return;
        emit(pending, rows - pending.len); ++starts; pending = {};
    }
    void consume(const std::string& path, U records, U expectedRows) {
        int fd = ::open(path.c_str(), O_RDONLY);
        require(fd >= 0, "open admitted run spool");
        struct stat st{};
        require(::fstat(fd, &st) == 0 && U(st.st_size) == 25 * records, "run spool size");
        U before = rows;
        constexpr U W = 4096;
        std::vector<char> b(25 * W);
        auto read64 = [](const char* q) { U v = 0; for (unsigned j = 0; j < 8; ++j) v |= U(uint8_t(q[j])) << (8*j); return v; };
        for (U off = 0; off < records; off += W) {
            U count = std::min<U>(W, records - off), done = 0;
            while (done < 25 * count) {
                ssize_t z = ::pread(fd, b.data() + done, 25 * count - done, 25 * off + done);
                if (z < 0 && errno == EINTR) continue;
                require(z > 0, "short run spool read"); done += U(z);
            }
            for (U j = 0; j < count; ++j) {
                const char* q = b.data() + 25*j;
                add({uint8_t(q[0]), read64(q+1), read64(q+9), read64(q+17)});
            }
        }
        require(rows - before == expectedRows, "run spool row count");
        require(::close(fd) == 0 && ::unlink(path.c_str()) == 0, "consume run spool");
    }
};

// Retired: ParallelSparseRuns count/replay admission and per-shard capture.

// ------------------------------------------------------------ external core merge
// Per-side anchor/repair stats: k-way generalized (k=2 is the pairwise case).
struct CoreStats {
    std::vector<U> anchors, steps, rows;
    void init(size_t k) { anchors.assign(k, 0); steps.assign(k, 0); rows.assign(k, 0); }
    U sum(const std::vector<U>& v) const { U t = 0; for (U x : v) t += x; return t; }
    U anchorsA() const { return anchors.size() > 0 ? anchors[0] : 0; }
    U anchorsB() const { return anchors.size() > 1 ? anchors[1] : 0; }
    U stepsA() const { return steps.size() > 0 ? steps[0] : 0; }
    U stepsB() const { return steps.size() > 1 ? steps[1] : 0; }
    U rowsA() const { return rows.size() > 0 ? rows[0] : 0; }
    U rowsB() const { return rows.size() > 1 ? rows[1] : 0; }
};
struct PairScratch {
    std::string dir;
    std::string hs, wm;
    std::vector<std::string> bwt, ov;   // per side
    void init(const std::string& out, U pid, size_t sides) {
        dir = out + ".xsc-" + std::to_string(pid);
        fs::create_directories(dir);
        hs = dir + "/hs.bin"; wm = dir + "/wm.bin";
        bwt.clear(); ov.clear();
        for (size_t j = 0; j < sides; ++j) {
            bwt.push_back(dir + "/bwt" + std::to_string(j) + ".bin");
            ov.push_back(dir + "/ov" + std::to_string(j) + ".bin");
        }
    }
    void release_walk() const {
        ::unlink(hs.c_str());
        for (const auto& f : bwt) ::unlink(f.c_str());
        for (const auto& f : ov) ::unlink(f.c_str());
    }
    void cleanup(bool keep_wm = false) {
        // Last-consumer ownership: v2 preserves seam.spool + .done for the
        // concurrent finish. Legacy callers can preserve wm.bin + .done.
        // Merge must never unlink either protocol's last-consumer files.
        // The finish unlinks them; both sides remove the then-empty dir.
        release_walk();
        if (!keep_wm) {
            ::unlink(wm.c_str());
            fs::remove_all(dir);
        } else {
            // Best-effort empty-dir removal: the finish may have already
            // unlinked wm.bin (+.done) - then this closes the loop at merge
            // exit; if it has not, this no-ops (dir non-empty) and the
            // finish's own remove after its unlink closes it instead.
            // Either interleaving ends with the dir gone.
            std::error_code ec;
            fs::remove(dir, ec);
        }
    }
};
// Compose references in merge-local coordinates. Legacy embedded sidecars
// remain readable for the existing selftest/old input protocol; no text is
// created here. Production sides always carry original-corpus references.
static void open_merge_text(const std::vector<SideExt>& sides, CorpusText& text, bool pooled = false) {
    std::vector<TextSeg> list;
    for (const auto& s : sides) {
        if (s.refText) list.insert(list.end(), s.segs.begin(), s.segs.end());
        else {
            TextSeg g; g.path = s.sxsPath; g.off = 20; g.len = s.n;
            for (U c = 0; c < 256; ++c) g.remap[c] = uint8_t(c);
            list.push_back(std::move(g));
        }
    }
    text.open(std::move(list), pooled);
    std::fprintf(stderr, "CROSS_TEXT_VIEW virtual_bytes=%llu corpus_files=%zu segments=%zu materialized_bytes=0 window_bytes=16384\n",
                 (unsigned long long)(2 * text.n), text.files.size(), text.segs.size());
}
// BWT of a side, streamed: bwt[i] = text[(pos[i]+n-1)%n], written sequentially.
static void build_bwt_file(const SideExt& S, const PosOverlay& pos, const std::string& path,
                           U threads, U base) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    require(fd >= 0, "create bwt scratch");
    U tc = std::max<U>(1, std::min(threads, S.n / (1u << 20) + 1));
    U band = (S.n + tc - 1) / tc;
    std::vector<std::thread> ts;
    auto work = [&](U t) {
        SideText text; open_side_text(S, text);
        U i0 = t * band, i1 = std::min(S.n, i0 + band);
        std::vector<uint8_t> buf(i1 > i0 ? i1 - i0 : 0);
        for (U i = i0; i < i1; ++i) {
            U p = pos.at(i);   // global; the BWT reads LOCAL text
            buf[i - i0] = text.at((p - base + S.n - 1) % S.n);
        }
        if (i1 > i0) xput(fd, buf.data(), i1 - i0, i0, "bwt write");
    };
    for (U t = 0; t < tc; ++t) ts.emplace_back(work, t);
    for (auto& t : ts) t.join();
    ::close(fd);
}
// Externalized core merge: sparse-hash/BWT/WM are scratch files; the
// children's pos are windowed with a repair overlay; per-position RAM is
// limited to window caches, the rank index, and bounded block buffers.
// K-WAY EXTERNAL CORE MERGE (k=2 is the certified pairwise case; ONE code
// path, k selected at launch). Sides tile the text T = text_0..text_{k-1};
// M2 = T||T. The anchor/repair theory is per-side and INDEPENDENT of k: two
// rows of one side diverge between their LOCAL cyclic order and the merged
// (global) order only on prefix pairs, whose short sides are exactly the
// side's own chunk-suffix anchors (the shorter suffix fits inside the side
// because both rows are in it), found by the same backward-search walk on the
// side's own BWT; blocks are sorted by the same merged comparator. The
// global order restricted to a side IS its repaired order, so the merged
// sequence is the k-way interleave of the k repaired orders.
static void core_merge_kway(const std::vector<SideExt>& sides, bool dollar, U threads,
                            Budget& bud, CoreStats& st, const PairScratch& sc, U hashK, bool pre,
                            SparseWalkSink* sparse = nullptr) {
    const Budget entryWork = bud;
    auto workLine = [](const char* stage, U pass, const Budget& work) {
        std::fprintf(stderr, "CROSS_STAGE_WORK stage=%s pass=%llu comparisons=%llu symbols_compared=%llu probes=%llu\n",
                     stage, (unsigned long long)pass, (unsigned long long)work.comparisons,
                     (unsigned long long)work.symbols, (unsigned long long)work.probes);
    };
    auto difference = [](const Budget& after, const Budget& before) {
        Budget result; result.comparisons = after.comparisons - before.comparisons;
        result.symbols = after.symbols - before.symbols; result.probes = after.probes - before.probes;
        return result;
    };
    double last = now_sec();
    const size_t k = sides.size();
    require(k >= 1, "k-way merge needs at least one side");
    st.init(k);
    U n = 0; for (auto& S : sides) n += S.n;
    CorpusText m2; open_merge_text(sides, m2, true);
    if (sparse) sparse->attachCorpus(m2); // reuse the comparator's text cache, within its RAM envelope
    if (!pre) {
        hashK = hash_spacing();
        if (hashK > n / 2) hashK = std::max<U>(1, n / 2);   // degenerate-grid guard
        build_hash_sparse(m2, 2 * n, hashK, sc.hs, threads);
        phase("hash-build", last);
    }
    HashSparse hp; hp.open_ro(sc.hs, hashK, 2 * n / hashK + 1);
    Keys keys; keys.init(&m2, &hp, n, dollar, &bud);
    // MERGE-LOCAL coordinate rebasing (the banked pairwise contract): every
    // merge's positions run in the merge's own coordinate space - side j is
    // rebased to localOff[j] = sum of the previous sides' lengths (the
    // pairwise path's A->0, B->nA). SideExt.offset is the CORPUS offset and
    // is used ONLY for the tiling contract and the emitted SXCR header.
    // (For a flat merge from chunk 0 they coincide; for tree intermediates
    // they do not, and the comparator's p < n bound is merge-local.)
    std::vector<U> localOff(k, 0);
    for (size_t j = 1; j < k; ++j) localOff[j] = localOff[j - 1] + sides[j - 1].n;
    // Per-side position overlays (positions rebased to merge-local space).
    std::vector<std::unique_ptr<PosOverlay>> pos(k);
    for (size_t j = 0; j < k; ++j)
        pos[j].reset(new PosOverlay());
    for (size_t j = 0; j < k; ++j)
        pos[j]->init(sides[j].sxsPath, sides[j].n, sides[j].posBase,
                     localOff[j], sc.ov[j]);
    // Anchor blocks per side, then repair (k-independent, per side - and
    // therefore PARALLEL ACROSS SIDES: every side's BWT/overlay/scratch file
    // is its own; the shared comparator is thread-safe with per-side
    // budgets, summed after the join like the walk's shard budgets. This
    // was the largest remaining serial core after the walks were sharded
    // (gate 1: 358s on the final fat pair), and a flat k-way merge would
    // otherwise serialize all k sides' anchor+repair phases).
    {
        // Memory-aware side parallelism: each concurrent side's anchor pass
        // holds ~n_j of BWT band + ~n_j/2 of rank table + buffers; bound the
        // concurrent sides to half the phase's RLIMIT_AS (the pools, the load
        // transient, and the walk/emit buffers own the rest).
        U sidePar = std::min<size_t>(k, threads ? threads : 1);
        {
            struct rlimit rl{};
            U perSide = 0;
            for (auto& S : sides) perSide = std::max(perSide, S.n + S.n / 2 + (64u << 20));
            if (perSide && getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
                U fit = U(rl.rlim_cur) / 2 / perSide;
                if (fit < 1) fit = 1;
                sidePar = std::min<U>(sidePar, fit);
            }
        }
        std::vector<Budget> sideBud(k);
        std::atomic<size_t> nextSide{0};
        std::vector<std::thread> ts;
        for (U t = 0; t < sidePar; ++t)
            ts.emplace_back([&, t] {
                for (;;) {
                    size_t j = nextSide.fetch_add(1);
                    if (j >= k) return;
                    Keys skeys; skeys.init(&m2, &hp, n, dollar, &sideBud[j]);
                    std::vector<BlockRec> blocks;
                    std::vector<uint64_t> anchors;
                    if (sides[j].bwtFromRuns)
                        build_bwt_from_runs(sides[j], *pos[j], sc.bwt[j], localOff[j]);
                    else
                        build_bwt_file(sides[j], *pos[j], sc.bwt[j],
                                       std::max<U>(1, threads / sidePar), localOff[j]);
                    { WinBytes bwt; bwt.open_ro(sc.bwt[j], sides[j].n, 0);
                      SideText text; open_side_text(sides[j], text);
                      anchor_blocks_ext(text, bwt, *pos[j], sides[j].n, localOff[j],
                                        sides[j].period, blocks, anchors,
                                        st.steps[j], st.rows[j]); }
                    ::unlink(sc.bwt[j].c_str()); // anchor scan was its last consumer
                    st.anchors[j] = anchors.size();
                    repair_ext(*pos[j], blocks, anchors, localOff[j], sides[j].n, skeys);
                }
            });
        for (auto& t : ts) t.join();
        for (size_t j = 0; j < k; ++j) {
            const Budget& sb = sideBud[j];
            bud.comparisons += sb.comparisons; bud.fast_decided += sb.fast_decided;
            bud.probe_decided += sb.probe_decided; bud.cap_decided += sb.cap_decided;
            bud.probes += sb.probes; bud.symbols += sb.symbols;
            bud.tie_decided += sb.tie_decided;
            bud.lce_over_10k += sb.lce_over_10k; bud.lce_over_100k += sb.lce_over_100k;
            bud.lce_over_1m += sb.lce_over_1m;
            if (sb.max_lce > bud.max_lce) bud.max_lce = sb.max_lce;
            bud.tFast += sb.tFast; bud.tPrep += sb.tPrep; bud.tProbe += sb.tProbe; bud.tScan += sb.tScan;
        }
    }
    phase("anchor+repair", last);
    const Budget preparedWork = bud;
    workLine("prepare", 0, difference(bud, entryWork));
    // K-way cross merge under the total order, streamed into the WM file.
    // PARALLEL EMISSION (k-way): the walk is sharded at fixed order
    // boundaries. The merged total order is strict and the global order
    // restricted to a side IS its repaired order, so the merged rank of
    // side i's a-th row is exactly a + sum_{j != i} rank_j(row). Planning
    // binary-searches, at merged-rank target s*n/S on the WIDEST side i,
    // the largest a_s with rank(A_i[a_s]) <= s*n/S; every side's boundary
    // is then rank_j(X_s) for the boundary element X_s = A_i[a_s] (and a_s
    // itself for side i), so every element of each side's s-range has
    // merged rank inside [R_s, R_{s+1}) with R_s = a_s + sum_j rank_j(X_s).
    // Shard s k-way-merges its per-side row ranges with a BINARY MIN-HEAP of
    // the k current fronts (O(log k) comparisons per emitted element, never
    // O(k)) and pwrites at element offset R_s: disjoint ranges, one code
    // path (k=2, S=1 is the serial pairwise walk), byte-identical by
    // construction. Planning costs (S-1)*log(n_i)*k*log(n_j) comparisons
    // (boundaries planned in parallel), charged to the same budget.
    int fd = -1;
    WmPacker wmPacker;
    const bool packWm = wm_pack_enabled();
    if (sparse) require(!fs::exists(sc.wm), "sparse walk refuses existing WM");
    else if (packWm) wmPacker.open(sc.wm, n);
    else {
        fd = ::open(sc.wm.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        require(fd >= 0, "create wm scratch");
    }
    double tPF = 0, tPop = 0;
    const U normalShards = emit_shard_count(threads, n);
    const U S = sparse ? sparse->shardCount(normalShards) : normalShards;
    size_t wide = 0;   // planning probe side: the widest
    for (size_t j = 1; j < k; ++j) if (sides[j].n > sides[wide].n) wide = j;
    std::vector<std::vector<U>> aB(S + 1, std::vector<U>(k, 0));
    for (size_t j = 0; j < k; ++j) aB[S][j] = sides[j].n;
    std::vector<Budget> planningWork(S);
    {
        require(sides[wide].n > 0, "k-way planning needs a non-empty probe side");
        std::vector<std::thread> pt;
        std::mutex planMu;
        std::atomic<U> nextPlan{1};
        for (U worker = 0; worker < std::min<U>(S-1, std::max<U>(1, threads)); ++worker) {
            pt.emplace_back([&] {
                for (;;) {
                U s = nextPlan.fetch_add(1);
                if (s >= S) break;
                // Thread-local keys/budget: the shared-key comparator is
                // never touched from planning threads (its scratch is
                // thread-local but its Budget is not).
                Budget pb;
                Keys pkeys; pkeys.init(&m2, &hp, n, dollar, &pb);
                // rank in side j of a global position p (side j's repaired
                // order IS the global order restricted to it):
                // #{rows of j ordered before p}.
                auto rankIn = [&](size_t j, U p) -> U {
                    U lo = 0, hi = sides[j].n;
                    while (lo < hi) {
                        U mid = lo + (hi - lo) / 2;
                        if (pkeys.cmp(pos[j]->at(mid), p) < 0) lo = mid + 1; else hi = mid;
                    }
                    return lo;
                };
                U target = s * n / S;
                U nI = sides[wide].n;
                auto rowRank = [&](U a) -> U {   // merged rank of wide's a-th row
                    U r = a, p = pos[wide]->at(a);
                    for (size_t j = 0; j < k; ++j)
                        if (j != wide) r += rankIn(j, p);
                    return r;
                };
                U lo = 0, hi = nI;         // f(a) = rank(A_wide[a]); f(nI) := n
                while (lo < hi) {
                    U mid = lo + (hi - lo + 1) / 2;
                    U f = (mid == nI) ? n : rowRank(mid);
                    if (f <= target) lo = mid; else hi = mid - 1;
                }
                require(lo < nI, "k-way boundary fell off the probe side");
                U bElem = pos[wide]->at(lo);   // the boundary element X_s
                for (size_t j = 0; j < k; ++j)
                    aB[s][j] = (j == wide) ? lo : rankIn(j, bElem);
                planningWork[s] = pb;
                std::lock_guard<std::mutex> g(planMu);
                bud.comparisons += pb.comparisons; bud.probes += pb.probes;
                bud.symbols += pb.symbols; bud.fast_decided += pb.fast_decided;
                bud.probe_decided += pb.probe_decided; bud.cap_decided += pb.cap_decided;
                bud.tie_decided += pb.tie_decided; bud.note_lce(pb.max_lce);
                bud.tFast += pb.tFast; bud.tPrep += pb.tPrep; bud.tProbe += pb.tProbe; bud.tScan += pb.tScan;
                }
            });
        }
        for (auto& t : pt) t.join();
    }
    workLine("plan", 0, difference(bud, preparedWork));
    // merged-rank shard start offsets: R_s = sum_j aB[s][j]
    std::vector<U> R(S + 1, 0);
    for (U s = 0; s <= S; ++s) for (size_t j = 0; j < k; ++j) R[s] += aB[s][j];
    // Experimental exact continuation oracle. Rank searches above do not
    // traverse a heap: acquiring these checkpoints requires a charged replay.
    // Kept opt-in because this construction adds a full serial walk of work.
    const bool seedHeap = S > 1 && getenv("CROSS_SEED_HEAP") != nullptr;
    struct HeapCheckpoint {
        std::vector<U> cur, values;
        std::vector<size_t> sides;
    };
    const bool seamOracle = S > 1 && getenv("CROSS_SEAM_ORACLE") != nullptr;
    const bool replayHeap = seedHeap || seamOracle;
    std::vector<HeapCheckpoint> checkpoints(seedHeap ? S : 0);
    std::vector<Budget> rangeReference(S);
    Budget seedWork;
#ifdef SOL7D_TRACE
    std::vector<std::pair<U, U>> seedTrace;
    std::vector<std::vector<std::pair<U, U>>> producerTrace(S);
#endif
    if (replayHeap) {
        Keys replayKeys; replayKeys.init(&m2, &hp, n, dollar, &seedWork);
#ifdef SOL7D_TRACE
        replayKeys.trace = &seedTrace;
#endif
        // The oracle must not turn each scalar front access into an SXP4
        // group decode. Borrow the same immutable bulk front windows as the
        // producer; this changes physical reads, never comparator operands.
        std::vector<U> replayBase(k, UINT64_MAX);
        std::vector<std::vector<U>> replayWindows(k);
        auto replayFront = [&](size_t side, U cursor) {
            auto& window = replayWindows[side];
            if (replayBase[side] == UINT64_MAX || cursor < replayBase[side] || cursor-replayBase[side] >= window.size()) {
                window.resize(std::min<U>(4096, sides[side].n-cursor));
                pos[side]->read(cursor,window.data(),window.size()); replayBase[side]=cursor;
            }
            return window[cursor-replayBase[side]];
        };
        std::vector<U> cur(k, 0), values;
        std::vector<size_t> heapSides;
        auto less = [&](size_t a, size_t b) {
            return replayKeys.cmp(values[a], values[b]) < 0;
        };
        auto swap = [&](size_t a, size_t b) {
            std::swap(values[a], values[b]); std::swap(heapSides[a], heapSides[b]);
        };
        for (size_t j = 0; j < k; ++j) if (sides[j].n) {
            values.push_back(replayFront(j,0)); heapSides.push_back(j);
            size_t i = values.size() - 1;
            while (i && less(i, (i - 1) / 2)) {
                size_t par = (i - 1) / 2; swap(i, par); i = par;
            }
        }
        U boundary = 1, referenceRange = 0;
        Budget referenceBefore;
        for (U row = 0; row <= n; ++row) {
            while (boundary < S && R[boundary] == row) {
                require(cur == aB[boundary], "checkpoint cursor/rank identity");
                if (seedHeap) checkpoints[boundary] = {cur, values, heapSides};
                rangeReference[referenceRange] = difference(seedWork, referenceBefore);
                referenceBefore = seedWork; referenceRange = boundary;
                ++boundary;
            }
            if (row == n) break;
            require(!values.empty(), "checkpoint replay heap coverage");
            size_t j = heapSides[0];
            if (++cur[j] < sides[j].n) values[0] = replayFront(j,cur[j]);
            else {
                values[0] = values.back(); heapSides[0] = heapSides.back();
                values.pop_back(); heapSides.pop_back();
            }
            size_t i = 0;
            for (;;) {
                size_t l = 2 * i + 1, r = l + 1, m = i;
                if (l < values.size() && less(l, m)) m = l;
                if (r < values.size() && less(r, m)) m = r;
                if (m == i) break;
                swap(i, m); i = m;
            }
        }
        rangeReference[referenceRange] = difference(seedWork, referenceBefore);
        require(boundary == S, "checkpoint boundary coverage");
        if (seamOracle) workLine("oracle_reference", 0, seedWork);
        if (seedHeap) {
        bud.comparisons += seedWork.comparisons; bud.symbols += seedWork.symbols;
        bud.probes += seedWork.probes; bud.fast_decided += seedWork.fast_decided;
        bud.probe_decided += seedWork.probe_decided; bud.cap_decided += seedWork.cap_decided;
        bud.tie_decided += seedWork.tie_decided;
        bud.lce_over_10k += seedWork.lce_over_10k; bud.lce_over_100k += seedWork.lce_over_100k;
        bud.lce_over_1m += seedWork.lce_over_1m;
        bud.max_lce = std::max(bud.max_lce, seedWork.max_lce);
        bud.tFast += seedWork.tFast; bud.tPrep += seedWork.tPrep;
        bud.tProbe += seedWork.tProbe; bud.tScan += seedWork.tScan;
        workLine("planning_seed", 0, seedWork);
        workLine("overlap", 0, Budget{});
        }
    }
    // A diagnostic oracle run can persist per-range reference counters. A
    // timed run reuses them without replay. Bind the journal to every split,
    // mode and source identity; changed input metadata refuses reuse.
    bool haveReference = replayHeap || S == 1;
    if (const char* referencePath = getenv("CROSS_SEAM_REFERENCE")) {
        std::string identity = "sol7e-range-v1";
        auto word = [&](U value) { identity.append(reinterpret_cast<const char*>(&value),8); };
        auto fileIdentity = [&](const std::string& path) {
            struct stat info{}; require(::stat(path.c_str(), &info) == 0, "reference source stat");
            word(path.size()); identity += path;
            word(info.st_dev); word(info.st_ino); word(info.st_size);
            word(info.st_mtim.tv_sec); word(info.st_mtim.tv_nsec);
        };
        word(n); word(k); word(S); word(dollar); word(hashK);
        for (auto& boundary : aB) for (U cursor : boundary) word(cursor);
        for (const auto& side : sides) {
            word(side.n); word(side.offset); word(side.period); word(side.posBase); word(side.refText);
            fileIdentity(side.sxsPath);
            for (const auto& seg : side.segs) {
                fileIdentity(seg.path); word(seg.off); word(seg.len);
                identity.append(reinterpret_cast<const char*>(seg.remap.data()), seg.remap.size());
            }
        }
        if (seamOracle) {
            require(!fs::exists(referencePath), "reference journal already exists");
            OutFile reference(referencePath); reference.w64(identity.size());
            reference.raw(identity.data(), identity.size());
            for (const auto& range : rangeReference) {
                reference.w64(range.comparisons); reference.w64(range.symbols); reference.w64(range.probes);
            }
        } else if (S > 1) {
            std::ifstream reference(referencePath, std::ios::binary);
            U size = 0; reference.read(reinterpret_cast<char*>(&size),8);
            require(bool(reference) && size == identity.size(), "reference journal identity size");
            std::string stored(size,'\0'); reference.read(stored.data(),size);
            require(bool(reference) && stored == identity, "reference journal input/split identity");
            for (auto& range : rangeReference) {
                reference.read(reinterpret_cast<char*>(&range.comparisons),8);
                reference.read(reinterpret_cast<char*>(&range.symbols),8);
                reference.read(reinterpret_cast<char*>(&range.probes),8);
            }
            require(bool(reference) && reference.peek() == EOF, "reference journal exact extent");
            haveReference = true;
        }
    }
    const U passes = 1;
    std::vector<Budget> shardBud(passes * S);
    std::vector<double> shardPF(passes * S, 0.0), shardPop(passes * S, 0.0);
    {
        const bool noPf = getenv("CROSS_NO_PF") != nullptr;
        std::vector<std::thread> ts;
        auto launch = [&](SparseWalkSink* target, U pass, std::vector<std::thread>& pool) {
        const U workers = target ? std::min<U>(S, target->admission()) : S;
        for (U worker = 0; worker < workers; ++worker) {
            pool.emplace_back([&, worker, workers, target, pass] {
                for (U s = worker; s < S; s += workers) {
                const U work = pass * S + s;
                SparseWalkSink* sink = target ? target->shardSink(s) : nullptr;
                if (sink) sink->beginShard(s, R[s], R[s + 1]);
                Keys skeys; skeys.init(&m2, &hp, n, dollar, &shardBud[work]);
#ifdef SOL7D_TRACE
                skeys.trace = &producerTrace[s];
#endif
                constexpr U W = 8192;
                std::vector<U> buf(W);
                U nb = 0, out = R[s], emitted = 0, step = 0;
                auto flushW = [&]() {
                    if (!nb) return;
                    if (packWm) { for (U z = 0; z < nb; ++z) wmPacker.put(out + z, buf[z]); out += nb; nb = 0; }
                    else { xput(fd, buf.data(), 8 * nb, 8 * out, "wm write"); out += nb; nb = 0; }
                };
                // per-side cursors and a binary min-heap of the k fronts.
                // WINDOWED POS FRONTS: the pairwise-era 4-slot thread-local
                // reader cache thrashes across k > 4 overlay objects (one
                // 32 KB pread per front value - gate 3's floor analysis:
                // t_pf 17.5k CPU-s, the walk at ~22 effective cores of 48,
                // PREAD-BOUND, not comparison-bound). Each side's fronts
                // come from a bulk window instead: ONE overlay read per
                // side per WIN emitted rows of that side.
                constexpr U WIN = 4096;
                std::vector<U> cur(k), end(k), wbase(k, 0);
                std::vector<std::vector<U>> pwin(k);
                for (size_t j = 0; j < k; ++j) { cur[j] = aB[s][j]; end[j] = seedHeap ? sides[j].n : aB[s + 1][j]; wbase[j] = cur[j]; }
                auto frontAt = [&](size_t j, U idx) -> U {   // idx: global row in [cur[j], end[j])
                    if (idx >= wbase[j] && idx - wbase[j] < pwin[j].size()) {
                        U w = idx - wbase[j];
                        // The immutable corpus has no asynchronous pool
                        // workers. Hide RAM latency using the already decoded
                        // next fronts, without changing heap order or work.
                        if (m2.cached && w + 16 < pwin[j].size())
                            m2.prefetch(pwin[j][w + 16], 16);
                        return pwin[j][w];
                    }
                    require(idx < end[j], "window front bounds");
                    U len = std::min<U>(WIN, end[j] - idx);
                    pwin[j].resize(len);
                    pos[j]->read(idx, pwin[j].data(), len);
                    wbase[j] = idx;
                    return pwin[j][0];
                };
                std::vector<U> hv(k);   // heap values
                std::vector<size_t> hs_(k);  // heap sides
                size_t hm = 0;
                auto hLess = [&](size_t a, size_t b) { return skeys.cmp(hv[a], hv[b]) < 0; };
                auto hPush = [&](U v, size_t side) {
                    size_t i = hm++;
                    hv[i] = v; hs_[i] = side;
                    while (i > 0) {
                        size_t par = (i - 1) / 2;
                        if (!hLess(i, par)) break;
                        std::swap(hv[i], hv[par]); std::swap(hs_[i], hs_[par]);
                        i = par;
                    }
                };
                auto hSift = [&]() {
                    size_t i = 0;
                    for (;;) {
                        size_t l = 2 * i + 1, r = l + 1, m = i;
                        if (l < hm && hLess(l, m)) m = l;
                        if (r < hm && hLess(r, m)) m = r;
                        if (m == i) break;
                        std::swap(hv[i], hv[m]); std::swap(hs_[i], hs_[m]);
                        i = m;
                    }
                };
                auto hPopReplace = [&](U v, size_t side) {   // root leaves, push (v,side)
                    hv[0] = v; hs_[0] = side;
                    hSift();
                };
                if (seedHeap && s) {
                    const auto& checkpoint = checkpoints[s];
                    require(cur == checkpoint.cur, "seeded producer cursors");
                    hm = checkpoint.values.size();
                    std::copy(checkpoint.values.begin(), checkpoint.values.end(), hv.begin());
                    std::copy(checkpoint.sides.begin(), checkpoint.sides.end(), hs_.begin());
                } else {
                    for (size_t j = 0; j < k; ++j)
                        if (cur[j] < end[j]) hPush(frontAt(j, cur[j]), j);
                }
                U pfAt = 0;
                while (hm > 0 && (!seedHeap || emitted < R[s + 1] - R[s])) {
                    if (!noPf && m2.pool && (step & 31) == 0) {
                        // Front speculation: each side's next few fronts,
                        // served from the bulk windows (no overlay reads).
                        double q0 = now_sec();
                        U firstJb = 16 / hashK + 1;
                        for (size_t j = 0; j < k; ++j)
                            for (U d = 1; d <= 4 && cur[j] + d < end[j]; ++d) {
                                U w = cur[j] + d - wbase[j];
                                if (w >= pwin[j].size()) break;   // next window: speculative skip
                                U p = pwin[j][w];
                                m2.prefetch(p, 16);
                                if (hp.col.pool) hp.col.prefetch(p / hashK + firstJb);
                            }
                        shardPF[work] += now_sec() - q0;
                        pfAt = step + 32;
                    }
                    double q1 = now_sec();
                    U v = hv[0]; size_t j = hs_[0];
                    ++cur[j];
                    shardPop[work] += now_sec() - q1;
                    if (sink) { sink->row(R[s] + emitted, v); ++out; }
                    else buf[nb++] = v;
                    ++emitted;
                    if (cur[j] < end[j]) hPopReplace(frontAt(j, cur[j]), j);
                    else {   // side exhausted: move last to root, shrink
                        hv[0] = hv[hm - 1]; hs_[0] = hs_[hm - 1]; --hm;
                        if (hm > 0) hSift();
                    }
                    if (nb == W) flushW();
                    ++step;
                }
                U want = 0; for (size_t j = 0; j < k; ++j) want += aB[s + 1][j] - aB[s][j];
                flushW();
                require(emitted == want && out == R[s + 1], "shard merge coverage");
                if (sink) sink->endShard();
                }
            });
        }
        };
        launch(sparse, 0, ts);
        for (auto& t : ts) if (t.joinable()) t.join();
        if (sparse) sparse->finishWalk();
    }
#ifdef SOL7D_TRACE
    if (seedHeap) {
        std::vector<std::pair<U, U>> combined;
        for (const auto& trace : producerTrace)
            combined.insert(combined.end(), trace.begin(), trace.end());
        require(combined == seedTrace, "seeded comparison trace identity");
        std::fprintf(stderr, "SEAM_TRACE_PASS pairs=%llu ordered_operands=exact\n",
                     (unsigned long long)combined.size());
    }
#endif
    if (!sparse) { if (packWm) wmPacker.close(); else ::close(fd); }
    for (U pass = 0; pass < passes; ++pass) {
        Budget heapWork;
        for (U s = pass * S; s < (pass + 1) * S; ++s) {
            heapWork.comparisons += shardBud[s].comparisons;
            heapWork.symbols += shardBud[s].symbols; heapWork.probes += shardBud[s].probes;
        }
        workLine("heap", pass, heapWork);
        Budget walkingWork = difference(preparedWork, entryWork);
        walkingWork.comparisons += heapWork.comparisons;
        walkingWork.symbols += heapWork.symbols; walkingWork.probes += heapWork.probes;
        workLine("WALK", pass, walkingWork);
        Budget planWork;
        for (const auto& pb : planningWork) {
            planWork.comparisons += pb.comparisons; planWork.symbols += pb.symbols; planWork.probes += pb.probes;
        }
        if (seedHeap) { planWork.comparisons += seedWork.comparisons; planWork.symbols += seedWork.symbols; planWork.probes += seedWork.probes; }
        workLine("PLANNING", pass, planWork);
        workLine("OVERLAP", pass, Budget{});
        if (haveReference) {
            Budget reference;
            if (S == 1) reference = heapWork;
            else for (const auto& rb : rangeReference) {
                reference.comparisons += rb.comparisons; reference.symbols += rb.symbols; reference.probes += rb.probes;
            }
            std::fprintf(stderr, "CROSS_SEAM_TOTAL comparisons=%lld symbols_compared=%lld probes=%lld reference=serial_range signed=1 replay=%u\n",
                (long long)heapWork.comparisons - (long long)reference.comparisons,
                (long long)heapWork.symbols - (long long)reference.symbols,
                (long long)heapWork.probes - (long long)reference.probes, unsigned(replayHeap));
        } else std::fprintf(stderr, "CROSS_SEAM_TOTAL status=unmeasured reference_required=CROSS_SEAM_REFERENCE\n");
        if (getenv("CROSS_SEED_HEAP") || getenv("CROSS_SEAM_ORACLE") || getenv("CROSS_SEAM_REFERENCE"))
            for (U s = 0; s < S; ++s) {
                workLine("producer", s, shardBud[pass * S + s]);
                workLine("planning_seam", s, planningWork[s]);
                workLine("overlap_seam", s, Budget{});
                const Budget& actual = shardBud[pass * S + s];
                const Budget& ref = S == 1 ? actual : rangeReference[s];
                workLine("reference_range", s, ref);
                std::fprintf(stderr, "CROSS_SEAM_WORK seam=%llu comparisons=%lld symbols_compared=%lld probes=%lld signed=1\n",
                    (unsigned long long)s,
                    (long long)actual.comparisons - (long long)ref.comparisons,
                    (long long)actual.symbols - (long long)ref.symbols,
                    (long long)actual.probes - (long long)ref.probes);
            }
    }
    for (U s = 0; s < passes * S; ++s) {
        const Budget& sb = shardBud[s];
        bud.comparisons += sb.comparisons; bud.fast_decided += sb.fast_decided;
        bud.probe_decided += sb.probe_decided; bud.cap_decided += sb.cap_decided;
        bud.probes += sb.probes; bud.symbols += sb.symbols;
        bud.tie_decided += sb.tie_decided;
        bud.lce_over_10k += sb.lce_over_10k; bud.lce_over_100k += sb.lce_over_100k;
        bud.lce_over_1m += sb.lce_over_1m;
        if (sb.max_lce > bud.max_lce) bud.max_lce = sb.max_lce;
        bud.tFast += sb.tFast; bud.tPrep += sb.tPrep; bud.tProbe += sb.tProbe; bud.tScan += sb.tScan;
        tPF += shardPF[s]; tPop += shardPop[s];
    }
    std::fprintf(stderr, "XMERG_TIMING t_pf=%.3f t_pop=%.3f cell_hits=%llu cell_miss=%llu shards=%llu\n",
                 tPF, tPop, (unsigned long long)cell_sum(g_cellHitsArr), (unsigned long long)cell_sum(g_cellMissArr),
                 (unsigned long long)S);
    phase("cross-merge", last);
}

// ---------------------------------------------------------------- emitters
// Final four files: rows = padding rotations n..n+9 then the $-order of M.
// Row char/sample: pad row t (0..9): sample n+t, char (t==0 ? M[n-1] : 0x02);
// suffix row p: sample p, char (p==0 ? 0x02 : M[p-1]).
//
// --emit-pf additionally writes the parse-free slim's consumed sidecars as a
// side stream of the FINAL pass, eliminating the slim's post-merge walk:
//   PREFIX.pftext  the normalized text M (byte-identical to the walk's
//                  emitted sidecar; gated by G0 walk-parity)
//   PREFIX.pfck    PFCK header + tau-spaced suffix-hash checkpoints
//                  (bit6/ext_columns.hpp format; byte-identical to the
//                  slim's own backward pass — the same recurrence over the
//                  same bytes). tau = (8n + final_runs - 1) / final_runs.
// All emitters read virtual corpus text and WM through windows: nothing
// per-position is resident.
static void emit_pf_side_ext(const std::string& prefix, const CorpusText& m2, U n, U final_runs) {
    double last = now_sec();
    {
        OutFile t(prefix + ".pftext");
        constexpr U W = 1u << 22;
        std::vector<uint8_t> buf(W);
        for (U i = 0; i < n;) {
            U len = std::min<U>(W, n - i);
            m2.read(i, buf.data(), len);
            t.raw(buf.data(), len);
            i += len;
        }
        t.flush();
    }
    phase("emit-pf-text", last);
    U tau = std::max<U>(1, (8 * n + final_runs - 1) / final_runs);
    U count = n / tau + 1;
    std::string p = prefix + ".pfck";
    int fd = open(p.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
    require(fd >= 0, "create pfck (exists?)");
    auto wfull = [&](const void* d, U len, U off) {
        const char* q = (const char*)d;
        while (len) {
            ssize_t z = pwrite(fd, q, len, off);
            require(z > 0, "pfck write");
            q += z; off += U(z); len -= U(z);
        }
    };
    {
        char hdr[40];
        uint32_t magic = 0x4B434650;  // "PFCK"
        uint32_t ver = 1; uint64_t rsv = 0;
        memcpy(hdr, &magic, 4); memcpy(hdr + 4, &ver, 4);
        memcpy(hdr + 8, &n, 8); memcpy(hdr + 16, &tau, 8); memcpy(hdr + 24, &count, 8);
        memcpy(hdr + 32, &rsv, 8);
        wfull(hdr, 40, 0);
    }
    // Suffix hashes h(i) = (M2[i]+1) + HASH_BASE*h(i+1), folded backward;
    // checkpoint k covers T[k*tau..n). Blocks arrive in descending index
    // order; each block's entries are contiguous after reversing. The text
    // is read in backward bands from the M2 window.
    {
        constexpr U EMB = 1u << 22;   // entries per block
        constexpr U BW = 1u << 20;    // backward read band
        std::vector<U> ent;
        std::vector<uint8_t> band(BW);
        U h = 0;
        for (U hi = n; hi > 0; ) {
            U lo = hi > tau * EMB ? hi - tau * EMB : 0;
            ent.clear();
            U bh = hi;
            while (bh > lo) {
                U bl = bh > BW ? bh - BW : lo;
                m2.read(bl, band.data(), bh - bl);
                for (U i = bh; i-- > bl; ) {
                    h = (U)band[i - bl] + 1 + HASH_BASE * h;
                    if (i % tau == 0) ent.push_back(h);
                }
                bh = bl;
            }
            std::reverse(ent.begin(), ent.end());
            U firstIdx = (lo + tau - 1) / tau;
            if (!ent.empty()) wfull(ent.data(), 8 * ent.size(), 40 + 8 * firstIdx);
            hi = lo;
        }
        if (n % tau == 0) { U zero = 0; wfull(&zero, 8, 40 + 8 * (n / tau)); }
    }
    require(close(fd) == 0, "close pfck");
    struct stat st{}; require(fstat(open(p.c_str(), O_RDONLY), &st) == 0 && U(st.st_size) == 40 + 8 * count, "pfck size");
    fprintf(stderr, "CROSS_PF_EMIT text=%s pfck=%s n=%llu final_runs=%llu tau=%llu count=%llu bytes=%llu\n",
            (prefix + ".pftext").c_str(), p.c_str(), (unsigned long long)n,
            (unsigned long long)final_runs, (unsigned long long)tau, (unsigned long long)count,
            (unsigned long long)(40 + 8 * count));
    phase("emit-pf-checkpoints", last);
}

// ------------------------------------------------------------- v2 pieces
// PILE-BUILDER v2 emission (CROSS_V2_PIECES=1): the final dollar merge
// emits frag.rlebwt + .rlebwt.meta byte-identically (the ONLY pre-lite
// whole file) plus per-run-band PIECES carrying the ONE canonical packed
// copy of each SA column: {char u8, len u32, head SA packed w, tail SA packed
// w}, w = bits(n+9). NO .ssa, NO .ssa_t, NO pftext, NO pfck. Pieces are
// band-aligned sub-files rename-published as shards produce them, flow-
// controlled against the consumer via the .runcols.consumed sentinel (the
// consumer unlinks each piece and advances the sentinel), and deleted after
// the lite container publishes. Scratch only - never a durable artifact.
static bool v2_pieces_enabled() {
    const char* e = getenv("CROSS_V2_PIECES");
    return e && e[0] == '1' && e[1] == '\0';
}
static U v2_band_size() { return 65536; }
static U v2_piece_window() {
    const char* e = getenv("CROSS_V2_PIECE_WINDOW");
    U window = e ? U(strtoull(e, nullptr, 10)) : 1100;
    require(window > 0 && window <= U(INT64_MAX), "v2 piece window must fit positive signed range");
    return window;
}
// Signed -1 is the initial highest consumed band; no off-by-one credit.
static int64_t v2_consumed_band(const std::string& prefix) {
    int fd = ::open((prefix + ".runcols.consumed").c_str(), O_RDONLY);
    if (fd < 0) return -1;
    char b[24]{}; ssize_t z = ::read(fd, b, sizeof b - 1); ::close(fd);
    require(z > 0, "empty consumed marker");
    char* end = nullptr; long long value = strtoll(b, &end, 10);
    require(end != b && value >= -1, "malformed consumed marker");
    return value;
}
static void v2_flow_wait(const std::string& prefix, U band, U window) {
    for (;;) {
        int64_t consumed = v2_consumed_band(prefix);
        // Both operands are <= INT64_MAX, so their unsigned sum fits U.
        U limit = consumed < 0 ? window - 1 : U(consumed) + window;
        if (band <= limit) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
// One band sub-piece: header + chars + lens + heads packed + tails packed.
// One band piece file body: header + chars + lens + packed heads + packed
// tails, published by rename. A single definition serves both the serial sink
// and the v2 parallel band writers, so the two paths cannot diverge. The
// caller owns flow control (v2_flow_wait) so that it can also own its own
// writer-slot credit before committing to a band. Returns the packed-carry
// word count (0 for an empty column).
static U v2_write_piece(const std::string& dir, const std::string& prefix, U band,
                        U shard, U firstRun, const uint8_t* chars,
                        const uint32_t* lens, const U* heads, const U* tails,
                        U rc, unsigned w) {
    if (!rc) return 0;
    (void)prefix;
    std::string tmp = dir + "/t-" + std::to_string(band) + "-" + std::to_string(shard);
    std::string fin = dir + "/p-" + std::to_string(band) + "-" + std::to_string(shard);
    require(!fs::exists(fin), "piece exists; refusing to clobber");
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    require(fd >= 0, "create piece tmp");
    auto wfull = [&](const void* d, U len) {
        const char* q = (const char*)d;
        while (len) { ssize_t z = ::write(fd, q, len); require(z > 0, "piece write"); q += z; len -= U(z); }
    };
    char hdr[44];
    uint32_t magic = 0x31435250;  // "PRC1"
    memcpy(hdr, &magic, 4);
    memcpy(hdr + 4, &band, 8); memcpy(hdr + 12, &shard, 4);
    memcpy(hdr + 16, &firstRun, 8); memcpy(hdr + 24, &rc, 8);
    memcpy(hdr + 32, &w, 4); U zero = 0; memcpy(hdr + 36, &zero, 8);
    wfull(hdr, 44);
    wfull(chars, rc);
    wfull(lens, 4 * rc);
    // Pack heads/tails LSB-first (the ri4 sample convention); each piece
    // is self-contained, so the reservoir drains fully here.
    U bits = U(w) * rc, words = (bits + 63) / 64;
    std::vector<U> out(words ? words : 1, 0);
    auto pack = [&](const U* v) {
        __uint128_t res = 0; unsigned avail = 0; U wi = 0;
        for (U i = 0; i < rc; ++i) {
            res |= __uint128_t(v[i]) << avail; avail += w;
            if (avail >= 64) { out[wi++] = U(res); res >>= 64; avail -= 64; }
        }
        if (avail) out[wi] = U(res);
    };
    pack(heads);
    wfull(out.data(), 8 * words);
    pack(tails);
    wfull(out.data(), 8 * words);
    require(::close(fd) == 0, "close piece tmp");
    require(::rename(tmp.c_str(), fin.c_str()) == 0, "publish piece");
    return words;
}
struct V2Piece {
    std::vector<uint8_t> chars;
    std::vector<uint32_t> lens;
    std::vector<U> heads, tails;
    void add(uint8_t c, U len, U head, U tail) {
        chars.push_back(c); lens.push_back(uint32_t(len)); heads.push_back(head); tails.push_back(tail);
    }
    bool empty() const { return chars.empty(); }
    U size() const { return chars.size(); }
    void flush(const std::string& dir, const std::string& prefix, U band, U shard,
                U firstRun, unsigned w, U window) {
        U rc = chars.size();
        if (!rc) return;
        v2_flow_wait(prefix, band, window);
        v2_write_piece(dir, prefix, band, shard, firstRun, chars.data(), lens.data(),
                       heads.data(), tails.data(), rc, w);
        chars.clear(); lens.clear(); heads.clear(); tails.clear();
    }
};

// The final v2 path has one ordered producer attached directly to the heap.
// No row spools, counting walk, replay walk, shard bases, or RCI1 publication.
// BPG1 markers carry band references and newly completed SCS1 class entries.
struct SequentialV2 : SparseWalkSink {
    struct Class {
        CapturedSeamClass value;
        U firstRun = 0, lastRun = UINT64_MAX;
    };
    const CorpusText* text;
    SparseSeamCapture seam;
    std::unordered_map<U, U> firstSizes;
    std::vector<Class> classes;
    U active = UINT64_MAX, publishedClasses = 0;
    U n, index = 0, rows = 0, next = 0, bandStart = 0, admittedBand = UINT64_MAX;
    U window, width;
    std::string prefix, dir;
    OutFile bwt;
    V2Piece piece;
    SparseRun pending;
    std::array<U, 256> counts{}, frequencies{};
    SequentialV2(const std::string& p, const CorpusText& t, U count, U inputRuns)
        : text(&t), n(count), window(v2_piece_window()),
          width(std::max<unsigned>(1, 64 - __builtin_clzll(n + 9))),
          prefix(p), dir(p + ".pieces"), bwt(p + ".rlebwt") {
        require(::mkdir(dir.c_str(), 0777) == 0, "create sequential pieces directory");
        seam.discover(t, n, std::min<U>(1u << 26, std::max<U>(1000, inputRuns / 1000)));
        for (const auto& cl : seam.raw) {
            firstSizes.emplace(cl.first, cl.hi);
        }
        {
            OutFile f(dir + "/start.tmp"); f.raw("BPS1", 4);
            f.w64(n); f.w64(width); f.w64(seam.depth); f.w64(window);
        }
        require(::rename((dir + "/start.tmp").c_str(), (dir + "/start").c_str()) == 0,
                "publish band stream start");
        admitBand();
        add({t.at(n-1), 1, n, n}); add({2, 9, n+1, n+9});
    }
    void admitBand() {
        U band = index / 65536;
        if (band != admittedBand) { v2_flow_wait(prefix, band, window); admittedBand = band; }
    }
    U shardCount(U) const override { return 1; }
    void attachCorpus(const CorpusText& t) override { text = &t; }
    void beginShard(U shard, U lo, U hi) override {
        require(shard == 0 && lo == 0 && hi == n, "sequential v2 walk bounds");
    }
    void publishBand() {
        const U band = bandStart / 65536, count = piece.size();
        if (const char* delay = getenv("CROSS_V2_PRODUCER_DELAY_MS"))
            std::this_thread::sleep_for(std::chrono::milliseconds(strtoull(delay,nullptr,10)));
        require(count && count <= 65536, "sequential band count");
        if (active != UINT64_MAX) {
            // An unclosed class must include a run in the next band. Refuse
            // before flow control could block on a class larger than W-1.
            require(band + 1 - classes[active].firstRun / 65536 < window,
                    "active seam class exceeds W-1 run bands");
        }
        piece.flush(dir, prefix, band, 0, bandStart, width, window);
        std::vector<U> refs;
        for (U id = 0; id < classes.size(); ++id) {
            const auto& cl = classes[id];
            if (cl.firstRun < bandStart + count &&
                (cl.lastRun == UINT64_MAX || cl.lastRun >= bandStart)) refs.push_back(id);
        }
        U complete = classes.size() - (active != UINT64_MAX);
        {
            OutFile f(dir + "/g-tmp"); f.raw("BPG1", 4);
            f.w64(band); f.w64(count); f.w64(refs.size()); f.w64(complete - publishedClasses);
            for (U id : refs) f.w64(id);
            for (U id = publishedClasses; id < complete; ++id) {
                const auto& cl = classes[id]; const auto& v = cl.value;
                f.w64(id); f.w64(cl.firstRun); f.w64(cl.lastRun);
                f.w64(v.lo); f.w64(v.hi); f.w64(v.first); f.w64(v.before); f.w64(v.after);
                for (U position : v.occ) f.w64(position);
            }
        }
        require(::rename((dir + "/g-tmp").c_str(),
                         (dir + "/g-" + std::to_string(band)).c_str()) == 0, "publish band progress");
        publishedClasses = complete;
        bandStart = index;
    }
    void flush() {
        if (!pending.len) return;
        U left = pending.len;
        while (left) {
            U take = std::min<U>(left, 0x7fffff); left -= take;
            bwt.w32(uint32_t(pending.c) | uint32_t(take << 8) | (left ? 0x80000000u : 0u));
        }
        counts[pending.c] += pending.len; ++frequencies[pending.c];
        piece.add(pending.c, pending.len, pending.head, pending.tail); ++index;
        pending = {};
        if (index % 65536 == 0) publishBand();
    }
    void add(const SparseRun& run) {
        if (pending.len && pending.c != run.c) flush();
        admitBand();
        if (!pending.len) pending = run;
        else { pending.len += run.len; pending.tail = run.tail; }
        rows += run.len;
    }
    void row(U row, U p) override {
        require(row == next++ && p < n, "sequential v2 row coverage");
        seam.row(row, p);
        if (active != UINT64_MAX && row == classes[active].value.hi) {
            auto& cl = classes[active]; cl.value.after = p; cl.lastRun = index;
            active = UINT64_MAX;
        }
        uint8_t c = p ? text->at(p-1) : uint8_t(2);
        if (pending.len && pending.c != c) flush();
        admitBand();
        auto found = firstSizes.find(p);
        if (found != firstSizes.end()) {
            if (active == UINT64_MAX) {
                Class cl; cl.firstRun = index;
                cl.value.lo = row; cl.value.hi = row + found->second; cl.value.first = p;
                cl.value.before = seam.targets.at(p).before;
                classes.push_back(std::move(cl)); active = classes.size()-1;
            } else require(row + found->second <= classes[active].value.hi, "crossing sequential seam class");
        }
        if (active != UINT64_MAX) classes[active].value.occ.push_back(p);
        if (!pending.len) pending = {c, 1, p, p};
        else { ++pending.len; pending.tail = p; }
        ++rows;
    }
    // A spool segment contains no target in its interior. Target rows are
    // always singleton records; preserving head/tail also preserves both
    // flanks when a class or a BWT run crosses any number of producer seams.
    void segment(const SparseRun& run) {
        require(run.len && run.len <= n - next, "parallel segment bounds");
        U start = next;
        row(start, run.head);
        require(pending.c == run.c, "parallel segment character");
        if (run.len == 1) {
            require(run.head == run.tail, "singleton segment endpoints");
            return;
        }
        require(active == UINT64_MAX && seam.previousHit == nullptr,
                "target rows must be singleton segments");
        next += run.len - 1; rows += run.len - 1;
        seam.nextRow += run.len - 1; seam.previous = run.tail;
        pending.len += run.len - 1; pending.tail = run.tail;
    }
    void endShard() override {
        require(next == n, "incomplete sequential v2 walk");
        if (active != UINT64_MAX) {
            auto& cl = classes[active]; require(cl.value.hi == n, "incomplete final class");
            cl.value.after = n; cl.lastRun = index; active = UINT64_MAX;
        }
        // Validate every target and both final flanks before terminal publish.
        auto expected = seam.finish();
        require(expected.size() == classes.size(), "sequential class count");
        for (U i = 0; i < expected.size(); ++i)
            require(expected[i] == classes[i].value, "sequential class capture mismatch");
        flush();
        if (!piece.empty()) publishBand();
        require(publishedClasses == classes.size() && rows == n+10 && counts[2] == 10,
                "sequential terminal coverage");
        const U limit = std::max<U>(1000, index / 1000);
        U targets = 0; for (const auto& cl : classes) targets += cl.value.occ.size();
        require(targets <= limit && seam.depth <= limit, "sequential product repair budget");
        for (const auto& cl : classes)
            require(cl.lastRun / 65536 - cl.firstRun / 65536 < window, "class exceeds W-1 bands");
        bwt.flush();
        { OutFile f(prefix + ".rlebwt.meta"); f.w64(n+10); f.w64(index);
          for (U value : counts) f.w64(value); for (U value : frequencies) f.w64(value); }
        {
            OutFile f(dir + "/terminal.tmp"); f.raw("BPT1", 4);
            f.w64((index-1)/65536); f.w64(index); f.w64(targets); f.w64(classes.size());
        }
        require(::rename((dir + "/terminal.tmp").c_str(), (dir + "/terminal").c_str()) == 0,
                "publish validated terminal marker");
    }
};
// Independent single-pass producers append immutable compressed segments.
// The ordered reducer reads committed prefixes before producer completion;
// only it owns SequentialV2, canonical run-band numbering and terminal EOF.
// RAM is O(P*4096) records plus existing decoded fronts. Physical spool
// scratch is credit-bounded, punched on consumption, and unlinked by range.
// This is not persistent crash recovery: process failure refuses completion.
// Shared byte credits retain one aligned batch exclusively for the current
// ordered range. Later producers can fill the remaining budget but cannot
// prevent its producer from progressing. Extents are punched before credits
// return. No text is written to these files.
// Independent fixed-address blocks allow variable-size segment payloads
// without a growing in-memory offset index. Gaps allocate no data pages.
static constexpr U spoolSlot = 106496; // page-rounded 8 + 4096*25
static U spoolRead64(const char* b) {
    U v=0; for(unsigned j=0;j<8;++j) v|=U(uint8_t(b[j]))<<(8*j); return v;
}
static std::vector<char> spoolEncode(const std::vector<char>& raw, unsigned width) {
    std::vector<char> packed; packed.reserve(raw.size()+32);
    for (size_t i=0;i<raw.size();i+=25) {
        const char* b=raw.data()+i; U len=spoolRead64(b+1);
        packed.push_back(b[0]);
        U x=len; do { packed.push_back(char((x&127) | (x>=128 ? 128 : 0))); x>>=7; } while(x);
        for(unsigned j=0;j<width;++j) packed.push_back(b[9+j]);
        if(len>1) for(unsigned j=0;j<width;++j) packed.push_back(b[17+j]);
        if(packed.size()>=raw.size()) break;
    }
    bool compressed=packed.size()<raw.size();
    const auto& payload=compressed ? packed : raw;
    std::vector<char> block(8);
    uint32_t bytes=payload.size(), records=uint32_t(raw.size()/25) | (compressed ? 0x80000000u : 0);
    for(unsigned j=0;j<4;++j) { block[j]=char(bytes>>(8*j)); block[4+j]=char(records>>(8*j)); }
    block.insert(block.end(),payload.begin(),payload.end()); return block;
}
static void spoolDecode(const std::vector<char>& block, U take, unsigned width, std::vector<char>& raw) {
    uint32_t bytes=0, flags=0;
    for(unsigned j=0;j<4;++j) { bytes|=uint32_t(uint8_t(block[j]))<<(8*j); flags|=uint32_t(uint8_t(block[4+j]))<<(8*j); }
    require(bytes+8==block.size() && (flags&0x7fffffffu)==take, "spool block header");
    if(!(flags&0x80000000u)) { require(bytes==25*take,"raw spool block size"); raw.assign(block.begin()+8,block.end()); return; }
    raw.assign(25*take,0); size_t cursor=8;
    for(U i=0;i<take;++i) {
        char* b=raw.data()+25*i;
        require(cursor<block.size(),"packed spool character"); b[0]=block[cursor++];
        U len=0; unsigned shift=0;
        for(;;) {
            require(cursor<block.size() && shift<64,"packed spool length");
            uint8_t byte=uint8_t(block[cursor++]);
            require(shift<63 || (byte&127)<=1,"packed spool length overflow");
            len|=U(byte&127)<<shift; if(!(byte&128)) break; shift+=7;
        }
        require(len>0 && cursor+width*(len>1 ? 2 : 1)<=block.size(),"packed spool endpoints");
        put_le64(b+1,len);
        for(unsigned j=0;j<width;++j) b[9+j]=block[cursor++];
        if(len>1) for(unsigned j=0;j<width;++j) b[17+j]=block[cursor++];
        else std::memcpy(b+17,b+9,8);
    }
    require(cursor==block.size(),"packed spool trailing data");
}
static U v2_spool_budget(U n, U p) {
    const char* e = getenv("CROSS_V2_SPOOL_BYTES");
    if (e) require(*e && std::string(e).find_first_not_of("0123456789") == std::string::npos, "CROSS_V2_SPOOL_BYTES must be an integer");
    U bytes = e ? std::stoull(e) : std::max<U>(p*spoolSlot, n > UINT64_MAX/2 ? UINT64_MAX : 2*n);
    require(bytes >= p*spoolSlot, "CROSS_V2_SPOOL_BYTES must be >= P*106496");
    return bytes;
}
struct ParallelV2 : SparseWalkSink {
    SequentialV2& output;
    U count, quota, occupied = 0, activeProducer = 0;
    unsigned positionBytes;
    std::mutex mu;
    std::condition_variable cv;
    std::thread reducer;
    struct Producer : SparseWalkSink {
        ParallelV2& parent;
        U id, lo = 0, hi = 0, next = 0, committed = 0, reclaimed = 0, blocks = 0;
        double blockedSeconds = 0;
        bool started = false, done = false;
        int fd = -1;
        std::string path;
        SparseRun pending;
        std::vector<char> buffer;
        Producer(ParallelV2& p, U i) : parent(p), id(i),
            path(p.output.dir + "/walk-" + std::to_string(i)) { buffer.reserve(25*4096); }
        void drain() {
            if (buffer.empty()) return;
            auto block = spoolEncode(buffer,parent.positionBytes);
            U base, charge = (block.size()+4095)/4096*4096;
            { std::unique_lock<std::mutex> lock(parent.mu);
              auto ready = [&] { return parent.occupied + charge <= parent.quota - (id == parent.activeProducer ? 0 : spoolSlot); };
              if (!ready()) {
                  ++blocks; auto start = std::chrono::steady_clock::now();
                  parent.cv.wait(lock, ready);
                  blockedSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
              }
              base = committed; parent.occupied += charge; }
            // XFS speculative EOF preallocation can otherwise allocate far
            // beyond the credited write. Explicitly allocate the charged
            // extent first, so pwrite only converts existing unwritten pages.
            require(::fallocate(fd, 0, (base/4096)*spoolSlot, charge) == 0,
                    "allocate credited parallel spool extent");
            xput(fd, block.data(), block.size(), (base/4096)*spoolSlot, "parallel spool write");
            { std::lock_guard<std::mutex> lock(parent.mu); committed += buffer.size()/25; }
            buffer.clear(); parent.cv.notify_all();
        }
        void flush() {
            if (!pending.len) return;
            char b[25]; b[0] = char(pending.c);
            put_le64(b+1,pending.len); put_le64(b+9,pending.head); put_le64(b+17,pending.tail);
            buffer.insert(buffer.end(), b, b+25); pending = {};
            if (buffer.size() == 25*4096) drain();
        }
        void beginShard(U shard, U begin, U end) override {
            require(shard == id && begin <= end && end <= parent.output.n, "parallel producer bounds");
            lo = next = begin; hi = end;
            fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            require(fd >= 0, "create parallel spool");
            { std::lock_guard<std::mutex> lock(parent.mu); started = true; }
            parent.cv.notify_all();
        }
        void row(U rank, U p) override {
            require(rank == next++ && rank < hi && p < parent.output.n, "parallel producer row coverage");
            // Keys only: the reducer mutates Hit values, never this map's
            // structure. Concurrent const key lookup does not read a Hit.
            const auto& targets = parent.output.seam.targets;
            bool target = targets.find(p) != targets.end();
            uint8_t c = p ? parent.output.text->at(p-1) : uint8_t(2);
            if (target || (pending.len && pending.c != c)) flush();
            if (!pending.len) pending = {c, 0, p, p};
            ++pending.len; pending.tail = p;
            if (target) flush();
        }
        void endShard() override {
            require(next == hi, "parallel producer completion");
            flush(); drain();
            { std::lock_guard<std::mutex> lock(parent.mu); done = true; }
            parent.cv.notify_all();
        }
    };
    std::vector<std::unique_ptr<Producer>> producers;
    ParallelV2(SequentialV2& o, U p) : output(o), count(p),
        quota(v2_spool_budget(o.n,p)/spoolSlot*spoolSlot),
        positionBytes(((64-__builtin_clzll(o.n-1 ? o.n-1 : 1))+7)/8) {
        for (U i = 0; i < p; ++i) producers.emplace_back(new Producer(*this,i));
    }
    U shardCount(U) const override { return count; }
    U admission() const override { return count; }
    SparseWalkSink* shardSink(U i) override { return producers.at(i).get(); }
    void beginShard(U, U, U) override { fail("parallel parent begin"); }
    void row(U, U) override { fail("parallel parent row"); }
    void endShard() override { fail("parallel parent end"); }
    void attachCorpus(const CorpusText& corpus) override {
        output.attachCorpus(corpus);
        reducer = std::thread([this] {
            output.beginShard(0,0,output.n);
            std::vector<char> buffer(25*4096), block(spoolSlot);
            auto read64 = [](const char* b) { U v=0; for(unsigned j=0;j<8;++j) v|=U(uint8_t(b[j]))<<(8*j); return v; };
            for (auto& pointer : producers) {
                auto& p = *pointer;
                { std::lock_guard<std::mutex> lock(mu); activeProducer = p.id; }
                cv.notify_all();
                U records = 0, row = 0;
                for (;;) {
                    U available; bool done;
                    {
                        std::unique_lock<std::mutex> lock(mu);
                        cv.wait(lock, [&] { return p.started && (p.committed > records || p.done); });
                        available = p.committed; done = p.done;
                    }
                    if (!records) { require(p.lo == output.next, "parallel range adjacency"); row = p.lo; }
                    while (records < available) {
                        U take = std::min<U>(4096, available-records), offset=(records/4096)*spoolSlot;
                        auto readFull = [&](char* data, U bytes, U start) {
                            U read=0;
                            while(read<bytes) {
                                ssize_t z=::pread(p.fd,data+read,bytes-read,start+read);
                                if(z<0 && errno==EINTR) continue;
                                require(z>0,"parallel spool read"); read+=U(z);
                            }
                        };
                        block.resize(8); readFull(block.data(),8,offset);
                        U payload=0; for(unsigned j=0;j<4;++j) payload|=U(uint8_t(block[j]))<<(8*j);
                        require(payload<=25*take,"spool payload bound");
                        block.resize(8+payload); readFull(block.data()+8,payload,offset+8);
                        spoolDecode(block,take,positionBytes,buffer);
                        U charge=(block.size()+4095)/4096*4096;
                        for (U j=0;j<take;++j) {
                            const char* b=buffer.data()+25*j;
                            SparseRun run{uint8_t(b[0]),read64(b+1),read64(b+9),read64(b+17)};
                            require(run.len <= p.hi-row, "parallel spool range coverage");
                            output.segment(run); row += run.len;
                        }
                        // All writes except final EOF are full aligned batches. Punch
                        // only consumed extents; the last partial batch has no successor.
                        require(::fallocate(p.fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                            offset, charge) == 0,
                                "reclaim parallel spool extents (requires hole punching)");
                        records += take;
                        { std::lock_guard<std::mutex> lock(mu); p.reclaimed = records; occupied -= charge; }
                        cv.notify_all();
                    }
                    if (done) {
                        require(row == p.hi && output.next == p.hi, "parallel reducer completion");
                        require(::close(p.fd) == 0 && ::unlink(p.path.c_str()) == 0, "parallel spool consume");
                        p.fd = -1; break;
                    }
                }
            }
            output.endShard();
        });
    }
    void finishWalk() override {
        require(reducer.joinable(), "parallel reducer started"); reducer.join();
        for (const auto& p : producers)
            std::fprintf(stderr, "CROSS_SPOOL_STALL producer=%llu blocks=%llu seconds=%.6f quota_bytes=%llu\n",
                (unsigned long long)p->id, (unsigned long long)p->blocks, p->blockedSeconds,
                (unsigned long long)quota);
    }
};
static U v2_producers(U n) {
    const char* e = getenv("CROSS_V2_PRODUCERS");
    U p = e ? std::stoull(e) : 1;
    require(p >= 1 && p <= 4096, "CROSS_V2_PRODUCERS out of range");
    return std::min(p,n);
}
static U emit_sparse_v2(const std::string& prefix, const std::vector<SideExt>& sides,
                        U threads, Budget& budget, CoreStats& stats,
                        const PairScratch& sc, U hashK, bool pre) {
    U n = 0, inputRuns = 0; for (const auto& side : sides) { n += side.n; inputRuns += side.runs; }
    CorpusText corpus; open_merge_text(sides, corpus, false);
    SequentialV2 output(prefix, corpus, n, inputRuns);
    const U producers = v2_producers(n);
    std::fprintf(stderr, "CROSS_PRODUCER_RESOURCES P=%llu decoded_front_bytes_max=%llu segment_buffer_bytes=%llu spool_bytes_bound=%llu\n",
        (unsigned long long)producers, (unsigned long long)(producers*sides.size()*4096*8),
        (unsigned long long)(producers*(3*4096*25+64)), (unsigned long long)(producers>1 ? v2_spool_budget(n,producers) : 0));
    if (producers == 1)
        core_merge_kway(sides, true, threads, budget, stats, sc, hashK, pre, &output);
    else {
        ParallelV2 parallel(output, producers);
        core_merge_kway(sides, true, threads, budget, stats, sc, hashK, pre, &parallel);
    }
    sc.release_walk();
    std::error_code cleanupError;
    fs::remove(output.dir, cleanupError); // consumer removes it after its last unlink
    std::fprintf(stderr, "SPARSE_V2_EMIT runs=%llu rows=%llu shards=0 WM=absent admission=%llu walk_shards=%llu passes=1 preparation_passes=1 planning_passes=1\n",
        (unsigned long long)output.index, (unsigned long long)(n+10),
        (unsigned long long)producers, (unsigned long long)producers);
    return output.index;
}

// Final four files, read from virtual corpus text and windowed WM.
// PARALLEL EMISSION: both walks are sharded at fixed run boundaries (the
// same combiner arithmetic as the SXCR emitter; rows are the n+10 rows of
// the padded order, pad rows included). The fixed-size outputs go out by
// pwrite at global offsets (.meta entirely from the combiner; .ssa/.ssa_t
// 8 bytes per run at 8*(1+runIndex), buffered per shard), and the
// variable-word .rlebwt goes through per-shard segment files concatenated
// in shard order. Every run is finalized by exactly one shard, so all four
// files are byte-identical to the serial writer by construction.
static U emit_four_ext(const std::string& prefix, const CorpusText& m2,
                       const WinU64& wm, U n, U threads, const std::string& segDir) {
    double last = now_sec();
    U total = n + 10;
    auto row_char_sample = [&](U t, uint8_t& c, U& sample) {
        if (t < 10) { sample = n + t; c = t == 0 ? m2.at(n - 1) : uint8_t(2); }
        else { U p = wm.at(t - 10); sample = p; c = p == 0 ? uint8_t(2) : m2.at(p - 1); }
    };
    const U S = emit_shard_count(threads, total);
    auto pfRows = [&](U t0, U t1) {   // rows [t0,t1): row t reads M[WM[t-10]-1]
        if (!m2.pool) return;
        for (U t = std::max<U>(10, t0); t < t1; ++t) {
            U p = wm.at(t - 10);
            if (p) m2.prefetch(p - 1, 1);
        }
    };
    struct CountInfo { std::array<U, 256> counts{}, rc{}; U internalStarts = 0; bool firstIsStart = false; };
    std::vector<CountInfo> info(S);
    {   // pass 1: sharded count (chars + run starts per row-range shard)
        std::vector<std::thread> ts;
        for (U s = 0; s < S; ++s) {
            U t0 = s * total / S, t1 = (s + 1) * total / S;
            if (t1 <= t0) continue;
            ts.emplace_back([&, s, t0, t1] {
                CountInfo& ci = info[s];
                pfRows(t0, std::min<U>(t1, t0 + 4096));
                U pfNext = t0 + 4096;
                uint8_t prevC; U prevS;
                row_char_sample(t0, prevC, prevS);
                ++ci.counts[prevC];
                ci.firstIsStart = true;
                if (t0) {
                    uint8_t pc; U ps;
                    row_char_sample(t0 - 1, pc, ps);
                    ci.firstIsStart = (prevC != pc);
                }
                if (ci.firstIsStart) ++ci.rc[prevC];
                for (U t = t0 + 1; t < t1; ++t) {
                    if (t >= pfNext) { pfRows(t, std::min<U>(t1, t + 4096)); pfNext = t + 4096; }
                    uint8_t c; U smp;
                    row_char_sample(t, c, smp);
                    ++ci.counts[c];
                    if (c != prevC) { ++ci.internalStarts; ++ci.rc[c]; }
                    prevC = c;
                }
            });
        }
        for (auto& t : ts) t.join();
    }
    std::array<U, 256> counts{}, rc{};
    U final_runs = 0;
    std::vector<U> base(S, 0);   // global run index of the first start >= t0_s
    for (U s = 0; s < S; ++s) {
        base[s] = final_runs;
        final_runs += info[s].internalStarts + (info[s].firstIsStart ? 1u : 0u);
        for (unsigned c = 0; c < 256; ++c) { counts[c] += info[s].counts[c]; rc[c] += info[s].rc[c]; }
    }
    require(final_runs >= 1, "final run count underflow");
    require(counts[2] == 10, "padding count mismatch");
    phase("emit-count", last);
    const bool v2 = v2_pieces_enabled();
    const U v2B = v2_band_size();
    const U v2W = v2_piece_window();
    const unsigned v2Bits = std::max<unsigned>(1, 64 - __builtin_clzll(n + 9));
    std::string v2Dir;
    if (v2) {   // the run-columns index: S shards, per-shard global run base,
        // n, sample width, band size. Written BEFORE emission so pile_finish
        // can start (PFCK build from the corpus) while shards still emit.
        v2Dir = prefix + ".pieces";
        require(::mkdir(v2Dir.c_str(), 0777) == 0 || errno == EEXIST, "create pieces dir");
        require(!fs::exists(prefix + ".runcols.idx"), "runcols idx exists; refusing to clobber");
        std::string tmp = prefix + ".runcols.idx.tmp";
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        require(fd >= 0, "create runcols idx");
        auto wfull = [&](const void* d, U len) {
            const char* q = (const char*)d;
            while (len) { ssize_t z = ::write(fd, q, len); require(z > 0, "idx write"); q += z; len -= U(z); }
        };
        char hdr[32];
        uint32_t magic = 0x31494352;  // "RCI1"
        memcpy(hdr, &magic, 4); memcpy(hdr + 4, &S, 4);
        memcpy(hdr + 8, &final_runs, 8); memcpy(hdr + 16, &n, 8);
        memcpy(hdr + 24, &v2Bits, 4); memcpy(hdr + 28, &v2B, 4);
        wfull(hdr, 32);
        for (U s = 0; s < S; ++s) {
            char row[16];
            U cnt = info[s].internalStarts + (info[s].firstIsStart ? 1u : 0u);
            memcpy(row, &base[s], 8); memcpy(row + 8, &cnt, 8);
            wfull(row, 16);
        }
        require(::close(fd) == 0, "close idx");
        require(::rename(tmp.c_str(), (prefix + ".runcols.idx").c_str()) == 0, "publish idx");
        std::fprintf(stderr, "CROSS_V2_IDX path=%s shards=%llu final_runs=%llu n=%llu w=%u band=%llu window=%llu\n",
                     (prefix + ".runcols.idx").c_str(), (unsigned long long)S,
                     (unsigned long long)final_runs, (unsigned long long)n,
                     v2Bits, (unsigned long long)v2B, (unsigned long long)v2W);
        phase("emit-v2-idx", last);
    }
    {
        OutFile b(prefix + ".rlebwt"), m(prefix + ".rlebwt.meta");
        m.w64(total); m.w64(final_runs);
        for (U c : counts) m.w64(c);
        for (U c : rc) m.w64(c);
        m.flush();
        auto openX = [&](const std::string& p) {
            int f = ::open(p.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
            require(f >= 0, "create output (exists?)");
            return f;
        };
        int hFd = -1, tFd = -1;
        if (!v2) {
            hFd = openX(prefix + ".ssa"), tFd = openX(prefix + ".ssa_t");
            {
                char hb[8];
                put_le64(hb, final_runs);
                xput(hFd, hb, 8, 0, "ssa header write");
                xput(tFd, hb, 8, 0, "ssa_t header write");
            }
        }
        std::vector<int> segFd(S, -1);
        std::vector<std::string> segPath(S);
        for (U s = 0; s < S; ++s) {
            segPath[s] = segDir + "/rlebwt-seg-" + std::to_string(s);
            segFd[s] = ::open(segPath[s].c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
            require(segFd[s] >= 0, "create rlebwt segment");
        }
        std::vector<std::thread> ts;
        for (U s = 0; s < S; ++s) {
            U t0 = s * total / S, t1 = (s + 1) * total / S;
            if (t1 <= t0) continue;
            U b0 = base[s];
            bool firstIsStart = info[s].firstIsStart;
            ts.emplace_back([&, s, t0, t1, b0, firstIsStart] {
                U t = t0;
                if (!firstIsStart) {  // skip the run continued from the previous shard
                    uint8_t c; U smp;
                    row_char_sample(t, c, smp);
                    for (++t; t < total; ) {
                        uint8_t c2; U s2;
                        row_char_sample(t, c2, s2);
                        if (c2 != c) break;
                        ++t;
                    }
                }
                std::vector<char> rec; rec.reserve(4 * 65536);
                U segOff = 0;
                auto flushRec = [&]() {
                    if (!rec.empty()) {
                        xput(segFd[s], rec.data(), rec.size(), segOff, "rlebwt segment write");
                        segOff += rec.size(); rec.clear();
                    }
                };
                auto emitWords = [&](uint8_t c, U len) {
                    U left = len;
                    while (left) {
                        U take = std::min<U>(left, 0x7fffff); left -= take;
                        char w[4];
                        put_le32(w, uint32_t(c) | uint32_t(take << 8) | (left ? 0x80000000u : 0u));
                        rec.insert(rec.end(), w, w + 4);
                    }
                    if (rec.size() >= 4 * 65536) flushRec();
                };
                std::vector<U> headBuf, tailBuf;
                headBuf.reserve(65536); tailBuf.reserve(65536);
                U bufIdx = b0;
                auto flushSamples = [&]() {
                    if (!headBuf.empty()) {
                        xput(hFd, headBuf.data(), 8 * headBuf.size(), 8 * (1 + bufIdx), "ssa write");
                        xput(tFd, tailBuf.data(), 8 * tailBuf.size(), 8 * (1 + bufIdx), "ssa_t write");
                        bufIdx += headBuf.size();
                        headBuf.clear(); tailBuf.clear();
                    }
                };
                U cnt = info[s].internalStarts + (firstIsStart ? 1u : 0u);
                U shardEnd = b0 + cnt;
                // v2: band-aligned piece accumulator. A piece is flushed ONLY
                // at a band boundary or the shard's end, so every piece
                // covers a sub-range of one band: [pieceStart, pieceEnd),
                // pieceEnd % B == 0 or pieceEnd == shardEnd.
                V2Piece piece;
                U pieceStart = b0;
                auto v2Emit = [&](uint8_t c, U len, U head, U tail) {
                    piece.add(c, len, head, tail);
                    U pieceEnd = pieceStart + piece.size();
                    if (pieceEnd % v2B == 0 || pieceEnd == shardEnd) {
                        piece.flush(v2Dir, prefix, pieceStart / v2B, s, pieceStart, v2Bits, v2W);
                        pieceStart = pieceEnd;
                    }
                };
                U pfAt = t;
                auto pfAhead = [&](U cur) {
                    if (m2.pool && cur >= pfAt) { pfRows(cur, std::min<U>(total, cur + 8192)); pfAt = cur + 8192; }
                };
                pfAhead(t);
                U runIdx = b0;
                while (t < t1) {
                    uint8_t c; U head;
                    row_char_sample(t, c, head);
                    U e = t + 1;
                    for (;;) {
                        if (e >= total) break;
                        if (e >= pfAt) pfAhead(e);
                        uint8_t c2; U s2;
                        row_char_sample(e, c2, s2);
                        if (c2 != c) break;
                        ++e;
                    }
                    emitWords(c, e - t);
                    uint8_t tc; U tailSmp;
                    row_char_sample(e - 1, tc, tailSmp);
                    if (v2) {
                        v2Emit(c, e - t, head, tailSmp);
                    } else {
                        headBuf.push_back(head);
                        tailBuf.push_back(tailSmp);
                        if (headBuf.size() >= 65536) flushSamples();
                    }
                    ++runIdx;
                    t = e;
                }
                flushRec();
                if (!v2) flushSamples();
                else require(piece.empty(), "v2 emission left an unflushed piece");
                require(runIdx == b0 + info[s].internalStarts + (firstIsStart ? 1u : 0u),
                        "emit shard run coverage");
            });
        }
        for (auto& t : ts) t.join();
        for (U s = 0; s < S; ++s) require(::close(segFd[s]) == 0, "close rlebwt segment");
        // ordered concatenation: shard order is run order (order-pure)
        constexpr U CW = 1u << 20;
        std::vector<char> cbuf(CW);
        for (U s = 0; s < S; ++s) {
            int in = ::open(segPath[s].c_str(), O_RDONLY);
            require(in >= 0, "reopen rlebwt segment");
            for (;;) {
                ssize_t z = ::read(in, cbuf.data(), CW);
                require(z >= 0, "read rlebwt segment");
                if (z == 0) break;
                b.raw(cbuf.data(), size_t(z));
            }
            require(::close(in) == 0, "close rlebwt segment read");
            ::unlink(segPath[s].c_str());
        }
        b.flush();
        if (!v2) require(::close(hFd) == 0 && ::close(tFd) == 0, "close ssa outputs");
    }
    std::fprintf(stderr, "FOUR_EMIT shards=%llu runs=%llu total=%llu v2=%d\n",
                 (unsigned long long)S, (unsigned long long)final_runs, (unsigned long long)total, int(v2));
    phase("emit-write", last);
    return final_runs;
}

// Intermediate: merged cyclic order as an SXCR chunk + sidecar, read from
// virtual corpus text and the WM scratch file. PARALLEL EMISSION: both walks are sharded
// at fixed run boundaries. Pass 1 counts runs per row-range shard (internal
// starts plus the shard's own first-row start flag); the combiner's prefix
// sums give the global run count and every shard's global start-index base.
// Pass 2 emits the fixed 25-byte records by pwrite at 25*globalRunIndex:
// every run is finalized by exactly one shard (the one containing its start
// row; a run straddling the shard end is scanned to its true end), so the
// chunk is byte-identical to the serial writer by construction.
static U emit_sxcr_ext(const std::string& path, const CorpusText& m2,
                       const WinU64& wm, U n, U offset, U threads,
                       const std::vector<SideExt>& inSides, const std::string& segDir) {
    double last = now_sec();
    const U S = emit_shard_count(threads, n);
    auto charAt = [&](U t) { return m2.at(wm.at(t) + n - 1); };
    struct CountInfo { U internalStarts = 0; bool firstIsStart = false; };
    std::vector<CountInfo> info(S);
    auto pfRows = [&](U t0, U t1) {   // enqueue the M2 pages rows [t0,t1) read
        if (!m2.pool) return;
        for (U t = t0; t < t1; ++t) m2.prefetch(wm.at(t) + n - 1, 1);
    };
    {   // pass 1: sharded run count
        std::vector<std::thread> ts;
        for (U s = 0; s < S; ++s) {
            U t0 = s * n / S, t1 = (s + 1) * n / S;
            if (t1 <= t0) continue;
            ts.emplace_back([&, s, t0, t1] {
                CountInfo& ci = info[s];
                pfRows(t0, std::min<U>(t1, t0 + 4096));
                U pfNext = t0 + 4096;
                uint8_t prev = charAt(t0);
                ci.firstIsStart = (t0 == 0);
                if (t0) ci.firstIsStart = (prev != charAt(t0 - 1));
                for (U t = t0 + 1; t < t1; ++t) {
                    if (t >= pfNext) { pfRows(t, std::min<U>(t1, t + 4096)); pfNext = t + 4096; }
                    uint8_t c = charAt(t);
                    if (c != prev) ++ci.internalStarts;
                    prev = c;
                }
            });
        }
        for (auto& t : ts) t.join();
    }
    U runs = 0;
    std::vector<U> base(S, 0);   // global run index of the first start >= t0_s
    for (U s = 0; s < S; ++s) {
        base[s] = runs;
        runs += info[s].internalStarts + (info[s].firstIsStart ? 1u : 0u);
    }
    require(runs >= 1, "sxcr run count underflow");
    phase("sxcr-count", last);
    {   // pass 2: sharded record emission. v2 (legacy .sxs set): fixed
        // 25-byte records, pwrite at 25 * global run index. v3 (persisted
        // set - the order column carries the samples): compact c+varint-len
        // records via per-shard segments concatenated in shard order.
        bool persistEmit = !inSides.empty() && !getenv("CROSS_NO_PERSIST_EMIT");
        U emitVer = 2;
        for (auto& S : inSides)
            persistEmit = persistEmit && S.refText && !S.segs.empty();
        if (persistEmit) emitVer = 3;
        int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
        require(fd >= 0, "create SXCR chunk (exists?)");
        char hdr[32];
        std::memcpy(hdr, "SXCR", 4);
        put_le32(hdr + 4, uint32_t(emitVer)); put_le64(hdr + 8, offset); put_le64(hdr + 16, n); put_le64(hdr + 24, runs);
        xput(fd, hdr, 32, 0, "sxcr header write");
        std::vector<std::thread> ts;
        std::vector<int> segFds;
        std::vector<std::string> segPaths;
        U v3Off = 32;   // byte offset of the concatenated v3 stream
        for (U s = 0; s < S; ++s) {
            U t0 = s * n / S, t1 = (s + 1) * n / S;
            if (t1 <= t0) continue;
            U b0 = base[s];
            bool firstIsStart = info[s].firstIsStart;
            std::string segPath;
            int segFd = -1;
            if (emitVer == 3) {   // compact records: per-shard segment files
                segPath = segDir + "/sxcr-seg-" + std::to_string(s);
                segFd = ::open(segPath.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
                require(segFd >= 0, "create sxcr v3 segment");
            }
            ts.emplace_back([&, s, t0, t1, b0, firstIsStart, segFd] {
                U t = t0;
                if (!firstIsStart) {  // skip the run continued from the previous shard
                    uint8_t c = charAt(t);
                    for (++t; t < n && charAt(t) == c; ++t) {}
                }
                std::vector<char> rec;
                U bufIdx = b0;        // global run index of rec[0] (v2)
                U recOff = 0;         // segment offset (v3)
                auto flushRec = [&]() {
                    if (!rec.empty()) {
                        if (segFd >= 0) {
                            xput(segFd, rec.data(), rec.size(), recOff, "sxcr v3 segment write");
                            recOff += rec.size(); rec.clear();
                        } else {
                            xput(fd, rec.data(), rec.size(), 32 + 25 * bufIdx, "sxcr record write");
                            bufIdx += rec.size() / 25; rec.clear();
                        }
                    }
                };
                U pfAt = t;
                auto pfAhead = [&](U cur) {
                    if (m2.pool && cur >= pfAt) { pfRows(cur, std::min<U>(n, cur + 8192)); pfAt = cur + 8192; }
                };
                pfAhead(t);
                U runIdx = b0;
                while (t < t1) {
                    uint8_t c = charAt(t);
                    U head = wm.at(t);
                    U e = t + 1;
                    while (e < n && charAt(e) == c) {
                        if (e >= pfAt) pfAhead(e);
                        ++e;
                    }
                    if (segFd >= 0) {   // v3: char + varint len (samples in .pos)
                        char vb[12];
                        vb[0] = char(c);
                        std::vector<char> lenbuf;
                        put_varint(lenbuf, e - t);
                        rec.push_back(vb[0]);
                        rec.insert(rec.end(), lenbuf.begin(), lenbuf.end());
                        if (rec.size() >= (1u << 20)) flushRec();
                    } else {           // v2: 25-byte sample-bearing record
                        char b[25];
                        b[0] = char(c);
                        put_le64(b + 1, e - t); put_le64(b + 9, head); put_le64(b + 17, wm.at(e - 1));
                        rec.insert(rec.end(), b, b + 25);
                        if (rec.size() >= 25 * 4096) flushRec();
                    }
                    ++runIdx;
                    t = e;
                }
                flushRec();
                require(runIdx == b0 + info[s].internalStarts + (firstIsStart ? 1u : 0u),
                        "sxcr emit shard run coverage");
            });
            if (segFd >= 0) {
                segFds.push_back(segFd);
                segPaths.push_back(segPath);
            }
        }
        for (auto& t : ts) t.join();
        if (emitVer == 3) {   // ordered concatenation: shard order is run order
            for (int sf : segFds) require(::close(sf) == 0, "close sxcr v3 segment");
            constexpr U CW = 1u << 20;
            std::vector<char> cbuf(CW);
            for (auto& sp : segPaths) {
                int in = ::open(sp.c_str(), O_RDONLY);
                require(in >= 0, "reopen sxcr v3 segment");
                for (;;) {
                    ssize_t z = ::read(in, cbuf.data(), CW);
                    require(z >= 0, "read sxcr v3 segment");
                    if (z == 0) break;
                    xput(fd, cbuf.data(), size_t(z), v3Off, "sxcr v3 concat");
                    v3Off += U(z);
                }
                require(::close(in) == 0, "close sxcr v3 segment read");
                ::unlink(sp.c_str());
            }
        }
        require(::close(fd) == 0, "close SXCR chunk");
    }
    std::fprintf(stderr, "SXCR_EMIT shards=%llu runs=%llu n=%llu\n",
                 (unsigned long long)S, (unsigned long long)runs, (unsigned long long)n);
    phase("sxcr-write", last);
    {
        // PROGRESSIVE-SHAPE INVARIANT: level N's output is a valid level
        // N+1 input in the PERSISTED format. When every input side's text
        // is referenced (segments), the intermediate emits .pos (SXP3 order
        // column + period) and a .ref v2 whose segments are the input
        // sides' segments CONCATENATED (the merged text is exactly the
        // inputs' texts in order) - zero text copies, zero re-derivation
        // at any level, and n bytes less disc than the legacy .sxs embed.
        bool allRef = !inSides.empty();
        U segTotal = 0;
        for (auto& S : inSides)
            allRef = allRef && S.refText && !S.segs.empty() && (segTotal += S.segs.size(), true);
        if (allRef && !getenv("CROSS_NO_PERSIST_EMIT")) {
            U per = n;
            {
                // minimal cyclic period over the virtual merged text
                std::vector<U> divs;
                for (U d = 1; d * d <= n; ++d) if (n % d == 0) { divs.push_back(d); if (d != n / d) divs.push_back(n / d); }
                std::sort(divs.begin(), divs.end());
                constexpr U W = 1u << 20;
                std::vector<uint8_t> a(W), b(W);
                for (U d : divs) {
                    if (d >= n) continue;
                    bool ok = true;
                    for (U i = 0; i + d < n && ok; i += W) {
                        U len = std::min<U>(W, n - d - i);
                        m2.read(i, a.data(), len);
                        m2.read(i + d, b.data(), len);
                        for (U z = 0; z < len; ++z) if (a[z] != b[z]) { ok = false; break; }
                    }
                    if (ok) { per = d; break; }
                }
            }
            // Intermediate pos columns: dense (SXP3) or compact (SXP4).
            // Compact cuts disc ~2.4x but costs ~2.3x walk wall on the
            // NEXT level's loads+walks; the plan (XSA_POS_MODE) picks per
            // scale. CROSS_NO_SXP4 remains the legacy dense alias.
            const char* cpm = getenv("CROSS_POS_MODE");
            bool densePos = (cpm && !std::strcmp(cpm, "dense")) || getenv("CROSS_NO_SXP4");
            if (densePos) {
                OutFile f(chunk_stem(path) + ".pos");
                char magic[4]{'S','X','P','3'}; f.raw(magic, 4);
                f.w32(1); f.w64(offset); f.w64(n); f.w64(per); f.w64(0);
                U pb[4096];
                for (U i = 0; i < n;) {
                    U len = std::min<U>(4096, n - i);
                    wm.read(i, pb, len);
                    f.raw(pb, 8 * len);
                    i += len;
                }
            } else {
                write_pos_sxp4(chunk_stem(path) + ".pos", offset, n, per,
                               [&](U i) { return wm.at(i); });
            }
            {
                OutFile f(chunk_stem(path) + ".ref");
                char magic[4]{'S','X','R','F'}; f.raw(magic, 4);
                f.w32(2); f.w8(0);
                f.w32(U(segTotal));
                for (auto& S : inSides)
                    for (auto& g : S.segs) {
                        f.raw(g.remap.data(), 256);
                        f.w32(U(g.path.size()));
                        f.raw(g.path.data(), g.path.size());
                        f.w64(g.off); f.w64(g.len);
                    }
                f.w64(n);
            }
        } else {
            OutFile f(path + ".sxs");
            char magic[4]{'S','X','S','2'}; f.raw(magic, 4);
            f.w64(offset); f.w64(n);
            constexpr U W = 1u << 22;
            std::vector<uint8_t> buf(W);
            for (U i = 0; i < n;) {
                U len = std::min<U>(W, n - i);
                m2.read(i, buf.data(), len);
                f.raw(buf.data(), len);
                i += len;
            }
            U pb[4096];
            for (U i = 0; i < n;) {
                U len = std::min<U>(4096, n - i);
                wm.read(i, pb, len);
                f.raw(pb, 8 * len);
                i += len;
            }
        }
    }
    phase("sidecar-write", last);
    return runs;
}

// ------------------------------------------------------------ node pipeline
// FIX 4 (node-level pipeline, serial-compatible): while pair k runs, ONE
// preflight thread builds pair k+1's load/hash phases into that pair's
// own scratch dir. The tree stays serial (one merge at a time); the
// lookahead is exactly one; RAM on both sides stays bounded. The preflight
// starts at ANNOUNCE time (the driver announces the next pair right before
// the current pair's merge_pair_files call), so it overlaps the current
// pair's whole body; the consuming pair joins it and skips the phases it
// already built.
struct Preflight {
    std::string left, right, out;
    std::thread th;
    std::atomic<bool> done{false};
    bool ok = false;
    std::string err;
    SideExt A, B;
    PairScratch sc;
    U hashK = 0;
};
static std::deque<std::unique_ptr<Preflight>> g_pfq;
static void preflight_run(Preflight* pf) {
    try {
        pf->sc.init(pf->out, U(getpid()), 2);
        load_side_ext(pf->left, 48, pf->sc.dir, pf->A);
        load_side_ext(pf->right, 48, pf->sc.dir, pf->B);
        require(pf->A.offset + pf->A.n == pf->B.offset, "pair chunks do not tile");
        U n = pf->A.n + pf->B.n;
        CorpusText m2; open_merge_text({pf->A, pf->B}, m2);
        pf->hashK = hash_spacing();
        if (pf->hashK > n / 2) pf->hashK = std::max<U>(1, n / 2);
        build_hash_sparse(m2, 2 * n, pf->hashK, pf->sc.hs, 48);
        pf->ok = true;
    } catch (const std::exception& e) {
        pf->err = e.what();
        pf->ok = false;
    }
    pf->done = true;
}
// Announce the upcoming pair and start its preflight (lookahead of one).
// OPT-IN via CROSS_PIPELINE=1: the serial tree is the shipped deterministic
// mode; the preflight thread-lifetime path is a post-pile performance item.
static void cross_announce_next(const std::string& L, const std::string& R, const std::string& out) {
    if (!getenv("CROSS_PIPELINE")) return;
    if (g_pfq.size() >= 2) fail("preflight lookahead exceeded one");
    auto pf = std::make_unique<Preflight>();
    pf->left = L; pf->right = R; pf->out = out;
    Preflight* raw = pf.get();
    g_pfq.push_back(std::move(pf));
    raw->th = std::thread(preflight_run, raw);
}
static std::unique_ptr<Preflight> cross_take_preflight(const std::string& L, const std::string& R) {
    for (auto it = g_pfq.begin(); it != g_pfq.end(); ++it) {
        if ((*it)->left == L && (*it)->right == R) {
            auto pf = std::move(*it);
            g_pfq.erase(it);
            if (pf->th.joinable()) pf->th.join();
            return pf;
        }
    }
    return nullptr;
}
static void cross_drop_stale_preflights(const std::string& L, const std::string& R) {
    for (auto it = g_pfq.begin(); it != g_pfq.end(); ) {
        if ((*it)->left == L && (*it)->right == R) { ++it; continue; }
        auto pf = std::move(*it);
        g_pfq.erase(it);
        if (pf->th.joinable()) pf->th.join();
        if (!pf->ok && !pf->err.empty())
            std::fprintf(stderr, "PREFLIGHT_DROPPED_STALE left=%s err=%s\n",
                         pf->left.c_str(), pf->err.c_str());
        pf->sc.cleanup();
        it = g_pfq.begin();
    }
}

// ------------------------------------------------------------ k-way merge
// ONE code path: k=2 is the certified pairwise merge (CROSS_PAIR line,
// byte-identical banked format); k>2 (CROSS_KWAY line) is the flat/inter-
// mediate arity selected at launch. Sides must tile the text: side j's
// global offset = sum of previous sides' lengths.
static U merge_kway_files(const std::vector<std::string>& inputs, bool dollar,
                          const std::string& out, U threads, bool emitPf = false,
                          bool deferWm = false) {
    require(dollar || !v2_pieces_enabled(),
            "WM-free v2 intermediate emission is unsupported; use a single final fan-in");
    const size_t k = inputs.size();
    require(k >= 1, "k-way merge needs at least one input");
    const std::string& leftPath = inputs.front();
    const std::string& rightPath = inputs.back();
    if (dollar)
        for (const char* ext : {".rlebwt", ".rlebwt.meta", ".ssa", ".ssa_t"})
            require(!fs::exists(out + ext), "output exists; refusing to clobber");
    else
        require(!fs::exists(out) && !fs::exists(out + ".sxs"), "output exists; refusing to clobber");
    double t0 = now_sec();
    Budget bud; CoreStats st;
    bool pre = false;
    PairScratch sc;
    std::vector<SideExt> sides(k);
    U hashK = 0;
    std::unique_ptr<Preflight> pf;
    if (k == 2) {
        cross_drop_stale_preflights(leftPath, rightPath);
        pf = cross_take_preflight(leftPath, rightPath);
    }
    if (pf) {
        // FIX 4: this pair's loads/hash were prefetched during the
        // previous pair's run.
        if (!pf->ok) fail(("preflight failed: " + pf->err).c_str());
        sides[0] = pf->A; sides[1] = pf->B; sc = std::move(pf->sc); hashK = pf->hashK;
        pre = true;
    } else {
        sc.init(out, U(getpid()), k);
        // PARALLEL SIDE LOADS: the per-side loads (walk, validation, pos
        // dump) are independent - the pairwise origin's serial 2-item loop
        // is the leftover. A thread pool loads them with a MEMORY-AWARE
        // width: each load's walk transient is ~9x its chunk (text/bwt/lf/
        // pos/visited, chunk-scale, freed per side), so the concurrent
        // width is bounded by the phase's RLIMIT_AS. Side j is loaded by
        // whichever thread takes it; the sides vector is index-stable, so
        // the load order cannot reach any output (loads are pure functions
        // of their chunk; the merge is deterministic given the loaded
        // sides). Barrier: the pool joins before the tiling check and the
        // core merge, which is already parallel and order-independent.
        U nMax = 0, runsMax = 0;
        for (auto& f : inputs) {
            U o = 0, n = 0, r = 0, ver = 0;
            sxcr_header(f, o, n, r, ver);
            nMax = std::max(nMax, n); runsMax = std::max(runsMax, r);
        }
        U loadPar = std::min<size_t>(k, threads ? threads : 1);
        bool anyDerive = false;
        for (auto& f : inputs) {
            std::string stem = chunk_stem(f);
            anyDerive = anyDerive || (!fs::exists(f + ".sxs")
                && !(fs::exists(stem + ".pos") && fs::exists(stem + ".ref") && !getenv("CROSS_NO_PERSIST")));
        }
        if (anyDerive) {
            // DERIVE-FALLBACK width from the REAL per-chunk header metadata,
            // not a corpus-size constant: walk_chunk's resident transient =
            // runs records (25B/run) + seeds (16B/run) + bwt+text+visited
            // (2n) + lf (8n) + pos (8n). The 1b run A crash was exactly this
            // misestimate (10n assumed vs ~35n at 62.5M/24M-runs chunks:
            // a 5-wide pool = ~11 GB virtual > the 8 GiB cap; std::bad_alloc
            // thrown from walk_chunk's vector allocations). The persist path
            // has NO walk transient and skips this bound entirely.
            struct rlimit rl{};
            U perSide = 42 * runsMax + 18 * nMax + (256u << 20);
            if (perSide && getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
                U fit = U(rl.rlim_cur) / 2 / perSide;   // pools + merge own the rest
                if (fit < 1) fit = 1;
                loadPar = std::min<size_t>(loadPar, size_t(std::min<U>(fit, 64)));
            }
            std::fprintf(stderr, "LOAD_PLAN derive_per_side_mb=%.0f par=%llu\n",
                         perSide / 1048576.0, (unsigned long long)loadPar);
        }
        U innerThreads = std::max<U>(1, threads ? threads / loadPar : 1);
        std::atomic<size_t> next{0};
        std::atomic<U> loadSides{0};
        double loadT0 = now_sec();
        {
            std::vector<std::thread> ts;
            for (U t = 0; t < loadPar; ++t)
                ts.emplace_back([&] {
                    for (;;) {
                        size_t j = next.fetch_add(1);
                        if (j >= k) return;
                        double t0 = now_sec();
                        load_side_ext(inputs[j], innerThreads, sc.dir, sides[j]);
                        U done = loadSides.fetch_add(1, std::memory_order_relaxed) + 1;
                        // Per-side progress (the 10GB run was blind within the
                        // load phase): index, size, wall, mode, done/total.
                        std::fprintf(stderr, "LOAD_SIDE idx=%llu n=%llu wall=%.3f mode=%s done=%llu/%llu\n",
                                     (unsigned long long)j, (unsigned long long)sides[j].n,
                                     now_sec() - t0,
                                     sides[j].bwtFromRuns ? "persisted-reads"
                                       : (sides[j].refText ? "walk+ref" : "walk"),
                                     (unsigned long long)done, (unsigned long long)k);
                    }
                });
            for (auto& t : ts) t.join();
        }
        std::fprintf(stderr, "LOAD_STATS sides=%llu par=%llu wall=%.3f per_side_avg=%.3f (serial-equivalent width 1)\n",
                     (unsigned long long)loadSides.load(), (unsigned long long)loadPar,
                     now_sec() - loadT0, (now_sec() - loadT0) * loadPar / (double)(k ? k : 1));
        for (size_t j = 1; j < k; ++j)
            require(sides[j - 1].offset + sides[j - 1].n == sides[j].offset,
                    "merge sides do not tile");
    }
    U n = 0; for (auto& S : sides) n += S.n;
    if (!pre) {
        hashK = hash_spacing();
        if (hashK > n / 2) hashK = std::max<U>(1, n / 2);
    }
    U out_runs;
    if (dollar && v2_pieces_enabled()) {
        out_runs = emit_sparse_v2(out, sides, threads, bud, st, sc, hashK, pre);
        sc.cleanup(true); // finish owns seam.spool + .done
    } else {
        core_merge_kway(sides, dollar, threads, bud, st, sc, hashK, pre);
        sc.release_walk();
        CorpusText m2; open_merge_text(sides, m2, true);
        WinU64 wm; wm.open_ro_auto(sc.wm, n);
        out_runs = dollar ? emit_four_ext(out, m2, wm, n, threads, sc.dir)
                         : emit_sxcr_ext(out, m2, wm, n, sides.front().offset, threads, sides, sc.dir);
        if (dollar && emitPf) emit_pf_side_ext(out, m2, n, out_runs);
        sc.cleanup(deferWm);
    }
    double total = now_sec() - t0;
    if (k == 2) {
        std::printf("CROSS_PAIR mode=%s left=%s right=%s nA=%llu nB=%llu out=%s out_runs=%llu "
                    "wall_total=%.3f comparisons=%llu symbols_compared=%llu probes=%llu max_lce=%llu "
                    "fast_decided=%llu probe_decided=%llu cap_decided=%llu tie_decided=%llu "
                    "lce_over_10k=%llu lce_over_100k=%llu lce_over_1m=%llu "
                    "anchors_A=%llu anchors_B=%llu walk_steps_A=%llu walk_steps_B=%llu "
                    "block_rows_A=%llu block_rows_B=%llu peak_rss_kib=%llu hash_k=%llu "
                    "t_fast=%.3f t_prep=%.3f t_probe=%.3f t_scan=%.3f\n",
                    dollar ? "dollar" : "cyclic", leftPath.c_str(), rightPath.c_str(),
                    (unsigned long long)sides[0].n, (unsigned long long)sides[1].n, out.c_str(),
                    (unsigned long long)out_runs, total,
                    (unsigned long long)bud.comparisons, (unsigned long long)bud.symbols,
                    (unsigned long long)bud.probes, (unsigned long long)bud.max_lce,
                    (unsigned long long)bud.fast_decided, (unsigned long long)bud.probe_decided,
                    (unsigned long long)bud.cap_decided, (unsigned long long)bud.tie_decided,
                    (unsigned long long)bud.lce_over_10k, (unsigned long long)bud.lce_over_100k,
                    (unsigned long long)bud.lce_over_1m,
                    (unsigned long long)st.anchorsA(), (unsigned long long)st.anchorsB(),
                    (unsigned long long)st.stepsA(), (unsigned long long)st.stepsB(),
                    (unsigned long long)st.rowsA(), (unsigned long long)st.rowsB(),
                    (unsigned long long)peak_rss_kib(), (unsigned long long)hashK,
                    bud.tFast, bud.tPrep, bud.tProbe, bud.tScan);
    } else {
        std::string sizes;
        for (size_t j = 0; j < k; ++j)
            sizes += (j ? "," : "") + std::to_string(sides[j].n);
        std::printf("CROSS_KWAY k=%llu mode=%s left=%s right=%s n=%llu sizes=%s out=%s out_runs=%llu "
                    "wall_total=%.3f comparisons=%llu symbols_compared=%llu probes=%llu max_lce=%llu "
                    "fast_decided=%llu probe_decided=%llu cap_decided=%llu tie_decided=%llu "
                    "lce_over_10k=%llu lce_over_100k=%llu lce_over_1m=%llu "
                    "anchors_sum=%llu walk_steps_sum=%llu block_rows_sum=%llu "
                    "peak_rss_kib=%llu hash_k=%llu "
                    "t_fast=%.3f t_prep=%.3f t_probe=%.3f t_scan=%.3f\n",
                    (unsigned long long)k, dollar ? "dollar" : "cyclic", leftPath.c_str(),
                    rightPath.c_str(), (unsigned long long)n, sizes.c_str(), out.c_str(),
                    (unsigned long long)out_runs, total,
                    (unsigned long long)bud.comparisons, (unsigned long long)bud.symbols,
                    (unsigned long long)bud.probes, (unsigned long long)bud.max_lce,
                    (unsigned long long)bud.fast_decided, (unsigned long long)bud.probe_decided,
                    (unsigned long long)bud.cap_decided, (unsigned long long)bud.tie_decided,
                    (unsigned long long)bud.lce_over_10k, (unsigned long long)bud.lce_over_100k,
                    (unsigned long long)bud.lce_over_1m,
                    (unsigned long long)st.sum(st.anchors), (unsigned long long)st.sum(st.steps),
                    (unsigned long long)st.sum(st.rows),
                    (unsigned long long)peak_rss_kib(), (unsigned long long)hashK,
                    bud.tFast, bud.tPrep, bud.tProbe, bud.tScan);
    }
    std::fflush(stdout);
    return out_runs;
}

// Single-chunk finalize: cyclic order -> $ order, then the four files.
// M2 = T||T over the single side; the repaired order is the WM.
static U finalize_file(const std::string& path, const std::string& prefix, U threads, bool emitPf = false) {
    if (1) for (const char* ext : {".rlebwt", ".rlebwt.meta", ".ssa", ".ssa_t"})
        require(!fs::exists(prefix + ext), "output exists; refusing to clobber");
    double t0 = now_sec(), last = t0;
    Budget bud; CoreStats st;
    PairScratch sc; sc.init(prefix, U(getpid()), 1);
    SideExt A;
    load_side_ext(path, threads, sc.dir, A);
    phase("load", last);
    CorpusText m2; open_merge_text({A}, m2, true);
    if (v2_pieces_enabled()) {
        U hashK = std::min<U>(hash_spacing(), std::max<U>(1, A.n / 2));
        U runs = emit_sparse_v2(prefix, {A}, threads, bud, st, sc, hashK, false);
        sc.cleanup(true);
        return runs;
    }
    U hashK = hash_spacing();
    if (hashK > A.n / 2) hashK = std::max<U>(1, A.n / 2);
    build_hash_sparse(m2, 2 * A.n, hashK, sc.hs, threads);
    phase("hash-build", last);
    {
        HashSparse hp; hp.open_ro(sc.hs, hashK, 2 * A.n / hashK + 1);
        Keys keys; keys.init(&m2, &hp, A.n, true, &bud);
        PosOverlay posA;
        posA.init(A.sxsPath, A.n, A.posBase, 0, sc.ov[0]);
        if (A.bwtFromRuns) build_bwt_from_runs(A, posA, sc.bwt[0], 0);
        else build_bwt_file(A, posA, sc.bwt[0], threads, 0);
        std::vector<BlockRec> blocks;
        std::vector<uint64_t> anchors;
        {
            WinBytes bwt; bwt.open_ro(sc.bwt[0], A.n, 0);
            SideText text; open_side_text(A, text);
            anchor_blocks_ext(text, bwt, posA, A.n, 0, A.period, blocks, anchors, st.steps[0], st.rows[0]);
        }
        ::unlink(sc.bwt[0].c_str()); // closed above; repair uses positions + corpus
        st.init(1);
        st.anchors[0] = anchors.size();
        repair_ext(posA, blocks, anchors, 0, A.n, keys);
        phase("anchor+repair", last);
        // WM = the repaired order, streamed from the overlay.
        if (wm_pack_enabled()) {
            WmPacker p;
            p.open(sc.wm, A.n);
            for (U i = 0; i < A.n;) {
                U len = std::min<U>(4096, A.n - i);
                for (U z = 0; z < len; ++z) p.put(i + z, posA.at(i + z));
                i += len;
            }
            p.close();
        } else {
            int fd = ::open(sc.wm.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            require(fd >= 0, "create wm scratch");
            U buf[4096];
            for (U i = 0; i < A.n;) {
                U len = std::min<U>(4096, A.n - i);
                for (U z = 0; z < len; ++z) buf[z] = posA.at(i + z);
                xput(fd, buf, 8 * len, 8 * i, "wm write");
                i += len;
            }
            ::close(fd);
        }
    } // hash pool and overlay readers are dead before emission
    sc.release_walk();
    WinU64 wm; wm.open_ro_auto(sc.wm, A.n);
    U out_runs = emit_four_ext(prefix, m2, wm, A.n, threads, sc.dir);
    if (emitPf) emit_pf_side_ext(prefix, m2, A.n, out_runs);
    sc.cleanup();
    double total = now_sec() - t0;
    std::printf("CROSS_PAIR mode=dollar-single left=%s right=- nA=%llu nB=0 out=%s out_runs=%llu "
                "wall_total=%.3f comparisons=%llu symbols_compared=%llu probes=%llu max_lce=%llu "
                "fast_decided=%llu probe_decided=%llu cap_decided=%llu tie_decided=%llu "
                "lce_over_10k=%llu lce_over_100k=%llu lce_over_1m=%llu "
                "anchors_A=%llu anchors_B=0 walk_steps_A=%llu walk_steps_B=0 "
                "block_rows_A=%llu block_rows_B=0 peak_rss_kib=%llu hash_k=%llu\n",
                path.c_str(), (unsigned long long)A.n, prefix.c_str(), (unsigned long long)out_runs,
                total, (unsigned long long)bud.comparisons, (unsigned long long)bud.symbols,
                (unsigned long long)bud.probes, (unsigned long long)bud.max_lce,
                (unsigned long long)bud.fast_decided, (unsigned long long)bud.probe_decided,
                (unsigned long long)bud.cap_decided, (unsigned long long)bud.tie_decided,
                (unsigned long long)bud.lce_over_10k, (unsigned long long)bud.lce_over_100k,
                (unsigned long long)bud.lce_over_1m, (unsigned long long)st.anchorsA(),
                (unsigned long long)st.stepsA(), (unsigned long long)st.rowsA(),
                (unsigned long long)peak_rss_kib(), (unsigned long long)hashK);
    std::fflush(stdout);
    return out_runs;
}

// ---------------------------------------------------------------- selftest
static std::vector<uint64_t> brute_cyclic(const std::string& T) {
    U n = T.size();
    std::vector<std::pair<std::string, uint64_t>> v;
    v.reserve(n);
    for (U p = 0; p < n; ++p) v.push_back({T.substr(p) + T.substr(0, p), uint64_t(p)});
    std::sort(v.begin(), v.end());
    std::vector<uint64_t> o(n);
    for (U i = 0; i < n; ++i) o[i] = v[i].second;
    return o;
}
static std::vector<uint64_t> brute_dollar(const std::string& T) {
    U n = T.size();
    std::vector<std::pair<std::string, uint64_t>> v;
    v.reserve(n);
    for (U p = 0; p < n; ++p)
        v.push_back({T.substr(p), uint64_t(p)});
    std::sort(v.begin(), v.end());
    std::vector<uint64_t> o(n);
    for (U i = 0; i < n; ++i) o[i] = v[i].second;
    return o;
}

// Independent legacy-style oracle: discover directly from the tiny string,
// locate first in the brute SA, read its interval/flanks from that SA. The
// sparse capture never uses this row-position vector.
static std::vector<CapturedSeamClass> brute_seam_classes(const std::string& text,
                                                        const std::vector<U>& sa, U& depth) {
    U n = text.size();
    std::vector<CapturedSeamClass> raw, result;
    for (U d = 1; d <= n; ++d) {
        depth = d;
        std::vector<U> occ;
        for (U p = 0; p + d <= n; ++p)
            if (text.compare(p, d, text, n - d, d) == 0) occ.push_back(p);
        if (occ.size() <= 1) break;
        CapturedSeamClass cl;
        cl.first = n - d;
        auto at = std::find(sa.begin(), sa.end(), cl.first);
        require(at != sa.end(), "brute seam first missing");
        cl.lo = at - sa.begin(); cl.hi = cl.lo + occ.size();
        require(cl.hi <= n, "brute seam interval bounds");
        cl.before = cl.lo ? sa[cl.lo - 1] : n + 9;
        cl.after = cl.hi < n ? sa[cl.hi] : n;
        cl.occ.assign(sa.begin() + cl.lo, sa.begin() + cl.hi);
        auto check = cl.occ; std::sort(check.begin(), check.end());
        require(check == occ, "brute seam occurrence set mismatch");
        raw.push_back(std::move(cl));
    }
    std::sort(raw.begin(), raw.end(), [](const auto& a, const auto& b) {
        return a.lo < b.lo || (a.lo == b.lo && a.hi > b.hi);
    });
    for (auto& cl : raw) {
        if (!result.empty() && cl.lo < result.back().hi) {
            require(cl.hi <= result.back().hi, "brute seam crossing intervals");
            continue;
        }
        result.push_back(std::move(cl));
    }
    return result;
}
static std::vector<char> seam_spool_bytes(U n, U depth,
                                        const std::vector<CapturedSeamClass>& classes) {
    std::vector<char> b{'S','C','S','1'};
    auto add = [&](U v) { char x[8]; put_le64(x, v); b.insert(b.end(), x, x + 8); };
    add(n); add(depth); add(classes.size());
    for (const auto& cl : classes) {
        add(cl.lo); add(cl.hi); add(cl.first); add(cl.before); add(cl.after);
        for (U p : cl.occ) add(p);
    }
    return b;
}
static void selftest_sparse_walk(const std::vector<SideExt>& sides, bool dollar,
                                U threads, U hashK, const PairScratch& sc,
                                const std::string& text, const std::vector<U>& want) {
    CorpusText corpus; open_merge_text(sides, corpus, true);
    SparseSeamCapture seam;
    // A fixed oracle-only bound, including raw nested classes. No production
    // sizing policy is implied by this test budget.
    if (dollar) seam.discover(corpus, text.size(), 100000);
    std::vector<SparseRun> got;
    std::array<U, 256> counts{}, runCounts{};
    U consumedRows = 0, consumedShards = 0, globalRuns = 0;
    std::vector<SparseRun> reduced;
    U reducedRows = 0;
    SparseRunReducer reducer([&](const SparseRun& run, U start) {
        require(start == reducedRows, "sparse reducer global row base");
        reducedRows += run.len; reduced.push_back(run);
    });
    auto readExact = [](int fd, void* data, U len, U off) {
        char* p = static_cast<char*>(data);
        while (len) {
            ssize_t z = ::pread(fd, p, len, off);
            if (z < 0 && errno == EINTR) continue;
            require(z > 0, "sparse oracle short read");
            p += z; off += z; len -= z;
        }
    };
    auto consume = [&](const std::string& path, U records, U rows) {
        require(!fs::exists(sc.wm), "sparse oracle created WM");
        struct stat st{};
        require(::stat(path.c_str(), &st) == 0 && U(st.st_size) == 25 * records,
                "sparse oracle run spool size");
        int fd = ::open(path.c_str(), O_RDONLY);
        require(fd >= 0, "sparse oracle open run spool");
        U total = 0;
        auto read64 = [](const char* b) {
            U v = 0; for (unsigned i = 0; i < 8; ++i) v |= U(uint8_t(b[i])) << (8 * i); return v;
        };
        for (U j = 0; j < records; ++j) {
            char b[25]; readExact(fd, b, sizeof b, 25 * j);
            SparseRun run{uint8_t(b[0]), read64(b + 1), read64(b + 9), read64(b + 17)};
            require(run.len > 0 && run.head < text.size() && run.tail < text.size(),
                    "sparse oracle record bounds");
            counts[run.c] += run.len; total += run.len;
            if (!got.empty() && got.back().c == run.c) {
                got.back().len += run.len; got.back().tail = run.tail;
            } else {
                got.push_back(run); ++runCounts[run.c]; ++globalRuns;
            }
        }
        require(total == rows && ::close(fd) == 0, "sparse oracle shard counts");
        // Stall the first consumer: no later shard may enter or overwrite
        // its spool while this callback still owns the admitted shard.
        if (!consumedShards) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        reducer.consume(path, records, rows);
        consumedRows += rows; ++consumedShards;
    };
    SparseRunSpool sink(corpus, text.size(), dollar, sc.dir + "/runs.spool",
                        dollar ? &seam : nullptr, consume);
    Budget bud; CoreStats stats;
    core_merge_kway(sides, dollar, threads, bud, stats, sc, hashK, false, &sink);
    reducer.flush();
    require(reduced == got && reducer.starts == globalRuns && reducedRows == text.size(),
            "sparse reducer boundary stitching/global bases");
    require(!fs::exists(sc.wm) && !fs::exists(sc.wm + ".done"), "sparse oracle WM absence");
    require(consumedRows == text.size() && consumedShards == emit_shard_count(threads, text.size()),
            "sparse oracle ordered admission coverage");
    std::vector<SparseRun> expected;
    std::array<U, 256> expectedCounts{}, expectedRunCounts{};
    for (U p : want) {
        uint8_t c = dollar ? (p ? uint8_t(text[p - 1]) : uint8_t(2))
                           : uint8_t(text[(p + text.size() - 1) % text.size()]);
        ++expectedCounts[c];
        if (!expected.empty() && expected.back().c == c) {
            ++expected.back().len; expected.back().tail = p;
        } else { expected.push_back({c, 1, p, p}); ++expectedRunCounts[c]; }
    }
    require(got == expected && counts == expectedCounts && runCounts == expectedRunCounts &&
            globalRuns == expected.size(), "sparse oracle run reconstruction mismatch");
    if (dollar) {
        U bruteDepth = 0;
        auto reference = brute_seam_classes(text, want, bruteDepth);
        auto captured = seam.finish();
        require(captured == reference && seam.depth == bruteDepth, "sparse oracle seam classes mismatch");
        auto bytes = seam_spool_bytes(text.size(), seam.depth, captured);
        auto oldBytes = seam_spool_bytes(text.size(), bruteDepth, reference);
        std::string spool = sc.dir + "/seam.spool";
        { OutFile f(spool); f.raw(bytes.data(), bytes.size()); }
        int fd = ::open(spool.c_str(), O_RDONLY);
        require(fd >= 0, "sparse oracle open seam spool");
        struct stat st{};
        require(::fstat(fd, &st) == 0 && U(st.st_size) == oldBytes.size(), "sparse oracle seam spool size");
        std::vector<char> disk(oldBytes.size());
        readExact(fd, disk.data(), disk.size(), 0);
        require(disk == oldBytes, "sparse oracle seam spool bytes mismatch");
        require(::close(fd) == 0 && ::unlink(spool.c_str()) == 0, "sparse oracle seam spool consume");
    }
}

static int selftest(U cases, uint64_t seed, U threads, U kway) {
    uint64_t s = seed ? seed : 1;
    auto next = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    const std::vector<std::string> alphabets = {
        {'\x00', '\x05', '\x1e'}, {'\xfc', '\xfd', '\xfe', '\xff', '\x00', '\x05'},
        {'\x06'}, {'\x06', '\x07'}, {'\x06', '\x07', '\x08'},
        {'\x06', '\x07', '\x08', '\x1e'}, {'\x06', '\x07', '\x08', '\x09', '\x0a', '\x1e'},
    };
    const char* scratchRoot = getenv("CROSS_SELFTEST_SCRATCH");
    const std::string scratch = (scratchRoot ? std::string(scratchRoot) : fs::temp_directory_path().string())
        + "/cross-selftest-scratch-" + std::to_string(U(getpid()));
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
        std::vector<std::string> As;
        std::string M;
        for (U z = 0; z < kway; ++z) { As.push_back(gen(1 + next() % 60, bool(next() & 1))); M += As.back(); }
        // Explicit nonperiodic wrap-prefix and nested-prefix shapes, followed
        // by the existing randomized periodic/deep-repetition cases.
        if (cs < 5) {
            const std::string patterns[] = {std::string("\x1e\x0a\x1e", 3),
                                           std::string("aaaaaa"), std::string("abababa"),
                                           std::string("\x00\x05\x1e\x00\x05", 5),
                                           std::string("\x1e\x0a\x1e\x0a\x1e", 5)};
            M.clear();
            for (auto& A : As) { A = patterns[cs]; M += A; }
        }
        std::vector<std::vector<uint64_t>> posS;
        for (auto& A : As) posS.push_back(brute_cyclic(A));
        // Route through the EXTERNAL path: tiny sidecars in a scratch dir, the
        // windowed core merge, WM read back from its scratch file.
        fs::remove_all(scratch);
        fs::create_directories(scratch);
        std::array<uint8_t, 256> remap{};
        for (U c = 0; c < 256; ++c) remap[c] = uint8_t(c);
        for (auto pair : {std::pair<unsigned,unsigned>{1,255}, {2,253}, {3,254}, {4,252}})
            std::swap(remap[pair.first], remap[pair.second]);
        const std::string corpus = scratch + "/corpus";
        {
            OutFile f(corpus);
            std::vector<uint8_t> raw(M.begin(), M.end());
            U at = 0;
            for (U z = 0; z < kway; ++z) {
                for (U j = 0; j < As[z].size(); ++j, ++at)
                    if (z & 1) raw[at] = remap[raw[at]]; // inverse equals forward
            }
            f.raw(raw.data(), raw.size());
        }
        for (int mode = 0; mode < 2; ++mode) {
            bool dollar = mode == 1;
            Budget bud; CoreStats st;
            std::vector<SideExt> sides(kway);
            U off = 0, nTot = 0;
            auto dump = [](const SideExt& S, const std::vector<uint8_t>& t, const std::vector<uint64_t>& pos) {
                fs::remove(S.sxsPath);   // second mode reuses the names
                OutFile f(S.sxsPath);
                char magic[4]{'S','X','S','2'}; f.raw(magic, 4);
                f.w64(S.offset); f.w64(S.n);
                f.raw(t.data(), t.size());
                f.raw(pos.data(), 8 * pos.size());
            };
            for (U z = 0; z < kway; ++z) {
                std::vector<uint8_t> tz(As[z].begin(), As[z].end());
                SideExt& S = sides[z];
                S.n = As[z].size(); S.offset = off; S.period = min_period(tz, tz.size());
                S.sxsPath = scratch + "/side-" + std::to_string(z) + ".sxs"; S.ownsSxs = false;
                S.refText = false; S.posBase = 20 + S.n;
                dump(S, tz, posS[z]);
                if ((cs & 1) || cs < 3) {
                    S.refText = true;
                    // Keep segment boundaries even when ranges are adjacent:
                    // alternate maps force both the segment and wrap paths.
                    TextSeg g; g.path = corpus; g.off = off; g.len = S.n; g.remap = remap;
                    if (!(z & 1)) for (U c = 0; c < 256; ++c) g.remap[c] = uint8_t(c);
                    S.segs.push_back(g);
                }
                off += S.n; nTot += S.n;
            }
            PairScratch sc; sc.init(scratch + "/case", 0, kway);
            U hashK = hash_spacing();
            if (hashK > nTot / 2) hashK = std::max<U>(1, nTot / 2);
            core_merge_kway(sides, dollar, threads, bud, st, sc, hashK, false);
            std::vector<uint64_t> WM(nTot, 0);
            {
                WinU64 wm; wm.open_ro_auto(sc.wm, nTot);
                wm.read(0, WM.data(), nTot);
                if (dollar && cs < 20) {
                    setenv("CROSS_V2_PIECES", "1", 1);
                    setenv("CROSS_V2_BAND", "3", 1);
                    setenv("CROSS_V2_PIECE_WINDOW", "1", 1);
                    CorpusText corpus; open_merge_text(sides, corpus, true);
                    emit_four_ext(scratch + "/dense", corpus, wm, nTot, threads, sc.dir);
                }
            }
            sc.cleanup();
            std::vector<uint64_t> want = dollar ? brute_dollar(M) : brute_cyclic(M);
            if (WM != want) {
                std::fprintf(stderr, "SELFTEST_FAIL case=%llu mode=%s k=%llu sizes=",
                             (unsigned long long)cs, dollar ? "dollar" : "cyclic",
                             (unsigned long long)kway);
                for (auto& Az : As) std::fprintf(stderr, "%zu,", Az.size());
                std::fprintf(stderr, "\n");
                auto hex = [](const std::string& s) {
                    std::string o;
                    char buf[8];
                    for (unsigned char ch : s) { std::snprintf(buf, sizeof buf, "%02x", ch); o += buf; }
                    return o;
                };
                for (U z = 0; z < kway; ++z)
                    std::fprintf(stderr, "SIDE%llu_hex=%s\n", (unsigned long long)z, hex(As[z]).c_str());
                std::fprintf(stderr, "got :");
                for (auto p : WM) std::fprintf(stderr, " %llu", (unsigned long long)p);
                std::fprintf(stderr, "\nwant:");
                for (auto p : want) std::fprintf(stderr, " %llu", (unsigned long long)p);
                std::fprintf(stderr, "\n");
                return 1;
            }
            sc.init(scratch + "/sparse", 0, kway);
            selftest_sparse_walk(sides, dollar, threads, hashK, sc, M, want);
            sc.cleanup();
            if (dollar && cs < 20) {
                sc.init(scratch + "/sparseprod", 0, kway);
                Budget productionBudget; CoreStats productionStats;
                emit_sparse_v2(scratch + "/prod", sides, threads, productionBudget,
                               productionStats, sc, hashK, false);
                require(!fs::exists(sc.wm), "production oracle WM absence");
                auto bytes = [](const std::string& path) {
                    std::ifstream f(path, std::ios::binary);
                    require(bool(f), "production oracle file missing");
                    return std::vector<char>(std::istreambuf_iterator<char>(f), {});
                };
                for (const char* ext : {".rlebwt", ".rlebwt.meta"}) {
                    require(bytes(scratch + "/dense" + ext) == bytes(scratch + "/prod" + ext),
                            "production oracle index/rlebwt mismatch");
                    fs::remove(scratch + "/dense" + ext); fs::remove(scratch + "/prod" + ext);
                }
                require(!fs::exists(scratch + "/prod.runcols.idx"), "production has no shard index");
                auto encoded = bytes(scratch + "/prod.pieces/p-0-0");
                U first, rc; unsigned width; memcpy(&first, encoded.data()+16,8);
                memcpy(&rc,encoded.data()+24,8); memcpy(&width,encoded.data()+32,4);
                require(first == 0, "trivial production base");
                U words=(rc*width+63)/64;
                auto sample = [&](U offset,U i) {
                    U bit=i*width, word=bit/64, shift=bit%64, lo=0,hi=0;
                    memcpy(&lo,encoded.data()+offset+8*word,8);
                    U value=lo>>shift;
                    if (shift+width>64) {memcpy(&hi,encoded.data()+offset+8*(word+1),8);value|=hi<<(64-shift);}
                    return value&((U(1)<<width)-1);
                };
                std::vector<SparseRun> actual;
                for(U i=0;i<rc;++i) {
                    uint32_t len;memcpy(&len,encoded.data()+44+rc+4*i,4);
                    actual.push_back({uint8_t(encoded[44+i]),len,sample(44+5*rc,i),sample(44+5*rc+8*words,i)});
                }
                std::vector<SparseRun> expectedRuns;
                SparseRunReducer reduce([&](const SparseRun& run,U){expectedRuns.push_back(run);});
                reduce.add({uint8_t(M.back()),1,nTot,nTot});reduce.add({2,9,nTot+1,nTot+9});
                for(U pos:want) reduce.add({pos?uint8_t(M[pos-1]):uint8_t(2),1,pos,pos});reduce.flush();
                require(actual==expectedRuns,"single-pass production run columns");
                auto marker=bytes(scratch+"/prod.pieces/g-0");U offset=4;
                auto next=[&](){U value;require(offset+8<=marker.size(),"production marker truncated");
                    memcpy(&value,marker.data()+offset,8);offset+=8;return value;};
                require(next()==0&&next()==rc,"production band progress");U refs=next(),entries=next();
                for(U i=0;i<refs;++i)require(next()<entries,"production progress reference");
                std::vector<CapturedSeamClass> captured;
                for(U id=0;id<entries;++id){require(next()==id,"production class id");next();next();
                    CapturedSeamClass cl;cl.lo=next();cl.hi=next();cl.first=next();cl.before=next();cl.after=next();
                    for(U row=cl.lo;row<cl.hi;++row)cl.occ.push_back(next());captured.push_back(std::move(cl));}
                require(offset==marker.size(),"production progress trailing data");
                U expectedDepth;auto expectedClasses=brute_seam_classes(M,want,expectedDepth);
                require(captured==expectedClasses,"production band seam capture");
                require(bytes(scratch+"/prod.pieces/terminal").size()==36,"production terminal assertion");
                fs::remove(scratch+"/dense.runcols.idx");
                fs::remove_all(scratch + "/dense.pieces"); fs::remove_all(scratch + "/prod.pieces");
                sc.cleanup();
            }
        }
        if ((cs + 1) % 200 == 0) {
            std::fprintf(stderr, "SELFTEST_PROGRESS done=%llu/%llu\n",
                         (unsigned long long)(cs + 1), (unsigned long long)cases);
            std::fflush(stderr);
        }
    }
    fs::remove_all(scratch);
    std::printf("SPARSE_ORACLE_PASS modes=cyclic,dollar runs=exact seam_spool=exact production_bands=exact trivial_bases=exact terminal=exact legacy_shard_admission=1 WM=absent\n");
    std::printf("SELFTEST_PASS count=%llu seed=%llu kway=%llu\n",
                (unsigned long long)cases, (unsigned long long)seed,
                (unsigned long long)kway);
    return 0;
}

// ---------------------------------------------------------------- driver
static void abortHandler(int sig) {
    void* bt[32];
    int n = backtrace(bt, 32);
    std::fprintf(stderr, "SIGABRT/terminate backtrace (%d frames):\n", n);
    backtrace_symbols_fd(bt, n, 2);
    _exit(99);
}
// Operator-amended D2: bounded transient positions, no full SA/position file.
// This producer shares the corpus view, exact LCE comparator and downstream
// SequentialV2 publisher with the older path. Text ranges are never merged.
struct RadixWork {
    U sweepBytes=0, sweeps=0, keyOps=0, keyBytes=0, singleton=0, residue=0;
    U segments=0, peakPositions=0, cap=0, selectionPasses=0;
};
struct RadixRecord { U key, position; };
static unsigned radix_length(const CorpusText& t,U p,unsigned N,bool dollar) {
    return dollar ? unsigned(std::min<U>(N,t.n-p)) : N;
}
static U radix_key(const CorpusText& t,U p,unsigned N,bool dollar,RadixWork& w) {
    unsigned len=radix_length(t,p,N,dollar);uint8_t storage[8];
    // For tiny cyclic corpora N may exceed n: never ask span for >2n.
    U k=0;
    if(len<=t.n){const uint8_t* bytes=t.span(p,storage,len);
        for(unsigned j=0;j<len;++j)k=(k<<8)|bytes[j];}
    else for(unsigned j=0;j<len;++j)k=(k<<8)|t.at((p+j)%t.n);
    k <<= 8*(N-len);++w.keyOps;w.keyBytes+=len;return k;
}
// Prefix symbols use byte+1 and a true end symbol 0, so legal NUL/05 and
// 0x1e are ordinary remapped bytes. No document-local truncation occurs.
template<class F> static void radix_scan(const CorpusText& t,bool dollar,RadixWork& w,F f) {
    constexpr U W=1u<<20;std::vector<uint8_t> storage(std::min(W,t.n));
    for(U start=0;start<t.n;){U len=std::min(W,t.n-start);
        const uint8_t* bytes=t.span(start,storage.data(),len);
        for(U j=0;j<len;++j){U p=start+j;
            unsigned next=j+1<len?unsigned(bytes[j+1])+1:
                (p+1<t.n?unsigned(t.at(p+1))+1:(dollar?0:unsigned(t.at(0))+1));
            f(p,(unsigned(bytes[j])+1)*257+next);
        }start+=len;
    }w.sweepBytes+=t.n;++w.sweeps;
}
template<class Emit> static void radix_order(const CorpusText& text,Keys& keys,unsigned N,U requested,
                                             RadixWork& w,Emit emit) {
    require(N>=2&&N<=8&&requested,"radix width/cap");
    // Two record banks together cannot cover the complete corpus (except
    // unavoidable 1/2-byte degenerate texts). Both are freed after emission.
    w.cap=std::min<U>(requested,std::max<U>(1,(text.n-1)/2));
    std::vector<U> counts(257*257,0);
    radix_scan(text,keys.dollar,w,[&](U,unsigned k){++counts[k];});
    U row=0;
    auto compare=[&](U a,U b,U certified){return a!=b&&keys.cmp(a,b,certified)<0;};
    for(U lower=0;lower<counts.size();){
        while(lower<counts.size()&&!counts[lower])++lower;
        if(lower==counts.size())break;
        U upper=lower,total=0;
        while(upper<counts.size()&&counts[upper]<=w.cap-total){total+=counts[upper++];}
        if(upper==lower){
            // A prefix bucket exceeds the rolling bound. Exact bounded
            // selection survives periodic texts even when every N-key ties.
            // Each selected batch is emitted before selecting the next.
            U done=0,previous=0;bool havePrevious=false;
            while(done<counts[lower]){
                U take=std::min(w.cap,counts[lower]-done);std::vector<U> heap;heap.reserve(take);
                U certified=keys.dollar?std::min<U>(2,text.n-1):2;
                auto less=[&](U a,U b){return compare(a,b,certified);};
                radix_scan(text,keys.dollar,w,[&](U p,unsigned k){
                    if(k!=lower||(havePrevious&&!less(previous,p)))return;
                    if(heap.size()<take){heap.push_back(p);std::push_heap(heap.begin(),heap.end(),less);}
                    else if(less(p,heap.front())){std::pop_heap(heap.begin(),heap.end(),less);heap.back()=p;std::push_heap(heap.begin(),heap.end(),less);}
                });
                require(heap.size()==take,"bounded radix selection coverage");
                std::sort_heap(heap.begin(),heap.end(),less);
                for(U p:heap)emit(row++,p);
                previous=heap.back();havePrevious=true;done+=take;
                w.peakPositions=std::max<U>(w.peakPositions,heap.capacity());
                ++w.selectionPasses;++w.segments;
            }
            ++lower;continue;
        }
        {
            std::vector<RadixRecord> records;records.reserve(total);
            radix_scan(text,keys.dollar,w,[&](U p,unsigned k){if(k>=lower&&k<upper)
                records.push_back({radix_key(text,p,N,keys.dollar,w),p});});
            require(records.size()==total&&total<=w.cap,"radix rolling segment bound");
            std::vector<RadixRecord> temp(total);
            w.peakPositions=std::max<U>(w.peakPositions,records.capacity()+temp.capacity());
            // Stable finite-length digit first, then big-endian key bytes.
            // Padding zero alone is insufficient when NUL is legal.
            for(unsigned pass=0;pass<=N;++pass){
                std::array<U,256> sizes{},off{};
                auto digit=[&](const RadixRecord& r){return pass==0?
                    radix_length(text,r.position,N,keys.dollar):unsigned((r.key>>(8*(pass-1)))&255);};
                for(const auto& r:records)++sizes[digit(r)];
                for(unsigned d=1;d<256;++d)off[d]=off[d-1]+sizes[d-1];
                for(const auto& r:records)temp[off[digit(r)]++]=r;
                records.swap(temp);
            }
            for(U lo=0;lo<total;){U hi=lo+1;
                while(hi<total&&records[hi].key==records[lo].key&&
                    radix_length(text,records[hi].position,N,keys.dollar)==radix_length(text,records[lo].position,N,keys.dollar))++hi;
                if(hi-lo==1)++w.singleton;
                else{w.residue+=hi-lo;
                    std::sort(records.begin()+lo,records.begin()+hi,[&](const RadixRecord& a,const RadixRecord& b){
                        return compare(a.position,b.position,std::min<U>(N,keys.dollar?text.n-std::max(a.position,b.position):text.n));});}
                for(U i=lo;i<hi;++i)emit(row++,records[i].position);
                lo=hi;
            }
            ++w.segments;
        } // Both position banks are destroyed before the next sweep.
        lower=upper;
    }
    require(row==text.n&&w.peakPositions<=2*w.cap,"radix full coverage/position bound");
}
static int radix_main(const std::string& path,const std::string& mapPath,const std::string& prefix,
                      unsigned N,U cap,U threads) {
    require(!prefix.empty()&&!fs::exists(prefix+".rlebwt")&&!fs::exists(prefix+".pieces"),"fresh radix output required");
    TextSeg segment;segment.path=path;segment.len=fs::file_size(path);
    std::ifstream map(mapPath,std::ios::binary);map.read((char*)segment.remap.data(),256);
    require(bool(map),"radix remap read");auto permutation=segment.remap;std::sort(permutation.begin(),permutation.end());
    for(unsigned c=0;c<256;++c)require(permutation[c]==c,"radix remap bijection");
    require(segment.remap[0x1e]==0x1e,"radix separator remap");
    CorpusText text;text.open({segment},true);
    const std::string hash=prefix+".radix-hs";require(!fs::exists(hash),"fresh radix hash scratch");
    build_hash_sparse(text,2*text.n,32,hash,threads);
    HashSparse hp;hp.open_ro(hash,32,2*text.n/32+1);
    require(::unlink(hash.c_str())==0,"unlink open radix hash scratch");
    Budget budget;Keys keys;keys.init(&text,&hp,text.n,true,&budget);
    SequentialV2 output(prefix,text,text.n,text.n);
    output.beginShard(0,0,text.n);RadixWork work;
    radix_order(text,keys,N,cap,work,[&](U row,U p){output.row(row,p);});
    output.endShard();std::error_code ec;fs::remove(output.dir,ec);
    std::fprintf(stderr,"RADIX_WORK N=%u sweep_bytes=%llu sweeps=%llu prefix_ops=%llu key_ops=%llu key_bytes=%llu singleton=%llu residue=%llu comparisons=%llu symbols=%llu probes=%llu segments=%llu selection_passes=%llu position_cap=%llu peak_position_slots=%llu peak_position_record_bytes=%llu full_SA=0 persistent_positions=0\n",
        N,(unsigned long long)work.sweepBytes,(unsigned long long)work.sweeps,(unsigned long long)(work.sweeps*text.n),(unsigned long long)work.keyOps,(unsigned long long)work.keyBytes,
        (unsigned long long)work.singleton,(unsigned long long)work.residue,(unsigned long long)budget.comparisons,(unsigned long long)budget.symbols,(unsigned long long)budget.probes,
        (unsigned long long)work.segments,(unsigned long long)work.selectionPasses,(unsigned long long)work.cap,(unsigned long long)work.peakPositions,(unsigned long long)(work.peakPositions*sizeof(RadixRecord)));
    return 0;
}

#include "binbwt_slice.h"

int main_impl(int argc, char** argv) {
    std::signal(SIGABRT, abortHandler);
    G_T0 = now_sec();
    // Cap glibc malloc arenas: every worker thread's arena reserves 64 MB of
    // address space, and the wide phases (48 hash threads + k anchor sides +
    // pool workers) reserved ~5 GB of pure arena VSZ - VmPeak 7.9 GB against
    // a 6 GiB fragment-gate RLIMIT_AS (std::bad_alloc, gate 3 first attempt).
    // 16 arenas keep parallel allocation wide without the blowup.
    mallopt(M_ARENA_MAX, 16);
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
    if (has("--radix")) return radix_main(flag("--radix"),flag("--remap"),flag("--out-prefix"),
        flag("--radix-bytes").empty()?8:std::stoul(flag("--radix-bytes")),
        flag("--position-cap").empty()?32000000:std::stoull(flag("--position-cap")),threads);
    if (has("--binbwt")) return binbwt_main(flag("--binbwt"),flag("--remap"),flag("--out-prefix"),
        threads,
        flag("--bin-leaf").empty()?24:std::stoull(flag("--bin-leaf")),
        flag("--bin-depth-cap").empty()?16:std::stoull(flag("--bin-depth-cap")),
        flag("--bin-live-cap").empty()?U(1)<<32:std::stoull(flag("--bin-live-cap")),
        flag("--bin-level-bytes").empty()?1:std::stoull(flag("--bin-level-bytes")),
        flag("--bin-mode")!="cyclic", has("--bin-freq-sched"),
        flag("--bin-split").empty()?0:std::stoull(flag("--bin-split")),
        flag("--bin-sink")=="serial");
    U kway = 2;
    if (!flag("--kway").empty()) {
        kway = std::stoull(flag("--kway"));
        require(kway >= (has("--selftest") ? 1u : 2u) && kway < 1000000, "--kway must be >= 2 (selftest permits 1)");
    }
    if (has("--selftest")) {
        U cases = flag("--selftest").empty() ? 2000 : std::stoull(flag("--selftest"));
        uint64_t seed = flag("--seed").empty() ? 20261002 : std::stoull(flag("--seed"));
        return selftest(cases, seed, threads, kway);
    }
    if (has("--pair")) {
        std::string L = flag("--pair"), R = flag("--right");
        require(!L.empty() && !R.empty(), "--pair needs LEFT and --right RIGHT");
        if (has("--sxcr")) {
            require(!fs::exists(flag("--sxcr")) && !fs::exists(flag("--sxcr") + ".sxs"),
                    "output exists; refusing to clobber");
            merge_kway_files({L, R}, false, flag("--sxcr"), threads);
        } else {
            require(has("--out-prefix"), "--pair needs --sxcr OUT or --out-prefix PREFIX");
            merge_kway_files({L, R}, true, flag("--out-prefix"), threads, has("--emit-pf"));
        }
        return 0;
    }
    if (has("--finalize")) {
        require(has("--out-prefix"), "--finalize needs --out-prefix PREFIX");
        finalize_file(flag("--finalize"), flag("--out-prefix"), threads, has("--emit-pf"));
        return 0;
    }
    // Driver: CHUNK_DIR COUNT TOTAL_BYTES OUT_PREFIX [--work DIR] [--serial]
    std::string dir, prefix, work;
    U count = 0, total = 0;
    bool serial = has("--serial");
    auto valued = [&](const std::string& a) {
        return a == "--threads" || a == "--work" || a == "--seed" || a == "--pair" ||
               a == "--right" || a == "--sxcr" || a == "--out-prefix" || a == "--finalize" ||
               a == "--selftest" || a == "--kway";
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
            "       cross_lcp_merge ... [--kway K]   (merge arity: 2 = pairwise tree, the default;\n"
            "                                       count = FLAT one-level, the primary shape)\n"
            "       cross_lcp_merge --pair LEFT --right RIGHT (--sxcr OUT | --out-prefix PREFIX)\n"
            "       cross_lcp_merge --finalize ONE --out-prefix PREFIX\n"
            "       cross_lcp_merge --selftest CASES [--seed N] [--kway K]");
    if (v2_pieces_enabled())
        require(count <= kway && (!serial || count <= 2),
                "WM-free v2 currently requires a single final fan-in; multilevel/serial intermediates are unsupported");
    struct Part { std::string file; U offset, n, version = 0; };
    std::vector<Part> parts;
    for (U i = 0; i < count; ++i) {
        Part p{dir + "/chunk-" + std::to_string(i) + ".crle", 0, 0};
        U runs = 0; sxcr_header(p.file, p.offset, p.n, runs, p.version);
        if (i) require(parts[i - 1].offset + parts[i - 1].n == p.offset, "chunks do not tile");
        parts.push_back(p);
    }
    require(parts[0].offset == 0 && parts.back().offset + parts.back().n == total,
            "chunk coverage does not match total");
    work = flag("--work");
    if (work.empty()) work = (fs::path(prefix).parent_path() / "cross-lcp-work").string();
    fs::create_directories(work);
    if (count == 1) {
        finalize_file(parts[0].file, prefix, threads, has("--emit-pf"));
        return 0;
    }
    if (serial) {
        Part acc = parts[0];
        for (U i = 1; i < count; ++i) {
            bool final = i + 1 == count;
            std::string out = final ? prefix : work + "/serial-" + std::to_string(i) + ".crle";
            if (!final)
                cross_announce_next(acc.file, parts[i].file, out);   // FIX 4 lookahead
            merge_kway_files({acc.file, parts[i].file}, final, out, threads, final && has("--emit-pf"), final);
            acc = {out, acc.offset, acc.n + parts[i].n};
        }
        return 0;
    }
    // Merge topology: --kway K (default 2 = the certified pairwise tree).
    // K = count is FLAT: one k-way merge, dollar mode, straight from the raw
    // chunks (the primary shape: one read pass + one write pass, full width
    // from minute one, boundaries double as restart checkpoints). The
    // pairwise path is the k=2 case of the ONE k-way core.
    U level = 0;
    while (parts.size() > 1) {
        std::vector<Part> next;
        for (size_t j = 0; j < parts.size(); j += kway) {
            size_t g = std::min<size_t>(kway, parts.size() - j);
            if (g == 1) { next.push_back(parts[j]); continue; }
            bool final = parts.size() <= kway && j == 0;
            std::string out = final ? prefix
                : work + "/L" + std::to_string(level) + "-" + std::to_string(j / kway) + ".crle";
            if (kway == 2 && j + 3 < parts.size())
                cross_announce_next(parts[j + 2].file, parts[j + 3].file,
                                    work + "/L" + std::to_string(level) + "-" +
                                        std::to_string(j / 2 + 1) + ".crle");   // FIX 4 lookahead (same level)
            std::vector<std::string> ins;
            U nn = 0;
            for (size_t z = 0; z < g; ++z) { ins.push_back(parts[j + z].file); nn += parts[j + z].n; }
            merge_kway_files(ins, final, out, threads, final && has("--emit-pf"), final);
            // Delete-as-you-go: this group's inputs are consumed. Skipped with
            // the opt-in preflight pipeline, which may still be reading them.
            if (getenv("CROSS_DELETE_CONSUMED") && !getenv("CROSS_PIPELINE"))
                for (const auto& f : ins) delete_consumed_side(f);
            next.push_back({out, parts[j].offset, nn});
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
