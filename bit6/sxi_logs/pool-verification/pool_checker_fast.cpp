// pool_checker.cpp -- bounded exhaustive interleaving checker for the
// externalized merge's window-pool concurrency protocol (bit6/cross_lcp_merge.cpp,
// struct SharedPages).
//
// WHAT IS CHECKED (reader soundness / the single correctness property):
//   If a reader thread observes a slot as VALID (try_page returns a stamp) and
//   its seqlock recheck (try_page_recheck) still matches that stamp, then the
//   byte it copied out of the slot data buffer must equal the true file byte at
//   the offset named by the stamp's page.  Failure = "stale-offset byte"
//   (ABA impostor: bytes from a different page) or a torn page.
//
// HOW: explicit-state reachability (BFS) over the product of the threads'
// atomic steps and the shared slot words.  Every reachable global state is
// visited once; a "bad" state is one in which some reader ACCEPTS a copied byte
// that disagrees with the file content of the page its stamp names.  Because
// reader soundness is a safety property, the set of reachable states is exactly
// the exhaustive union over all interleavings -- no path needs to be
// enumerated separately.
//
// FIDELITY: the packed word arithmetic below is copied from the shipped code:
//   word   = (page+1) << 9 | version << 1 | ready      (PS=256, 8-bit version)
//   pageOf = (word >> 9) - 1
//   packFresh(page, prev) = ((page+1)<<9) | ((((prev>>1)+1) & 0xFF) << 1)
//   fill:  CAS(word, packFresh) -> write page bytes -> CAS(word|1)
//   try_page: load word; require pageOf==page && ready; stamp=word; out=data
//   recheck: load word == stamp
// The version width is a parameter so the SAME arithmetic can be run at width 8
// (shipped) and at reduced widths (to expose the wrap mechanism at small depth).
//
// BUILD: c++ -O2 -std=c++17 pool_checker.cpp -o pool_checker
// RUN:   see run_checker.sh

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <cstdlib>

using u64 = uint64_t;

// ------------------------------------------------------------------ variants
enum Variant {
    PACKED_VERSIONED = 0,   // shipped fix-4 protocol
    PACKED_UNVERSIONED = 1, // pre-fix race #2: single packed word, no version
    SPLIT_UNVERSIONED = 2,  // pre-fix race #1: tag and state are separate atomics
    OWNED_VERSIONED = 3,     // merge-lane fix v3: word=(page+1)<<2|state{invalid,
                             // filling,ready} + separate 64-bit per-slot seqlock
                             // version bumped BEFORE every claim; FILLING slots
                             // are never claimed (no orphaned buffer write is
                             // possible: only the owner writes, only the owner
                             // publishes); reader latches ver, rechecks ver.
};

struct Config {
    Variant variant;
    int  versionBits;   // PACKED_VERSIONED only; shipped = 8
    int  pages;         // number of file pages (== M in "M slots" when slots==pages)
    int  slots;         // number of slot entries
    int  ps;            // page size in bytes
    int  readers;
    int  fillers;
    int  fillerDepth;   // max fill operations any one filler performs
    int  cap;           // state cap (0 = unlimited)
    const char* name;
};

// ------------------------------------------------------------------- globals
static Config g_cfg;

static inline int slotOf(int page) { return page & (g_cfg.slots - 1); }

// True file content: injective per (page,byte-offset) so a cross-page copy is
// always detectable.  page<8, ps<=64 keeps these distinct.
static inline unsigned char FILEBYTE(int page, int z) {
    return (unsigned char)(page * 64 + z);
}

// --------------------------------------------------------- packed arithmetic
// Shift to the page field: 1 ready bit + versionBits version bits.
static inline int pageShift() { return 1 + g_cfg.versionBits; }
static inline u64 verMask() {
    if (g_cfg.versionBits <= 0) return 0;
    return ((u64)1 << g_cfg.versionBits) - 1;
}
static inline int pageOf(u64 w) { return (int)(w >> pageShift()) - 1; }
static inline bool readyOf(u64 w) { return (w & 1) != 0; }
static inline u64 packFresh(int page, u64 prev) {
    u64 ver = g_cfg.versionBits > 0 ? (((prev >> 1) & verMask()) + 1) & verMask() : 0;
    return ((u64)(page + 1) << pageShift()) | (ver << 1);
}

