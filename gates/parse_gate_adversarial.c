// parse_gate_adversarial - the DISPUTED PREFIX-RELATION CASE, constructed.
//
//   parse_gate_adversarial OUT BYTES
//
// The level-3 parse's original mandate asserted a hard prefix-free gate on
// the theorem that the trigger discipline yields a prefix-free dictionary.
// The two-sided strict window-min rule (needed so phrases are >= k and end
// with their own trigger k-mer) does NOT guarantee that theorem: a trigger
// k-mer's occurrence inside another phrase need not be a trigger there,
// because its FORWARD window bytes differ. This fixture constructs exactly
// that case, deterministically, so the oracle tests it directly instead of
// relying on 'mixed' to hit it by accident:
//
//   X  an 8-byte k-mer that IS a trigger in region A (its FNV-1a hash is
//      the strict minimum of its two-sided window inside the Z margin)
//   X' the SAME 8 bytes placed in region B right after M, where M is a
//      k-mer with a SMALLER hash at distance 8 in X''s backward window, so
//      X' is NOT a trigger there
//   Z = 0x9A fill: all-equal k-mers are never strict minima (equal hashes),
//      so a 200-byte Z margin has no triggers and the only triggers near
//      a construct are the designed ones.
//
// Region A: the phrase containing X ends AT X's end -> the class
//   dictionary holds a proper phrase whose LAST k-mer is X.
// Region B: M is a trigger, so a phrase boundary falls at M's end; the
//   phrase that starts at X' runs PAST X' (X' is not a trigger there) ->
//   the dictionary holds a proper phrase with X INSIDE it at a non-end
//   offset.  => prefix_relations >= 1, and the emitted order must still be
//   byte-exact (the comparator walks bytes; the SA oracle is the belt, the
//   construction the braces).
//
// THE TRIGGER RULE COMPARES HASHES, NOT BYTE ORDER: the generator SEARCHES
// for X and M whose FNV-1a hashes satisfy the rule (byte-order intuition
// produced k-mers the rule cut mid-phrase), and then SELF-VERIFIES by
// simulating the two-sided rule over the emitted block: X must be a
// trigger, X' must not be, M must be. A failed verification aborts the
// generation - the fixture never ships unverified.
//
// One block of BYTES/4 bytes is written 4 times: rows at corresponding
// offsets in adjacent copies tie past any 4096-byte window, so they
// SURVIVE level 2 and reroute through the parse. FOUR copies: a row's full
// phrase stream is then inside the default 64-phrase budget, so classes
// resolve in ROUND 1 and the fixture stays a seconds-scale gate.
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static const unsigned K = 8;    // XSA_PFP_K default
static const unsigned W = 8;    // XSA_PFP_W default

static uint64_t fnv(const unsigned char* q) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < K; ++i) { h ^= q[i]; h *= 1099511628211ull; }
    return h;
}

