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
#include <deque>
#include <memory>
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
// slot word on cmpxchg16b; validated by the pool-verification lane):
//   word128 = (page+1) << 67 | bufId << 66 | ver(64 bits) << 2 | state
//   state: 0 invalid, 1 filling, 2 ready. Two 256-byte buffers per slot.
//   claim  : CAS128( cur -> (page, flip(cur.buf), cur.ver+1, FILLING) )
//   publish: CAS128( myClaim -> (page, myBuf, myVer+1, READY) )  (atomic with the bump)
//   reader : atomic 128-bit load; if (page, ready): memcpy buffer[bufId];
//            atomic reload; accept iff the whole word is unchanged.
// Every transition is one atomic instruction, which closes all five verified
// races by construction: tag/state split, version wrap (64-bit: 2^64 claims
// per slot is unreachable — a claim issues an NVMe pread, so a slot sustains
// at most ~1e5-1e6 claims/s and 2^64 needs >= ~5.8e5 years), stale CAS
// re-match (the full 64-bit ver is inside every expectation), the mid-fill
// orphan write (the claim flips the buffer ATOMICALLY: a superseded fill's
// pread lands in a buffer no live word references; a thread completes its
// pread before it can claim again, so two writers never share a buffer),
// and the publish flash window (the ready bit appears in the same atomic
// transition as its version bump).
// Bounded model-checking verdicts (bit6/sxi_logs/pool-verification): two
// fillers, one slot: CLEAN at depths 2..96 (10.7M states at 96); two
// readers/two fillers: CLEAN at depths 12/16; two slots/four pages: CLEAN
// at depth 16. The three pre-fix protocols still reproduce their
// counterexamples under the same enumerator (calibration), and the exact
// 11-step pre-fix orphan schedules replay clean.
struct SharedPages {
    static constexpr U PS = 256;              // page size (bytes)
    static constexpr U SLOTS = 1u << 20;      // 1 Mi slots = 512 MiB (2 bufs/slot)
    struct Slot {
        alignas(16) unsigned __int128 w;      // raw 128-bit word via cmpxchg16b builtins
        std::unique_ptr<uint8_t[]> data[2];
        Slot() : w(0) {}
    };
    int fd = -1; U bytes = 0, pages = 0;
    std::unique_ptr<Slot[]> slots;
    static constexpr U QCAP = 1u << 16;       // prefetch ring capacity
    std::unique_ptr<std::atomic<U>[]> queue;
    std::atomic<U> qh{0}, qt{0};
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
    std::atomic<U> nFills{0}, nHits{0}, nMiss{0}, nTorn{0};
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
        slots.reset(new Slot[SLOTS]);
        for (U z = 0; z < SLOTS; ++z) {
            slots[z].data[0].reset(new uint8_t[PS]);
            slots[z].data[1].reset(new uint8_t[PS]);
        }
        queue.reset(new std::atomic<U>[QCAP]);
        for (U t = 0; t < nthreads; ++t)
            workers.emplace_back([this] { worker(); });
    }
    void stop() {
        stopping = true;
        for (auto& w : workers) if (w.joinable()) w.join();
        workers.clear();
        std::fprintf(stderr, "POOL_STATS fd=%d bytes=%llu fills=%llu hits=%llu misses=%llu torn=%llu\n",
                     fd, (unsigned long long)bytes,
                     (unsigned long long)nFills.load(), (unsigned long long)nHits.load(),
                     (unsigned long long)nMiss.load(), (unsigned long long)nTorn.load());
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
        unsigned __int128 cur = __atomic_load_n(&sl.w, __ATOMIC_ACQUIRE);
        if (pageOf(cur) == page && stateOf(cur) == ST_READY) return;   // advisory: ready
        // Claim: CAS128 from the latched word. The expectation carries the
        // full 64-bit version (a stale latch can never re-match) and the
        // buffer flip is part of the same atomic transition.
        unsigned __int128 mine = packW(page, 1 - bufOf(cur), verOf(cur) + 1, ST_FILLING);
        if (__atomic_compare_exchange_n(&sl.w, &cur, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            U myBuf = bufOf(mine);
            U off = page * PS;
            U need = std::min(PS, bytes - off);
            xw(fd, sl.data[myBuf].get(), need, off, "shared page fill");
            nFills.fetch_add(1, std::memory_order_relaxed);
            // Publish: CAS128 from OUR claim (atomic with the ver bump).
            unsigned __int128 ready = packW(page, myBuf, verOf(mine) + 1, ST_READY);
            if (__atomic_compare_exchange_n(&sl.w, &mine, ready, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                // our page is live
            } else {
                nTorn.fetch_add(1, std::memory_order_relaxed);   // stolen mid-fill: disowned
            }
        }
    }
    inline bool try_page(U page, const uint8_t*& out, unsigned __int128& stamp) {
        if (page >= pages) return false;
        Slot& sl = slots[slotOf(page)];
        stamp = __atomic_load_n(&sl.w, __ATOMIC_ACQUIRE);      // one atomic 128-bit load
        if (pageOf(stamp) != page || stateOf(stamp) != ST_READY) return false;
        out = sl.data[bufOf(stamp)].get();
        return true;
    }
    inline bool try_page_recheck(U page, unsigned __int128 stamp) {
        if (page >= pages) return false;
        Slot& sl = slots[slotOf(page)];
        return __atomic_load_n(&sl.w, __ATOMIC_ACQUIRE) == stamp;   // unchanged => untorn
    }
    inline void prefetch(U page) {
        if (page >= pages) return;
        unsigned __int128 w = __atomic_load_n(&slots[slotOf(page)].w, __ATOMIC_ACQUIRE);
        if (pageOf(w) == page && stateOf(w) == ST_READY) return;
        U t = qt.load(std::memory_order_relaxed);
        U h = qh.load(std::memory_order_acquire);
        if (t - h >= QCAP - 2) return;             // ring full: drop
        queue[t & (QCAP - 1)].store(page, std::memory_order_relaxed);
        qt.store(t + 1, std::memory_order_release);
    }
    void worker() {
        unsigned idle = 0;
        for (;;) {
            U h = qh.load(std::memory_order_acquire);
            U t = qt.load(std::memory_order_relaxed);
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
            qh.store(h + 1, std::memory_order_release);
            fill(page);
        }
    }
};

static std::atomic<U> g_cellHits{0}, g_cellMiss{0};   // grid-cell cache telemetry

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
        if (pooled && !getenv("CROSS_NO_POOL")) {
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
            const uint8_t* p; unsigned __int128 stamp;
            if (pool->try_page(page, p, stamp)) {
                U z = off & (SharedPages::PS - 1);
                uint8_t v = p[z];
                if (pool->try_page_recheck(page, stamp)) {
                    pool->nHits.fetch_add(1, std::memory_order_relaxed);
                    return v;   // seqlock clean
                }
                pool->nTorn.fetch_add(1, std::memory_order_relaxed);
                // torn: fall through to the synchronous path
            }
            pool->nMiss.fetch_add(1, std::memory_order_relaxed);
            if (!getenv("CROSS_ASYNC_FILL")) pool->fill(page);   // default: synchronous fill (deterministic mode)
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
            const uint8_t* p; unsigned __int128 stamp;
            if (pool->try_page(page, p, stamp)) {
                U z = off & (SharedPages::PS - 1);
                std::memcpy(out, p + z, len);
                if (pool->try_page_recheck(page, stamp)) return;   // seqlock clean
            }
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

// Cached u64-column reader with byte base and optional constant shift.
struct WinU64 {
    int fd = -1; U base = 0, count = 0, shift = 0, gen = 0;
    std::shared_ptr<SharedPages> pool;
    ~WinU64() { pool.reset(); if (fd >= 0) ::close(fd); }
    void open_ro(const std::string& path, U elems, U byteBase, U shiftIn = 0, bool pooled = false) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) fail("open windowed u64 file");
        base = byteBase; count = elems; shift = shiftIn; gen = g_winGen.fetch_add(1);
        if (pooled && !getenv("CROSS_NO_POOL")) {
            pool = std::make_shared<SharedPages>();
            pool->start(fd, base + 8 * elems, 8);
        }
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
        if (pool) {
            U off = base + 8 * i;
            U page = off / SharedPages::PS;
            const uint8_t* p; unsigned __int128 stamp;
            if (pool->try_page(page, p, stamp)) {
                U z = off & (SharedPages::PS - 1);
                std::memcpy(&val, p + z, 8);
                if (pool->try_page_recheck(page, stamp)) {
                    return val + shift;
                }
            }
            if (!getenv("CROSS_ASYNC_FILL")) pool->fill(page);   // default: synchronous fill (deterministic mode)
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
    void read(U i, U* buf, U len) const {
        if (i + len > count) fail("windowed u64 bulk bounds");
        xw(fd, buf, 8 * len, base + 8 * i, "windowed u64 bulk");
        if (shift) for (U k = 0; k < len; ++k) buf[k] += shift;
    }
    void prefetch(U i) const {
        if (!pool) return;
        pool->prefetch((base + 8 * i) / SharedPages::PS);
    }
};
// Overlay of repaired slices over an immutable position column: the repair
// writes each sorted block slice sequentially and registers [lo,hi) with its
// file offset. Anchor blocks NEST (the backward-search walk shrinks the row
// interval as the suffix grows), so a later region overrides an earlier one
// for shared rows — matching the resident in-place semantics exactly.
// Region count per side is bounded by the walk depth (small); lookups scan
// backward, last pushed first.
struct PosOverlay {
    struct Region { U lo, hi, off; };
    WinU64 src;
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
    void prefetch_for(const WinBytes& m2, U a, U len) const {
        if (!col.pool) return;
        U ja = (a + k - 1) / k, je = (a + len) / k;
        col.prefetch(ja); col.prefetch(je);
        U hl = ja * k > a ? ja * k - a : 0;
        U tl = a + len > je * k ? a + len - je * k : 0;
        if (hl) m2.prefetch(a, hl);
        if (tl) m2.prefetch(je * k, tl);
    }
};
static U fold_bytes(const WinBytes& m2, U a, U e) {   // fold(a,e), first byte at B^(e-a-1)
    U h = 0;
    for (U i = a; i < e; ++i) h = h * HASH_BASE + ((U)m2.at(i) + 1);
    return h;
}
// Window prefix-hash F(a,e) = fold(M2[a..e)) from sparse anchors + tails:
//   ja = ceil(a/k), je = floor(e/k)
//   F = head*B^(e-ja*k) + mid*B^(e-je*k) + tail   (mid = P[je]-P[ja]*B^((je-ja)*k))
static U win_hash(const WinBytes& m2, const HashSparse& hp, U a, U e) {
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

// ---------------------------------------------------------------- fingerprints
struct Keys {
    const WinBytes* m2 = nullptr;   // windowed M.M, 2n bytes
    const HashSparse* hp = nullptr; // k-spaced prefix hashes over m2
    U n = 0;
    bool dollar = false;           // final $-suffix mode vs cyclic (M^infty) mode
    Budget* bud = nullptr;

    void init(const WinBytes* m, const HashSparse* h, U len, bool dol, Budget* b) {
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
    int cmp(U p1, U p2) const {
        require(p1 != p2 && p1 < n && p2 < n, "comparator bounds");
        ++bud->comparisons;
        U cap = dollar ? std::min(n - p1, n - p2) : n;
        U fast = std::min<U>(cap, 16), j = 0;
        double t0 = now_sec();
        {
            uint8_t ab[16], bb[16];
            m2->at_span(p1, ab, fast);
            m2->at_span(p2, bb, fast);
            for (; j < fast; ++j) {
                if (ab[j] != bb[j]) {
                    bud->symbols += j + 1; ++bud->fast_decided; bud->note_lce(j);
                    bud->tFast += now_sec() - t0;
                    return ab[j] < bb[j] ? -1 : 1;
                }
            }
        }
        bud->tFast += now_sec() - t0;
        bud->symbols += j;
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
            uint8_t hb[64];
            m2->at_span(sc.p, hb, hlen);
            U h = 0;
            for (U z = 0; z < hlen; ++z) h = h * HASH_BASE + ((U)hb[z] + 1);
            sc.h = h;
            Cell& c = cells[&sc == &tscr[0] ? 0 : 1][sc.aq & 4095];
            if (c.aq == sc.aq && c.gen == hsGen) { sc.pj = c.pj; g_cellHits.fetch_add(1, std::memory_order_relaxed); }
            else {
                sc.pj = hp->P(sc.aq + 1);
                c.aq = sc.aq; c.gen = hsGen; c.pj = sc.pj;
                g_cellMiss.fetch_add(1, std::memory_order_relaxed);
            }
            sc.ready = true;
        };
        double t1 = now_sec();
        if (fast / hp->k < cap / hp->k) {   // lazy: degenerate grids (cap<k) scan directly
            prep(p1, tscr[0]); prep(p2, tscr[1]);
        }
        bud->tPrep += now_sec() - t1;
        auto winFold = [&](SideScr& sc, U m) {         // fold(p, p + m*k)
            materialize(sc);
            U jb = sc.aq + m;
            U mid = (m <= 1) ? 0 : (hp->P(jb) - sc.pj * upow((m - 1) * hp->k));
            U tail = 0;
            if (sc.r) {
                uint8_t tb[64];
                U tlen = std::min<U>(sc.r, 64);
                m2->at_span(jb * hp->k, tb, tlen);
                for (U i = 0; i < tlen; ++i) tail = tail * HASH_BASE + ((U)tb[i] + 1);
                for (U i = 64; i < sc.r; ++i)   // k<=64 enforced by the clamp
                    tail = tail * HASH_BASE + ((U)m2->at(jb * hp->k + i) + 1);
            }
            return sc.h * upow((m - 1) * hp->k + sc.r) + mid * upow(sc.r) + tail;
        };
        auto eqm = [&](U m) { ++bud->probes; return winFold(tscr[0], m) == winFold(tscr[1], m); };
        U k = hp->k;
        double t2 = now_sec();
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
        bud->tProbe += now_sec() - t2;
        // Exact boundary: scan from the certified prefix to the next grid
        // point (or cap when the grid is exhausted). This scan is the
        // verification of every hash claim below it.
        double t3 = now_sec();
        U scanEnd = (lo >= mMax || (lo + 1) * k > cap) ? cap : (lo + 1) * k;
        U i = fast;
        {
            uint8_t ab[64], bb[64];
            while (i < scanEnd) {
                U w = std::min<U>(64, scanEnd - i);
                m2->at_span(p1 + i, ab, w);
                m2->at_span(p2 + i, bb, w);
                U z = 0;
                while (z < w && ab[z] == bb[z]) ++z;
                i += z;
                if (z < w) break;
            }
        }
        bud->symbols += i - fast;
        bud->tScan += now_sec() - t3;
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

// k-spaced prefix hashes over the windowed M2 FILE: P[j] = fold(M2[0..j*k)).
// Parallel 3-pass (relative entries in place, absolute range starts, fix-up)
// over a shared scratch file; verified at boundaries like the dense build was.
static U hash_spacing() {
    const char* e = getenv("CROSS_HASH_K");
    U k = e ? std::stoull(e) : 8;
    require(k >= 1 && k <= 4096, "CROSS_HASH_K out of range");
    return k;
}
static void build_hash_sparse(const std::string& m2Path, U n2, U k,
                              const std::string& hsPath, U threads) {
    U entries = n2 / k + 1;
    int fd = ::open(hsPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    require(fd >= 0, "create sparse hash file");
    if (ftruncate(fd, 8 * entries) != 0) fail("truncate sparse hash file");
    U tcount = threads;
    if (n2 < (1u << 20) || tcount <= 1) tcount = 1;
    U segE = (entries + tcount - 1) / tcount;
    std::vector<U> relend(tcount, 0);
    WinBytes m2; m2.open_ro(m2Path, n2, 0);
    std::vector<std::thread> ts;
    auto rel = [&](U t) {
        U j0 = t * segE, j1 = std::min(entries - 1, j0 + segE);
        if (j1 <= j0) { if (j0 < entries) relend[t] = 0; return; }
        // Chunked: a per-thread range buffer would be O(entries/threads) —
        // hundreds of MB per thread at pile scale. Fold and flush in
        // 1 Mi-entry chunks instead (8 MiB per thread, bounded).
        constexpr U CHUNK = 1u << 20;
        std::vector<U> buf(CHUNK);
        U h = 0;
        U jc = j0;
        while (jc < j1) {
            U je = std::min(j1, jc + CHUNK);
            for (U j = jc; j < je; ++j) {
                buf[j - jc] = h;
                U e = std::min(n2, (j + 1) * k);
                for (U i = j * k; i < e; ++i) h = h * HASH_BASE + ((U)m2.at(i) + 1);
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
    // must equal a direct recomputation over the windowed text.
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
// A side is either an on-disk SXS2 sidecar (all intermediates) or a raw SXCR
// chunk, which is walked once (chunk-scale resident transient) and dumped to a
// scratch sidecar; from there every consumer is windowed.
struct SideExt {
    U offset = 0, n = 0, runs = 0, period = 0; // period==n means aperiodic
    std::string sxsPath;       // the sidecar actually used
    bool ownsSxs = false;      // we wrote it (scratch; unlink at pair end)
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
    require(rd_le(f, 4) == 2, "unsupported SXCR version (this build reads the widened v2: 64-bit run lens; v1 pre-widening chunks are refused)");
    offset = rd_le(f, 8); n = rd_le(f, 8); runs = rd_le(f, 8);
    require(n && n < (1ull << 62) && runs, "invalid SXCR header");
    require(fs::file_size(path) == 32 + 25 * runs, "SXCR file size mismatch");
}


// Validate runs against the STREAMED (text,pos): preceding char per row and
// head/tail samples. Same contract as the resident build.
static void validate_runs_ext(const std::string& path, U n, U runs,
                              const WinBytes& text, const WinU64& pos) {
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
static U min_period_stream(const WinBytes& text, U n) {
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

static void walk_chunk(const std::string& path, U n, U runs, U threads,
                       std::vector<uint8_t>& text, std::vector<uint64_t>& pos, U& period) {
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


// Load a side into its STREAMED form (SXS2 sidecar path). Raw chunks are
// walked once (chunk-scale resident transient: text, bwt, lf, pos, visited)
// and dumped to a scratch sidecar; then the sidecar contract is checked the
// same way as for intermediates: positions, period, and run validation.
static void load_side_ext(const std::string& path, U threads,
                          const std::string& scratch, SideExt& s) {
    sxcr_header(path, s.offset, s.n, s.runs);
    std::string side = path + ".sxs";
    if (fs::exists(side)) {
        s.sxsPath = side; s.ownsSxs = false;
    } else {
        std::vector<uint8_t> text; std::vector<uint64_t> pos;
        walk_chunk(path, s.n, s.runs, threads, text, pos, s.period);
        s.sxsPath = scratch + "/" + fs::path(path).filename().string() + ".walked.sxs";
        s.ownsSxs = true;
        {
            OutFile f(s.sxsPath);
            char magic[4]{'S','X','S','2'}; f.raw(magic, 4);
            f.w64(s.offset); f.w64(s.n);
            f.raw(text.data(), s.n);
            f.raw(pos.data(), 8 * s.n);
        }
    }
    WinBytes text; text.open_ro(s.sxsPath, s.n, 20);
    if (s.period == 0) s.period = min_period_stream(text, s.n);
    {
        WinU64 pos; pos.open_ro(s.sxsPath, s.n, 20 + s.n);
        U buf[4096];
        for (U i = 0; i < s.n; i += 4096) {
            U len = std::min<U>(4096, s.n - i);
            pos.read(i, buf, len);
            for (U z = 0; z < len; ++z) require(buf[z] < s.n, "sidecar position out of range");
        }
        validate_runs_ext(path, s.n, s.runs, text, pos);
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
static void anchor_blocks_ext(const WinBytes& text, const WinBytes& bwt,
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

// ------------------------------------------------------------ external core merge
struct CoreStats { U anchorsA = 0, anchorsB = 0, rowsA = 0, rowsB = 0, stepsA = 0, stepsB = 0; };
struct PairScratch {
    std::string dir;
    std::string m2, hs, wm, bwtA, bwtB, ovA, ovB;
    void init(const std::string& out, U pid) {
        dir = out + ".xsc-" + std::to_string(pid);
        fs::create_directories(dir);
        m2 = dir + "/m2.bin"; hs = dir + "/hs.bin"; wm = dir + "/wm.bin";
        bwtA = dir + "/bwtA.bin"; bwtB = dir + "/bwtB.bin";
        ovA = dir + "/ovA.bin"; ovB = dir + "/ovB.bin";
    }
    void cleanup() { for (const char* f : {"m2.bin","hs.bin","wm.bin","bwtA.bin","bwtB.bin","ovA.bin","ovB.bin"})
            ::unlink((dir + "/" + f).c_str()); fs::remove_all(dir); }
};
// M2 file = T||T with T = textA||textB (streamed from the sidecars).
static void build_m2_file(const SideExt& A, const SideExt& B, const std::string& path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    require(fd >= 0, "create m2 scratch");
    U n = A.n + B.n;
    constexpr U W = 1u << 22;
    std::vector<uint8_t> buf(W);
    for (int copy = 0; copy < 2; ++copy) {
        for (int side = 0; side < 2; ++side) {
            const SideExt& S = side ? B : A;
            WinBytes t; t.open_ro(S.sxsPath, S.n, 20);
            for (U i = 0; i < S.n;) {
                U len = std::min<U>(W, S.n - i);
                t.read(i, buf.data(), len);
                xput(fd, buf.data(), len, (U)copy * n + (side ? A.n : 0) + i, "m2 write");
                i += len;
            }
        }
    }
    ::close(fd);
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
        WinBytes text; text.open_ro(S.sxsPath, S.n, 20);
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
// Externalized core merge: M2/sparse-hash/BWT/WM are scratch FILES; the
// children's pos are windowed with a repair overlay; per-position RAM is
// limited to window caches, the rank index, and bounded block buffers.
static void core_merge_ext(const SideExt& A, const SideExt& B, bool dollar, U threads,
                           Budget& bud, CoreStats& st, const PairScratch& sc, U hashK, bool pre) {
    double last = now_sec();
    U nA = A.n, nB = B.n, n = nA + nB;
    if (!pre) {
        build_m2_file(A, B, sc.m2);
        phase("m2-build", last);
        hashK = hash_spacing();
        if (hashK > n / 2) hashK = std::max<U>(1, n / 2);   // degenerate-grid guard
        build_hash_sparse(sc.m2, 2 * n, hashK, sc.hs, threads);
        phase("hash-build", last);
    }
    WinBytes m2; m2.open_ro(sc.m2, 2 * n, 0, true);
    HashSparse hp; hp.open_ro(sc.hs, hashK, 2 * n / hashK + 1);
    Keys keys; keys.init(&m2, &hp, n, dollar, &bud);
    PosOverlay posA, posB;
    posA.init(A.sxsPath, nA, 20 + nA, 0, sc.ovA);
    posB.init(B.sxsPath, nB, 20 + nB, nA, sc.ovB);
    // Anchor blocks per side, then repair.
    std::vector<BlockRec> blocks;
    std::vector<uint64_t> anchors;
    build_bwt_file(A, posA, sc.bwtA, threads, 0);
    { WinBytes bwt; bwt.open_ro(sc.bwtA, nA, 0);
      WinBytes textA; textA.open_ro(A.sxsPath, nA, 20);
      anchor_blocks_ext(textA, bwt, posA, nA, 0, A.period, blocks, anchors, st.stepsA, st.rowsA); }
    st.anchorsA = anchors.size();
    repair_ext(posA, blocks, anchors, 0, nA, keys);
    blocks.clear(); anchors.clear();
    build_bwt_file(B, posB, sc.bwtB, threads, nA);
    { WinBytes bwt; bwt.open_ro(sc.bwtB, nB, 0);
      WinBytes textB; textB.open_ro(B.sxsPath, nB, 20);
      anchor_blocks_ext(textB, bwt, posB, nB, nA, B.period, blocks, anchors, st.stepsB, st.rowsB); }
    st.anchorsB = anchors.size();
    repair_ext(posB, blocks, anchors, nA, nB, keys);
    phase("anchor+repair", last);
    // Linear cross merge under the total order, streamed into the WM file.
    int fd = ::open(sc.wm.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    require(fd >= 0, "create wm scratch");
    double tPF = 0, tPop = 0;
    {
        constexpr U W = 8192;
        U buf[W]; U nb = 0; U written = 0;
        auto flushW = [&]() { if (nb) { xput(fd, buf, 8 * nb, 8 * written, "wm write"); written += nb; nb = 0; } };
        U i = 0, j = 0, k = 0;
        while (i < nA && j < nB) {
            double q0 = now_sec();
            if (m2.pool && (k & 31) == 0 && !getenv("CROSS_NO_PF")) {
                // Front speculation: the next 32 fronts of both streams
                // (deduplicated inside prefetch against owned slots).
                // FIX 3: also prefetch the m2 text spans AND the sparse-hash
                // anchor entries those fronts will touch (the head span is
                // the fast-path page; the first gallop probe reads P at
                // p/k + fast/k + 1, so that entry goes too).
                U firstJb = 16 / hashK + 1;
                for (U d = 1; d <= 32 && i + d < nA; ++d) {
                    U p = posA.at(i + d);
                    m2.prefetch(p, 16);
                    if (hp.col.pool) hp.col.prefetch(p / hashK + firstJb);
                }
                for (U d = 1; d <= 32 && j + d < nB; ++d) {
                    U p = posB.at(j + d);
                    m2.prefetch(p, 16);
                    if (hp.col.pool) hp.col.prefetch(p / hashK + firstJb);
                }
            }
            tPF += now_sec() - q0;
            double q1 = now_sec();
            U va = posA.at(i), vb = posB.at(j);
            tPop += now_sec() - q1;
            if (keys.cmp(va, vb) < 0) { buf[nb++] = va; ++i; }
            else { buf[nb++] = vb; ++j; }
            if (nb == W) flushW();
            ++k;
        }
        while (i < nA) { buf[nb++] = posA.at(i++); if (nb == W) flushW(); ++k; }
        while (j < nB) { buf[nb++] = posB.at(j++); if (nb == W) flushW(); ++k; }
        flushW();
        require(k == n && written == n, "merge coverage");
    }
    ::close(fd);
    std::fprintf(stderr, "XMERG_TIMING t_pf=%.3f t_pop=%.3f cell_hits=%llu cell_miss=%llu\n",
                 tPF, tPop, (unsigned long long)g_cellHits.load(), (unsigned long long)g_cellMiss.load());
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
// All emitters read the M2/WM scratch files through windows: nothing
// per-position is resident.
static void emit_pf_side_ext(const std::string& prefix, const WinBytes& m2, U n, U final_runs) {
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

// Enqueue, one band ahead, the M2 pages a coming band of WM rows will read.
// four=true: row t reads M[WM[t-10]-1]; else M[WM[t]+n-1].
static void prefetch_wm_rows(const WinBytes& m2, const WinU64& wm, U t0, U t1, U n, bool four) {
    if (!m2.pool) return;
    if (four) {
        for (U t = std::max<U>(10, t0); t < t1; ++t) {
            U p = wm.at(t - 10);
            if (p) m2.prefetch(p - 1, 1);
        }
    } else {
        for (U t = t0; t < t1; ++t) m2.prefetch(wm.at(t) + n - 1, 1);
    }
}

// Final four files, read from the M2 and WM scratch files (sequential WM).
static U emit_four_ext(const std::string& prefix, const WinBytes& m2,
                       const WinU64& wm, U n) {
    double last = now_sec();
    U total = n + 10;
    auto row_char_sample = [&](U t, uint8_t& c, U& sample) {
        if (t < 10) { sample = n + t; c = t == 0 ? m2.at(n - 1) : uint8_t(2); }
        else { U p = wm.at(t - 10); sample = p; c = p == 0 ? uint8_t(2) : m2.at(p - 1); }
    };
    std::array<U, 256> counts{}, rc{};
    U final_runs = 0;
    {
        uint8_t prev = 0xff; bool any = false;
        prefetch_wm_rows(m2, wm, 0, std::min(total, U(4096)), n, true);
        for (U t = 0; t < total; ++t) {
            if ((t & 4095) == 0)
                prefetch_wm_rows(m2, wm, t + 4096, std::min(total, t + 8192), n, true);
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
        prefetch_wm_rows(m2, wm, 0, std::min(total, U(4096)), n, true);
        for (U t = 0; t < total; ++t) {
            if ((t & 4095) == 0)
                prefetch_wm_rows(m2, wm, t + 4096, std::min(total, t + 8192), n, true);
            uint8_t c; U s; row_char_sample(t, c, s);
            if (!any || c != cur) { flush(); cur = c; runlen = 0; head = s; any = true; }
            ++runlen; tail = s;
        }
        flush();
    }
    phase("emit-write", last);
    return final_runs;
}

// Intermediate: merged cyclic order as an SXCR chunk + sidecar, read from
// the M2 and WM scratch files.
static U emit_sxcr_ext(const std::string& path, const WinBytes& m2,
                       const WinU64& wm, U n, U offset) {
    double last = now_sec();
    U runs = 0;
    prefetch_wm_rows(m2, wm, 0, std::min(n, U(4096)), n, false);
    for (U t = 0; t < n; ++t) {
        if ((t & 4095) == 0)
            prefetch_wm_rows(m2, wm, t + 4096, std::min(n, t + 8192), n, false);
        uint8_t c = m2.at(wm.at(t) + n - 1);
        if (t == 0 || c != m2.at(wm.at(t - 1) + n - 1)) ++runs;
    }
    phase("sxcr-count", last);
    {
        OutFile f(path);
        char magic[4]{'S','X','C','R'}; f.raw(magic, 4);
        f.w32(2); f.w64(offset); f.w64(n); f.w64(runs);
        uint8_t cur = 0; U runlen = 0, head = 0, tail = 0; bool any = false;
        auto flush = [&]() {
            if (!any) return;
            f.w8(cur); f.w64(runlen); f.w64(head); f.w64(tail);
        };
        prefetch_wm_rows(m2, wm, 0, std::min(n, U(4096)), n, false);
        for (U t = 0; t < n; ++t) {
            if ((t & 4095) == 0)
                prefetch_wm_rows(m2, wm, t + 4096, std::min(n, t + 8192), n, false);
            U p = wm.at(t); uint8_t c = m2.at(p + n - 1);
            if (!any || c != cur) { flush(); cur = c; runlen = 0; head = p; any = true; }
            ++runlen; tail = p;
        }
        flush();
    }
    phase("sxcr-write", last);
    {
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
    phase("sidecar-write", last);
    return runs;
}

// ------------------------------------------------------------ node pipeline
// FIX 4 (node-level pipeline, serial-compatible): while pair k runs, ONE
// preflight thread builds pair k+1's load/m2/hash phases into that pair's
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
        pf->sc.init(pf->out, U(getpid()));
        load_side_ext(pf->left, 48, pf->sc.dir, pf->A);
        load_side_ext(pf->right, 48, pf->sc.dir, pf->B);
        require(pf->A.offset + pf->A.n == pf->B.offset, "pair chunks do not tile");
        U n = pf->A.n + pf->B.n;
        build_m2_file(pf->A, pf->B, pf->sc.m2);
        pf->hashK = hash_spacing();
        if (pf->hashK > n / 2) pf->hashK = std::max<U>(1, n / 2);
        build_hash_sparse(pf->sc.m2, 2 * n, pf->hashK, pf->sc.hs, 48);
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

// ---------------------------------------------------------------- pair merge
static U merge_pair_files(const std::string& leftPath, const std::string& rightPath,
                          bool dollar, const std::string& out, U threads, bool emitPf = false) {
    if (dollar)
        for (const char* ext : {".rlebwt", ".rlebwt.meta", ".ssa", ".ssa_t"})
            require(!fs::exists(out + ext), "output exists; refusing to clobber");
    else
        require(!fs::exists(out) && !fs::exists(out + ".sxs"), "output exists; refusing to clobber");
    double t0 = now_sec();
    Budget bud; CoreStats st;
    bool pre = false;
    PairScratch sc;
    SideExt A, B;
    U hashK = 0;
    cross_drop_stale_preflights(leftPath, rightPath);
    if (auto pf = cross_take_preflight(leftPath, rightPath)) {
        // FIX 4: this pair's loads/m2/hash were prefetched during the
        // previous pair's run.
        if (!pf->ok) fail(("preflight failed: " + pf->err).c_str());
        A = pf->A; B = pf->B; sc = std::move(pf->sc); hashK = pf->hashK;
        pre = true;
    } else {
        sc.init(out, U(getpid()));
        load_side_ext(leftPath, threads, sc.dir, A);
        load_side_ext(rightPath, threads, sc.dir, B);
        require(A.offset + A.n == B.offset, "pair chunks do not tile");
    }
    core_merge_ext(A, B, dollar, threads, bud, st, sc, hashK, pre);
    U n = A.n + B.n;
    WinBytes m2; m2.open_ro(sc.m2, 2 * n, 0, true);
    WinU64 wm; wm.open_ro(sc.wm, n, 0);
    U out_runs = dollar ? emit_four_ext(out, m2, wm, n)
                        : emit_sxcr_ext(out, m2, wm, n, A.offset);
    if (dollar && emitPf) emit_pf_side_ext(out, m2, n, out_runs);
    sc.cleanup();
    double total = now_sec() - t0;
    std::printf("CROSS_PAIR mode=%s left=%s right=%s nA=%llu nB=%llu out=%s out_runs=%llu "
                "wall_total=%.3f comparisons=%llu symbols_compared=%llu probes=%llu max_lce=%llu "
                "fast_decided=%llu probe_decided=%llu cap_decided=%llu tie_decided=%llu "
                "lce_over_10k=%llu lce_over_100k=%llu lce_over_1m=%llu "
                "anchors_A=%llu anchors_B=%llu walk_steps_A=%llu walk_steps_B=%llu "
                "block_rows_A=%llu block_rows_B=%llu peak_rss_kib=%llu hash_k=%llu "
                "t_fast=%.3f t_prep=%.3f t_probe=%.3f t_scan=%.3f\n",
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
                (unsigned long long)peak_rss_kib(), (unsigned long long)hashK,
                bud.tFast, bud.tPrep, bud.tProbe, bud.tScan);
    std::fflush(stdout);
    return out_runs;
}

// Single-chunk finalize: cyclic order -> $ order, then the four files.
// M2 = T||T over the single side; the repaired order is the WM.
static void build_m2_single(const SideExt& A, const std::string& path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    require(fd >= 0, "create m2 scratch");
    WinBytes t; t.open_ro(A.sxsPath, A.n, 20);
    constexpr U W = 1u << 22;
    std::vector<uint8_t> buf(W);
    for (int copy = 0; copy < 2; ++copy)
        for (U i = 0; i < A.n;) {
            U len = std::min<U>(W, A.n - i);
            t.read(i, buf.data(), len);
            xput(fd, buf.data(), len, (U)copy * A.n + i, "m2 write");
            i += len;
        }
    ::close(fd);
}
static U finalize_file(const std::string& path, const std::string& prefix, U threads, bool emitPf = false) {
    if (1) for (const char* ext : {".rlebwt", ".rlebwt.meta", ".ssa", ".ssa_t"})
        require(!fs::exists(prefix + ext), "output exists; refusing to clobber");
    double t0 = now_sec(), last = t0;
    Budget bud; CoreStats st;
    PairScratch sc; sc.init(prefix, U(getpid()));
    SideExt A;
    load_side_ext(path, threads, sc.dir, A);
    phase("load", last);
    build_m2_single(A, sc.m2);
    U hashK = hash_spacing();
    if (hashK > A.n / 2) hashK = std::max<U>(1, A.n / 2);
    build_hash_sparse(sc.m2, 2 * A.n, hashK, sc.hs, threads);
    phase("hash-build", last);
    WinBytes m2; m2.open_ro(sc.m2, 2 * A.n, 0, true);
    HashSparse hp; hp.open_ro(sc.hs, hashK, 2 * A.n / hashK + 1);
    Keys keys; keys.init(&m2, &hp, A.n, true, &bud);
    PosOverlay posA;
    posA.init(A.sxsPath, A.n, 20 + A.n, 0, sc.ovA);
    build_bwt_file(A, posA, sc.bwtA, threads, 0);
    std::vector<BlockRec> blocks;
    std::vector<uint64_t> anchors;
    {
        WinBytes bwt; bwt.open_ro(sc.bwtA, A.n, 0);
        WinBytes text; text.open_ro(A.sxsPath, A.n, 20);
        anchor_blocks_ext(text, bwt, posA, A.n, 0, A.period, blocks, anchors, st.stepsA, st.rowsA);
    }
    st.anchorsA = anchors.size();
    repair_ext(posA, blocks, anchors, 0, A.n, keys);
    phase("anchor+repair", last);
    // WM = the repaired order, streamed from the overlay.
    {
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
    WinU64 wm; wm.open_ro(sc.wm, A.n, 0);
    U out_runs = emit_four_ext(prefix, m2, wm, A.n);
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
                (unsigned long long)bud.lce_over_1m, (unsigned long long)st.anchorsA,
                (unsigned long long)st.stepsA, (unsigned long long)st.rowsA,
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
        v.push_back({T.substr(p) + std::string(p, '\x00'), uint64_t(p)});
    std::sort(v.begin(), v.end());
    std::vector<uint64_t> o(n);
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
        std::vector<uint64_t> posA = brute_cyclic(A), posB = brute_cyclic(B);
        // Route through the EXTERNAL path: tiny sidecars in a scratch dir, the
        // windowed core merge, WM read back from its scratch file.
        std::string scratch = "cross-selftest-scratch-" + std::to_string(U(getpid()));
        fs::remove_all(scratch);
        fs::create_directories(scratch);
        for (int mode = 0; mode < 2; ++mode) {
            bool dollar = mode == 1;
            std::vector<uint8_t> tA(A.begin(), A.end()), tB(B.begin(), B.end());
            Budget bud; CoreStats st;
            SideExt SA, SB;
            SA.n = A.size(); SA.offset = 0; SA.period = min_period(tA, tA.size());
            SA.sxsPath = scratch + "/A.sxs"; SA.ownsSxs = false;
            SB.n = B.size(); SB.offset = A.size(); SB.period = min_period(tB, tB.size());
            SB.sxsPath = scratch + "/B.sxs"; SB.ownsSxs = false;
            auto dump = [](const SideExt& S, const std::vector<uint8_t>& t, const std::vector<uint64_t>& pos) {
                fs::remove(S.sxsPath);   // second mode reuses the names
                OutFile f(S.sxsPath);
                char magic[4]{'S','X','S','2'}; f.raw(magic, 4);
                f.w64(S.offset); f.w64(S.n);
                f.raw(t.data(), t.size());
                f.raw(pos.data(), 8 * pos.size());
            };
            dump(SA, tA, posA);
            dump(SB, tB, posB);
            PairScratch sc; sc.init(scratch + "/case", 0);
            U hashK = hash_spacing();
            if (hashK > (SA.n + SB.n) / 2) hashK = std::max<U>(1, (SA.n + SB.n) / 2);
            core_merge_ext(SA, SB, dollar, threads, bud, st, sc, hashK, false);
            std::vector<uint64_t> WM(SA.n + SB.n, 0);
            {
                WinU64 wm; wm.open_ro(sc.wm, SA.n + SB.n, 0);
                wm.read(0, WM.data(), SA.n + SB.n);
            }
            sc.cleanup();
            std::vector<uint64_t> want = dollar ? brute_dollar(M) : brute_cyclic(M);
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
    fs::remove_all("cross-selftest-scratch-" + std::to_string(U(getpid())));
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
            merge_pair_files(L, R, true, flag("--out-prefix"), threads, has("--emit-pf"));
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
            merge_pair_files(acc.file, parts[i].file, final, out, threads, final && has("--emit-pf"));
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
            if (j + 3 < parts.size())
                cross_announce_next(parts[j + 2].file, parts[j + 3].file,
                                    work + "/L" + std::to_string(level) + "-" +
                                        std::to_string(j / 2 + 1) + ".crle");   // FIX 4 lookahead (same level)
            merge_pair_files(parts[j].file, parts[j + 1].file, final, out, threads, final && has("--emit-pf"));
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
