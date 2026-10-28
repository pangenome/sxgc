// scatter_sort - THE TAPE MACHINE, prototype.
//
// Replaces "sort positions with random-access LCE at every level" with
// "scatter rows into key-ordered bands once, then sort each band in memory".
//
//   scatter_sort --corpus C --out SA [--ram-gb G] [--threads T]
//                [--sample N] [--scratch DIR] [--keep-rows]
//
// Pipeline:
//   1. SAMPLE   jittered-stride sample of 16-byte padded keys -> sorted ->
//               quantile BOUNDARIES. A sample suffices: bands only have to be
//               balanced, and every row is routed by a key COMPARISON, so a
//               sampling error can only unbalance a band, never misorder a row.
//   2. SCATTER  ONE sequential pass over the tape (T threads over contiguous
//               text ranges). Each row's 16-byte key is compared against the
//               boundaries and the row (key, position) is appended to that
//               band's file. Text is read exactly once and never again during
//               sorting; the sort is pure in-memory work on fixed-width rows.
//   3. SORT     per band, in RAM: compare keys as two big-endian u64s; only
//               rows whose 16-byte keys are EQUAL fall through to a real
//               suffix comparison. On web text that tie set is ~1% (and 97% of
//               that is uniform-byte runs) - see tools/binbwt/kgram_survey.
//   4. EMIT     bands are key ranges, so emitting them in boundary order IS
//               suffix order: the output is a plain concatenation.
//
// Output: the SA stream in the emitted-SSA format (48-byte header + u64/rank),
// which is exactly what --sa-sample-k 1 writes, so `sa_check` and the banked
// containers can verify it directly.
//
// Convention: dollar (plain suffix order, shorter suffix smaller) - established
// empirically against the bin machine's own emitted SA on a 4 MB slice.

#include <algorithm>
#include <array>
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
#include <functional>
#include <memory>

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

// One row of the intermediate: a 24-symbol big-endian key, the position, the
// carried BWT symbol and the row's KEY PREFIX LENGTH.
//
// The prefix length is what makes the key exact without a pad value: `aux` is
// prefix_len+1 for a row of the TAIL GROUP (a row whose suffix has fewer than
// KEY_BYTES data symbols) and 0 for every other row, i.e. "the whole key". The
// comparator masks the key at min(prefix) and then applies the END rule, so the
// zero fill past a short suffix is NEVER read as a symbol. (Big-endian packing
// makes a full-key comparison a plain integral compare.)
struct Row {
    u64 klo, khi, kq, pos;   // 24-symbol key (3 big-endian words) + pos/sym/aux
};
static constexpr u64 POS_MASK = (1ull << 48) - 1;

// ================================ THE DECLARED END SYMBOL ===================
// FORMAT (declared, not inferred): the alphabet is k = 257 CODES. Code 0 is
// END; codes 1..256 are the 256 byte values. THE DATA PATH IS 8-BIT: no key
// byte, row field, comparison operand or remap entry here is wider than a byte.
// A code is only ever computed as an unsigned integer where a bucket index or an
// ordering decision needs one (exactly what binbwt_slice's bin_code has always
// done: code = v+1, 0 == END).
//
// REALIZATION is a PREFLIGHT DECISION, made once by `xsa_finish --sentinel-remap`
// (journaled `END_SYMBOL mode=...`), and passed to every stage as a DECLARED
// fact (`--end-mode`), never re-derived here:
//
//   free-byte      some byte value never occurs in this corpus (valid UTF-8
//                  excludes 0xC0,0xC1,0xF5..0xFF). That byte is the end-of-text
//                  marker: the table maps it to stream symbol 0, which IS the
//                  declared END code. Nothing is written to the corpus; the
//                  marker is the virtual text index FILE_N.
//   declared-code  all 256 byte values occur. The table is the IDENTITY (the
//                  unique order-preserving 8-bit map onto the raw bytes) and END
//                  exists only in the frame: virtual index FILE_N, declared code
//                  0, never a stored value.
//
// THE TWO MODES SHARE ONE CODE SPACE. Written once:
//
//     code(i) = 0                              for i >= FILE_N (END)
//             = STREAM(REMAP[corpus[i]]) + SHIFT  otherwise
//
// STREAM(v) is the 8-bit value on disc and SHIFT is 0 for free-byte (data
// symbols start at 1, so END=0 is directly representable) and 1 for
// declared-code (data codes are exactly 1..256, the k=257 declaration). Code 0
// is therefore STRICTLY BELOW every data code in both modes, which is the whole
// point: every suffix comparison terminates on a data symbol difference and the
// suffix order is finite. k holds no storage anywhere.
static const u8* T = nullptr;
// N IS THE TEXT LENGTH AND INCLUDES THE DECLARED END SYMBOL: T' = corpus . END,
// one occurrence at index FILE_N. FILE_N is what is on disc; the END is a
// virtual index, never materialised.
static u64 N = 0, FILE_N = 0;
static constexpr unsigned DECLARED_K = 257;      // the declared alphabet size
static constexpr unsigned CODE_END = 0;          // the declared END code
static constexpr unsigned KEY_BYTES = 24;        // key width, in symbols
static constexpr unsigned CODE_SHIFT_FREE = 0, CODE_SHIFT_DECLARED = 1;
static u8 END_BYTE = 0xF5;                       // free-byte marker (declared)
static bool DECLARED_CODE = false;               // the preflight decision
static unsigned CODE_SHIFT = CODE_SHIFT_FREE;
// The stream value carried for the END row (the row whose predecessor is END,
// i.e. the row of position 0). free-byte: the marker byte's symbol, 0. In
// declared-code mode there IS no 8-bit END code (k=257 needs 257 codes), so the
// END row is POSITIONAL and its carried byte is declared as such in the header.
static constexpr unsigned END_STREAM_DECLARED_CODE = 256u;
static unsigned END_STREAM = 0;
// SENTINEL_BYTE/SENTINEL_SYMBOL keep their names for the callers that predate the
// declaration: they mean "the free-byte marker" and "its (0) symbol".
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
static inline u64 unpack_pos(u64 v) { return v & POS_MASK; }
// `aux` = key prefix length + 1 (1..24) for a TAIL GROUP row, 0 for a row whose
// key is the full KEY_BYTES symbols.
static inline u64 pack_aux(u64 v, unsigned aux) { return v | (u64(aux) << 56); }
static inline unsigned unpack_aux(u64 v) { return unsigned(v >> 56); }
static inline unsigned row_prefix(const Row& r) {
    const unsigned a = unpack_aux(r.pos);
    return a ? a - 1 : KEY_BYTES;
}
static inline unsigned key_byte(const Row& r, unsigned i) {
    const u64 w = i < 8 ? r.klo : (i < 16 ? r.khi : r.kq);
    return unsigned(w >> (56 - 8 * (i & 7))) & 0xffu;
}

