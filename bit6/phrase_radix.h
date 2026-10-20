// ---------------------------------------------------------------------------
// phrase-radix: THE PARSE BECOMES THE BINNING
//
// tools/binbwt/PHRASE_RADIX_DESIGN.md is the argument; this is the machinery.
//
// The bin machine's cost along a shared prefix is DEPTH: the census journals
// `depth_max=879` at 10 GB, i.e. there are suffixes sharing 878+ bytes, and each
// of those levels is a count pass plus a scatter pass over the node's rows with
// one random byte read per row. A PHRASE PARSE answers the same question once,
// content-addressed: "the text at offset x is entry (block, offset) of length s"
// collapses 879 levels into ONE level of width s. A phrase IS a pre-computed
// 879-level-deep radix path.
//
// WHAT THIS FILE IS, and what it deliberately is NOT:
//   * It IS a degenerate PFP dictionary: only strings of length >= minLen (L)
//     occurring at least minFreq (F) times are candidates, the block pool and the
//     entry table are fixed-size caps, and the table is direct-mapped and small
//     enough to live in RAM. A collision can only LOSE a skip opportunity; it can
//     never produce a wrong answer, because every use of an entry is
//     BYTE-VERIFIED against the pool before it may advance a depth.
//   * It is NOT a complete parse and it is NOT on the product path unless
//     --phrase-radix is given. The product is chi of the RAW text; the parse is
//     internal scaffolding that chooses where the tree spends its levels.
//
// THE HARD CORE, handled by construction: the dictionary registers an entry for
// EVERY OFFSET of every block, not just offset 0. A suffix that starts INSIDE a
// phrase occurrence is then an ordinary entry (block, o), and the members of one
// (phrase, offset) bin are byte-identical through the phrase - so they order by
// their continuation, which is PFP's end-occurrence sort generalised. Registering
// offsets is also what makes the FIRST probe succeed at a divergence point whose
// offset inside the block is essentially arbitrary; without it the byte radix
// would grind up to one phrase spacing before it could jump.
//
// THE SKIP LEMMA (the whole correctness argument, proved in the design doc):
// at a node certified at depth d, if every row resolves to the SAME entry E and
// every row's text at tail+d compares equal to E for E.len bytes, then replacing
// the node by (rows, d+E.len) is EXACT: no byte before d+E.len differs between
// rows, so no discriminating information is discarded.
// ---------------------------------------------------------------------------
#ifndef SXGC_PHRASE_RADIX_H
#define SXGC_PHRASE_RADIX_H

struct PhraseOpts {
    bool enabled = false;
    U minLen = 64;               // L: a phrase shorter than this is not a phrase
    U minFreq = 2;               // F: more often than this to be worth a token
    U maxLen = 4096;             // per-block length cap (a block may be huge)
    U seed = 32;                 // K: the k-mer the winnow hashes
    U window = 24;               // W: winnow window in k-mers (keeps ~2/(W+1))
    U blockMax = 4096;           // blocks admitted
    U poolMax = 16u << 20;       // phrase text bytes resident (the "small" cap)
    U tableBits = 20;            // 2^20 slots x 24 B = 24 MB, cache-resident-ish
    U reps = 8;                  // representative occurrences per candidate
    U skipMinRows = 4096;        // a node smaller than this is not probed
    U skipMinLen = 16;           // a skip shorter than this is not worth a probe
};

// The parse's journal. Every field is EXACT except where labelled "estimate".
struct PhraseStats {
    U kmers = 0;                 // k-mers hashed in the sweep
    U samples = 0;               // winnow-emitted samples
    U parsePasses = 0;           // corpus sweeps the parse made (1 or 2)
    U distinct = 0;              // distinct sampled hashes
    U candidates = 0;            // distinct hashes with count >= minFreq
    U blocks = 0;                // blocks admitted to the dictionary
    U blocksRejected = 0;        // candidates dropped (short after LCP / no room)
    U poolBytes = 0;             // phrase text bytes resident
    U entries = 0;               // offset entries inserted
    U entriesDropped = 0;        // offsets refused because the table filled
    U tableSlots = 0, tableUsed = 0;
    U lenMin = 0, lenMax = 0, lenSum = 0;   // admitted block lengths
    U freqMax = 0;
    double covEstimate = 0;      // estimate: sampled occurrences x len / n
    double tSweep = 0, tSort = 0, tReps = 0, tBuild = 0, tTotal = 0;
    U peakSamples = 0;           // peak live sample array (slots)
};

