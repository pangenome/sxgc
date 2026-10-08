// Targeted witness replay for the FINAL buffer-ownership protocol: a single
// 128-bit packed slot word (cmpxchg16b), so every transition is atomic:
//   word128 = (page+1) << 67 | bufId << 66 | ver(64) << 2 | state{invalid,filling,ready}
//   claim  : CAS128( any -> (page, !bufId(cur), ver+1, FILLING) )  (flip is atomic with the claim)
//   publish: CAS128( (page, buf, ver+1, FILLING) -> (page, buf, ver+2, READY) ) (atomic with the bump)
//   reader : atomic 128-bit load; valid(page,ready) -> copy buffer[bufId]; reload; accept iff equal.
// Closes all five known races by construction:
//   tag/state race (one word), ABA/wrap (64-bit ver in the word; 2^64 steals
//   unreachable: steal-rate ~1e5-1e6/s x any run duration << 2^64), stale
//   CAS re-match (full 64-bit ver in every expectation), mid-fill orphan
//   write (the claim flips the buffer atomically: a superseded fill's pread
//   lands in a buffer no live word references), flash window (publish is one
//   atomic transition — the ready bit never appears without its ver bump).
#include <cstdio>
#include <cstdint>
using u64 = uint64_t;
using u128 = __uint128_t;

static int  pageOf(u128 w) { return (int)(w >> 67) - 1; }
static u64  bufOf(u128 w) { return (u64)(w >> 66) & 1; }
static u64  verOf(u128 w) { return ((u64)(w >> 2)) & 0xFFFFFFFFFFFFFFFFULL; }
static u64  stateOf(u128 w) { return (u64)w & 3; }
static u128 packW(int page, u64 buf, u64 ver, u64 st) {
    return ((u128)(u64)(page + 1) << 67) | ((u128)buf << 66) | ((u128)ver << 2) | st;
}
enum { INVALID = 0, FILLING = 1, READY = 2 };

struct Sim {
    u128 word = 0;                 // INVALID
    int dataBuf[2] = {-1, -1};
    int failures = 0;
    void check(const char* where) {
        if (stateOf(word) == READY && dataBuf[bufOf(word)] != pageOf(word)) {
            std::printf("REPLAY_FAIL %s: word=(p%d,b%llu,ready) but buf holds p%d\n",
                        where, pageOf(word), (unsigned long long)bufOf(word), dataBuf[bufOf(word)]);
            failures++;
        }
    }
};

struct Filler {
    int page; u128 mine = 0; int myBuf = -1; int stage = 0; bool active = true;
    bool claim(Sim& s) {                 // atomic CAS128 claim
        if (stage != 0) return false;
        u128 cur = s.word;
        mine = packW(page, 1 - bufOf(cur), verOf(cur) + 1, FILLING);
        s.word = mine;                    // the CAS succeeds against `cur` (model: expectation == cur)
        myBuf = (int)bufOf(mine); stage = 1; return true;
    }
    bool write(Sim& s) {                  // private pread into OUR buffer
        if (stage != 1) return false;
        s.dataBuf[myBuf] = page; stage = 2; return true;
    }
    bool publish(Sim& s) {                // atomic CAS128 publish (expectation = our claim word)
        if (stage != 2) return false;
        if (s.word == mine) {             // CAS succeeds only if untouched since our claim
            s.word = packW(page, myBuf, verOf(mine) + 1, READY);
            stage = 3; return true;
        }
        stage = 4; active = false;        // stolen or epoch moved: disowned
        return false;
    }
};

struct Reader {
    u128 l1 = 0; u128 l2 = 0; int page; int copied = -1; int stage = 0;
    bool accepted = false; int gotPage = -1;
    void step1(Sim& s) { if (!stage) { l1 = s.word; stage = 1; } }            // atomic 128-bit load
    void step2(Sim& s) { if (stage == 1) {
        if (pageOf(l1) == page && stateOf(l1) == READY) { copied = s.dataBuf[bufOf(l1)]; stage = 2; }
        else stage = 5; } }                                                   // miss -> fallback
    void step3(Sim& s) { if (stage == 2) { l2 = s.word; stage = 3; } }        // atomic reload
    void step4() { if (stage == 3) {
        if (l2 == l1) { accepted = true; gotPage = copied; }
        stage = 5; } }                                                        // else torn -> fallback
    bool sound() const { return !accepted || gotPage == page; }
};