// Declared code of text index i, in the k=257 code space (0 = END).
static inline unsigned code_at(u64 i) {
    if (i >= FILE_N) return CODE_END;
    return unsigned(REMAP ? REMAP[T[i]] : T[i]) + CODE_SHIFT;
}

// The 24-byte key at p, big-endian packed. SAMPLE/BOUNDS ONLY: a row's key is
// built by key_row()/tail_row() below, which mask it at the row's own prefix
// length. Band edges are QUANTILES, so a padded sample key costs nothing but a
// marginally different quantile - it never decides an order.
static inline void key_at(u64 p, u64& klo, u64& khi, u64& kq) {
    u8 b[KEY_BYTES];
    // REAL BYTES ONLY. The declared END is the virtual index FILE_N, so nothing
    // is read past the corpus and no byte value is invented for the key: the
    // zero fill below is a SAMPLE PAD, and the row comparator never reads it.
    const u64 avail = p < FILE_N ? FILE_N - p : 0;
    const u64 take = avail < KEY_BYTES ? avail : KEY_BYTES;
    if (take) std::memcpy(b, T + p, take);
    if (take < KEY_BYTES) std::memset(b + take, 0, KEY_BYTES - take);
    // THE KEY LIVES IN THE STREAM ALPHABET: every present corpus byte keeps its
    // relative order and the four absent/marker bytes are only ever reached by
    // the sample, so the big-endian key compare is the symbol compare.
    if (REMAP) for (u64 i = 0; i < take; ++i) b[i] = REMAP[b[i]];
    u64 lo, hi, q;
    std::memcpy(&lo, b, 8);
    std::memcpy(&hi, b + 8, 8);
    std::memcpy(&q, b + 16, 8);
    klo = bswap64(lo);
    khi = bswap64(hi);
    kq = bswap64(q);
}

// --------------------------------------------------------------------------
// SUFFIX COMPARISON OF THE DECLARED TEXT. Two rules, and nothing else:
//   1. compare SYMBOLS (stream values: the code map is monotone, so value order
//      IS code order) up to the shorter suffix's DATA length;
//   2. a suffix whose data part is a prefix of the other's is SMALLER, because
//      its next symbol is END, declared code 0, below every data code.
// `cap` is the number of comparable DATA symbols: min(|a|,|b|) - 1. Nothing at
// index FILE_N (the declared END) is ever read from the corpus, so there is no
// out-of-bounds byte and no reliance on a pad value.
// --------------------------------------------------------------------------
static inline int suf_cmp_lcp(u64 a, u64 b, u64 from, u64& lcp) {
    const u64 cap = std::min(N - a, N - b) - 1;   // END excluded: it is not a byte
    u64 i = from;
    if (i > cap) i = cap;
    lcp = i;
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
    }
    for (; i < cap; ++i) {
        const u8 xb = REMAP ? REMAP[T[a + i]] : T[a + i];
        const u8 yb = REMAP ? REMAP[T[b + i]] : T[b + i];
        if (xb != yb) { lcp = i; return xb < yb ? -1 : 1; }
    }
    // END RULE. All comparable data symbols agree, so the next symbol of the
    // shorter suffix is END (code 0) and of the longer one is a data symbol:
    // the shorter suffix sorts FIRST. Equal lengths mean the same suffix.
    lcp = cap;
    return (N - a) < (N - b) ? -1 : ((N - a) > (N - b) ? 1 : 0);
}

static inline int suf_cmp_from(u64 a, u64 b, u64 from) {
    u64 ignored = 0;
    return suf_cmp_lcp(a, b, from, ignored);
}

// --------------------------------------------------------------------------
// ROW CONSTRUCTION.
//
// make_row() is the reference builder (used by the selftest for EVERY position,
// and by the 150 MB scan for TAIL GROUP rows through tail_row()). A row whose
// suffix is at least KEY_BYTES data symbols long is keyed by the KEY_BYTES
// stream symbols that follow it; a row whose suffix is shorter is keyed by the
// symbols that exist and carries that length, so no pad value is ever compared.
// --------------------------------------------------------------------------
static inline void key_words(const u8* b, u64& a1, u64& a2, u64& a3) {
    u64 lo, hi, q;
    std::memcpy(&lo, b, 8);
    std::memcpy(&hi, b + 8, 8);
    std::memcpy(&q, b + 16, 8);
    a1 = bswap64(lo); a2 = bswap64(hi); a3 = bswap64(q);
}

// The carried BWT symbol of position p: its predecessor's stream symbol, and the
// declared END placeholder for position 0 (the row whose predecessor is END).
static inline u8 carried_symbol(u64 p) {
    if (p == 0) return u8(END_STREAM);
    return REMAP ? REMAP[T[p - 1]] : T[p - 1];
}

static Row tail_row(u64 p) {
    if (!(p + KEY_BYTES > FILE_N && p <= FILE_N))
        die("tail_row: position is not in the tail group");
    const unsigned dl = unsigned(FILE_N - p);   // 0..KEY_BYTES-1 data symbols
    u8 b[KEY_BYTES] = {0};
    for (unsigned i = 0; i < dl; ++i) b[i] = REMAP ? REMAP[T[p + i]] : T[p + i];
    Row r{};
    key_words(b, r.klo, r.khi, r.kq);
    // aux = prefix length + 1, so 0 unambiguously means "the whole key".
    r.pos = pack_aux(pack_sym(p, carried_symbol(p)), dl + 1);
    return r;
}