static inline uint64_t phrase_mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Fixed chunk size: the winnow is chunk-local so the sample set cannot depend on
// the thread count (a dictionary that changed with -t would make every gate
// unreproducible).
static constexpr U phrase_chunk_bytes = U(1) << 26;

namespace phrase_detail {
// Rolling polynomial hash over `seed` bytes, then a monotonic-deque winnow of
// window `window` k-mers, leftmost tie-break. Emits (hash, position).
struct KmerWinnow {
    uint64_t B = 0x9E3779B97F4A7C15ULL;
    uint64_t BK1 = 1;
    U K = 0, W = 0;
    void init(U k, U w) {
        K = k; W = w; BK1 = 1;
        for (U i = 0; i + 1 < K; ++i) BK1 *= B;
    }
    template <class Emit>
    void run(const uint8_t* buf, U len, U base, std::vector<uint64_t>& dq,
             std::vector<U>& dqi, Emit&& emit) const {
        if (len < K) return;
        const U M = len - K + 1;              // k-mers in this chunk
        dq.clear(); dqi.clear();
        size_t head = 0;                       // deque front (no erase-from-front)
        uint64_t h = 0;
        for (U j = 0; j < K; ++j) h = h * B + uint64_t(buf[j]) + 1u;
        for (U i = 0;; ++i) {
            const uint64_t hv = phrase_mix(h);
            while (dq.size() > head && dq.back() > hv) { dq.pop_back(); dqi.pop_back(); }
            dq.push_back(hv); dqi.push_back(i);
            while (head < dqi.size() && dqi[head] + W <= i) ++head;
            if (head > 4096 && head * 2 > dq.size()) {   // compact the ring
                dq.erase(dq.begin(), dq.begin() + long(head));
                dqi.erase(dqi.begin(), dqi.begin() + long(head));
                head = 0;
            }
            if (i + 1 >= W) emit(dq[head], base + dqi[head]);
            if (i + 1 == M) break;
            h = (h - (uint64_t(buf[i]) + 1u) * BK1) * B + uint64_t(buf[i + K]) + 1u;
        }
    }
};
}  // namespace phrase_detail

struct PhraseDict {
    PhraseOpts o;
    bool on = false;
    bool dollar = false;
    // ---- the phrase text (blocks, concatenated) --------------------------
    std::vector<uint8_t> pool;
    struct Block { U off = 0, len = 0, freq = 0; };
    std::vector<Block> blocks;
    // ---- the entry table: direct-mapped, key 0 = empty ---------------------
    // Entries are ordered by (block content, offset) because blocks are admitted
    // in (freq desc, hash asc) order and offsets ascend, so ENTRY ORDER IS
    // LEXICOGRAPHIC ORDER of the phrase content. That is what lets a phrase be a
    // legitimate symbol rather than a label: phrase order = byte order = bin
    // order, for free.
    struct Entry { uint64_t key = 0; uint32_t block = 0, off = 0, len = 0; };
    std::vector<Entry> tab;
    U mask = 0;
    PhraseStats st;

    inline bool armed() const { return on; }

    // The lookup: one 8-byte key against one direct-mapped slot. Returns the
    // entry index of the CANDIDATE (never a certificate - the caller verifies).
    inline int32_t lookup8(const uint8_t* b8) const {
        uint64_t raw;
        std::memcpy(&raw, b8, 8);
        const uint64_t key = phrase_mix(raw) | 1u;
        const size_t slot = size_t(key) & mask;
        const Entry& e = tab[slot];
        if (!e.key || e.key != key) return -1;
        return int32_t(slot);
    }
    inline const Entry& entry(int32_t i) const { return tab[size_t(i)]; }
    inline const uint8_t* text(const Entry& e) const { return pool.data() + blocks[e.block].off + e.off; }