static int fails = 0;
static void replay(const char* name, void (*script)(Sim&, Filler&, Filler&, Reader&)) {
    Sim s; Filler f0, f1; Reader r;
    f0.page = 0; f1.page = 1; r.page = 0;
    script(s, f0, f1, r);
    s.check(name);
    if (!r.sound()) {
        std::printf("REPLAY_FAIL %s: reader accepted page %d bytes for page %d\n", name, r.gotPage, r.page);
        fails++;
    } else std::printf("REPLAY_OK %s\n", name);
}

int main() {
    replay("S1_orphan_write", [](Sim& s, Filler& f0, Filler& f1, Reader& r) {
        f0.claim(s); f0.write(s);          // F0: (p0, b?, v1, filling), buffer b = flip
        f1.claim(s); f1.write(s);          // F1: OTHER buffer (atomic flip)
        f0.write(s);                       // schedule fidelity
        f1.publish(s);                     // F1 publishes its own buffer
        f0.publish(s);                     // F0's CAS fails (word moved) -> disowned
        r.step1(s); r.step2(s); r.step3(s); r.step4();
    });
    replay("S2_stale_publish", [](Sim& s, Filler& f0, Filler& f1, Reader& r) {
        f0.claim(s); f0.write(s);
        f1.claim(s); f1.write(s);          // thief claims while F0 mid-fill
        bool p = f0.publish(s);            // must FAIL (word != F0's claim)
        if (p) { std::printf("REPLAY_FAIL S2: stale publish succeeded\n"); fails++; }
        f1.publish(s);
        r.step1(s); r.step2(s); r.step3(s); r.step4();
    });
    replay("S3_flash_window", [](Sim& s, Filler& f0, Filler& f1, Reader& r) {
        f0.claim(s); f0.write(s); f0.publish(s);  // F0 publishes atomically (no split store)
        r.step1(s);                                // reader latches a valid ready word
        f1.claim(s);                               // thief claims: ver bumps, buffer flips
        f1.write(s);                               // thief writes ITS buffer only
        r.step2(s); r.step3(s); r.step4();          // reader's recheck must fail (word changed)
        f1.publish(s);
    });
    replay("S4_same_page_duel", [](Sim& s, Filler& f0, Filler& f1, Reader& r) {
        f1.page = 0;
        f0.claim(s); f1.claim(s);          // distinct epochs -> distinct buffers (atomic flips)
        f0.write(s); f1.write(s);
        f1.publish(s);
        f0.publish(s);                     // disowned (word != F0's claim)
        r.step1(s); r.step2(s); r.step3(s); r.step4();
    });
    replay("S5_reader_midcopy_claim", [](Sim& s, Filler& f0, Filler& f1, Reader& r) {
        f0.claim(s); f0.write(s); f0.publish(s);
        r.step1(s);                        // latch valid (p0, ready, v2)
        f1.page = 0; f1.claim(s);          // claim bumps ver, flips buffer
        f1.write(s);                       // writes the OTHER buffer
        r.step2(s);                        // copies the OLD buffer (p0 data, complete)
        r.step3(s); r.step4();             // recheck: word changed -> torn -> fallback
        f1.publish(s);
    });
    replay("S6_wrap_arithmetic_note", [](Sim& s, Filler& f0, Filler& f1, Reader& r) {
        // 64-bit ver: wrap needs 2^64 claims on one slot; at the physical
        // steal-rate bound (1e5-1e6 claims/s: each claim issues an NVMe pread)
        // that is >= 5.8e5 years. Not replayable by enumeration; the bound is
        // arithmetic. This case exercises 3 rapid claims for regression only.
        f0.claim(s); f1.claim(s); f0.claim(s); f1.claim(s);
        f1.write(s); f1.publish(s);
        r.step1(s); r.step2(s); r.step3(s); r.step4();
    });
    std::printf("WITNESS_REPLAY %s\n", fails ? "FAILURES_PRESENT" : "ALL_OK");
    return fails ? 1 : 0;
}