// --------------------------------------------------------------- thread/state
enum { KIND_READER = 0, KIND_FILLER = 1 };

struct Thread {
    int      kind;
    int      pc;
    int      p;         // target page (reader) / chosen page (filler)
    int      sh;        // slot index
    int      z;         // byte offset within page (reader)
    int      depth;     // filler fill count
    int      accepted;  // reader: 0 pending, 1 accepted, 2 missed/fallback
    u64      a, b, c, d, e; // registers (stamp / copied value / etc.)
};

struct State {
    u64  word[8];       // packed variants: slot word; split: unused
    u64  tag[8];        // split: tag word
    u64  stt[8];        // split: state word
    u64  ver[8];        // OWNED_VERSIONED: low word of the 128-bit slot word
    int  dataPage[8];   // slot data currently holds this file page (-1 = none)
    int  dataBuf[8][2]; // OWNED_VERSIONED: page id each buffer holds (-1 = none)
    Thread th[4];
    int  nthreads;
};

// OWNED_VERSIONED v5 (FINAL, cmpxchg16b): the slot word is 128 bits, modeled
// here as two u64s (word=high: (page+1)<<1|bufId, ver=low: ver<<2|state) that
// ALWAYS transition together (the model's step is atomic, matching the
// hardware CAS128):
//   claim  : CAS128( cur -> (page, flip(curBuf), ver+1, FILLING) )
//   publish: CAS128( myClaim -> (page, myBuf, ver+2, READY) )   (atomic with the bump)
//   reader : atomic 128-bit load; copy buffer[bufId]; reload; accept iff equal.
// All five known races closed by construction: tag/state (single word), ABA
// (64-bit ver inside the word; 2^64 claims unreachable by steal-rate
// arithmetic), stale re-match (full ver in every CAS expectation), mid-fill
// orphan (claim flips the buffer atomically; superseded preads land
// disowned), flash window (publish is one atomic transition).
static inline int  ovPageOf(u64 hi) { return (int)(hi >> 1) - 1; }
static inline u64  ovBufOf(u64 hi) { return hi & 1; }
static inline u64  ovStateOf(u64 lo) { return lo & 3; }
static inline u64  ovVerOf(u64 lo)  { return lo >> 2; }
static inline u64  ovHi(int page, int buf) { return (((u64)page + 1) << 1) | (u64)buf; }
static inline u64  ovLo(u64 ver, u64 st) { return (ver << 2) | st; }
enum { OV_INVALID = 0, OV_FILLING = 1, OV_READY = 2 };

static inline bool operator==(const State& x, const State& y) {
    return std::memcmp(&x, &y, sizeof(State)) == 0;
}
struct StateHash {
    size_t operator()(const State& s) const {
        const unsigned char* p = (const unsigned char*)&s;
        u64 h = 1469598103934665603ULL;
        for (size_t i = 0; i < sizeof(State); ++i) { h ^= p[i]; h *= 1099511628211ULL; }
        return (size_t)h;
    }
};

// Open-addressing index set: stores only 32-bit indices into the state store,
// so each reachable state costs one copy of State (not a second copy inside an
// unordered_map node).  Necessary to enumerate the ~10^9-state depth>=256
// product at the shipped 8-bit version width.
struct StateTable {
    std::deque<State>* all = nullptr;
    std::vector<uint32_t> tab;   // 0 = empty, else index+1
    size_t mask = 0, used = 0;
    static size_t hashOf(const State& s) { StateHash h; return h(s); }
    void init(size_t want, std::deque<State>* a) {
        all = a; size_t sz = 16; while (sz < want * 2) sz <<= 1;
        tab.assign(sz, 0); mask = sz - 1; used = 0;
    }
    void grow() {
        std::vector<uint32_t> nt(tab.size() * 2, 0); size_t nm = nt.size() - 1;
        for (size_t i = 0; i < tab.size(); ++i) {
            if (!tab[i]) continue;
            uint32_t v = tab[i] - 1;
            size_t j = hashOf((*all)[v]) & nm;
            while (nt[j]) j = (j + 1) & nm;
            nt[j] = v + 1;
        }
        tab.swap(nt); mask = nm;
    }
    int find(const State& s) const {
        size_t i = hashOf(s) & mask;
        while (tab[i]) { uint32_t v = tab[i] - 1; if ((*all)[v] == s) return (int)v; i = (i + 1) & mask; }
        return -1;
    }
    void insert(const State& s, int idx) {
        if ((used + 1) * 2 > tab.size()) grow();
        size_t i = hashOf(s) & mask; while (tab[i]) i = (i + 1) & mask;
        tab[i] = (uint32_t)idx + 1; used++;
    }
};