    // ---- reading the corpus at a PHRASE coordinate ------------------------
    // `x` is a raw tail+depth coordinate: in cyclic mode it may exceed n and the
    // text wraps; in dollar mode the suffix ends at n. `span` is used so a
    // RAM-cached view costs a pointer and a pooled view costs one bulk read.
    static void copyAt(const CorpusText& t, bool dollar, U x, U len, uint8_t* buf) {
        const U n = t.n;
        if (dollar) require(x + len <= n, "phrase dollar read bounds");
        // Cyclic coordinates are M^inf: a read may wrap ANY number of times, so
        // the copy is a loop over single wraps (never one `read` of `len` - that
        // is how a 4096-byte phrase over a 786-byte fixture broke the bounds
        // check, loudly, which is the whole point of the check).
        U done = 0;
        while (done < len) {
            const U xx = dollar ? (x + done) : ((x + done) % n);
            const U take = std::min<U>(len - done, n - xx);
            const uint8_t* p = t.span(xx, buf + done, take);
            if (p != buf + done) std::memcpy(buf + done, p, take);
            done += take;
        }
    }
    // Verifies that the corpus at `x` equals `ref[0..len)`. BYTE-COMPARED, never
    // hash-compared: the hash only picks a candidate, it never certifies one.
    static bool verify(const CorpusText& t, bool dollar, U x, U len,
                       const uint8_t* ref, uint8_t* buf) {
        const U n = t.n;
        if (dollar && (x > n || len > n - x)) return false;
        U done = 0;
        while (done < len) {
            const U xx = dollar ? (x + done) : ((x + done) % n);
            const U take = std::min<U>(len - done, n - xx);
            const uint8_t* p = t.span(xx, buf, take);
            if (std::memcmp(p, ref + done, take) != 0) return false;
            done += take;
        }
        return true;
    }

    // ---- THE PARSE -------------------------------------------------------
    // One winnowing sweep for candidates, one to pick representatives, then the
    // blocks and the offset entries. Deterministic: the sampling is a function of
    // the corpus and the flags ONLY (fixed chunk size, never thread count), the
    // candidate order is (count desc, hash asc), the representative rule is "the
    // first `reps` occurrences in position order", and the table is filled in
    // that order. Two runs of the same corpus and flags produce the same
    // dictionary, byte for byte.
    void parse(const CorpusText& t, bool dollarMode, const PhraseOpts& opts) {
        o = opts;
        on = false;
        dollar = dollarMode;
        blocks.clear(); pool.clear(); tab.clear(); st = PhraseStats();
        if (!o.enabled || t.n < 2 * (o.minLen + o.seed)) {
            // The empty dictionary is a legitimate answer: with no discoverable
            // phrase the byte radix must run EXACTLY as today.
            return;
        }
        const U n = t.n;
        const double t0 = now_sec();
        phrase_detail::KmerWinnow kw;
        kw.init(std::max<U>(8, o.seed), std::max<U>(2, o.window));
        const U K = kw.K, W = kw.W;

        // ---- PASS 1: collect the sampled hashes (parallel, order-free) -----
        {
            std::vector<uint64_t> hs;
            const U chunks = (n + phrase_chunk_bytes - 1) / phrase_chunk_bytes;
            const U NT = std::max<U>(1, std::min<U>(chunks, 64));
            std::vector<std::vector<uint64_t>> per(NT);
            auto one = [&](U ci) {
                auto& out = per[ci];
                std::vector<uint8_t> buf;
                std::vector<uint64_t> dq; std::vector<U> dqi;
                for (U c = ci; c < chunks; c += NT) {
                    const U lo = c * phrase_chunk_bytes;
                    const U hi = std::min<U>(n, lo + phrase_chunk_bytes);
                    const U want = (c + 1 < chunks)
                                       ? std::min<U>(hi - lo + K, n - lo)
                                       : (hi - lo);
                    buf.resize(want);
                    t.read(lo, buf.data(), want);
                    kw.run(buf.data(), want, lo, dq, dqi,
                           [&](uint64_t h, U) { out.push_back(h); });
                }
            };
            {
                std::vector<std::thread> pool_;
                for (U c = 0; c < NT; ++c) pool_.emplace_back(one, c);
                for (auto& th : pool_) th.join();
            }
            U total = 0;
            for (auto& v : per) total += U(v.size());
            st.samples = total; st.kmers = total;
            st.peakSamples = total;
            hs.reserve(total);
            for (auto& v : per) {
                hs.insert(hs.end(), v.begin(), v.end());
                std::vector<uint64_t>().swap(v);
            }
            ++st.parsePasses;
            st.tSweep = now_sec() - t0;
            if (hs.size() < 2) { st.tTotal = now_sec() - t0; return; }
            // ---- sort -> groups -> candidates (count >= F), ranked ----------
            const double ts = now_sec();
            std::sort(hs.begin(), hs.end());
            struct Cand { uint64_t h; U count; };
            std::vector<Cand> cand;
            {
                size_t i = 0;
                while (i < hs.size()) {
                    size_t j = i;
                    while (j < hs.size() && hs[j] == hs[i]) ++j;
                    ++st.distinct;
                    if (U(j - i) >= o.minFreq) {
                        ++st.candidates;
                        cand.push_back(Cand{hs[i], U(j - i)});
                    }
                    i = j;
                }
            }
            std::vector<uint64_t>().swap(hs);
            st.tSort = now_sec() - ts;
            if (cand.empty()) { st.tTotal = now_sec() - t0; return; }
            std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) {
                if (a.count != b.count) return a.count > b.count;
                return a.h < b.h;
            });
            if (cand.size() > o.blockMax) cand.resize(o.blockMax);