// the tool's trigger rule, two-sided strict, over a buffer
static bool is_trigger(const unsigned char* buf, long long n, long long s) {
    if (s < 0 || s + (long long)K > n) return false;
    const uint64_t h = fnv(buf + s);
    for (long long j = 1; j <= (long long)W; ++j) {
        if (s - j >= 0 && !(h < fnv(buf + s - j))) return false;
        if (s + j + (long long)K <= n && !(h < fnv(buf + s + j))) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: parse_gate_adversarial OUT BYTES\n");
        return 2;
    }
    const long long want = std::atoll(argv[2]);
    if (want < 16 * 1024) {
        std::fprintf(stderr, "parse_gate_adversarial: BYTES >= 16384\n");
        return 2;
    }
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) { std::perror("open"); return 2; }
    const unsigned COPIES = 4;
    const long long block = want / COPIES;
    const long long MARG = 200;
    const long long regA = block / 4 - MARG;   // X lands at regA + MARG
    const long long regB = block / 2 - MARG;   // M at regB+MARG, X' after it
    if (regB + 3 * MARG + 8 > block) {
        std::fprintf(stderr, "parse_gate_adversarial: block too small\n");
        return 2;
    }

    // ---- search for X and M by HASH, against the actual Z context ----
    unsigned char trial[block > 0 ? 8192 : 8192];
    for (long long i = 0; i < 8192; ++i) trial[i] = 0x9A;
    unsigned char X[8], M[8];
    // X: the strict two-sided hash-minimum inside the Z margin
    {
        unsigned char cand[8];
        for (unsigned c = 0; c < 8; ++c) cand[c] = 0x50 + c;   // deterministic start
        bool found = false;
        for (long long it = 0; it < (1 << 24) && !found; ++it) {
            // mix the counter into the candidate bytes (printable range)
            long long v = it;
            for (unsigned c = 1; c < 8; ++c) { cand[c] = 0x41 + (v & 31); v >>= 5; }
            // place it and test the rule in situ
            for (unsigned c = 0; c < 8; ++c) trial[MARG + c] = cand[c];
            if (is_trigger(trial, 2 * MARG + 8, MARG)) {
                for (unsigned c = 0; c < 8; ++c) X[c] = cand[c];
                found = true;
            }
        }
        if (!found) { std::fprintf(stderr, "no X found\n"); return 2; }
    }
    // M: a trigger whose hash also beats X's, placed 8 before X'
    {
        unsigned char cand[8];
        for (unsigned c = 0; c < 8; ++c) cand[c] = 0x60 + c;
        for (long long i = 0; i < 8192; ++i) trial[i] = 0x9A;
        for (unsigned c = 0; c < 8; ++c) trial[MARG + 8 + c] = X[c];  // X' after M
        bool found = false;
        for (long long it = 0; it < (1 << 24) && !found; ++it) {
            long long v = it;
            for (unsigned c = 1; c < 8; ++c) { cand[c] = 0x41 + (v & 31); v >>= 5; }
            for (unsigned c = 0; c < 8; ++c) trial[MARG + c] = cand[c];
            if (is_trigger(trial, 2 * MARG + 16, MARG) &&   // M is a trigger
                fnv(cand) < fnv(X) &&                        // M's hash < X's
                !is_trigger(trial, 2 * MARG + 16, MARG + 8)) {  // X' is not
                for (unsigned c = 0; c < 8; ++c) M[c] = cand[c];
                found = true;
            }
        }
        if (!found) { std::fprintf(stderr, "no M found\n"); return 2; }
    }

    // ---- build the block ----
    unsigned char* B = new unsigned char[size_t(block)];
    for (long long i = 0; i < block; ++i) B[i] = 0x9A;
    for (unsigned c = 0; c < 8; ++c) B[regA + MARG + c] = X[c];
    for (unsigned c = 0; c < 8; ++c) B[regB + MARG + c] = M[c];
    for (unsigned c = 0; c < 8; ++c) B[regB + MARG + 8 + c] = X[c];

    // ---- SELF-VERIFY over the emitted block: fail loud, never ship a
    // fixture whose disputed case did not materialize ----
    if (!is_trigger(B, block, regA + MARG)) {
        std::fprintf(stderr, "parse_gate_adversarial: X is not a trigger in the"
                             " block - refusing\n");
        return 2;
    }
    if (!is_trigger(B, block, regB + MARG)) {
        std::fprintf(stderr, "parse_gate_adversarial: M is not a trigger - refusing\n");
        return 2;
    }
    if (is_trigger(B, block, regB + MARG + 8)) {
        std::fprintf(stderr, "parse_gate_adversarial: X' IS a trigger - the"
                             " disputed case vanished; refusing\n");
        return 2;
    }

    for (unsigned c = 0; c < COPIES; ++c)
        std::fwrite(B, 1, size_t(block), f);
    delete[] B;
    std::fclose(f);
    return 0;
}