// A "bad" event: a reader accepted a byte that disagrees with the true file
// byte of the page named by the stamp.  We flag it when the reader transitions
// into "accepted".  Returns true if bad.
static bool readerAcceptCheck(const Thread& t) {
    if (t.kind != KIND_READER) return false;
    if (t.accepted != 1) return false;
    // t.p is the page the stamp named; t.z the in-page offset; t.b the value.
    return t.b != (u64)FILEBYTE(t.p, t.z);
}

// ----------------------------------------------------------- step generation
// Expand all one-atomic-step successors of `s` into `out`.
static void step(const State& s, std::vector<State>& out, bool& foundBad) {
    for (int ti = 0; ti < s.nthreads; ++ti) {
        const Thread& t = s.th[ti];

        // ---------------- READER ------------------------------------------
        if (t.kind == KIND_READER) {
            if (t.pc == 0) {
                State n = s;
                // load slot word (packed) or tag (split) or latch the
                // seqlock version (OWNED_VERSIONED)
                if (g_cfg.variant == SPLIT_UNVERSIONED)
                    n.th[ti].a = s.tag[t.sh];
                else if (g_cfg.variant == OWNED_VERSIONED) {
                    n.th[ti].a = s.word[t.sh];   // high half
                    n.th[ti].c = s.ver[t.sh];    // low half (one atomic 128-bit load)
                } else
                    n.th[ti].a = s.word[t.sh];
                n.th[ti].pc = 1;
                out.push_back(n);
            } else if (t.pc == 1) {
                if (g_cfg.variant == OWNED_VERSIONED) {
                    State n = s;
                    bool valid = (ovPageOf(t.a) == t.p) && (ovStateOf(t.c) == OV_READY);
                    if (valid) {
                        int dp = s.dataBuf[t.sh][ovBufOf(t.a)];
                        n.th[ti].b = (dp >= 0) ? (u64)FILEBYTE(dp, t.z) : 0xFFFF;
                        n.th[ti].pc = 2;
                    } else {
                        n.th[ti].accepted = 2;
                        n.th[ti].pc = 4;
                    }
                    out.push_back(n);
                    continue;
                }
                // Validate the observed tag/state; if valid, copy the byte from
                // the slot data buffer.  This is the "observe as VALID" point.
                bool valid;
                if (g_cfg.variant == SPLIT_UNVERSIONED) {
                    State n = s;
                    n.th[ti].c = s.stt[t.sh]; // separate load of the state word
                    valid = (((int)t.a - 1) == t.p) && (n.th[ti].c == 1);
                    if (valid) {
                        int dp = s.dataPage[t.sh];
                        n.th[ti].b = (dp >= 0) ? (u64)FILEBYTE(dp, t.z) : 0xFFFF;
                        n.th[ti].pc = 2;
                    } else {
                        n.th[ti].accepted = 2; // miss -> synchronous fallback
                        n.th[ti].pc = 4;
                    }
                    out.push_back(n);
                    continue;
                }
                valid = (pageOf(t.a) == t.p) && readyOf(t.a);
                State n = s;
                if (valid) {
                    int dp = s.dataPage[t.sh];
                    n.th[ti].b = (dp >= 0) ? (u64)FILEBYTE(dp, t.z) : 0xFFFF;
                    n.th[ti].pc = 2;
                } else {
                    n.th[ti].accepted = 2;
                    n.th[ti].pc = 4;
                }
                out.push_back(n);
            } else if (t.pc == 2) {
                State n = s;
                if (g_cfg.variant == SPLIT_UNVERSIONED) {
                    // split recheck: tag reload, then state reload (two atomics)
                    n.th[ti].d = s.tag[t.sh];
                    n.th[ti].pc = 3;
                } else if (g_cfg.variant == OWNED_VERSIONED) {
                    n.th[ti].d = s.word[t.sh];  // atomic 128-bit reload (both halves)
                    n.th[ti].e = s.ver[t.sh];
                    n.th[ti].pc = 3;
                } else {
                    n.th[ti].d = s.word[t.sh];
                    n.th[ti].pc = 3;
                }
                out.push_back(n);
            } else if (t.pc == 3) {
                State n = s;
                bool ok;
                if (g_cfg.variant == SPLIT_UNVERSIONED) {
                    n.th[ti].e = s.stt[t.sh];
                    ok = (n.th[ti].d == t.a) && (n.th[ti].e == t.c);
                } else if (g_cfg.variant == OWNED_VERSIONED) {
                    ok = (t.d == t.a) && (t.e == t.c);   // full 128-bit word unchanged
                } else {
                    ok = (t.d == t.a);
                }
                if (ok) {
                    n.th[ti].accepted = 1; // ACCEPT
                    if (readerAcceptCheck(n.th[ti])) foundBad = true;
                } else {
                    n.th[ti].accepted = 2; // torn -> synchronous fallback
                }
                n.th[ti].pc = 4;
                out.push_back(n);
            }
            // pc==4: reader done, no step
            continue;
        }

        // ---------------- FILLER ------------------------------------------
        if (t.kind == KIND_FILLER) {
            if (t.depth >= g_cfg.fillerDepth) continue; // done
            if (t.pc == 0) {
                // nondeterministically choose the page to fill
                for (int pg = 0; pg < g_cfg.pages; ++pg) {
                    State n = s;
                    n.th[ti].p = pg;
                    n.th[ti].sh = slotOf(pg);
                    if (g_cfg.variant == SPLIT_UNVERSIONED) {
                        n.th[ti].a = s.tag[n.th[ti].sh]; // load tag (separate atomic)
                        n.th[ti].pc = 1;
                    } else {
                        n.th[ti].a = s.word[n.th[ti].sh]; // load cur for CAS
                        n.th[ti].pc = 1;
                    }
                    out.push_back(n);
                }
            } else if (t.pc == 1) {
                if (g_cfg.variant == OWNED_VERSIONED) {
                    // advisory guards on the latched 128-bit word
                    if ((ovPageOf(t.a) == t.p && ovStateOf(t.c) == OV_READY)) {
                        State n = s;
                        n.th[ti].depth++; n.th[ti].pc = 0;
                        out.push_back(n);
                    } else {
                        State n = s;
                        n.th[ti].pc = 2;   // proceed to the CAS128 claim
                        out.push_back(n);
                    }
                    continue;
                }
                if (g_cfg.variant == SPLIT_UNVERSIONED) {
                    // early-out if already owned+ready; else state=0 (claim)
                    if (((int)t.a - 1) == t.p && s.stt[t.sh] == 1) {
                        State n = s;
                        n.th[ti].depth++; n.th[ti].pc = 0;
                        out.push_back(n);
                    } else {
                        State n = s;
                        n.stt[n.th[ti].sh] = 0;   // claim: state word -> busy
                        n.th[ti].pc = 2;
                        out.push_back(n);
                    }
                } else {
                    u64 want = packFresh(t.p, t.a);
                    if (s.word[t.sh] == t.a) {
                        State n = s;
                        n.word[n.th[ti].sh] = want; // CAS claim: tag+version flip, ready clears
                        n.th[ti].b = want;
                        n.th[ti].pc = 2;
                        out.push_back(n);
                    } else {
                        State n = s; // CAS lost: no-op
                        n.th[ti].depth++; n.th[ti].pc = 0;
                        out.push_back(n);
                    }
                }
            } else if (t.pc == 2) {
                State n = s;
                if (g_cfg.variant == SPLIT_UNVERSIONED) {
                    n.tag[n.th[ti].sh] = (u64)(t.p + 1); // tag update (separate atomic)
                } else if (g_cfg.variant == OWNED_VERSIONED) {
                    // CLAIM: CAS128 from the latched pair. Expectation carries
                    // the full 64-bit version: a stale latch can never
                    // re-match. The buffer flip is atomic with the claim.
                    if (s.word[t.sh] == t.a && s.ver[t.sh] == t.c) {
                        State n = s;
                        u64 myBuf = 1 - ovBufOf(t.a);
                        u64 v = ovVerOf(t.c) + 1;
                        n.word[n.th[ti].sh] = ovHi(t.p, (int)myBuf);
                        n.ver[n.th[ti].sh] = ovLo(v, OV_FILLING);
                        n.th[ti].e = myBuf;              // our buffer
                        n.th[ti].d = ovVerOf(t.c) + 1;   // our claim ver
                        n.th[ti].pc = 3;
                        out.push_back(n);
                    } else {                              // claim lost
                        State n = s;
                        n.th[ti].depth++; n.th[ti].pc = 0;
                        out.push_back(n);
                    }
                    continue;
                } else {
                    n.dataPage[n.th[ti].sh] = t.p;        // write page bytes
                }
                n.th[ti].pc = 3;
                out.push_back(n);
            } else if (t.pc == 3) {
                if (g_cfg.variant == OWNED_VERSIONED) {
                    // pread into OUR buffer only (claimed atomically above)
                    State n = s;
                    n.dataBuf[n.th[ti].sh][t.e] = t.p;
                    n.th[ti].pc = 4;
                    out.push_back(n);
                    continue;
                }
                State n = s;
                if (g_cfg.variant == SPLIT_UNVERSIONED) {
                    n.dataPage[n.th[ti].sh] = t.p;        // write page bytes
                    n.th[ti].pc = 4;
                } else {
                    if (s.word[t.sh] == t.b)              // publish ready
                        n.word[n.th[ti].sh] = t.b | 1;
                    n.th[ti].depth++;
                    n.th[ti].pc = 0;
                }
                out.push_back(n);
            } else if (t.pc == 4) {
                State n = s;
                if (g_cfg.variant == OWNED_VERSIONED) {
                    // PUBLISH: CAS128 from OUR claim word. Atomic with the
                    // ver bump: no flash window. A thief's claim (word moved)
                    // makes it fail: our fill stays disowned in our buffer.
                    if (s.word[t.sh] == ovHi(t.p, (int)t.e) && s.ver[t.sh] == ovLo(t.d, OV_FILLING)) {
                        n.word[n.th[ti].sh] = ovHi(t.p, (int)t.e);
                        n.ver[n.th[ti].sh] = ovLo(t.d + 1, OV_READY);
                        n.th[ti].depth++;
                        n.th[ti].pc = 0;
                    } else {                              // stolen: discard our fill
                        n.th[ti].depth++;
                        n.th[ti].pc = 0;
                    }
                    out.push_back(n);
                    continue;
                }
                n.stt[n.th[ti].sh] = 1;                   // publish ready
                n.th[ti].depth++;
                n.th[ti].pc = 0;
                out.push_back(n);
            }
            continue;
        }
    }
}

