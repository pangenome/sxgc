// EXTRACTED for the pool stress harness (generated; source of truth is
// bit6/cross_lcp_merge.cpp). Do not edit by hand.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
using U = uint64_t;
static void fail(const char* m) { std::fprintf(stderr, "FATAL: %s\n", m); std::exit(2); }
#define require(c, m) do { if (!(c)) fail(m); } while (0)
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