            // ---- PASS 2 (SERIAL): the first `reps` occurrences per candidate --
            // Serial on purpose: the representative rule is "the first reps
            // occurrences in POSITION order", and any parallel merge that is not
            // range-ordered would make the dictionary depend on the thread count.
            const double tr = now_sec();
            std::vector<U> candPos(cand.size() * o.reps, U(~U(0)));
            std::vector<U> candCnt(cand.size(), 0);
            {
                std::unordered_map<uint64_t, U> idx;
                idx.reserve(cand.size() * 2);
                for (U c = 0; c < cand.size(); ++c) idx[cand[c].h] = c;
                std::vector<uint8_t> buf;
                std::vector<uint64_t> dq; std::vector<U> dqi;
                const U chunks = (n + phrase_chunk_bytes - 1) / phrase_chunk_bytes;
                for (U c = 0; c < chunks; ++c) {
                    const U lo = c * phrase_chunk_bytes;
                    const U hi = std::min<U>(n, lo + phrase_chunk_bytes);
                    const U want = (c + 1 < chunks)
                                       ? std::min<U>(hi - lo + K, n - lo)
                                       : (hi - lo);
                    buf.resize(want);
                    t.read(lo, buf.data(), want);
                    kw.run(buf.data(), want, lo, dq, dqi, [&](uint64_t h, U pos) {
                        auto it = idx.find(h);
                        if (it == idx.end()) return;
                        const U ci = it->second;
                        if (candCnt[ci] >= o.reps) return;
                        candPos[ci * o.reps + candCnt[ci]++] = pos;
                    });
                }
            }
            ++st.parsePasses;
            st.tReps = now_sec() - tr;

            // ---- blocks: sample-LCP length, then the pooled text -------------
            const double tb = now_sec();
            auto lcpAt = [&](U a, U b, U cap) -> U {
                uint8_t ba[256], bb[256];
                U j = 0;
                while (j < cap) {
                    const U take = std::min<U>(256, cap - j);
                    U avail = take;
                    if (dollar) {
                        avail = (a + j < n && b + j < n)
                                    ? std::min<U>(take, std::min(n - (a + j), n - (b + j)))
                                    : 0;
                    }
                    if (!avail) return j;
                    copyAt(t, dollar, a + j, avail, ba);
                    copyAt(t, dollar, b + j, avail, bb);
                    U d = 0;
                    while (d < avail && ba[d] == bb[d]) ++d;
                    j += d;
                    if (d < avail) return j;
                }
                return j;
            };
            // ---- the table, allocated before admission so a REDUNDANT candidate
            // (one whose head key is already taken by an admitted block, i.e. an
            // extension or a suffix of a block already in) never consumes pool. A
            // block whose head is not addressable is dead weight: it can never
            // produce a skip.
            tab.assign(size_t(1) << o.tableBits, Entry());
            mask = U(tab.size() - 1);
            size_t used = 0;
            const size_t stopAt = tab.size() * 9 / 10;
            for (U c = 0; c < cand.size(); ++c) {
                if (!candCnt[c]) { ++st.blocksRejected; continue; }
                const U p0 = candPos[c * o.reps];
                U len = dollar ? (p0 < n ? std::min<U>(o.maxLen, n - p0) : 0) : o.maxLen;
                if (dollar && p0 >= n) { ++st.blocksRejected; continue; }
                // The length only has to be SAFE (it is verified at use); it does
                // not have to be the true maximal repeat, so a bounded sample of
                // occurrences bounds it.
                for (U r = 1; r < candCnt[c] && len >= o.minLen; ++r)
                    len = std::min<U>(len, lcpAt(p0, candPos[c * o.reps + r], len));
                if (len < o.minLen) { ++st.blocksRejected; continue; }
                if (pool.size() + len > o.poolMax) { ++st.blocksRejected; continue; }
                // probe the head key against the corpus bytes we are about to add
                uint8_t head[8];
                copyAt(t, dollar, p0, 8, head);
                uint64_t hraw = 0;
                std::memcpy(&hraw, head, 8);
                const uint64_t hkey = phrase_mix(hraw) | 1u;
                if (tab[size_t(hkey) & mask].key) { ++st.blocksRejected; continue; }
                const U off = U(pool.size());
                pool.resize(off + len);
                copyAt(t, dollar, p0, len, pool.data() + off);
                const U bi = U(blocks.size());
                blocks.push_back(Block{off, len, cand[c].count});
                st.lenMin = st.lenMin ? std::min(st.lenMin, len) : len;
                st.lenMax = std::max(st.lenMax, len);
                st.lenSum += len;
                st.freqMax = std::max(st.freqMax, cand[c].count);
                // coverage ESTIMATE: sampled occurrences x length. The winnow
                // keeps about 2/(W+1) of the k-mers, so the raw sample count
                // understates occurrences by that factor; the claim is labelled
                // an estimate and the EXACT coverage is the tree's own
                // phrase_skip_bytes counter.
                st.covEstimate += double(cand[c].count) * double(len) *
                                  double(W + 1) / 2.0;
                // ---- this block's OFFSET ENTRIES: every offset, the mid-phrase
                // core. Inserted now so the head is present before any later
                // candidate is tested for redundancy.
                for (U oo = 0; oo + o.minLen <= len; ++oo) {
                    if (used >= stopAt) { ++st.entriesDropped; break; }
                    const uint32_t l = uint32_t(len - oo);
                    const uint8_t* src = pool.data() + off + oo;
                    uint64_t raw = 0;
                    std::memcpy(&raw, src, l < 8 ? l : 8);
                    const uint64_t key = phrase_mix(raw) | 1u;
                    Entry& e = tab[size_t(key) & mask];
                    if (!e.key) ++used;
                    else if (e.len >= l) { ++st.entriesDropped; continue; }
                    e.key = key;
                    e.block = bi;
                    e.off = uint32_t(oo);
                    e.len = l;
                    ++st.entries;
                }
            }
            st.covEstimate /= double(t.n);
            st.blocks = U(blocks.size());
            st.poolBytes = U(pool.size());
            st.tableUsed = U(used);
            st.tableSlots = U(tab.size());
            st.tBuild = now_sec() - tb;
        }
        on = true;
        st.tTotal = now_sec() - t0;
    }
};