// ------------------------------------------------------------------- engine
struct Hit {
    std::vector<State> path;
    int  readerIdx;
    int  page, z;
    u64  got, want;
};

static bool runConfig(const Config& cfg, Hit& hit, long long& explored, long long& transitions) {
    g_cfg = cfg;
    State s0; std::memset(&s0, 0, sizeof(s0));
    for (int i = 0; i < 8; ++i) { s0.word[i] = 0; s0.tag[i] = 0; s0.stt[i] = 0; s0.dataPage[i] = -1;
                                   s0.dataBuf[i][0] = -1; s0.dataBuf[i][1] = -1; }
    // readers: each targets a distinct page (page = reader index) at offset z = reader index.
    s0.nthreads = cfg.readers + cfg.fillers;
    for (int r = 0; r < cfg.readers; ++r) {
        Thread& t = s0.th[r];
        t.kind = KIND_READER; t.pc = 0;
        t.p = r % cfg.pages; t.sh = slotOf(t.p); t.z = r;
        t.accepted = 0;
    }
    for (int f = 0; f < cfg.fillers; ++f) {
        Thread& t = s0.th[cfg.readers + f];
        t.kind = KIND_FILLER; t.pc = 0; t.depth = 0;
    }

    std::deque<int> q;
    std::deque<State> all;              // stable refs: push_back never dangles &all[i]
    std::deque<int> parent;
    StateTable idx;
    all.push_back(s0); parent.push_back(-1);
    idx.init(1u << 20, &all);
    idx.insert(all[0], 0);
    q.push_back(0);

    std::vector<State> succ;
    bool bad = false;
    int badFrom = -1, badReader = -1;
    explored = 0; transitions = 0;

    // Bounded enumeration (OOM rule): hard state cap + wall alarm + periodic
    // progress. A run that exceeds its bound dies LOUD and SMALL.
    long long hardCap = (long long)cfg.cap * (cfg.cap > 0 ? 1 : 0);
    if (!hardCap) hardCap = 40000000LL;              // default: 40M states (~45 GB)
    auto t0c = std::chrono::steady_clock::now();
    double alarmSec = 300.0;
    if (getenv("CHECKER_ALARM_SEC")) alarmSec = atof(getenv("CHECKER_ALARM_SEC"));
    long long nextProg = 100000;
    while (!q.empty()) {
        int cur = q.front(); q.pop_front();
        explored++;
        const State& cs = all[cur];
        if (explored >= nextProg) {
            nextProg = explored * 2;
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0c).count();
            std::fprintf(stderr, "PROG explored=%lld queued=%zu elapsed=%.1fs\n", explored, q.size(), el);
            if (el > alarmSec) {
                std::printf("   states=%lld transitions=%lld verdict=ALARM_WALL_BOUND_EXCEEDED(%.0fs)\n",
                            explored, transitions, alarmSec);
                std::fflush(stdout);
                exit(4);
            }
        }
        if ((long long)all.size() > hardCap) {
            std::printf("   states=%lld transitions=%lld verdict=CAP_EXHAUSTED(cap=%lld)\n",
                        explored, transitions, hardCap);
            std::fflush(stdout);
            exit(3);
        }
        succ.clear();
        bool localBad = false;
        step(cs, succ, localBad);
        transitions += (long long)succ.size();
        for (auto& n : succ) {
            // detect bad on the specific reader that just accepted
            bool nb = false;
            for (int ti = 0; ti < n.nthreads; ++ti) {
                if (n.th[ti].kind == KIND_READER && n.th[ti].accepted == 1 && cs.th[ti].accepted != 1) {
                    if (readerAcceptCheck(n.th[ti])) { nb = true; badReader = ti; }
                }
            }
            if (idx.find(n) >= 0) continue;
            if (cfg.cap > 0 && (long long)all.size() >= cfg.cap) {
                // hit cap: stop expanding (report honestly)
                q.clear();
                break;
            }
            int ni = (int)all.size();
            idx.insert(n, ni);
            all.push_back(n); parent.push_back(cur);
            if (nb && !bad) {
                bad = true; badFrom = ni;
                // reconstruct path
                hit.path.clear();
                for (int k = ni; k != -1; k = parent[k]) hit.path.push_back(all[k]);
                std::reverse(hit.path.begin(), hit.path.end());
                hit.readerIdx = badReader;
            }
            q.push_back(ni);
        }
        if (bad) {
            // keep a small hunt but we can stop: safety property violated
            hit.page = all[badFrom].th[badReader].p;
            hit.z    = all[badFrom].th[badReader].z;
            hit.got  = all[badFrom].th[badReader].b;
            hit.want = (u64)FILEBYTE(hit.page, hit.z);
            return true;
        }
    }
    return false;
}