static Row make_row(u64 p) {
    if (p + KEY_BYTES > FILE_N) return tail_row(p);
    u8 b[KEY_BYTES];
    // Non-tail: KEY_BYTES data symbols exist (p + KEY_BYTES <= FILE_N).
    std::memcpy(b, T + p, KEY_BYTES);
    if (REMAP) for (unsigned i = 0; i < KEY_BYTES; ++i) b[i] = REMAP[b[i]];
    Row r{};
    key_words(b, r.klo, r.khi, r.kq);
    r.pos = pack_sym(p, carried_symbol(p));   // aux 0
    return r;
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
// big-endian, so an integer compare IS the symbol order, and clz of the xor
// gives the number of leading EQUAL symbols for free. Only a full 24-symbol tie
// falls through to the text.
//
// THE TAIL GROUP IS THE ONE PLACE THE END MEETS DATA. A row whose suffix has
// fewer than KEY_BYTES data symbols cannot be keyed by padding: no pad value is
// below every symbol (in declared-code mode every one of the 256 values IS a
// symbol). Those rows carry their prefix length instead, the comparison masks
// the key at min(prefix), and the shorter prefix sorts first - which is the END
// rule, applied WITHOUT a pad.
static inline int row_cmp_lcp(const Row& a, const Row& b, u64& lcp) {
    const unsigned pa = row_prefix(a), pb = row_prefix(b);
    const unsigned lim = pa < pb ? pa : pb;
    if (lim == KEY_BYTES) {   // both keys are full: integral compare, as before
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
        return suf_cmp_lcp(unpack_pos(a.pos), unpack_pos(b.pos), KEY_BYTES, lcp);
    }
    unsigned d = 0;
    while (d < lim) {
        const unsigned x = key_byte(a, d), y = key_byte(b, d);
        if (x != y) { lcp = d; return x < y ? -1 : 1; }
        ++d;
    }
    lcp = d;
    if (pa != pb) return pa < pb ? -1 : 1;   // END (code 0) is below every code
    die("tail group: two rows have equal finite keys (the END is unique)");
    return 0;
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

// ---------------------------------------------------------------------------
// G6 - THE TAIL GROUP ORACLE.  (scatter_sort --selftest-tail)
//
// A hand-built fixture whose text ENDS MID-RECORD: fewer than KEY_BYTES bytes
// follow its last record separator (0x1E, the pile fixture's separator), so the
// final positions cannot be keyed at all and the declared END meets DATA inside
// the key window. Band/edge code is where every bug of this week lived, so this
// gate is a brute-force comparison, not a smoke test:
//
//   * the producer's OWN row construction (make_row: tail_row for the short rows,
//     the full KEY_BYTES key for the rest) and the producer's OWN comparator
//     (row_cmp_lcp through merge_sort_lcp, i.e. the LCP-carrying merge) produce
//     the order;
//   * a brute-force SA of the DECLARED text corpus . END is computed by direct
//     suffix comparison over the k=257 code space, with no key, no band, no
//     padding and no shared code with the producer;
//   * EVERY rank is printed (position, key prefix length, carried symbol) and the
//     BWT is printed symbol by symbol, in BOTH realizations.
//
// A corpus that ends mid-record is the adversarial case for the tail group; a
// declared-code fixture additionally contains ALL 256 byte values, which is what
// makes that realization legal.
// ---------------------------------------------------------------------------
static std::string hex_bytes(const std::vector<u8>& v, size_t from, size_t to) {
    std::string out;
    char b[8];
    for (size_t i = from; i < to && i < v.size(); ++i) {
        std::snprintf(b, sizeof b, "%02x", unsigned(v[i]));
        if (!out.empty()) out.push_back(' ');
        out += b;
    }
    return out;
}

// Brute-force suffix array of the declared text, comparing CODES: code 0 is END
// and is below every data code. This is the oracle: O(n^2 log n), no keys.
static std::vector<u64> brute_sa(const u8* text, u64 fileN, const u8* remap, unsigned shift) {
    const u64 n = fileN + 1;
    std::vector<u64> sa(n);
    for (u64 i = 0; i < n; ++i) sa[i] = i;
    auto code = [&](u64 i) -> unsigned {
        if (i >= fileN) return CODE_END;
        return unsigned(remap ? remap[text[i]] : text[i]) + shift;
    };
    std::sort(sa.begin(), sa.end(), [&](u64 a, u64 b) {
        const u64 la = n - a, lb = n - b;
        const u64 cap = std::min(la, lb) - 1;   // the END is not a byte
        for (u64 i = 0; i < cap; ++i) {
            const unsigned x = code(a + i), y = code(b + i);
            if (x != y) return x < y;
        }
        return la < lb;    // END (code 0) first
    });
    return sa;
}

static bool tail_case(const char* name, const std::vector<u8>& corpus, bool declared,
                      const u8* remap) {
    FILE_N = corpus.size();
    N = FILE_N + 1;
    T = corpus.data();
    REMAP = remap;
    DECLARED_CODE = declared;
    CODE_SHIFT = declared ? CODE_SHIFT_DECLARED : CODE_SHIFT_FREE;
    END_STREAM = 0;
    build_pairmap();

    // The producer's rows, sorted by the producer's own merge sort (with LCPs).
    std::vector<Row> rows(N);
    for (u64 p = 0; p < N; ++p) rows[p] = make_row(p);
    std::vector<u8> lcp;
    std::vector<std::pair<u64, u64>> esc;
    merge_sort_lcp(rows, lcp, esc, 0);

    const std::vector<u64> truth = brute_sa(corpus.data(), FILE_N, remap, CODE_SHIFT);

    std::vector<u64> got(N);
    bool same = true;
    size_t firstBad = SIZE_MAX;
    u64 tailSeen = 0, endRankSeen = SIZE_MAX;
    for (u64 r = 0; r < N; ++r) {
        got[r] = unpack_pos(rows[r].pos);
        if (unpack_aux(rows[r].pos)) ++tailSeen;
        if (got[r] != truth[r] && same) { same = false; firstBad = r; }
        if (unpack_pos(rows[r].pos) == FILE_N) endRankSeen = r;
    }
    const u64 tailExpect = N < KEY_BYTES ? N : KEY_BYTES;

    std::printf("TAIL_CASE name=%s realization=%s n=%llu k=%u tail_rows=%llu/%llu "
                "end_row_rank=%lld sa_match=%d\n",
                name, declared ? "declared-code" : "free-byte",
                (unsigned long long)N, DECLARED_K,
                (unsigned long long)tailSeen, (unsigned long long)tailExpect,
                (long long)(endRankSeen == SIZE_MAX ? -1 : (long long)endRankSeen),
                int(same));
    {
        long long lastSep = -1;
        for (size_t i = 0; i < corpus.size(); ++i) if (corpus[i] == 0x1E) lastSep = (long long)i;
        std::printf("TAIL_CASE_TEXT n_bytes=%llu last_sep=%lld bytes_after_last_sep=%lld "
                    "key_window=%u ends_mid_record=%d\n",
                    (unsigned long long)corpus.size(), lastSep,
                    (long long)((long long)corpus.size() - 1 - lastSep), KEY_BYTES,
                    int(lastSep >= 0 && (long long)corpus.size() - 1 - lastSep < (long long)KEY_BYTES));
    }
    std::printf("TAIL_CASE_SA_EXACT");
    for (u64 r = 0; r < N; ++r) std::printf(" %llu", (unsigned long long)got[r]);
    std::printf("\n");
    if (!same) {
        std::printf("TAIL_CASE_SA_BRUTE");
        for (u64 r = 0; r < N; ++r) std::printf(" %llu", (unsigned long long)truth[r]);
        std::printf("\nTAIL_CASE_FIRST_DIFF rank=%zu got=%llu want=%llu\n", firstBad,
                    (unsigned long long)got[firstBad], (unsigned long long)truth[firstBad]);
        return false;
    }
    // Every rank: position, key prefix length in symbols, carried symbol (as the
    // DECLARED code, so EOF is printed as END rather than as a byte).
    std::printf("TAIL_CASE_RANKS rank pos key_symbols carried_code\n");
    for (u64 r = 0; r < N; ++r) {
        const u64 p = unpack_pos(rows[r].pos);
        const unsigned sy = unsigned((rows[r].pos >> 48) & 0xffu);
        const unsigned carriedCode = (p == 0) ? CODE_END
                                              : unsigned(remap ? remap[corpus[p - 1]] : corpus[p - 1]) + CODE_SHIFT;
        (void)sy;
        std::printf("TAIL_CASE_RANK %llu %llu %u %u%s\n", (unsigned long long)r,
                    (unsigned long long)p, row_prefix(rows[r]), carriedCode,
                    (p == 0) ? " END" : "");
    }
    // THE BWT, symbol by symbol, against the oracle's own derivation.
    std::printf("TAIL_CASE_BWT");
    bool bwtOk = true;
    for (u64 r = 0; r < N; ++r) {
        const u64 p = truth[r];
        const unsigned want = (p == 0) ? CODE_END
                                       : unsigned(remap ? remap[corpus[p - 1]] : corpus[p - 1]) + CODE_SHIFT;
        const unsigned have = unsigned((rows[r].pos >> 48) & 0xffu);
        const unsigned haveCode = (p == 0) ? CODE_END : have + CODE_SHIFT;
        if (haveCode != want) {
            bwtOk = false;
            std::printf(" [rank %llu have=%u want=%u]", (unsigned long long)r, haveCode, want);
        } else if (want == CODE_END) std::printf(" END");
        else std::printf(" %u", want);
    }
    std::printf("\n");
    // Adjacent-rank LCP column vs a direct brute-force LCP (symbols, END excluded).
    bool lcpOk = true;
    for (u64 r = 1; r < N; ++r) {
        const u64 a = truth[r - 1], b = truth[r];
        const u64 cap = std::min(N - a, N - b) - 1;
        u64 k = 0;
        while (k < cap) {
            const unsigned x = (a + k >= FILE_N) ? CODE_END : unsigned(remap ? remap[corpus[a + k]] : corpus[a + k]) + CODE_SHIFT;
            const unsigned y = (b + k >= FILE_N) ? CODE_END : unsigned(remap ? remap[corpus[b + k]] : corpus[b + k]) + CODE_SHIFT;
            if (x != y) break;
            ++k;
        }
        u64 reported = lcp[r];
        if (reported == 255)
            for (auto& e : esc) if (e.first == r) { reported = e.second; break; }
        if (reported != k) { lcpOk = false; }
    }
    std::printf("TAIL_CASE_BWT_MATCH=%d LCP_MATCH=%d\n", int(bwtOk), int(lcpOk));
    return bwtOk && lcpOk;
}

static int selftest_tail() {
    // ---- fixtures. Every one ENDS MID-RECORD: the last 0x1E is followed by
    // fewer than KEY_BYTES bytes, so the final positions are unkeyable.
    std::vector<std::vector<u8>> fixtures;
    // (1) the pile shape: records separated by 0x1E, ending mid-record; the filler
    // contains ALL 256 byte values so declared-code mode is legal for it.
    {
        std::vector<u8> f;
        for (unsigned i = 0; i < 256; ++i) f.push_back(u8(i));
        for (unsigned rep = 0; rep < 3; ++rep)
            for (unsigned i = 0; i < 256; ++i) f.push_back(u8((i * 7 + rep) & 0xff));
        f.push_back(0x1E);
        const char* tail = "305,1,1,0,0,3";      // 15 bytes: < KEY_BYTES after the sep
        for (const char* q = tail; *q; ++q) f.push_back(u8(*q));
        fixtures.push_back(f);
    }
    // (2) a tail that is ALL ZERO bytes after a separator: the case where a zero
    // pad and a real zero symbol are indistinguishable.
    {
        std::vector<u8> f;
        for (unsigned i = 0; i < 40; ++i) f.push_back(u8('a' + (i % 3)));
        f.push_back(0x1E);
        for (unsigned i = 0; i < 9; ++i) f.push_back(0x00);
        f.push_back(0x1E);
        for (unsigned i = 0; i < 5; ++i) f.push_back(0x00);
        fixtures.push_back(f);
    }
    // (3) periodic: every suffix shares a long prefix with its neighbour.
    {
        std::vector<u8> f;
        for (unsigned i = 0; i < 60; ++i) f.push_back(u8(i % 2 ? 'a' : 'b'));
        f.push_back(0x1E);
        for (unsigned i = 0; i < 7; ++i) f.push_back(u8(i % 2 ? 'a' : 'b'));
        fixtures.push_back(f);
    }
    // (4) tiny: shorter than the key window, so EVERY row is in the tail group.
    {
        std::vector<u8> f{0x1E, 'x', 0x1E, 'y', 0x00};
        fixtures.push_back(f);
    }

    bool all = true;
    for (size_t c = 0; c < fixtures.size(); ++c) {
        std::vector<u8> corpus = fixtures[c];
        u8 freeMap[256], ident[256];
        std::array<bool, 256> present{};
        for (u8 b : corpus) present[b] = true;
        const u8 marker = 0xF5;
        unsigned next = 1;
        for (unsigned b = 0; b < 256; ++b)
            if (b != marker && present[b]) freeMap[b] = u8(next++);
        freeMap[marker] = 0;
        unsigned fill = next;
        for (unsigned b = 0; b < 256; ++b)
            if (!present[b] && b != marker) freeMap[b] = u8(fill++);
        for (unsigned b = 0; b < 256; ++b) ident[b] = u8(b);
        bool everyByte = true;
        for (unsigned b = 0; b < 256; ++b) if (!present[b]) everyByte = false;

        char label[64];
        std::snprintf(label, sizeof label, "fixture%zu", c);
        std::printf("TAIL_FIXTURE index=%zu bytes=%zu all_256_present=%d marker_0x%02x_occurs=%d\n",
                    c, corpus.size(), int(everyByte), unsigned(marker), int(present[marker]));
        std::printf("TAIL_FIXTURE_BYTES %s\n", hex_bytes(corpus, 0, corpus.size()).c_str());
        // BOTH REALIZATIONS for every fixture. free-byte requires the marker byte
        // to be absent (a corpus that uses it cannot carry the marker); the
        // declared-code realization is computable for ANY corpus - the k=257
        // declaration with the identity table - it is merely FORCED when all 256
        // byte values occur, which is what fixture 0 is for.
        if (!present[marker])
            all &= tail_case(label, corpus, false, freeMap);
        else
            std::printf("TAIL_CASE_SKIP fixture=%zu realization=free-byte "
                        "reason=marker_byte_0x%02x_occurs\n", c, unsigned(marker));
        all &= tail_case(label, corpus, true, ident);
        std::fprintf(stderr, "TAIL_FIXTURE_LEGAL index=%zu free_byte_legal=%d "
                             "declared_code_forced=%d\n", c,
                             int(!present[marker]), int(everyByte));
    }
    std::printf("TAIL_SELFTEST_%s fixtures=%zu realizations_both=1 pad_free=1\n",
                all ? "PASS" : "FAIL", fixtures.size());
    return all ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string corpus, out, scratch = "/tmp/ss_scratch", lcpOut, isaOut;
    u64 isaK = 1024;
    double ram_gb = 1.0;
    double scratch_cap_gb = 0.0;      // 0 = 2n (the operator's law)
    u64 threads = 32, par_bands = 1, sample_n = 4000000;
    bool keep_rows = false;
    std::string remapPath, sort_mode = "std", tapeBands, orderSample;
    u64 sampleLo = 0, sampleCount = 4096;
    u64 pendingBands = 8;
    std::string endMode;              // the preflight decision, DECLARED not inferred
    unsigned endByte = 0xF5;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() { if (i + 1 >= argc) die("missing value for " + a); return std::string(argv[++i]); };
        if (a == "--order-sample-out") orderSample = nxt();
        else if (a == "--order-sample-lo") sampleLo = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--order-sample-count") sampleCount = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--tape-bands") tapeBands = nxt();
        else if (a == "--pending-bands") pendingBands = std::strtoull(nxt().c_str(), nullptr, 10);
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
        else if (a == "--isa-sample-out") isaOut = nxt();
        else if (a == "--isa-sample-k") isaK = std::strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--end-mode") endMode = nxt();
        else if (a == "--end-byte") endByte = unsigned(std::strtoul(nxt().c_str(), nullptr, 0));
        else if (a == "--selftest-tail") return selftest_tail();
        else die("unknown arg " + a);
    }
    (void)sort_mode;
    if (corpus.empty() || (out.empty() && tapeBands.empty())) die("--corpus and --out required");

    if (!tapeBands.empty() && (!pendingBands || keep_rows))
        die("tape bands require K>0 and no keep-rows");
    int fd = ::open(corpus.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st)) die("cannot open/stat corpus");
    FILE_N = (u64)st.st_size;
    if (!FILE_N) die("empty corpus");
    N = FILE_N + 1;                 // the appended end-of-text byte
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
        // The table's mode-specific claims are proved in the DECLARED-REALIZATION
        // block below (free-byte: the marker maps to 0 and nothing else does;
        // declared-code: the table is the identity).
    }
    // ---- THE REALIZATION IS DECLARED, NEVER INFERRED. `--end-mode` is the
    // preflight decision `xsa_finish --sentinel-remap` journaled, and every claim
    // it makes is CHECKED against the table and the corpus here (a corpus that
    // fits neither mode is an internal error, so it refuses).
    if (endMode.empty()) endMode = "free-byte";
    if (endByte > 255) die("--end-byte must be a byte value");
    END_BYTE = u8(endByte);
    if (endMode == "free-byte") {
        DECLARED_CODE = false;
        CODE_SHIFT = CODE_SHIFT_FREE;
        if (REMAP) {
            if (REMAP[END_BYTE] != SENTINEL_SYMBOL)
                die("free-byte mode: the remap must map the end byte to stream symbol 0");
            for (unsigned c = 0; c < 256; ++c)
                if (c != END_BYTE && REMAP[c] == SENTINEL_SYMBOL)
                    die("free-byte mode: the remap maps a text byte onto the END symbol");
        } else {
            // no table: the end byte must be absent from the corpus (proved by the
            // class histogram below) - identity stream symbols, END still 0.
        }
        END_STREAM = SENTINEL_SYMBOL;
    } else if (endMode == "declared-code") {
        DECLARED_CODE = true;
        CODE_SHIFT = CODE_SHIFT_DECLARED;
        if (REMAP) {
            for (unsigned c = 0; c < 256; ++c)
                if (REMAP[c] != u8(c))
                    die("declared-code mode: the remap must be the IDENTITY (the unique "
                        "order-preserving 8-bit map onto the raw bytes)");
        }
        // END has NO 8-bit stream code here: k=257 needs 257 codes and all 256 are
        // data. The END row is POSITIONAL (declared in the stream header).
        END_STREAM = 0;
    } else {
        die("--end-mode must be free-byte or declared-code");
    }
    {
        char hex[8];
        std::snprintf(hex, sizeof hex, "0x%02x", unsigned(END_BYTE));
        std::fprintf(stderr,
            "END_SYMBOL mode=%s k=%u end_symbol=%u end_byte=%s code_shift=%u "
            "tail_bytes=%u end_stream=%u end_positional=%u\n",
            DECLARED_CODE ? "declared-code" : "free-byte", DECLARED_K, CODE_END,
            DECLARED_CODE ? "none" : hex, CODE_SHIFT, KEY_BYTES, END_STREAM,
            DECLARED_CODE ? 1u : 0u);
    }
    // The tail group: every position whose KEY_BYTES-symbol key would reach the
    // declared END. They are sorted by the FULL comparator, never by a pad.
    const u64 tailRows = N < KEY_BYTES ? N : KEY_BYTES;
    std::fprintf(stderr, "TAIL_GROUP rows=%llu first_pos=%llu end_pos=%llu full_comparator=1 pad_free=1\n",
                 (unsigned long long)tailRows,
                 (unsigned long long)(N - tailRows),
                 (unsigned long long)(N - 1));
    build_pairmap();
    // CLASS OF A TEXT POSITION in the k=257 code space (0 = END, 1..256 = data
    // codes). This is the ONLY place a code (rather than a stored symbol) enters
    // the producer: waves are key ranges in this space, and the space is
    // order-isomorphic to the stream alphabet, so nothing downstream changes.
    auto sym_at = [](u64 p) -> unsigned { return code_at(p); };

    const u64 row_bytes = sizeof(Row);
    const u64 ram_bytes = (u64)(ram_gb * 1e9);
    if (par_bands < 1) par_bands = 1;
    u64 rows_per_band = std::max<u64>(1, ram_bytes / (row_bytes * par_bands));
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
        "SS_START n=%" PRIu64 " row_bytes=%zu cap_bytes=%" PRIu64 " (%.2fn) "
        "rows_per_wave=%" PRIu64 " waves=%" PRIu64 " ram_gb=%.2f rows_per_band=%" PRIu64
        " par_bands=%" PRIu64 " threads=%" PRIu64 "\n",
        N, row_bytes, cap_bytes, double(cap_bytes) / double(N), rows_per_wave, waves,
        ram_gb, rows_per_band, par_bands, threads);

    // ---- exact first-symbol histogram: one parallel pass, DECLARED_K (=257)
    // counters. This is what makes the per-wave FILTER cheap: a rejected position
    // costs one load instead of a 24-symbol key computation (16 waves x n key
    // computations was 4.6x the whole sort at 150 MB). Class 0 is the END row and
    // carries exactly one row; the code space, not a byte value, is what the
    // histogram is over.
    std::vector<u64> hist(DECLARED_K, 0);
    {
        const u64 nt = std::max<u64>(1, threads);
        std::vector<std::vector<u64>> lh(nt, std::vector<u64>(DECLARED_K, 0));
        std::vector<std::thread> ts;
        for (u64 t = 0; t < nt; ++t)
            ts.emplace_back([&, t]() {
                const u64 lo = N * t / nt, hi = N * (t + 1) / nt;
                auto& h = lh[t];
                for (u64 p = lo; p < hi; ++p) ++h[sym_at(p)];
            });
        for (auto& th : ts) th.join();
        for (u64 t = 0; t < nt; ++t)
            for (unsigned i = 0; i < DECLARED_K; ++i) hist[i] += lh[t][i];
    }
    {
        u64 total = 0;
        for (unsigned i = 0; i < DECLARED_K; ++i) total += hist[i];
        if (total != N) die("first-symbol histogram does not cover the declared text");
        if (hist[CODE_END] != 1) die("the declared END code must occur exactly once");
    }
    if (!DECLARED_CODE && !REMAP && hist[unsigned(END_BYTE)] != 0)
        die("free-byte mode without a table: the end byte must not occur in the corpus");
    if (!DECLARED_CODE && REMAP) {
        // END is directly representable: no DATA code may collide with it.
        if (hist[CODE_END] != 1 || hist[SENTINEL_SYMBOL] != 1)
            die("free-byte mode: END must be the only row in the minimal code class");
    }
    if (DECLARED_CODE) {
        // Every data code of the k=257 declaration must be reachable, and END must
        // be the only symbol with no 8-bit code.
        unsigned present = 0;
        for (unsigned i = 1; i < DECLARED_K; ++i) if (hist[i]) ++present;
        std::fprintf(stderr, "END_SYMBOL_DECLARED_CODES classes=%u data_codes_present=%u\n",
                     DECLARED_K, present);
    }
    // ---- wave list: greedy byte ranges, each <= rows_per_wave rows. A byte whose
    // class ALONE exceeds the cap is split by its second byte (2-level wave).
    struct Wave { unsigned lo, hi; bool lvl2; unsigned b; unsigned lo2, hi2; };
    std::vector<Wave> wl;
    {
        unsigned lo = 0;
        u64 acc = 0;
        for (unsigned c = 0; c < DECLARED_K; ++c) {
            if (hist[c] > rows_per_wave) {
                if (lo < c) { wl.push_back({lo, c, false, 0, 0, 0}); lo = c; acc = 0; }
                // split this class by its second symbol (also a code)
                std::vector<u64> h2(DECLARED_K, 0);
                for (u64 p = 0; p < N; ++p) {
                    if (sym_at(p) != c) continue;
                    h2[sym_at(p + 1)]++;
                }
                unsigned l2 = 0; u64 a2 = 0;
                for (unsigned d = 0; d < DECLARED_K; ++d) {
                    if (a2 && a2 + h2[d] > rows_per_wave) {
                        wl.push_back({c, c + 1, true, c, l2, d}); l2 = d; a2 = 0;
                    }
                    if (!tapeBands.empty() && h2[d] > rows_per_wave) die("class-pair exceeds wave cap");
                    a2 += h2[d];
                }
                if (l2 < DECLARED_K) wl.push_back({c, c + 1, true, c, l2, DECLARED_K});
                lo = c + 1; acc = 0;
                continue;
            }
            if (acc && acc + hist[c] > rows_per_wave) {
                wl.push_back({lo, c, false, 0, 0, 0}); lo = c; acc = 0;
            }
            acc += hist[c];
        }
        if (lo < DECLARED_K) wl.push_back({lo, DECLARED_K, false, 0, 0, 0});
    }
    waves = wl.size();
    std::fprintf(stderr, "SS_WAVES count=%" PRIu64 " (byte-range waves)\n", waves);

    // ---------------- SAMPLE once: band edges inside each wave ----------------
    const double t0 = now_sec();
    const u64 stride = std::max<u64>(1, N / std::max<u64>(1, sample_n));
    std::vector<u64> sk;
    sk.reserve(N / stride + 2);
    u64 rng = 0x243f6a8885a308d3ull;
    for (u64 p = 0; p < N; p += stride) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        u64 q = p + (rng >> 11) % stride;
        if (q >= N) q = N - 1;
        u64 a, b, c;
        key_at(q, a, b, c);
        sk.push_back(a);
    }
    std::sort(sk.begin(), sk.end());
    const double t_sample = now_sec() - t0;
    std::fprintf(stderr, "SS_SAMPLE samples=%zu stride=%" PRIu64 " wall=%.2f\n",
                 sk.size(), stride, t_sample);

    // quantile index helper on the sampled keys
    auto qkey = [&](double frac) -> u64 {
        if (sk.empty()) return ~0ull;
        double idx = frac * double(sk.size() - 1);
        if (idx < 0) idx = 0;
        u64 i = (u64)idx;
        if (i >= sk.size()) i = sk.size() - 1;
        return sk[i];
    };

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
    auto marker = [&](const std::string& path) {
        int f = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (f < 0) die("create tape marker " + path);
        ::close(f);
    };
    if (!tapeBands.empty()) {
        struct stat ts{};
        if (::stat(tapeBands.c_str(), &ts) != 0 || !S_ISDIR(ts.st_mode)) die("tape directory must exist and be fresh");
        if (::access((tapeBands + "/manifest").c_str(), F_OK) == 0) die("stale tape manifest");
        int mf = ::open((tapeBands + "/manifest").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (mf < 0) die("create tape manifest");
        std::string meta = std::to_string(N) + " 1\n";
        if (::write(mf, meta.data(), meta.size()) != (ssize_t)meta.size()) die("write tape manifest");
        ::close(mf);
        marker(tapeBands + "/ready");
        std::fprintf(stderr, "SS_TAPE K=%llu packed_row_bytes=8 wave_cap=%llu\n", (unsigned long long)pendingBands, (unsigned long long)cap_bytes);
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
        // h[5]: 0 for the free-byte realization (the SA file is byte-identical to
        // the one 4c4d086 wrote), and the DECLARATION word in declared-code mode:
        // bit 0 set, bits 8..15 = the placeholder byte carried on the END row.
        // There is no transport frame in either mode.
        h[5] = DECLARED_CODE ? (1ull | (u64(END_STREAM) << 8)) : 0ull;
        if (ofd >= 0 && ::write(ofd, h, 48) != 48) die("write header");
    }
    const std::string symPath = out + ".sym";
    int sfd = tapeBands.empty() ? ::open(symPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (tapeBands.empty() && sfd < 0) die("create sym stream");

    // the LCP column: n bytes of adjacent-rank LCP + escapes, in the SXLC format
    // the finish already reads via --lcp-column.
    int lfd = -1;
    if (!lcpOut.empty()) {
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
    std::vector<u64> bandFirstPos, bandLastPos;
    std::vector<u8> cat(1u << 22), symcat(1u << 22);
    u64 rows_done = 0, ties_total = 0, emitted = 0, wave_rows = 0, tail_total = 0;
    u64 prev_wave_last = ~0ull;   // last row position emitted by the previous wave
    const double t1 = now_sec();

    for (u64 w = 0; w < waves; ++w) {
        const double tw = now_sec();
        const Wave& W = wl[w];
        // Bands inside the wave: rows_per_band each, from the same sample.
        // BAND EDGES MUST BE DATA QUANTILES, NOT UNIFORM IN KEY SPACE. Uniform
        // subdivision put nearly every row of a fat wave in ONE band (spaces
        // dominate the alphabet), which serialised the sort: measured 271 s for a
        // single wave of the 10 GB run. The sample is already sorted, so the
        // wave's slice of it gives exact-enough quantiles.
        const u64 nb = (u64)std::max<double>(1.0, double(rows_per_wave) / double(rows_per_band));
        std::vector<u64> bounds;
        {
            // For a TWO-LEVEL wave the population is the wave's two-byte range,
            // not the whole first byte: quantiles taken over the wrong population
            // produce wildly unbalanced bands (measured: one band's sort at 341 s
            // inside a 352 s wave - the space wave).
            u64 klo_s, khi_s;
            if (W.lvl2) {
                klo_s = (u64(W.lo) << 56) | (u64(W.lo2) << 48);
                khi_s = (u64(W.hi) << 56) | (u64(W.hi2 >= 256 ? 255 : W.hi2) << 48) |
                        ((W.hi2 >= 256) ? ~0ull : 0ull);
                if (W.hi2 >= 256) khi_s = ~0ull;
            } else {
                klo_s = (u64(W.lo) << 56);
                khi_s = (W.hi >= 256) ? ~0ull : ((u64(W.hi) << 56) - 1);
            }
            auto lo_it = std::lower_bound(sk.begin(), sk.end(), klo_s);
            auto hi_it = std::upper_bound(sk.begin(), sk.end(), khi_s);
            const u64 n_in = (u64)(hi_it - lo_it);
            for (u64 b = 1; b < nb && n_in > 1; ++b) {
                const u64 idx = (u64)((long double)n_in * b / nb);
                if (idx == 0 || idx >= n_in) continue;
                const u64 k = *(lo_it + idx);
                if (bounds.empty() || k > bounds.back()) bounds.push_back(k);
            }
            if (bounds.empty() && nb > 1) bounds.push_back(khi_s);  // degenerate: one band
        }
        const u64 nbands = bounds.size() + 1;

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
                // look-ahead so a 16-byte key and the symbol are always in range
                const u64 from = (base ? base - 1 : base);
                // NEVER READ PAST THE FILE: the declared END (index FILE_N) has no
                // byte on disc, so the buffer tail is zero-filled rather than read.
                // A zero here is a BUFFER fill, never a key: the TAIL GROUP rows are
                // built from the corpus by tail_row() and are not keyed from buf.
                const u64 want = std::min<u64>(chunk + 1 + 48, FILE_N > from ? FILE_N - from : 0);
                size_t got = 0;
                while (got < want) {
                    ssize_t r = ::pread(fd, buf.data() + got, want - got, off_t(from + got));
                    if (r <= 0) die("pread scan");
                    got += size_t(r);
                }
                if (want < buf.size())
                    std::memset(buf.data() + want, 0, buf.size() - want);
                // REMAP THE CHUNK ONCE, in place, instead of 24 lookups per row.
                // Per-row key building with byte-by-byte REMAP[] was the real cost:
                // ~400 ns/row for the included rows (~43M per wave at 1 GB), which
                // dominated the whole scan. One sequential pass over the chunk
                // makes every key a plain big-endian load.
                if (REMAP)
                    for (u64 i = 0; i < got; ++i) buf[i] = REMAP[buf[i]];
                const u64 off = base - from;   // index in buf of position `base`
                for (u64 j = 0; j < chunk; ++j) {
                    const u64 p = base + j;
                    const u64 bj = off + j;
                    if (p + KEY_BYTES > FILE_N) {
                        // ---- THE TAIL GROUP: this row's 24-symbol key would reach
                        // the declared END, so it is NOT keyed from the buffer. It
                        // is built by tail_row() with its own prefix length and
                        // sorted by the FULL comparator.
                        Row r = tail_row(p);
                        const unsigned cls = code_at(p);
                        if (cls < W.lo || cls >= W.hi) continue;
                        if (W.lvl2) {
                            const unsigned c1 = code_at(p + 1);
                            if (c1 < W.lo2 || c1 >= W.hi2) continue;
                        }
                        u64 b = (u64)(std::upper_bound(bounds.begin(), bounds.end(), r.klo) - bounds.begin());
                        if (b >= nbands) b = nbands - 1;
                        auto& v = bufs[t][b];
                        v.insert(v.end(), (const u8*)&r, (const u8*)&r + sizeof(Row));
                        if (v.size() >= bufsz) flush_buf(t, b);
                        continue;
                    }
                    // class code of the row's first symbol, in the k=257 code space
                    const unsigned c0 = unsigned(buf[bj]) + CODE_SHIFT;
                    if (c0 < W.lo || c0 >= W.hi) continue;
                    if (W.lvl2) {
                        const unsigned c1 = unsigned(buf[bj + 1]) + CODE_SHIFT;
                        if (c1 < W.lo2 || c1 >= W.hi2) continue;
                    }
                    // THE WHOLE 24-SYMBOL KEY. Leaving kq unset here was a real bug:
                    // the comparator's third word then read uninitialised memory,
                    // and it still passed sa_check once - the verifier catches order
                    // errors, not undefined values that happen to agree.
                    u64 a1, a2, a3;
                    std::memcpy(&a1, buf.data() + bj, 8);
                    std::memcpy(&a2, buf.data() + bj + 8, 8);
                    std::memcpy(&a3, buf.data() + bj + 16, 8);
                    a1 = bswap64(a1); a2 = bswap64(a2); a3 = bswap64(a3);
                    // The carried BWT symbol. Position 0's predecessor is the
                    // declared END: the marker byte's symbol in free-byte mode, and
                    // the positional code in declared-code mode (declared in the
                    // stream header, never an 8-bit guess).
                    const u8 sy = p == 0 ? u8(END_STREAM) : buf[bj - 1];
                    Row r{};
                    r.klo = a1;
                    r.khi = a2;
                    r.kq = a3;
                    r.pos = pack_sym(p, sy);   // aux 0: the full KEY_BYTES prefix
                    u64 b = (u64)(std::upper_bound(bounds.begin(), bounds.end(), a1) - bounds.begin());
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
            u64 acc = emitted;
            for (u64 b = 0; b < nbands; ++b) { band_off[b] = acc; acc += band_rows[b]; }
            wave_rows = acc - emitted;
        }
        // ---- sort this wave's bands (parallel), writing at their own offsets ----
        std::atomic<u64> next_band{0};
        std::atomic<u64> ties_w{0};
        std::atomic<u64> tail_w{0};   // rows carrying a masked (tail-group) key
        std::vector<double> tRead(nbands, 0.0), tSort(nbands, 0.0), tWrite(nbands, 0.0);
        bandFirstPos.assign(nbands, 0);
        bandLastPos.assign(nbands, 0);
        auto band_worker = [&]() {
            std::vector<Row> rows;
            for (;;) {
                const u64 b = next_band.fetch_add(1);
                if (b >= nbands) return;
                const u64 tapeBand = globalBand + b;
                if (!tapeBands.empty() && tapeBand >= pendingBands) {
                    while (::access(bandPath(tapeBand - pendingBands, ".consumed").c_str(), F_OK) != 0)
                        ::usleep(10000);
                }
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
                std::vector<u8> bandLcp;
                std::vector<std::pair<u64,u64>> bandEsc;
                if (!lcpOut.empty()) {
                    merge_sort_lcp(rows, bandLcp, bandEsc, band_off[b]);
                } else {
                    // product path: the plain comparison sort. The merge-with-LCP
                    // is opt-in until its propagation is verified exact.
                    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                        if (a.klo != b.klo) return a.klo < b.klo;
                        if (a.khi != b.khi) return a.khi < b.khi;
                        if (a.kq != b.kq) return a.kq < b.kq;
                        return suf_cmp_from(unpack_pos(a.pos), unpack_pos(b.pos), 24) < 0;
                    });
                }
                if (lfd >= 0) {
                    // the band's LCP stream lands at its own offset: rank order is
                    // band order, so this is a straight pwrite.
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
                    bandFirstPos[b] = rows.empty() ? ~0ull : unpack_pos(rows.front().pos);
                    bandLastPos[b] = rows.empty() ? ~0ull : unpack_pos(rows.back().pos);
                }
                tSort[b] = now_sec() - bs0;
                const double bw0 = now_sec();
                u64 ties = 0, tailHere = 0;
                for (size_t i = 0; i < rows.size(); ++i) {
                    if (unpack_aux(rows[i].pos)) ++tailHere;
                    if (i && rows[i].klo == rows[i-1].klo && rows[i].khi == rows[i-1].khi &&
                        rows[i].kq == rows[i-1].kq) ++ties;
                }
                tail_w.fetch_add(tailHere);
                ties_w.fetch_add(ties);
                // THE END ROW IS EMITTED AT ITS RANK: rank 0 of the whole text is the
                // suffix that is just the declared END, so the first row of the first
                // band must be position N-1 with an empty key prefix. This is the
                // certificate for the tail group's one non-negotiable member.
                if (band_off[b] == 0 && !rows.empty()) {
                    const u64 p0 = unpack_pos(rows.front().pos);
                    if (p0 != N - 1 || row_prefix(rows.front()) != 0)
                        die("the END row is not at rank 0 (declared END broken)");
                    std::fprintf(stderr,
                        "TAIL_GROUP_END_ROW rank=0 pos=%llu prefix_symbols=0 carried_symbol=%u "
                        "code=%u\n",
                        (unsigned long long)p0, unsigned((rows.front().pos >> 48) & 0xffu), CODE_END);
                }
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
        pool->run([&](unsigned) { band_worker(); });
        // BAND BOUNDARIES: the LCP between a band's last row and the next band's
        // first row belongs to neither merge, so it is computed here - one
        // comparison per boundary, and there are only ~31 per wave.
        if (lfd >= 0) {
            // EMPTY BANDS ARE SKIPPED: an empty band has no row, and using 0 as its
            // "first position" made the boundary comparison "(0,0)" whose LCP is n -
            // the 7 bad ranks col_check found. Chain each non-empty band to the
            // previous NON-EMPTY one instead.
            // The FIRST non-empty band of this wave is chained to the LAST row of the
            // previous WAVE: wave seams were not being written at all, and 4 of the 22
            // seams at 150 MB had a true LCP of 1 (the rest were 0 and passed by luck).
            u64 prev_last = prev_wave_last;
            for (u64 b = 0; b < nbands; ++b) {
                if (bandFirstPos[b] == ~0ull) continue;          // empty band
                if (prev_last == ~0ull) { prev_last = bandLastPos[b]; continue; }
                u64 cross = 0;
                suf_cmp_lcp(prev_last, bandFirstPos[b], 0, cross);
                const u8 v = (cross >= 255) ? 255 : u8(cross);
                if (cross >= 255) {
                    std::lock_guard<std::mutex> lk(escMu);
                    gEsc.emplace_back(band_off[b], cross);
                }
                if (::pwrite(lfd, &v, 1, off_t(48 + band_off[b])) != 1) die("pwrite boundary lcp");
                prev_last = bandLastPos[b];
            }
            prev_wave_last = prev_last;
        }
        if (!tapeBands.empty()) {
            // Drain before scattering another wave: no cross-wave disk overlap.
            for (u64 b = 0; b < nbands; ++b)
                while (::access(bandPath(globalBand + b, ".consumed").c_str(), F_OK) != 0) ::usleep(10000);
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
        tail_total += tail_w.load();
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
    // THE TAIL GROUP CERTIFICATE: every position whose key would reach the END was
    // built by tail_row() with an explicit prefix length, and there are exactly
    // min(KEY_BYTES, N) of them (one of which is the END row itself, at rank 0 -
    // proved where it is emitted). Nothing in the run depended on a pad value.
    std::fprintf(stderr, "TAIL_GROUP_SEEN rows=%llu expected=%llu pad_free_keys=1\n",
                 (unsigned long long)tail_total, (unsigned long long)tailRows);
    if (tail_total != tailRows) die("tail group coverage mismatch");
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
    std::fprintf(stderr,
        "SS_DONE n=%" PRIu64 " waves=%" PRIu64 " ties=%" PRIu64
        " sample_s=%.2f waves_s=%.2f total_s=%.2f\n",
        N, waves, ties_total, t_sample, now_sec() - t1, now_sec() - t0);
    return 0;
}