// ---- the journal ----------------------------------------------------------
static void phrase_journal_dict(const PhraseDict& pd, U n) {
    const PhraseStats& s = pd.st;
    std::fprintf(stderr,
        "PHRASE_DICT enabled=%u armed=%u dollar=%u min_len=%llu min_freq=%llu "
        "max_len=%llu seed=%llu window=%llu block_max=%llu pool_max=%llu "
        "table_bits=%llu kmers=%llu samples=%llu parse_passes=%llu distinct=%llu "
        "candidates=%llu blocks=%llu blocks_rejected=%llu pool_bytes=%llu "
        "entries=%llu entries_dropped=%llu table_slots=%llu table_used=%llu "
        "len_min=%llu len_max=%llu len_mean=%.1f freq_max=%llu "
        "coverage_estimate=%.6f byte_radix_share_estimate=%.6f peak_samples=%llu "
        "sweep_s=%.3f sort_s=%.3f reps_s=%.3f build_s=%.3f parse_s=%.3f n=%llu "
        "deterministic=1 full_SA=0 persistent_positions=0\n",
        unsigned(pd.o.enabled), unsigned(pd.on), unsigned(pd.dollar),
        (unsigned long long)pd.o.minLen, (unsigned long long)pd.o.minFreq,
        (unsigned long long)pd.o.maxLen, (unsigned long long)pd.o.seed,
        (unsigned long long)pd.o.window, (unsigned long long)pd.o.blockMax,
        (unsigned long long)pd.o.poolMax, (unsigned long long)pd.o.tableBits,
        (unsigned long long)s.kmers, (unsigned long long)s.samples,
        (unsigned long long)s.parsePasses, (unsigned long long)s.distinct,
        (unsigned long long)s.candidates, (unsigned long long)s.blocks,
        (unsigned long long)s.blocksRejected, (unsigned long long)s.poolBytes,
        (unsigned long long)s.entries, (unsigned long long)s.entriesDropped,
        (unsigned long long)s.tableSlots, (unsigned long long)s.tableUsed,
        (unsigned long long)s.lenMin, (unsigned long long)s.lenMax,
        s.blocks ? double(s.lenSum) / double(s.blocks) : 0.0,
        (unsigned long long)s.freqMax, s.covEstimate,
        1.0 - std::min(1.0, s.covEstimate),
        (unsigned long long)s.peakSamples, s.tSweep, s.tSort, s.tReps, s.tBuild,
        s.tTotal, (unsigned long long)n);
}

#endif  // SXGC_PHRASE_RADIX_H