// --------------------------------------------------------------------- main
static void printTrace(const Hit& h) {
    std::printf("  counterexample path length = %zu steps\n", h.path.size() - 1);
    int k = 0;
    for (const auto& s : h.path) {
        if (k <= 12 || k + 3 >= (int)h.path.size())
            std::printf("    step %3d: word0=%llu dataPage0=%d  reader(pc=%d a=%llu b=%llu acc=%d)\n",
                        k, (unsigned long long)s.word[0], s.dataPage[0],
                        s.th[h.readerIdx].pc, (unsigned long long)s.th[h.readerIdx].a,
                        (unsigned long long)s.th[h.readerIdx].b, s.th[h.readerIdx].accepted);
        else if (k == 13) std::printf("    ...\n");
        ++k;
    }
    std::printf("  STALE READ: reader wanted FILE[page=%d][z=%d]=%llu but accepted %llu\n",
                h.page, h.z, (unsigned long long)h.want, (unsigned long long)h.got);
}

static const char* variantName(Variant v) {
    switch (v) {
        case PACKED_VERSIONED:   return "packed_versioned";
        case PACKED_UNVERSIONED: return "packed_unversioned";
        case SPLIT_UNVERSIONED:  return "split_unversioned";
    }
    return "?";
}

int main(int argc, char** argv) {
    // Optional CLI: <variant> <versionBits> <pages> <slots> <ps> <readers> <fillers> <depth> <cap>
    if (argc >= 10) {
        Config c;
        c.variant     = (Variant)std::atoi(argv[1]);
        c.versionBits = std::atoi(argv[2]);
        c.pages       = std::atoi(argv[3]);
        c.slots       = std::atoi(argv[4]);
        c.ps          = std::atoi(argv[5]);
        c.readers     = std::atoi(argv[6]);
        c.fillers     = std::atoi(argv[7]);
        c.fillerDepth = std::atoi(argv[8]);
        c.cap         = std::atoi(argv[9]);
        c.name        = "cli";
        Hit h; long long ex, tr;
        std::printf("== %s: vb=%d pages=%d slots=%d ps=%d R=%d F=%d depth=%d cap=%d\n",
                    variantName(c.variant), c.versionBits, c.pages, c.slots, c.ps,
                    c.readers, c.fillers, c.fillerDepth, c.cap);
        bool bad = runConfig(c, h, ex, tr);
        std::printf("   states=%lld transitions=%lld verdict=%s\n",
                    ex, tr, bad ? "COUNTEREXAMPLE" : "clean");
        if (bad) printTrace(h);
        return bad ? 0 : 0;
    }

    // default battery
    struct Row { Variant v; int vb; int pages; int slots; int R; int F; int depth; const char* note; };
    Row rows[] = {
        { SPLIT_UNVERSIONED,   0, 2, 1, 1, 1, 16, "pre-fix race #1 (split tag+state), one shared slot" },
        { SPLIT_UNVERSIONED,   0, 4, 2, 1, 2, 16, "pre-fix race #1, 2 slots 4 pages, 2 fillers" },
        { PACKED_UNVERSIONED,  0, 2, 1, 1, 1, 16, "pre-fix race #2 (no version), one shared slot" },
        { PACKED_UNVERSIONED,  0, 4, 2, 1, 2, 16, "pre-fix race #2, 2 slots 4 pages, 2 fillers" },
        { PACKED_VERSIONED,    8, 2, 1, 1, 1, 40,  "shipped, depth 40" },
        { PACKED_VERSIONED,    1, 2, 1, 1, 1, 12,  "shipped arithmetic, 1-bit version" },
        { PACKED_VERSIONED,    2, 2, 1, 1, 1, 24,  "shipped arithmetic, 2-bit version" },
    };
    for (const auto& r : rows) {
        Config c{ r.v, r.vb, r.pages, r.slots, 4, r.R, r.F, r.depth, 0, r.note };
        Hit h; long long ex, tr;
        std::printf("== %s pages=%d slots=%d R=%d F=%d depth=%d  [%s]\n",
                    variantName(c.variant), c.pages, c.slots, c.readers, c.fillers, c.fillerDepth, r.note);
        bool bad = runConfig(c, h, ex, tr);
        std::printf("   states=%lld transitions=%lld verdict=%s\n",
                    ex, tr, bad ? "COUNTEREXAMPLE" : "clean");
        if (bad) printTrace(h);
    }
    return 0;
}
