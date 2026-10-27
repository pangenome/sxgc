// ------------------------------------------------- bin-bwt v1 (operator shape)
// ONE BIN PER BYTE. Sweep the corpus once, group every position into the bin of
// its first remapped byte, order each bin by its TAIL (the suffix at p+1), then
// concatenate the bin segments in byte order. Bin b's rows are exactly the
// global suffix rows whose first symbol is b, in global order, so concatenation
// IS the global rle-BWT: the SA is never materialized, no heap of sorted
// producers, no merge and no ordered walk exists, and the sink coalesces the
// single boundary run between adjacent bins by itself.
//
// Inside a bin the construction carries LCPs structurally: the bin is refined by
// deeper symbols (MSD bucketing over transient positions), so a prefix shared
// inside a bin is consumed once by a bucketing pass and is never walked again by
// a comparison; leaves sort by raw LCE through the CorpusText cache starting at
// the certified depth.
//
// PHRASE RADIX (tools/binbwt/PHRASE_RADIX_DESIGN.md) adds ONE thing to this
// machine: at a node whose split made no progress (`biggest == m`, the
// deep-boilerplate signature) a small content-addressed dictionary may certify a
// WHOLE PHRASE at the node's depth, so 879 byte levels become one level of width
// s. It is off unless `--phrase-radix` is given, and when it is off no line of it
// executes (`bin_phrase_now == nullptr`).
#include "phrase_radix.h"
// The dictionary of the CURRENT bin_order call (null when the flag is off).
// Constant for the whole call: the build threads only ever read it, and the
// driver sets/clears it, so no per-node pointer has to be threaded through the
// queue's task structs.
static const PhraseDict* bin_phrase_now = nullptr;
// The per-depth resolution of the entropy rule's H journal (ENTROPY_RULE_DESIGN
// §7). Depths past the last slot fold into it: the shapes that reach them are
// the no-progress monster chains, and the journal only has to say so.
static constexpr U bin_entropy_depth_slots = 512;
struct BinWork {
    U sweepBytes = 0, sweepPasses = 0, windows = 0, bins = 0, binPasses = 0;
    U levelPasses = 0, levelElems = 0;      // bucketing passes / elements coded
    U comparisons = 0, symbols = 0, ties = 0, certDecided = 0;
    U leafSorts = 0, leafElems = 0, singletonBins = 0;
    U peakLiveSlots = 0, peakLiveBytes = 0, emittedRows = 0;
    U builtBins = 0;
    // ---- two-byte top level (level 1 = the byte-1 bins, level 2 = one more
    // sequential 1-byte level) --------------------------------------------
    U topBins = 0;                      // non-empty 2-gram sub-bins dispatched
    // ---- TARGET-SIZE RECURSION (depth is an OUTPUT of the data) ------------
    // The leaf bound in rows (`targetSize`, 0 = automatic) and what the tree
    // actually did: the deepest split depth reached, the largest leaf that was
    // handed to a comparison sort, and the leaves the depth cap had to take
    // over target (the fail-loud backstop, see `bin_sort_rec`).
    U targetSize = 0;                   // the leaf bound this window used
    U depthMax = 0;                     // deepest split depth actually coded
    U leafRowsMax = 0;                  // largest single leaf (rows)
    U overTargetLeaves = 0, overTargetRows = 0;  // the depth-cap backstop fired
    U structuralLeaves = 0, structuralRows = 0;  // the TEXT ended inside the node
    U noProgressLevels = 0;             // splits that could not divide a node
    // PART 4: the periodic tie-break (see bin_node_period) - the nodes that
    // could never be divided because their rotations are identical, ordered by
    // ascending source position with ZERO comparisons, and the largest period
    // proven. `overTargetLeaves` can no longer be produced by periodicity.
    U periodicLeaves = 0, periodicRows = 0, periodicPeriodMax = 0;
    // PART 1: leaf-task staging (see BinLeafStage): the leaves that staged, the
    // bytes one bulk pread moved, how many comparator reads the stage answered
    // (both sides / one side / neither) and how many leaves skipped it because
    // the view was already RAM-cached.
    U stageLeaves = 0, stageMembers = 0, stageBytes = 0;
    U stageHits = 0, stagePartials = 0, stageMisses = 0, stageSkippedRam = 0;
    // PART B: the read discipline's counters - the member windows the staged
    // leaves asked for, and the coalesced monotone reads that answered them.
    U stageRawWindows = 0, stageMergedReads = 0;
    double tStage = 0;
    double tLeaf = 0;                   // wall/cpu seconds in leaf comparison sorts
    // Hot-bin anatomy: the largest byte-1 bin of each window, told apart from
    // the window totals (rows, the split's sub-bins, the busiest thread, the
    // leaf work billed to that bin, and the wall from availability to done).
    U hotRows = 0, hotBin = 0, hotTopBins = 0, hotSubMax = 0, hotWindows = 0;
    U hotLeafSorts = 0, hotLeafElems = 0, hotLeafRowsMax = 0, hotLevelElems = 0;
    U hotSymbols = 0, hotComparisons = 0;
    double hotCpuS = 0, hotThreadS = 0, hotSpanS = 0;
    double tSweep = 0, tBuild = 0, tEmit = 0, cpuBuild = 0;
    double tAlloc = 0, tWait = 0;
    // ---- v2 parallel sink, charged SEPARATELY from construction -----------
    U runsPublished = 0, bands = 0;     // BWT runs and run bands published
    U targetHits = 0;                   // capture targets found by the join
    U bandJobs = 0;                     // band payloads handed to a writer
    double tCount = 0, tFill = 0;       // the two placing sweeps, separately
    double tStitch = 0;                 // ordered pass: rows -> runs -> bands
    double tBandWrite = 0;              // writer threads' busy seconds (sum)
    double tFlowWait = 0;               // v2_flow_wait seconds in the slot wait
    // ---- disc-backed windows (the operator's bounded rolling files) --------
    U winFileWriteBytes = 0;            // window-file bytes the fill wrote
    U winFileReadBytes = 0;             // window-file bytes the build read back
    U peakWindowFileBytes = 0;          // live scratch: every live window file
    U windowFiles = 0, windowLoads = 0; // files created / per-bin bulk loads
    double tWinWrite = 0, tWinRead = 0; // the two file phases, wall
    double tCreditWait = 0;             // build waiting for a filled window
    double tSlotWait = 0;               // publish waiting for a free band writer slot
    // ---- the write path's own cost (pwrite, no mapping) --------------------
    U winFillBufBytes = 0;              // peak staging buffers (threads x <= 4 MiB)
    U winFillWrites = 0;                // pwrite() calls the fill issued
    // ---- the SAMPLED-SA anchor side-stream (--sa-sample-k) ----------------
    // The emit pass holds the global SA in order, so every k-th rank's SA value
    // is a sequential read of the slice it is streaming anyway. OFF (k=0) is the
    // product default and changes nothing at all.
    U saSampleK = 0;                    // the sampling constant this run used
    U saSamples = 0;                    // anchors written (== count in the header)
    U saSampleBytes = 0;                // the file's bytes, header included
    double tSaSample = 0;               // wall spent emitting them
    // ---- PHRASE RADIX: the census of the OTHER regime ----------------------
    // Probes fired at no-progress nodes, the rows and bytes they certified in ONE
    // level each, and the deepest depth a phrase certified. `phrase_skips` is what
    // replaces runs of `no_progress_levels`, and `phrase_skip_bytes` is the EXACT
    // number of text bytes the dictionary answered (the coverage estimate in
    // PHRASE_DICT is only an estimate; this is the measured one).
    U phraseProbes = 0, phraseProbeRows = 0;    // probes fired / rows examined
    U phraseSkips = 0, phraseSkipRows = 0;      // skips taken / rows certified
    U phraseSkipBytes = 0;                      // EXACT bytes certified by skips
    U phraseSkipMaxLen = 0, phraseSkipDepthMax = 0;
    U phraseVerifyFails = 0;                     // candidates byte-verify rejected
    U phraseProbeNoEnt = 0, phraseProbeDis = 0;  // probe refused: no entry / not uniform
    // ---- THE ENTROPY RULE (tools/binbwt/ENTROPY_RULE_DESIGN.md) ------------
    // The stopping rule's own instrument. `entropyEvals` are the level
    // histograms the TREE rule looked at, `entropyTaken` the ones it kept
    // (H > theta), `entropyExits` the buckets it handed to the LCP-carrying
    // comparison sort instead of splitting again, `entropyNoProgExits` the
    // subset of those that were taken on a no-progress (H = 0, not provably
    // periodic) node - the byte-0x20-monster regime. The leaf arm is counted
    // apart (`entropyLeaf*`) because it runs in RAM at a different threshold.
    U entropyEvals = 0, entropyTaken = 0;
    U entropyExits = 0, entropyExitRows = 0, entropyNoProgExits = 0;
    U entropyLeafEvals = 0, entropyLeafTaken = 0;
    U entropyLeafExits = 0, entropyLeafRows = 0;
    // Leaves a comparison sort had to take OVER the target because the rule
    // stopped the split (exact, bounded by `m`, and counted apart so that
    // `structural_leaves` / `over_target_leaves` keep their exact meanings).
    U entropyOverTargetLeaves = 0, entropyOverTargetRows = 0;
    // H distribution by depth: min / mean / max of the entropy the rule saw.
    struct BinHEnt { U n = 0; double sum = 0, mn = 1e30, mx = 0; };
    BinHEnt hDepth[bin_entropy_depth_slots];
    U hDepthOver = 0;                   // evaluations past the last slot
    void absorb(const BinWork& o) {
        sweepBytes += o.sweepBytes; sweepPasses += o.sweepPasses;
        windows += o.windows; bins += o.bins; binPasses += o.binPasses;
        levelPasses += o.levelPasses; levelElems += o.levelElems;
        comparisons += o.comparisons; symbols += o.symbols; ties += o.ties;
        certDecided += o.certDecided; leafSorts += o.leafSorts;
        leafElems += o.leafElems; singletonBins += o.singletonBins;
        builtBins += o.builtBins;
        topBins += o.topBins;
        if (o.targetSize > targetSize) targetSize = o.targetSize;
        depthMax = std::max(depthMax, o.depthMax);
        leafRowsMax = std::max(leafRowsMax, o.leafRowsMax);
        overTargetLeaves += o.overTargetLeaves;
        overTargetRows += o.overTargetRows;
        structuralLeaves += o.structuralLeaves;
        structuralRows += o.structuralRows;
        noProgressLevels += o.noProgressLevels;
        periodicLeaves += o.periodicLeaves;
        periodicRows += o.periodicRows;
        periodicPeriodMax = std::max(periodicPeriodMax, o.periodicPeriodMax);
        stageLeaves += o.stageLeaves; stageMembers += o.stageMembers;
        stageBytes += o.stageBytes; stageHits += o.stageHits;
        stagePartials += o.stagePartials; stageMisses += o.stageMisses;
        stageSkippedRam += o.stageSkippedRam; tStage += o.tStage;
        stageRawWindows += o.stageRawWindows; stageMergedReads += o.stageMergedReads;
        tLeaf += o.tLeaf;
        if (o.hotRows > hotRows) {
            hotRows = o.hotRows; hotBin = o.hotBin; hotTopBins = o.hotTopBins;
            hotSubMax = o.hotSubMax; hotLeafSorts = o.hotLeafSorts;
            hotLeafElems = o.hotLeafElems; hotLeafRowsMax = o.hotLeafRowsMax;
            hotLevelElems = o.hotLevelElems;
            hotSymbols = o.hotSymbols; hotComparisons = o.hotComparisons;
            hotCpuS = o.hotCpuS; hotThreadS = o.hotThreadS; hotSpanS = o.hotSpanS;
        }
        hotWindows += o.hotWindows;
        peakLiveSlots = std::max(peakLiveSlots, o.peakLiveSlots);
        peakLiveBytes = std::max(peakLiveBytes, o.peakLiveBytes);
        emittedRows += o.emittedRows;
        tSweep += o.tSweep; tBuild += o.tBuild; tEmit += o.tEmit;
        cpuBuild += o.cpuBuild; tAlloc += o.tAlloc; tWait += o.tWait;
        runsPublished += o.runsPublished; bands += o.bands;
        targetHits += o.targetHits; bandJobs += o.bandJobs;
        tCount += o.tCount; tFill += o.tFill;
        tStitch += o.tStitch; tBandWrite += o.tBandWrite; tFlowWait += o.tFlowWait;
        winFileWriteBytes += o.winFileWriteBytes;
        winFileReadBytes += o.winFileReadBytes;
        peakWindowFileBytes = std::max(peakWindowFileBytes, o.peakWindowFileBytes);
        windowFiles += o.windowFiles; windowLoads += o.windowLoads;
        tWinWrite += o.tWinWrite; tWinRead += o.tWinRead;
        tCreditWait += o.tCreditWait;
        tSlotWait += o.tSlotWait;
        winFillBufBytes = std::max(winFillBufBytes, o.winFillBufBytes);
        winFillWrites += o.winFillWrites;
        if (o.saSampleK) saSampleK = o.saSampleK;
        saSamples += o.saSamples; saSampleBytes += o.saSampleBytes;
        tSaSample += o.tSaSample;
        phraseProbes += o.phraseProbes; phraseProbeRows += o.phraseProbeRows;
        phraseSkips += o.phraseSkips; phraseSkipRows += o.phraseSkipRows;
        phraseSkipBytes += o.phraseSkipBytes;
        phraseSkipMaxLen = std::max(phraseSkipMaxLen, o.phraseSkipMaxLen);
        phraseSkipDepthMax = std::max(phraseSkipDepthMax, o.phraseSkipDepthMax);
        phraseVerifyFails += o.phraseVerifyFails;
        phraseProbeNoEnt += o.phraseProbeNoEnt; phraseProbeDis += o.phraseProbeDis;
        entropyEvals += o.entropyEvals; entropyTaken += o.entropyTaken;
        entropyExits += o.entropyExits; entropyExitRows += o.entropyExitRows;
        entropyNoProgExits += o.entropyNoProgExits;
        entropyLeafEvals += o.entropyLeafEvals;
        entropyLeafTaken += o.entropyLeafTaken;
        entropyLeafExits += o.entropyLeafExits;
        entropyLeafRows += o.entropyLeafRows;
        entropyOverTargetLeaves += o.entropyOverTargetLeaves;
        entropyOverTargetRows += o.entropyOverTargetRows;
        hDepthOver += o.hDepthOver;
        for (U d = 0; d < bin_entropy_depth_slots; ++d) {
            const BinHEnt& s = o.hDepth[d];
            if (!s.n) continue;
            BinHEnt& t = hDepth[d];
            t.n += s.n; t.sum += s.sum;
            t.mn = std::min(t.mn, s.mn); t.mx = std::max(t.mx, s.mx);
        }
    }
};

// THE ARENA WORD. A bin's construction sorts positions by their TAIL, and only
// the sort knows where a position ends up, so anything derived per row has to
// travel WITH the element or it is silently wrong after the sort. The arena
// slot is therefore one packed word:
//
//     word = tail << 10 | symbol << 2 | hit
//
//  * tail    the tail coordinate the comparator orders on (bin_tail_of);
//  * symbol  the row's BWT symbol M[p-1] (2 for the p == 0 padding row), which
//            the fill pass already has in hand one byte behind, so the ordered
//            publication pass never chases M[p-1] at random - one cache miss
//            per row is exactly what the serial sink used to pay;
//  * hit     "this position is a seam capture target", decided by the fill
//            pass's merge join against the sink's ascending target list, so the
//            ordered pass never hashes a row either.
//
// Packing is free in the construction: the word is what gets moved, bucketed
// and compared, so the sort permutes tail+symbol+hit as one element. Every use
// of a word as a tail decodes first, and no use is order-sensitive to the low
// bits (bucket-0's descending numeric sort sees distinct tails, 256 apart).
static constexpr unsigned bin_tag_bits = 10;
static inline U bin_word(U tail, uint8_t sym, bool hit) {
    require(tail <= (U(1) << 53), "bin arena tail width");
    return (tail << bin_tag_bits) | (U(sym) << 2) | U(hit ? 1u : 0u);
}
static inline U bin_word_tail(U word) { return word >> bin_tag_bits; }
static inline uint8_t bin_word_sym(U word) { return uint8_t((word >> 2) & 0xffu); }
static inline bool bin_word_hit(U word) { return (word & 3u) != 0; }

// Bin rows are ordered by the TAIL of a suffix: bin b holds the positions p with
// first byte b, and its rows are ordered by the suffix at p+1. In dollar mode the
// tail of p carries no wrap, so tail n is the empty suffix; in cyclic mode tails
// are cyclic rotations and tail 0 is the rotation at position 0.
static inline U bin_tail_of(U p, U n, bool dollar) {
    if (dollar) return p + 1;
    return p + 1 == n ? 0 : p + 1;
}
static inline U bin_pos_of(U t, U n, bool dollar) {
    if (dollar) return t - 1;
    return t ? t - 1 : n - 1;
}

// ------------------------------------------------- PART 1: LEAF-TASK STAGING
// The members of a leaf are known BEFORE the comparison sort runs (they are the
// leaf's arena slice) and the comparator's first read for every member starts at
// tail + certified. So the leaf can ask for its own text in advance, in bulk,
// instead of paying a demand miss per comparison.
//
// The regime this exists for is the one where the corpus does NOT fit in RAM
// (`CorpusText::ramCached() == false`, i.e. the pile and any pooled view): there
// every comparator span is a page-pool probe, and a leaf sort's 2 log m probes
// per row at ~1 us each is what the 77x un-cached construction measured. When
// the view IS RAM-cached the stage is a memcpy and is switched OFF (counted as
// `stageSkippedRam`) - the measurement says so, the code does not guess.
//
// THE BOUND, and why the stage is a SPAN and not a per-member window. A leaf can
// hold `targetSize` rows (1M by default) and its members are scattered over the
// whole corpus, so "512 B for every member" is 512 MB per thread and the
// per-member pread count is one syscall per row - at pile scale that is n
// syscalls and 64x the corpus in scattered reads. What IS affordable is ONE bulk
// read of a bounded span: the leaf's tails are sorted, a two-pointer pass picks
// the (span, member-count) window that covers the MOST members within the byte
// budget, and that span is read with a single sequential pread (the stream
// floor). Members inside the span read warm memory; members outside it fall back
// to the pool exactly as before, so the ORDER and every construction counter are
// unchanged by construction (the stage only decides WHERE a byte comes from).
static constexpr U bin_leaf_stage_bytes_per_thread = 8u << 20;   // per-thread budget
static constexpr U bin_leaf_stage_min_bytes = 4096;  // below this (and below the
// whole corpus, so a small corpus can still stage its own text), skip the read
// The per-thread budget is a constant; BIN_LEAF_STAGE=0 is the LAZY arm of the
// charter's prefetch-vs-lazy equivalence gate (the same construction with the
// staging removed - every counter must be identical), and
// BIN_LEAF_STAGE_BYTES resizes it. Nothing in the product route sets either.
// TEST HOOK: stage even when the view is RAM-cached. A tiny fixture cannot make
// the staging engage on its own (any corpus under the pool budget is cached), so
// the brute-force oracle forces it to drive the staged comparator path at
// thousands of tiny scales. Product never sets it.
static inline bool bin_leaf_stage_force() {
    // Deliberately NOT cached: the oracle toggles it between the two arms of the
    // equivalence pass in ONE process (a cached static made that pass vacuous -
    // stage_hits=0 - which is exactly what a gate is for).
    const char* e = std::getenv("BIN_LEAF_STAGE_FORCE");
    return e && *e && *e != '0';
}
static inline U bin_leaf_stage_budget() {
    static const U v = [] {
        const char* off = std::getenv("BIN_LEAF_STAGE");
        if (off && *off == '0') return U(0);
        const char* e = std::getenv("BIN_LEAF_STAGE_BYTES");
        if (!e || !*e) return bin_leaf_stage_bytes_per_thread;
        return U(std::strtoull(e, nullptr, 10));
    }();
    return v;
}
// PART B (operator's design): the staging buffer IS the reorder buffer.
// The single densest span of PART 1 leaves budget on the table whenever a leaf's
// starts fall into SEVERAL dense clusters (which is what a leaf of scattered SA
// rows looks like on a real corpus), and one span per leaf is one monotone read
// where a few coalesced reads would cover strictly more members for the same
// bytes. So the stage is now a small set of DISJOINT, ASCENDING spans that share
// one buffer: the leaf's start positions are sorted, each span COALESCES every
// member whose start falls inside it (the gaps between those members are read
// too - that is what turns many scattered windows into one monotone read), and
// the spans are read in ascending order. Members inside any span read warm
// memory, members outside fall back to the pool exactly as before, so the ORDER
// and every construction counter are unchanged by construction.
//
// `BIN_LEAF_STAGE_SEGS` (default 4) is how many fronts one leaf may open; 1 is
// byte-for-byte the PART 1 behaviour (the densest single span), so the two can be
// measured against each other.
static constexpr U bin_leaf_stage_segments_max = 16;
static inline U bin_leaf_stage_segments() {
    static const U v = [] {
        const char* e = std::getenv("BIN_LEAF_STAGE_SEGS");
        if (!e || !*e) return U(4);
        char* end = nullptr; errno = 0;
        unsigned long long z = std::strtoull(e, &end, 10);
        if (!(*e && *e != '-' && end && !*end && !errno) || z < 1 ||
            z > bin_leaf_stage_segments_max) return U(4);
        return U(z);
    }();
    return v;
}
struct BinLeafStage {
    std::vector<uint8_t> buf;   // ONE buffer: the read target and the reorder buffer
    struct Seg { U lo, hi, off; };
    std::vector<Seg> segs;      // ascending, disjoint corpus spans
    bool on = false;
    U bytes = 0;                // bytes actually read into buf
    // PART B counters: the member windows the leaf asked for, and how many
    // coalesced monotone reads answered them (raw_windows -> merged_reads).
    U rawWindows = 0, mergedReads = 0;
    mutable size_t hint = 0;
    // Is [q, q+take) inside a staged span? Most hits repeat the last segment (a
    // comparison walks forward), so the hint is checked first; with at most
    // bin_leaf_stage_segments_max segments the fallback scan is bounded.
    inline bool covers(U q, U take) const {
        if (!on) return false;
        if (hint < segs.size()) {
            const Seg& s = segs[hint];
            if (q >= s.lo && q + take <= s.hi) return true;
        }
        for (size_t i = 0; i < segs.size(); ++i) {
            const Seg& s = segs[i];
            if (q >= s.lo && q + take <= s.hi) { hint = i; return true; }
        }
        return false;
    }
    inline const uint8_t* at(U q) const {
        if (hint < segs.size()) {
            const Seg& s = segs[hint];
            if (q >= s.lo && q < s.hi) return buf.data() + s.off + (q - s.lo);
        }
        for (size_t i = 0; i < segs.size(); ++i) {
            const Seg& s = segs[i];
            if (q >= s.lo && q < s.hi) { hint = i; return buf.data() + s.off + (q - s.lo); }
        }
        require(false, "bin leaf stage: byte outside every staged span");
        return buf.data();   // unreachable: require() aborts
    }
};
// Fill `st` with up to BIN_LEAF_STAGE_SEGS coalesced spans of this leaf's starts
// that together fit `budget` bytes of text (each span one bulk read). Returns the
// number of members covered.
inline U bin_stage_leaf(const CorpusText& t, const U* src, U m, U depth, U budget,
                        BinLeafStage& st, BinWork& w) {
    st.on = false; st.bytes = 0; st.segs.clear(); st.hint = 0;
    st.rawWindows = st.mergedReads = 0;
    if (!budget || m < 2) return 0;
    const U n = t.n;
    // The comparator starts at `certified` and the certified prefix is free, so
    // every staged window starts at tail + depth; it must also clear the end of
    // the text for a dollar suffix, so the starts are clamped by n.
    std::vector<U> starts;
    starts.reserve(m);
    for (U i = 0; i < m; ++i) {
        const U s = bin_word_tail(src[i]) + depth;
        if (s < n) starts.push_back(s);
    }
    if (starts.empty()) return 0;
    std::sort(starts.begin(), starts.end());
    st.rawWindows = U(starts.size());
    const U segMax = bin_leaf_stage_segments();
    const U floorBytes = std::min<U>(bin_leaf_stage_min_bytes, n);
    std::vector<uint8_t> taken(starts.size(), 0);
    std::vector<std::pair<U, U>> spans;   // (lo, hi), ascending
    U remaining = budget;
    for (U s = 0; s < segMax; ++s) {
        const U perSeg = std::min<U>(std::max<U>(floorBytes, budget / segMax), remaining);
        if (perSeg <= 1) break;
        // Densest cluster of the NOT-YET-TAKEN starts that fits perSeg bytes: the
        // same two-pointer as PART 1, over the remaining starts only.
        U bestI = 0, bestJ = 0, bestMembers = 0;
        for (U i = 0; i < starts.size(); ++i) {
            if (taken[i]) continue;
            U j = i, members = 0;
            while (j < starts.size() && starts[j] - starts[i] < perSeg) {
                if (!taken[j]) ++members;
                ++j;
            }
            if (members > bestMembers) { bestMembers = members; bestI = i; bestJ = j; }
        }
        if (!bestMembers) break;
        const U lo = starts[bestI];
        U hi = lo + 1;
        for (U z = bestI; z < bestJ; ++z) {
            if (taken[z]) continue;
            hi = std::max<U>(hi, std::min<U>(n, starts[z] + 1));
        }
        // The span is budget-capped and can never clip a member it counted: every
        // counted start is < lo + perSeg by construction, and perSeg <= remaining.
        hi = std::min<U>(hi, lo + perSeg);
        if (hi <= lo) break;
        for (U z = bestI; z < bestJ; ++z)
            if (!taken[z] && starts[z] < hi) taken[z] = 1;
        if (hi - lo >= floorBytes || hi - lo >= n) {
            spans.emplace_back(lo, hi);
            remaining -= hi - lo;
        }
    }
    if (spans.empty()) return 0;
    std::sort(spans.begin(), spans.end());
    U total = 0;
    for (const auto& sp : spans) total += sp.second - sp.first;
    if (total < floorBytes && total < n) return 0;
    st.buf.resize(total);
    U off = 0;
    for (const auto& sp : spans) {                 // ascending monotone front
        const U len = sp.second - sp.first;
        t.read(sp.first, st.buf.data() + off, len);
        st.segs.push_back(BinLeafStage::Seg{sp.first, sp.second, off});
        off += len;
    }
    st.on = true; st.bytes = total; st.mergedReads = U(spans.size());
    U covered = 0;
    for (U i = 0; i < starts.size(); ++i) {
        for (const auto& sp : spans)
            if (starts[i] >= sp.first && starts[i] < sp.second) { ++covered; break; }
    }
    ++w.stageLeaves;
    w.stageBytes += st.bytes;
    w.stageMembers += covered;
    w.stageRawWindows += st.rawWindows;
    w.stageMergedReads += st.mergedReads;
    return covered;
}

// Total order on two distinct tails, exact, through the CorpusText view only.
// certified = a provably equal prefix length (the bin's refined depth), so no
// shared prefix inside a bin is ever re-walked by a comparison.
static inline int bin_tail_cmp(const CorpusText& t, bool dollar, U word_a, U word_b,
                               U certified, BinWork& w,
                               const BinLeafStage* stage = nullptr) {
    const U n = t.n;
    // Arena words in, tails out (bin_word): the packed symbol/hit bits must not
    // influence the order.
    const U a = bin_word_tail(word_a), b = bin_word_tail(word_b);
    require(a != b && a <= n && b <= n, "bin tail comparator bounds");
    ++w.comparisons;
    // The empty suffix (dollar tail n) is minimal and needs no symbol read.
    if (dollar && (a == n || b == n)) { ++w.certDecided; return a == n ? -1 : 1; }
    require(a < n && b < n, "bin tail comparator bounds");
    // dollar: key = M[p..n).$^inf, the shorter suffix is smaller once the
    //         common prefix covers it. cyclic: M^inf[p..], ties by position.
    const U cap = dollar ? std::min(n - a, n - b) : n;
    const U entry = std::min(certified, cap);
    U j = entry;
    while (j < cap) {
        U take = std::min<U>(64, cap - j);
        // The view is M.M; one wrap of the doubled text covers every cyclic
        // range, and dollar ranges never cross n.
        U room = 2 * n - (a + j);
        if (take > room) take = room;
        require(take, "bin tail compare window");
        uint8_t ab[64], bb[64];
        // PART 1: the staged span answers the read when it covers the range; the
        // bytes are the same bytes (one bulk pread of the same immutable text),
        // so no counter and no comparison can move.
        const bool ha = stage && stage->covers(a + j, take);
        const bool hb = stage && stage->covers(b + j, take);
        const uint8_t* A = ha ? stage->at(a + j) : t.span(a + j, ab, take);
        const uint8_t* B = hb ? stage->at(b + j) : t.span(b + j, bb, take);
        if (stage) {
            if (ha && hb) ++w.stageHits;
            else if (ha || hb) ++w.stagePartials;
            else ++w.stageMisses;
        }
        U d = equal_prefix(A, B, take);
        if (d < take) {
            j += d; w.symbols += j + 1 - entry;
            return A[d] < B[d] ? -1 : 1;
        }
        j += take;
    }
    w.symbols += j - entry;
    ++w.certDecided;
    if (dollar) return a > b ? -1 : 1;   // finite suffix: shorter is smaller
    // Equal rotations: the global rule is ascending SOURCE position.
    ++w.ties;
    return bin_pos_of(a, n, false) < bin_pos_of(b, n, false) ? -1 : 1;
}

// ------------------------------------------- PART 4: THE PERIODIC TIE-BREAK
// The target-size rule is the ONLY policy: a node is subdivided until it holds
// <= targetSize rows, and depth is an OUTPUT of the data. There is exactly ONE
// shape in which subdivision cannot progress, and it is not a tuning question:
// PERFECT PERIODICITY. Every member of the node lands in the same sub-bin at
// every depth because the rotations are literally identical, so no byte the
// bucketing codes can ever separate them. The old answer was a user-facing
// `--bin-depth-cap` (default 4096) that took the node as an over-target LEAF and
// comparison-sorted it: exact, but each comparison walks the rotations to their
// end, i.e. m log m full-length LCE walks for a node whose order is known.
//
// The exact answer is cheaper and needs no knob: two rotations a and b of a
// cyclic text are equal iff the text has period (b - a) around the cycle, and
// every member's rotation is equal to every other's iff
//
//     G = gcd(n, (tail_i - tail_0) mod n)   is a period of the cyclic text.
//
// `G is a period` is tested exactly (one span equality of length n-G, the same
// CorpusText view bin_tail_cmp walks), so the probe NEVER produces a wrong
// answer - a node it cannot prove periodic just keeps subdividing. When it does
// prove it, all m rotations are identical and the total order is the
// comparator's own cyclic tie-break: ASCENDING SOURCE POSITION, which in tail
// coordinates is ascending tail (`tail = p + 1 mod n`, so 1,2,...,n-1,0 is
// exactly positions 0,1,...,n-2,n-1). That sort touches no text at all.
//
// THE SANITY BOUND. `--bin-depth-cap` is gone from the user's face; the
// recursion is bounded only by BIN_NO_PROGRESS_SANITY_BOUND, which is not a
// policy but a BUG DETECTOR: a node that still makes no progress at that depth
// and is not provably periodic means the period probe or the code table is
// broken, and the producer FAILS LOUD instead of quietly taking an unbounded
// serial stretch.
static constexpr U bin_no_progress_sanity_bound = U(1) << 20;
// The period probe's cadence and its scan budget. The probe is O(m) gcd work
// plus ONE exact span equality, so it must not run at every no-progress level
// (a duplicate block reaches depth 879 on the 10 GB probe: 879 probes x m rows
// of divides would cost more than the bucketing they sit in). Every 64th
// certified byte is enough for the answer to arrive early, and the span scan is
// bounded so a NEAR-periodic text (period p for a long run, then a difference)
// costs one bounded scan per probe instead of a corpus pass. When the scan
// budget cannot certify the period and the node's own certificate does not
// cover it either, the node is NOT claimed periodic and keeps dividing - the
// probe never answers wrong, it only answers late.
static constexpr U bin_period_probe_stride = 64;
static constexpr U bin_period_scan_budget = U(1) << 28;

// Do the n-`len`-long cyclic spans at `a` and `b` agree? (The period test: period
// G of a cyclic word of length n == T[0..n-G) equals T[G..n).) Same 64-byte
// windowed span read bin_tail_cmp uses, so it sees exactly the adapter's text.
static inline bool bin_ranges_equal(const CorpusText& t, U a, U b, U len) {
    const U n = t.n;
    uint8_t ab[64], bb[64];
    U j = 0;
    while (j < len) {
        U take = std::min<U>(64, len - j);
        U room_a = 2 * n - (a + j), room_b = 2 * n - (b + j);
        take = std::min(take, std::min(room_a, room_b));
        require(take, "bin period compare window");
        const uint8_t* A = t.span(a + j, ab, take);
        const uint8_t* B = t.span(b + j, bb, take);
        if (std::memcmp(A, B, size_t(take)) != 0) return false;
        j += take;
    }
    return true;
}

static inline U bin_gcd(U a, U b) {
    while (b) { U r = a % b; a = b; b = r; }
    return a;
}

// The period that makes this node's m rotations provably identical, or 0 when
// the node is not provably periodic (finite suffixes are never equal, so dollar
// mode never takes this path). Cost: O(m) tail reads plus, only when a period
// candidate survives the gcd, ONE exact span comparison of n-G symbols bounded
// by BIN_PERIOD_SCAN_BUDGET (or by the node's own certified `depth`, which is
// free: if the node already agreed on >= n-G symbols then the period holds).
static inline U bin_node_period(const CorpusText& t, bool dollar, const U* src, U m,
                                U depth) {
    if (dollar || m < 2) return 0;
    // TEST HOOK for the fail-loud sanity path (BIN_PERIOD_PROBE=0): with the
    // probe off, a periodic node cannot progress and cannot be certified, so the
    // producer must refuse at BIN_NO_PROGRESS_SANITY_BOUND instead of quietly
    // taking an unbounded serial stretch. The oracle drives that refusal in a
    // child process; the product never sets it.
    static const bool probeOn = [] {
        const char* e = std::getenv("BIN_PERIOD_PROBE");
        return !(e && *e == '0');
    }();
    if (!probeOn) return 0;
    const U n = t.n;
    const U t0 = bin_word_tail(src[0]);
    U g = n;
    for (U i = 1; i < m; ++i) {
        const U d = bin_word_tail(src[i]);
        const U diff = d > t0 ? d - t0 : t0 - d;
        g = bin_gcd(g, std::min(diff, n - diff));
        if (g <= 1) break;          // period 1 == "every byte equal": still tested
    }
    if (g == 0 || g >= n) return 0;
    const U span = n - g;
    // The node's own certificate: it has already proven that its members agree
    // on their first `depth` symbols, so a period that fits inside that window
    // needs no further reading.
    if (depth >= span) return g;
    if (span <= bin_period_scan_budget) {
        // One exact test. `g` divides n by construction (it is a gcd with n).
        if (!bin_ranges_equal(t, 0, g, span)) return 0;
        return g;
    }
    // A period longer than the scan budget cannot be certified by scanning, but
    // it CAN be certified by the node itself when the node holds EVERY position
    // of its residue class: consecutive members g apart prove T[a..a+depth) ==
    // T[a+g..a+g+depth) pointwise (the certificate), so a closed chain covers
    // the whole cycle for any depth >= 1. That is the shape of a whole-corpus
    // repeat (a 10 GB run of one byte is a node of all n positions, g = 1), and
    // it is exact: every member is present, no gap is assumed.
    if (m >= 2) {
        std::vector<U> tails(m);
        for (U i = 0; i < m; ++i) tails[i] = bin_word_tail(src[i]);
        std::sort(tails.begin(), tails.end());
        bool chain = (tails[0] + n - tails[m - 1]) == g;   // closes the cycle
        for (U i = 1; chain && i < m; ++i) chain = (tails[i] - tails[i - 1]) == g;
        if (chain) return g;
    }
    return 0;   // cannot certify: keep dividing (both answers stay exact)
}

// The exact order of a provably periodic node: ascending source position.
// In tail coordinates `tail = p + 1 mod n`, so ascending position is ascending
// tail EXCEPT that tail 0 is position n-1 and therefore sorts LAST - the same
// mapping bin_pos_of() applies, which is what bin_tail_cmp's cyclic tie-break
// orders by. (Getting this wrong is a silent permutation: the probe's first
// version emitted 63,0,1,...,62 for a 64-row all-'a' node and the ADAPTIVE
// oracle caught it in one run.)
static inline void bin_periodic_sort(U* src, U m, U n) {
    std::sort(src, src + m, [n](U a, U b) {
        const U ka = bin_word_tail(a), kb = bin_word_tail(b);
        return (ka ? ka : n) < (kb ? kb : n);
    });
}

// ====================================================== THE ENTROPY RULE
// tools/binbwt/ENTROPY_RULE_DESIGN.md. THE RULE, in one line:
//
//     take a radix level on a node of m rows iff H > theta,
//
// where H is the Shannon entropy (bits) of the node's next-byte histogram and
// theta = (cost of one level row) / (cost of one comparison row), both measured
// (§1-§2 of the design). A level costs`m` level rows and buys `m * H`
// comparison-bits, so the comparison IS the derivation; there is no tuning knob
// in the product's default beyond the flag that arms it.
//
// THE HISTOGRAM IS ALREADY THERE: `bin_sort_rec` counts the next byte of every
// row before it decides anything, so the counting pass and the entropy
// evaluation are the same operation and the rule's own cost is O(codes) flops
// per NODE, zero per row.
//
// THE DEFAULT IS OFF (theta = 0 never fires), so the product path executes the
// same instructions it does today and every existing gate is unchanged.
struct BinEntropyOpts {
    bool armed = false;
    double minBits = 0.0;       // theta for the TREE (bits)
    double leafMinBits = 0.0;   // theta for the in-RAM LEAF arm (bits)
    U leafFloor = 16;           // below this many rows the leaf arm compares
};
// Set by `bin_order` for the duration of one call and read (never written) by
// the build threads - exactly the `bin_phrase_now` discipline above.
static BinEntropyOpts bin_entropy_now;

// H (bits) of the next-byte histogram, over the codes the recursion will visit.
// Code 0 is the dollar-exhausted bucket, which is ordered WITHOUT comparisons
// (descending tail = ascending length), so it is not information the level buys
// and it is excluded from the normalisation. A node left with <= 1 live row has
// nothing to decide: H = 0.
static inline double bin_hist_entropy(const U* count, U codes, U m) {
    const U mtot = m - count[0];
    if (mtot <= 1) return 0.0;
    double H = 0.0;
    for (U c = 1; c < codes; ++c) {
        const U cc = count[c];
        if (!cc) continue;
        const double p = double(cc) / double(mtot);
        H -= p * std::log2(p);
    }
    return H > 0.0 ? H : 0.0;
}

// The rule's own instrument: the H distribution by depth (min / mean / max).
static inline void bin_entropy_journal(BinWork& w, U depth, double H) {
    const U s = depth < bin_entropy_depth_slots ? depth : bin_entropy_depth_slots - 1;
    BinWork::BinHEnt& e = w.hDepth[s];
    ++e.n; e.sum += H; e.mn = std::min(e.mn, H); e.mx = std::max(e.mx, H);
    if (depth >= bin_entropy_depth_slots) ++w.hDepthOver;
}

// The ONE decision. Called right after the level's histogram (and after the
// periodic probe, see bin_sort_rec): returns true when the node is
// COMPARISON-BOUND, i.e. the level just taken was the last one it is worth
// taking. A ZERO-entropy node (`H == 0`: the level bought nothing - the byte
// 0x20 monster, and the `biggest == m` case inside it) is counted apart, because
// it is the regime the rule exists for.
static inline bool bin_entropy_exit(BinWork& w, const U* count, U codes, U m,
                                    U depth) {
    const double H = bin_hist_entropy(count, codes, m);
    bin_entropy_journal(w, depth, H);
    ++w.entropyEvals;
    if (H > bin_entropy_now.minBits) { ++w.entropyTaken; return false; }
    ++w.entropyExits; w.entropyExitRows += m;
    if (H <= 0.0) ++w.entropyNoProgExits;
    return true;
}

// Bucket code of tail p at `depth`: a present byte v becomes v+1 so an
// exhausted suffix (v = -1) sorts below every present byte, which is exactly the
// dollar order at this depth. Codes span 257^levelBytes buckets, code 0 being
// the all-exhausted one.
static inline unsigned bin_code(const CorpusText& t, bool dollar, U word, U depth,
                                U levelBytes) {
    const U n = t.n;
    const U tail = bin_word_tail(word);
    unsigned code = 0;
    for (U d = 0; d < levelBytes; ++d) {
        unsigned v = 0;
        // dollar: the tail's suffix has length n-tail, so the symbol at `depth`
        // exists iff tail+depth < n; cyclic tails never run out.
        if (!(dollar && tail + depth + d >= n)) {
            U q = tail + depth + d;
            while (q >= n) q -= n;
            v = unsigned(t.at(q)) + 1u;
        }
        code = code * 257u + v;   // v in [0,256]: 0 = exhausted here
    }
    return code;
}

// Window storage for transient positions: uninitialised (no zero pass) and
// hugepage-backed where the kernel allows it, because every byte is written by
// the fill pass before it is read.
struct BinArena {
    U* p = nullptr;
    U slots = 0;
    explicit BinArena(U n) : slots(n) {
        if (!n) return;
        void* q = nullptr;
        require(posix_memalign(&q, 4096, size_t(n) * sizeof(U)) == 0, "bin arena alloc");
        p = static_cast<U*>(q);
        if (n * sizeof(U) >= (1u << 21)) ::madvise(p, size_t(n) * sizeof(U), MADV_HUGEPAGE);
    }
    ~BinArena() { std::free(p); }
    BinArena(const BinArena&) = delete;
    BinArena& operator=(const BinArena&) = delete;
    U* data() { return p; }
};

// -------------------------------------------------- disc-backed window files
// THE OPERATOR'S ORIGINAL MULTI-PASS SCHEME, RESTORED. A window's positions
// live in a BOUNDED ROLLING FILE in the scratch tree, never in an anonymous RAM
// arena:
//
//   [0, 4096)                header (page aligned, so the slot array starts on
//                            a page boundary and every pread is aligned)
//   [4096, 4096 + 8*slots)   `slots` 8-byte arena words, slot i at 4096 + 8i
//
// The on-disc image IS the arena: the fill writes the SAME slot for the SAME
// position as the RAM arena did (the (bin, chunk) slot0 prefix-sum layout), so
// nothing downstream of the read can tell the difference. The fill writes
// through buffered `pwrite` at absolute slot offsets (see BinFillSink below - a
// window file of ANY size therefore charges ZERO bytes of address space); the
// build reads each bin's slice back with ONE sequential bulk pread into its
// bounded working set.
//
// WHY PWRITE AND NOT MAP_SHARED. The write path used to be a MAP_SHARED mapping
// of the whole window, i.e. the mapping charged the window's bytes against the
// phase's RLIMIT_AS. At 10 GB with the DEFAULT live cap that window was
// 44.6 GB, and under PHASE_CAP_KB=104857600 (--memory-gb 100) the mmap FAILED
// live (sol10.log: BIN_WINDOW index=0 live_slots=5.58e9 file_bytes=44.6 GB,
// CROSS_LCP_FATAL bin window file mmap). A window file is DISC: writing it
// through pwrite charges page cache (reclaimable, throttled by the kernel at
// write() time) and not one byte of address space.
//
// THE HEADER IS THE CRASH REFUSAL. It carries the window ordinal, the admitted
// byte range, the slot count, the words written and a completion word. A reader
// verifies size + magic + ordinal + geometry + completion BEFORE it trusts a
// byte, and REFUSES LOUD otherwise: a missing, stale or truncated window file is
// a hard error, never a silent rebuild.
//
// SLOT WIDTH. 8 B is the honest width and it is what the arena word already is
// (`bin_word`: tail 54b | symbol 8b | hit 2b). It is the minimum that carries
// the three per-row facts the sort must permute together; stripping it to 6 B
// (41-bit pile tail + 8-bit symbol + 1-bit hit) would shave 25% of the window
// I/O at pile scale but costs a repack pass per element on the build's critical
// path, so the width stays 8 B and the number is reported, not assumed.
static constexpr U bin_window_hdr_bytes = 4096;
static constexpr U bin_window_slot_bytes = sizeof(U);
static constexpr char bin_window_magic[8] = {'B','W','I','N','D','O','W','1'};
static constexpr char bin_window_done[8]  = {'B','W','D','O','N','E','0','1'};

struct BinWindowHeader {
    char magic[8];
    char done[8];
    U version, index, slots, b0, b1, wordsWritten;
};  // 8 + 8 + 6*8 = 64 B, the rest of the 4096 B page is unused

// CACHE HYGIENE (operator, prefetch charter): a window file is bounded on disc
// and its bytes are dead the instant its bins are built, so tell the kernel NOW
// instead of leaving tens of GB of dead page cache competing with the corpus -
// the corpus is the second-tier pool and our own scratch writes are what evict
// it. Advisory, byte-neutral, and issued at the one place that knows the file is
// finished (the unlink site).
static inline void bin_window_release_cache(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);
}
// The charter also asks for a release "after each window SEALS". It is a KNOB and
// not the default, because this same file is READ BACK by the build in this very
// process a moment later: dropping it at seal makes the build pay device reads
// for bytes it just wrote. Both ways are measured (RESULTS_PREFETCH2.md); the
// erase-time release (above) is the default because it cannot cost a read.
static inline bool bin_window_dontneed_seal() {
    const char* e = ::getenv("BIN_WINDOW_DONTNEED_SEAL");
    return e && *e && *e != '0';
}
// The ERASE-time release is the default and is what the pile-regime A/B turns
// off for its control arm (`BIN_WINDOW_DONTNEED=0`): without it the window files
// stay in the page cache after their bins are built, which is exactly the state
// the hygiene work exists to prevent. Product default 1.
static inline bool bin_window_dontneed_erase() {
    const char* e = ::getenv("BIN_WINDOW_DONTNEED");
    return !(e && *e == '0');
}
static inline void bin_window_sabotage(const std::string& path);

// The write path's staging budget, PER FILL THREAD. The scatter interleaves up
// to 256 bin streams, so each (thread, bin) region gets its own append buffer
// and no flush is ever triggered per position: a thread's buffers together are
// `bin_fill_stage_bytes_per_thread` bytes (water-filled over the window's bins,
// see BinFillSink::view), so the write path's RAM is threads x 32 MiB whatever
// the window size is (the plan prices the same term: xsa/src/plan.rs
// BIN_FILL_STAGE_BYTES_PER_THREAD).
//
// WHY 32 MiB AND NOT 4. The write rate is a function of the WRITE SIZE, not of
// the byte count: measured on this box (XFS, nvme2n1, load ~90), pwrite of
// 16 KB = 3.3 GB/s, 64 KB = 6.2, 256 KB = 8.2, while the mmap path's dirty
// stores are ~16 GB/s but leave the writeback to the kernel. A per-stream
// buffer sized by the budget (4 MiB / 256 bins = 16 KB) measured 12x the mmap
// fill; water-filled to ~128 KB per stream it lands within ~2x of the mmap
// fill's store phase, and the window's fsync - the phase both arms pay - is
// unchanged. The buffers never exceed a (bin, chunk) region, so the write
// pattern is identical to the mmap arm's page pattern.
static constexpr U bin_fill_stage_bytes_per_thread = 32u << 20;
static constexpr U bin_fill_stage_slots_per_thread =
    bin_fill_stage_bytes_per_thread / bin_window_slot_bytes;

// ------------------------------------------ the FILL's DESTINATION (2 arms)
// The scatter writes `word` into slot `slot` of bin `c`. Both arms present the
// SAME two calls inside the chunk loop - `view.put(c, slot, word)` and, once,
// `view.finish()` - so bin_order's scatter and every counter it keeps are the
// same code on both arms and only the store behind the call differs:
//
//   * BinRamFillSink : `p[slot] = word` (the A/B reference arm: the RAM arena);
//   * BinFillSink    : append to this thread's buffer for that bin, and pwrite
//                      the buffer out when it is full. Charges ZERO address
//                      space, whatever the window's size.
//
// WHY THE BUFFER IS PER (thread, bin). The scatter writes each (bin, chunk)
// region STRICTLY ASCENDING (`slot = cur[c]++`), so a per-bin stream is a pure
// append - a flush writes exactly the slots it holds, at their absolute file
// offset, and the on-disc image is byte-identical to the RAM arena. One buffer
// per thread would be useless instead: consecutive positions land in arbitrary
// bins, so a single stream would flush on nearly every write.
struct BinRamFillSink {
    U* p = nullptr;
    U stageBytes() const { return 0; }
    U writes() const { return 0; }
    struct View {
        U* p = nullptr;
        inline void put(unsigned, U slot, U word) { p[slot] = word; }
        void finish() {}
    };
    View view(unsigned, unsigned, const std::array<U, 256>&,
              const std::array<U, 256>&) const {
        return View{p};
    }
};

struct BinFillSink {
    struct Counters { std::atomic<U> stageBytes{0}, writes{0}; };
    int fd = -1;
    U slots = 0;        // the window's slot count (every write is bounded by it)
    U stageSlots = 0;   // per-stream buffer capacity, slots (>= 1)
    // Reference-counted so the sink is copyable and every fill thread's view can
    // charge the same counters (one window is filled by many threads).
    std::shared_ptr<Counters> c = std::make_shared<Counters>();
    U stageBytes() const { return c->stageBytes.load(std::memory_order_relaxed); }
    U writes() const { return c->writes.load(std::memory_order_relaxed); }
    struct Stream {
        std::unique_ptr<U[]> buf;
        U cap = 0;              // allocated slots
        U next = 0;             // absolute slot that buf[0] holds (flush cursor)
        U fill = 0;             // slots valid in buf
        U slot0 = 0, count = 0; // the (bin, chunk) region this stream owns
    };
    struct View {
        BinFillSink* s = nullptr;
        unsigned b0 = 0;
        std::vector<Stream> st;   // [bin - b0]
        inline void put(unsigned c, U slot, U word) {
            Stream& t = st[c - b0];
            if (t.fill == t.cap) flush(t);
            require(slot == t.next + t.fill, "bin window fill stream append (slot order)");
            t.buf[t.fill++] = word;
        }
        void flush(Stream& t) {
            if (!t.fill) return;
            const size_t off = size_t(bin_window_hdr_bytes) +
                               size_t(t.next) * bin_window_slot_bytes;
            const size_t len = size_t(t.fill) * bin_window_slot_bytes;
            require(off + len <= size_t(bin_window_hdr_bytes) +
                                    size_t(s->slots) * bin_window_slot_bytes,
                    "bin window fill write beyond the window");
            const uint8_t* p = reinterpret_cast<const uint8_t*>(t.buf.get());
            for (size_t done = 0; done < len;) {
                const ssize_t got = ::pwrite(s->fd, p + done, len - done,
                                             off_t(off + done));
                require(got > 0, "bin window fill pwrite (short write)");
                done += size_t(got);
            }
            t.next += t.fill; t.fill = 0;
            s->c->writes.fetch_add(1, std::memory_order_relaxed);
        }
        void finish() {
            for (Stream& t : st) {
                flush(t);
                require(t.next == t.slot0 + t.count,
                        "bin window fill region coverage (every slot written once)");
            }
        }
    };
    View view(unsigned b0, unsigned b1, const std::array<U, 256>& start,
              const std::array<U, 256>& count) {
        View v;
        v.s = this; v.b0 = b0;
        v.st.resize(b1 - b0);
        // WATER-FILL the thread's staging budget over the window's bins: the
        // largest per-stream buffer `s` such that sum_c min(count_c, s) fits the
        // budget. Small regions get their whole (bin, chunk) and are flushed
        // ONCE; the big ones share what is left, so every pwrite is as large as
        // the budget allows (the write rate is a function of the write size).
        U total = 0, maxCount = 0;
        for (unsigned c = b0; c < b1; ++c) total += count[c], maxCount = std::max(maxCount, count[c]);
        const U budget = stageSlots ? stageSlots : 1;
        U lo_ = 1, hi_ = std::max<U>(1, maxCount);
        while (lo_ < hi_) {
            const U mid = lo_ + (hi_ - lo_ + 1) / 2;
            U sum = 0;
            for (unsigned c = b0; c < b1; ++c) sum += std::min<U>(count[c], mid);
            if (sum <= budget) lo_ = mid; else hi_ = mid - 1;
        }
        U allocated = 0;
        for (unsigned c = b0; c < b1; ++c) {
            Stream& t = v.st[c - b0];
            t.slot0 = start[c]; t.count = count[c]; t.next = start[c];
            if (!t.count) continue;
            t.cap = std::min<U>(t.count, lo_);
            t.buf.reset(new U[t.cap]);
            allocated += t.cap;
        }
        (void)total;
        c->stageBytes.fetch_add(allocated * bin_window_slot_bytes,
                                std::memory_order_relaxed);
        return v;
    }
};

// One window file. `create` truncates it and writes the header; `fillSink`
// hands the fill a buffered-pwrite destination; `openRead` opens an EXISTING
// file, verifies the header and hands out bulk sequential reads. All failures
// are `require` -> CROSS_LCP_FATAL.
struct BinWindow {
    std::string path;
    int fd = -1;
    U slots = 0, b0 = 0, b1 = 0, index = 0;
    U bytes = 0;           // header + slot array

    BinWindow() = default;
    BinWindow(const BinWindow&) = delete;
    BinWindow& operator=(const BinWindow&) = delete;

    static U slotBytes(U slots) {
        return bin_window_hdr_bytes + slots * bin_window_slot_bytes;
    }
    void writeHeader(U wordsWritten) {
        BinWindowHeader h{};
        std::memcpy(h.magic, bin_window_magic, 8);
        if (wordsWritten == slots) std::memcpy(h.done, bin_window_done, 8);
        h.version = 1; h.index = index; h.slots = slots;
        h.b0 = b0; h.b1 = b1; h.wordsWritten = wordsWritten;
        const ssize_t got = ::pwrite(fd, &h, sizeof(h), 0);
        require(got == ssize_t(sizeof(h)), "bin window header write");
    }
    // ---- create (the FILL arm) --------------------------------------------
    // The file is created at its FULL size (sparse - ftruncate allocates no
    // blocks) and the header is written; the slot array is filled by pwrite in
    // the order the fill scatters it. No mapping exists, so nothing here can
    // fail on the phase's address-space limit.
    void create(const std::string& p, U idx, U sl, U lo, U hi) {
        path = p; index = idx; slots = sl; b0 = lo; b1 = hi;
        bytes = slotBytes(sl);
        ::unlink(path.c_str());
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        require(fd >= 0, "bin window file create (scratch tree writable?)");
        require(::ftruncate(fd, off_t(bytes)) == 0, "bin window file truncate");
        // PREALLOCATE the extents. The scatter writes each (bin, chunk) region
        // separately, i.e. tens of thousands of 8-64 KB writes spread over the
        // window, so on a sparse file EVERY write would allocate blocks (and pay
        // an XFS transaction) - measured at 17 us per 16 KB write, 12x the mmap
        // path. With the extents already there a write is a page-cache copy.
        if (bytes) {
            const int rc = ::posix_fallocate(fd, 0, off_t(bytes));
            require(rc == 0, "bin window file preallocate (scratch full?)");
        }
        writeHeader(0);
    }
    // ---- the FILL arm's destination: buffered pwrite, no mapping ----------
    BinFillSink fillSink(unsigned /*b0_*/, unsigned /*b1_*/) const {
        BinFillSink s;
        s.fd = fd; s.slots = slots;
        // The whole per-thread budget; the view water-fills it over the bins.
        s.stageSlots = bin_fill_stage_slots_per_thread;
        return s;
    }
    // ---- seal (the FILL arm ends here) ------------------------------------
    // The completion word is written after every slot is written, so a file
    // whose fill was killed is REFUSED by its reader instead of being rebuilt.
    void seal() {
        writeHeader(slots);
        ::fsync(fd);
        if (bin_window_dontneed_seal()) ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    }
    // ---- open an existing file and verify it (the BUILD arm) --------------
    void openRead(U idx, U sl, U lo, U hi) {
        index = idx; slots = sl; b0 = lo; b1 = hi;
        bytes = slotBytes(sl);
        bin_window_sabotage(path);
        fd = ::open(path.c_str(), O_RDONLY);
        require(fd >= 0, "bin window file MISSING under the build - refusing to rebuild");
        struct stat st{};
        require(::fstat(fd, &st) == 0, "bin window file stat");
        require(U(st.st_size) == bytes, "bin window file TRUNCATED/WRONG SIZE - refusing to rebuild");
        BinWindowHeader h{};
        require(::pread(fd, &h, sizeof(h), 0) == ssize_t(sizeof(h)), "bin window header read");
        require(std::memcmp(h.magic, bin_window_magic, 8) == 0,
                "bin window file BAD MAGIC - refusing to rebuild");
        require(std::memcmp(h.done, bin_window_done, 8) == 0,
                "bin window file INCOMPLETE (no completion word) - refusing to rebuild");
        require(h.version == 1, "bin window file version");
        require(h.index == index, "bin window file STALE (ordinal mismatch) - refusing");
        require(h.slots == slots && h.b0 == b0 && h.b1 == b1,
                "bin window file GEOMETRY MISMATCH - refusing");
        require(h.wordsWritten == slots, "bin window file short fill - refusing");
    }
    // One bulk sequential read of a bin's slice, at its arena slot offset.
    void read(U slot, U* dst, U count) {
        if (!count) return;
        require(fd >= 0, "bin window read before open");
        require(slot + count <= slots, "bin window read range");
        const size_t off = size_t(bin_window_hdr_bytes + slot * bin_window_slot_bytes);
        const size_t len = size_t(count * bin_window_slot_bytes);
        uint8_t* p = reinterpret_cast<uint8_t*>(dst);
        for (size_t done = 0; done < len;) {
            const ssize_t got = ::pread(fd, p + done, len - done, off_t(off + done));
            require(got > 0, "bin window bulk read (short pread)");
            done += size_t(got);
        }
    }
    // Keep the FILE, drop the handles (the builder re-opens it with openRead).
    void close() {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    void erase() {
        // THE window-file cache release (see bin_window_release_cache): the
        // file's bytes are dead once its bins are built, and at pile scale this
        // is the single largest thing that competes with the corpus for the page
        // cache. Advisory, byte-neutral, and done on the fd that is still open.
        if (fd >= 0 && bin_window_dontneed_erase()) ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close();
        if (!path.empty()) { ::unlink(path.c_str()); path.clear(); }
    }
};

// TEST INSTRUMENT, NOT A PRODUCT PATH: `BIN_WINDOW_SABOTAGE={unlink,truncate,
// stale,nomagic,nodone}` damages every window file immediately before the build
// opens it, so the crash-refusal path (missing / truncated / stale / corrupt
// window file => CROSS_LCP_FATAL, never a silent rebuild) is EXERCISED by the
// oracle instead of trusted in prose. No product run sets this variable.
static inline void bin_window_sabotage(const std::string& path);
static inline void bin_window_sabotage(const std::string& path) {
    const char* mode = ::getenv("BIN_WINDOW_SABOTAGE");
    if (!mode || !*mode) return;
    const std::string m(mode);
    if (m == "unlink") { ::unlink(path.c_str()); return; }
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) return;
    BinWindowHeader h{};
    const bool have = ::pread(fd, &h, sizeof(h), 0) == ssize_t(sizeof(h));
    if (have && m == "truncate") {
        struct stat st{};
        (void)!::fstat(fd, &st);
        (void)!::ftruncate(fd, st.st_size / 2);
    } else if (have && m == "stale") {
        h.index += 7;
        (void)!::pwrite(fd, &h, sizeof(h), 0);
    } else if (have && m == "nomagic") {
        std::memcpy(h.magic, "XVANDAL1", 8);
        (void)!::pwrite(fd, &h, sizeof(h), 0);
    } else if (have && m == "nodone") {
        std::memcpy(h.done, "\0\0\0\0\0\0\0\0", 8);
        (void)!::pwrite(fd, &h, sizeof(h), 0);
    }
    ::close(fd);
}

// ------------------------------------------------- THE SAMPLED-SA ANCHORS
// Option "sample the suffix array": the emit pass of `bin_order` walks the
// finished per-bin slices in GLOBAL RANK ORDER on one thread, and every word in
// a slice already carries its text position (bin_word_tail -> bin_pos_of). So
// emitting the suffix array's value at every k-th RANK is a sequential read of
// an array the pass is streaming anyway: 8 B/entry, no comparisons, no LCE, no
// text read. This is the ONLY way to get SA[rank] out of the producer without a
// persistent SA column (the charter's `full_SA=0 persistent_positions=0`).
//
// FILE LAYOUT (little endian), version 1 - all fixed width, sequential:
//   0  u32 magic   'SXSA' == 0x41535853
//   4  u32 version == 1
//   8  u64 n        the corpus length (rows emitted == n)
//  16  u64 k        the sampling constant (>= 1); rank of entry j is j*k
//  24  u64 count    number of anchor entries (patched at close)
//  32  u64 rows     rows the emit pass saw (patched at close)
//  40  u64 pad      reserved 0: THE RANK SPACE IS THE EMIT PASS'S ROW COUNTER,
//                   i.e. rank r is the r-th REAL BWT row. The container's row
//                   numbering prepends 10 padding rows (BinBandSink::addRun in
//                   its constructor), so container_row = r + 10. `pad` records
//                   that offset so a consumer can never confuse the two.
//  48  ...          count x u64: SA[j*k] for j = 0..count-1, ascending rank
// The consumer recovers SA[rank] for a non-anchor rank by walking from the
// nearest anchor below it - that is the curve Phase 4 measures. Correctness of
// every entry against the brute-force SA is gated by sa_anchor_oracle.
static constexpr uint32_t SA_ANCHOR_MAGIC = 0x41535853u;   // 'S','X','S','A'
static constexpr uint32_t SA_ANCHOR_VERSION = 1;
static constexpr uint32_t SA_ANCHOR_HEADER_BYTES = 48;
// The container's row numbering: BinBandSink seeds 1 + 9 padding rows before the
// first real bin (binbwt_slice.h, its constructor), so a container row is an
// emit-pass rank + 10. Recorded in the header, never assumed.
static constexpr uint64_t SA_ANCHOR_ROW_PAD = 10;
struct SaAnchorWriter {
    int fd = -1;
    U k = 0, n = 0, count = 0, rows = 0, lastRank = 0;
    std::string path;
    std::vector<uint64_t> buf;          // one buffered write per 1 MiB
    double tWrite = 0;
    bool started = false, finished = false;
    SaAnchorWriter() = default;
    SaAnchorWriter(const SaAnchorWriter&) = delete;
    SaAnchorWriter& operator=(const SaAnchorWriter&) = delete;
    ~SaAnchorWriter() { if (fd >= 0) { ::close(fd); fd = -1; } }
    bool on() const { return k != 0; }
    static U expectedCount(U rows, U k) { return rows ? (rows - 1) / k + 1 : 0; }
    void open(const std::string& p, U kk, U nn) {
        require(kk >= 1, "sa-sample-k must be >= 1 (0 is OFF)");
        require(!p.empty(), "--sa-sample-out is required when --sa-sample-k > 0");
        k = kk; n = nn; path = p;
        fd = ::open(p.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
        require(fd >= 0, "create --sa-sample-out (refusing to clobber)");
        buf.reserve(1u << 17);          // 1 MiB of SA values
        uint8_t h[SA_ANCHOR_HEADER_BYTES] = {};
        const uint32_t magic = SA_ANCHOR_MAGIC, ver = SA_ANCHOR_VERSION;
        std::memcpy(h + 0, &magic, 4);
        std::memcpy(h + 4, &ver, 4);
        std::memcpy(h + 8, &n, 8);
        std::memcpy(h + 16, &k, 8);
        const uint64_t padRows = SA_ANCHOR_ROW_PAD;
        std::memcpy(h + 40, &padRows, 8);
        // count/rows stay 0 here and are patched by finish().
        require(::pwrite(fd, h, SA_ANCHOR_HEADER_BYTES, 0) == ssize_t(SA_ANCHOR_HEADER_BYTES),
                "write sa-anchor header");
        started = true;
    }
    void flush() {
        if (buf.empty()) return;
        const size_t bytes = buf.size() * sizeof(uint64_t);
        const off_t at = off_t(SA_ANCHOR_HEADER_BYTES) + off_t(count - buf.size()) * 8;
        size_t done = 0;
        while (done < bytes) {
            const ssize_t w = ::pwrite(fd, (const char*)buf.data() + done, bytes - done,
                                      at + off_t(done));
            require(w > 0, "write sa-anchor entry");
            done += size_t(w);
        }
        const double t0 = now_sec();
        (void)t0;
        buf.clear();
    }
    // One bin's ordered slice: emit the ranks in [baseRow, baseRow+m) that are
    // multiples of k. Sequential over the slice (stride k), never a random read.
    void emitSlice(const U* slice, U m, U baseRow, U nn, bool dollar) {
        if (!on() || !m) return;
        const double t0 = now_sec();
        require(baseRow == rows, "sa-anchor slice rank continuity");
        U r = ((baseRow + k - 1) / k) * k;          // first rank >= baseRow, % k == 0
        for (; r < baseRow + m; r += k) {
            const U i = r - baseRow;
            const U p = bin_pos_of(bin_word_tail(slice[i]), nn, dollar);
            require(p < nn, "sa-anchor position bounds");
            if (count) require(r > lastRank, "sa-anchor rank order");
            lastRank = r;
            buf.push_back(uint64_t(p));
            ++count;
            if (buf.size() >= (1u << 17)) flush();
        }
        rows = baseRow + m;
        tWrite += now_sec() - t0;
    }
    // Patch count/rows into the header, prove the count matches the arithmetic,
    // and stamp the counters into the run's work record.
    void finish(BinWork& w) {
        if (!on()) return;
        require(!finished, "sa-anchor finish twice");
        flush();
        const U want = expectedCount(rows, k);
        require(count == want, "sa-anchor count does not match (rows-1)/k+1");
        require(::pwrite(fd, &count, 8, 24) == 8, "patch sa-anchor count");
        require(::pwrite(fd, &rows, 8, 32) == 8, "patch sa-anchor rows");
        require(::fsync(fd) == 0, "sync sa-anchor file");
        struct stat st{};
        require(::fstat(fd, &st) == 0, "stat sa-anchor file");
        require(uint64_t(st.st_size) == SA_ANCHOR_HEADER_BYTES + uint64_t(count) * 8,
                "sa-anchor file size");
        w.saSampleK = k; w.saSamples = count;
        w.saSampleBytes = uint64_t(st.st_size); w.tSaSample += tWrite;
        std::fprintf(stderr,
            "SA_ANCHOR_DONE k=%llu entries=%llu bytes=%llu rows=%llu n=%llu "
            "bytes_per_entry=8 write_s=%.3f path=%s\n",
            (unsigned long long)k, (unsigned long long)count,
            (unsigned long long)w.saSampleBytes, (unsigned long long)rows,
            (unsigned long long)n, tWrite, path.c_str());
        ::close(fd); fd = -1;
        finished = true;
    }
};

// How the windows are backed and how far the fill may run ahead of the build.
// The calibrated default of the entropy rule's threshold (bits) and the in-leaf
// rescaling. `1.5` is the middle of the MEASURED bracket [1.18, 1.67] (1 GB and
// 10 GB, §2 of the design doc) - it is a measurement of this machine's two costs,
// not a preference. `0.25` is the in-leaf ramp: a leaf row is two STAGED reads
// instead of two 57 ns windows, and RESULTS_ENTROPY_RULE.md carries the measured
// ratio; the env knob below exists so the A/B can move it.
static constexpr double bin_entropy_default_bits = 1.5;
static constexpr double bin_entropy_leaf_ramp = 0.25;

struct BinWindowOpts {
    std::string dir;      // the rolling files' directory (scratch tree)
    U credit = 2;         // windows the fill may be ahead of the build
    U buildCap = 0;       // per-group build working set, slots (0 = whole window)
    bool disc = true;     // false = the RAM-arena A/B arm
    // THE SAMPLED-SA ANCHORS (default OFF; the product path is untouched):
    // emit SA[j*k] for every rank j*k to `saSamplePath` during the emit pass.
    U saSampleK = 0;
    std::string saSamplePath;
    // THE TAPE-EMIT PATH: an external SA stream to build the product from.
    std::string tapeEmitSa;
    U tapeEmitChunk = 0;
    // PHRASE RADIX (off by default; the product path is unchanged). `phraseOpts`
    // is the policy the caller asked for; `phrase` is the dictionary built from
    // it, and it is null whenever the flag is absent or the dictionary turned out
    // empty - in which case the byte radix runs EXACTLY as today.
    PhraseOpts phraseOpts;
    const PhraseDict* phrase = nullptr;
    // THE ENTROPY RULE (tools/binbwt/ENTROPY_RULE_DESIGN.md). Default OFF
    // (`armed == false`, theta == 0): the target-size rule is the only policy and
    // the producer executes today's instructions. Armed, the level's own
    // histogram decides whether the level is taken (H > theta, bits) and
    // `targetSize` becomes the BACKSTOP rather than the driver.
    BinEntropyOpts entropyOpts;
};

// ------------------------------------------------- THE PHASE CAP (auto cap)
// `--bin-live-cap` USED TO DEFAULT TO UNBOUNDED, i.e. "one window over the whole
// corpus", and the plan priced the arena pair (16 B/slot) as if that were the
// only RAM term. It is not: the phase also holds the corpus text and the fill's
// staging, so at 10 GB under --memory-gb 100 the derived window was 44.6 GB, the
// arena pair 89 GB and the corpus cache 10 GB - over the phase's RLIMIT_AS, and
// the run died (sol10.log). The DEFAULT cap is therefore DERIVED here, from the
// phase budget, with these terms (bytes):
//
//   usable = BIN_PHASE_HEADROOM_PCT% x RLIMIT_AS      10% headroom
//   cache  = min(n, 2 x CROSS_POOL_SLOTS x 256 B)     the corpus text CorpusText
//            keeps in RAM: it caches the corpus when it fits the page-pool budget
//            (2 buffers x 256 B page per slot), else the pools themselves
//   stage  = threads x bin_fill_stage_bytes_per_thread  the fill's pwrite staging
//   group  = 16 B x min(cap, n)                        arena + scratch pair of the
//            group the build sorts in place (a bin is indivisible, so the group
//            is the largest bin of the window, never above the whole window)
//   usable - cache - stage >= group                    ==> cap
//
// THE WINDOW FILE IS NOT IN THAT EQUATION, on purpose: since the write path is
// buffered pwrite (BinFillSink) a window file of ANY size charges ZERO bytes of
// address space. The window's bytes are DISC and are priced against the scratch
// budget by the plan (xsa/src/plan.rs: credit x cap x 8 B <= 90% of free).
//
// xsa/src/plan.rs implements the same formula for its render and its refusal; the
// constants below are the contract between the two (BIN_GROUP_SLOT_BYTES,
// BIN_FILL_STAGE_BYTES, BIN_PHASE_HEADROOM_PCT, the 2x256 B pool page).
struct BinCapTerms {
    U limit = 0;    // RLIMIT_AS of this phase (0 = unbounded)
    U usable = 0;   // headroom x limit
    U cache = 0;    // the corpus text in RAM
    U stage = 0;    // the fill's staging buffers
    U group = 0;    // what is left for the arena+scratch pair
    U cap = 0;      // derived live cap, slots
    std::string source;
};
static constexpr U bin_group_slot_bytes = 2 * bin_window_slot_bytes;   // arena+scratch
static constexpr U bin_pool_page_bytes = 256;                          // SharedPages::PS
static constexpr U bin_pool_pages_per_slot = 2;                        // two buffers/slot
static constexpr U bin_phase_headroom_pct = 90;                        // fit inside 90%

static inline U bin_corpus_cache_bytes(U n) {
    const U budget = bin_pool_pages_per_slot * SharedPages::pool_slot_count() *
                     bin_pool_page_bytes;
    return std::min<U>(n, budget);
}
static inline U bin_phase_limit_bytes() {
    struct rlimit rl{};
    if (::getrlimit(RLIMIT_AS, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY) return 0;
    return U(rl.rlim_cur);
}
// The derived default. LOUD when the fixed terms alone cannot fit the phase.
static inline BinCapTerms bin_derive_live_cap(U n, U threads) {
    BinCapTerms t;
    t.limit = bin_phase_limit_bytes();
    t.cache = bin_corpus_cache_bytes(n);
    // PART 1/3: the phase's staging RAM is now TWO terms, both per thread and
    // both constants: the fill's pwrite buffers and the leaf-sort staging span.
    // The plan prices the identical arithmetic (xsa/src/plan.rs
    // BIN_FILL_STAGE_BYTES_PER_THREAD + BIN_LEAF_STAGE_BYTES_PER_THREAD), so the
    // derived cap and the producer's own check can never disagree.
    t.stage = threads * (bin_fill_stage_bytes_per_thread +
                         bin_leaf_stage_bytes_per_thread);
    if (!t.limit) {
        // No RLIMIT_AS: there is no budget to derive from, so nothing is derived
        // silently - the cap is the whole corpus (one window) and the journal says
        // so. The front door always runs under `ulimit -v`, so this is the
        // bare-binary case.
        t.cap = std::max<U>(1, n);
        t.source = "no RLIMIT_AS on this phase: one window over the corpus (nothing to derive from)";
        return t;
    }
    t.usable = t.limit * bin_phase_headroom_pct / 100;
    require(t.usable > t.cache + t.stage,
            "bin live cap: the phase budget cannot hold the corpus cache and the fill staging");
    t.group = t.usable - t.cache - t.stage;
    t.cap = std::max<U>(1, std::min<U>(t.group / bin_group_slot_bytes, std::max<U>(1, n)));
    t.source = "auto from RLIMIT_AS " + std::to_string(t.limit) + " B: " +
               std::to_string(bin_phase_headroom_pct) + "% usable " +
               std::to_string(t.usable) + " - corpus cache " + std::to_string(t.cache) +
               " - staging (fill " + std::to_string(threads * bin_fill_stage_bytes_per_thread) +
               " + leaf " + std::to_string(threads * bin_leaf_stage_bytes_per_thread) + ") " +
               std::to_string(t.stage) + " = " +
               std::to_string(t.group) + " B for the " +
               std::to_string(bin_group_slot_bytes) + " B/slot arena pair";
    return t;
}
// An EXPLICIT cap is only accepted if it fits the same budget: a cap the phase
// cannot hold is refused HERE, loudly, instead of failing as a bad_alloc (or an
// mmap) in the middle of a window.
static inline void bin_check_live_cap(U cap, U n, U threads, const char* source) {
    const U limit = bin_phase_limit_bytes();
    if (!limit) return;
    const U usable = limit * bin_phase_headroom_pct / 100;
    const U cache = bin_corpus_cache_bytes(n);
    const U stage = threads * bin_fill_stage_bytes_per_thread;
    const U group = bin_group_slot_bytes * std::min<U>(cap, n);
    if (cache + stage + group <= usable) return;
    const U fits = usable > cache + stage
                     ? (usable - cache - stage) / bin_group_slot_bytes : 0;
    char msg[512];
    std::snprintf(msg, sizeof(msg),
        "bin live cap: %s %llu slots does not fit the phase budget (RLIMIT_AS %llu B): "
        "corpus cache %llu + fill staging %llu + arena pair %llu B (16 B/slot) = %llu B "
        "exceeds the usable %llu B (%d%% of the limit); %llu slots is the largest cap "
        "this phase can hold - lower --bin-live-cap (or raise --memory-gb)",
        source, (unsigned long long)cap, (unsigned long long)limit,
        (unsigned long long)cache, (unsigned long long)stage,
        (unsigned long long)group, (unsigned long long)(cache + stage + group),
        (unsigned long long)usable, int(bin_phase_headroom_pct),
        (unsigned long long)fits);
    fail(msg);
}

static inline U bin_codes(U levelBytes) {
    U c = 1;
    for (U d = 0; d < levelBytes; ++d) c *= 257u;
    return c;
}


// A sink that publishes whole bins instead of single rows. bin_order detects
// this interface and then delivers each bin's ordered arena slice, so the sink
// owns everything downstream of the concat (run coalescing, run-band numbering,
// band pieces, progress records). Without it, bin_order falls back to the
// per-row emit(row,p) callback, which is what the brute oracle drives - both
// paths consume the SAME arena slices.
template <class E, class = void> struct bin_publish_sink : std::false_type {};
template <class E> struct bin_publish_sink<E, std::void_t<
    decltype(std::declval<E&>().joinTargets()),
    decltype(std::declval<E&>().publishBin(0u, (const U*)nullptr, U(0), U(0))),
    decltype(std::declval<E&>().windowDone())>> : std::true_type {};

// Order `m` tails that all share their first `depth` symbols. `src` and `dst`
// are disjoint equal-length slices; the array that holds the sorted slice is
// returned. Depth-first refinement keeps every transient position array inside
// the caller's bounded bin slice.
//
// THE RULE IS THE TARGET, NOT A DEPTH. A node splits by its next byte for as
// long as it holds MORE THAN `targetSize` rows, so the tree's depth is an
// OUTPUT of the data: a bucket whose rows keep sharing prefixes keeps being
// divided (one cheap bucketing level = one symbol coded per row) instead of
// being handed to a comparison sort that has to re-walk those shared prefixes
// (log m walks of the same bytes). `depthCap` is not the rule any more - it is
// the fail-loud BACKSTOP for the one case that cannot be divided (a cyclic
// corpus of literally identical rotations), and when it fires the leaf is
// journalled (`overTargetLeaves` / BIN_LEAF_OVER_TARGET) instead of being
// silently taken. The order is exact whatever the cap: the leaf sort is a total
// order at any m.
// ------------------------------------------ PHRASE RADIX: the skip, in the tree
// THE SKIP LEMMA (tools/binbwt/PHRASE_RADIX_DESIGN.md §2). A node certified at
// depth d holds rows that all agree on the first d bytes. If EVERY row resolves
// to the SAME dictionary entry E and every row's text at tail+d compares equal to
// E's bytes for E.len, then the node may be re-certified at depth d+E.len: no
// byte before d+E.len differs between rows, so no discriminating information is
// discarded and the order inside the node is decided by the bytes after it,
// exactly as before. That is the whole correctness argument, and the oracle
// proves it against a brute suffix array.
//
// The lookup is one 8-byte key against one direct-mapped slot; the hash only
// picks a CANDIDATE and the byte comparison is what certifies it. A hash
// collision, a stale entry or an evicted entry can therefore only make the skip
// not fire - never make it wrong.
//
// WHERE IT FIRES: only when the byte radix is already grinding, i.e. a split that
// put every row in one bucket. The diverse majority of the corpus divides
// normally and never pays a probe. "Both regimes automatically": the data decides
// node by node, there is no flag or threshold deciding which region is which.
static U bin_phrase_skip(const CorpusText& t, bool dollar, U depth, const U* src,
                         U m, BinWork& w) {
    const PhraseDict* pd = bin_phrase_now;
    if (!pd || !pd->armed() || m < pd->o.skipMinRows) return 0;
    const U n = t.n;
    ++w.phraseProbes; w.phraseProbeRows += m;
    // THE EXHAUSTED-INSIDE-THE-PHRASE CASE, which is what a first attempt at this
    // got wrong. A node holds rows that agree on `depth` bytes, and in dollar mode
    // some of them may END inside the phrase - at most `depth + 16` positions of
    // the corpus can, but ONE of them in a 6M-row node was enough to refuse every
    // skip at 10 GB (measured: 82 probes, 486M probe_rows, 0 skips). It is not a
    // refusal, it is the ordinary bucket-0 rule: a suffix that ends inside the
    // certified region is a PREFIX of the other rows, so it is the smallest, and
    // `bin_code` already sends an exhausted row to code 0 (which sorts first) with
    // ascending length as its exact order. So such a row takes no part in choosing
    // the entry and is verified only for the bytes it actually has.
    int32_t chosen = -1;
    {
        uint8_t buf[16];
        for (U i = 0; i < m; ++i) {
            const U x = bin_word_tail(src[i]) + depth;
            const uint8_t* b16;
            if (dollar) {
                if (x >= n || n - x < 16) continue;   // ends inside the phrase
                b16 = t.span(x, buf, 16);
            } else {
                const U xr = x % n;
                if (xr + 16 <= n) b16 = t.span(xr, buf, 16);
                else { PhraseDict::copyAt(t, false, xr, 16, buf); b16 = buf; }
            }
            const int32_t e = pd->lookup16(b16);
            if (e < 0) { ++w.phraseProbeNoEnt; return 0; }   // no phrase here
            if (chosen < 0) chosen = e;
            else if (chosen != e) { ++w.phraseProbeDis; return 0; }
        }
    }
    if (chosen < 0) return 0;                    // every row ends inside the key
    const PhraseDict::Entry& E = pd->entry(chosen);
    const U len = U(E.len);
    if (len < std::max<U>(pd->o.skipMinLen, 16)) return 0;
    {
        // BYTE-VERIFY every row, for as many bytes as it HAS. Mid-phrase suffixes
        // are the correctness core (design doc §4) and this is the line that makes
        // them safe: the hash picked the candidate, it may not certify it.
        const uint8_t* ref = pd->text(E);
        uint8_t vb[4096];
        for (U i = 0; i < m; ++i) {
            const U x = bin_word_tail(src[i]) + depth;
            U take = len;
            if (dollar) {
                if (x >= n) continue;
                take = std::min<U>(len, n - x);
            }
            if (!PhraseDict::verify(t, dollar, x, take, ref, vb)) {
                ++w.phraseVerifyFails;
                return 0;
            }
        }
    }
    ++w.phraseSkips; w.phraseSkipRows += m;
    w.phraseSkipBytes += len * m;
    w.phraseSkipMaxLen = std::max(w.phraseSkipMaxLen, len);
    w.phraseSkipDepthMax = std::max(w.phraseSkipDepthMax, depth + len);
    w.depthMax = std::max<U>(w.depthMax, depth + len);
    return len;
}

// THE LEAF: an LCP-carrying comparison sort that starts at the depth the
// bucketing already certified, so no shared prefix inside this leaf is ever
// re-walked. Leaf rows are bounded by `targetSize`, which is what makes the
// largest serial stretch of the whole pile build a KNOWN number of rows
// (BIN_WORK leaf_rows_max). Factored out of bin_sort_rec so that the entropy
// rule's exit (and its in-leaf arm) reaches the SAME sort with the SAME
// counters: with the rule off there is exactly one call site per leaf and every
// counter is today's.
static U* bin_leaf_cmp_sort(const CorpusText& t, bool dollar, U* src, U m, U depth,
                            U targetSize, U depthCap, BinWork& w) {
    const bool capHit = depthCap && depth >= depthCap;
    const double tl = now_sec();
    // PART 1: stage the leaf's members BEFORE the comparison sort runs. The
    // budget is per thread, the read is ONE bulk pread of the densest covered
    // span, and a RAM-cached view skips it (a memcpy would only add traffic).
    BinLeafStage stage;
    const U stageBudget = bin_leaf_stage_budget();
    if (!t.ramCached() || bin_leaf_stage_force()) {
        if (stageBudget) {
            const double ts = now_sec();
            bin_stage_leaf(t, src, m, depth, stageBudget, stage, w);
            w.tStage += now_sec() - ts;
        }
    } else {
        ++w.stageSkippedRam;
    }
    auto less = [&](U a, U b) {
        return bin_tail_cmp(t, dollar, a, b, depth, w, stage.on ? &stage : nullptr) < 0;
    };
    if (m <= 16) {                       // insertion sort for tiny leaves
        for (U i = 1; i < m; ++i) {
            U v = src[i], j = i;
            while (j && less(v, src[j - 1])) { src[j] = src[j - 1]; --j; }
            src[j] = v;
        }
    } else std::sort(src, src + m, less);
    ++w.leafSorts; w.leafElems += m;
    w.leafRowsMax = std::max(w.leafRowsMax, m);
    if (m > targetSize) {
        if (capHit) {
            // The cap, not the target, decided this leaf: say so, loudly. The
            // oracle drives depth caps of 1 and 4 ON PURPOSE (to prove the order
            // is exact at any cap), so the line is rate-limited to the first few
            // and every millionth rather than drowned in; the COUNTER is exact.
            ++w.overTargetLeaves; w.overTargetRows += m;
            if (w.overTargetLeaves <= 8 || w.overTargetLeaves % 1000000 == 0)
            std::fprintf(stderr,
                "BIN_LEAF_OVER_TARGET rows=%llu target=%llu depth=%llu "
                "depth_cap=%llu (the depth cap had to take a leaf over target; "
                "the order is exact, the serial stretch is not bounded by the target)\n",
                (unsigned long long)m, (unsigned long long)targetSize,
                (unsigned long long)depth, (unsigned long long)depthCap);
        } else if (depth >= t.n) {
            // The text, not the cap: the node's suffixes cannot be divided any
            // further (they share every symbol to their end), so a leaf over the
            // target is the exact answer. Counted, not warned about - and it is
            // the shape a duplicate-only corpus has.
            ++w.structuralLeaves; w.structuralRows += m;
        } else {
            // THE ENTROPY RULE took this leaf over target: the bucket was
            // comparison-bound (H <= theta), so the tree stopped splitting and
            // the residue is this sort. It is EXACT, and it is counted apart so
            // that `structural_leaves` keeps meaning "the text ended inside the
            // node" and `over_target_leaves` keeps meaning "the cap fired".
            ++w.entropyOverTargetLeaves; w.entropyOverTargetRows += m;
        }
    }
    w.tLeaf += now_sec() - tl;
    return src;
}

// ============================================ PART 2: THE RULE IN THE LEAF
// The same arithmetic as the tree, at the leaf's own prices. A leaf is
// `m <= targetSize` rows whose text the stage already put in RAM, so a level row
// is two BUFFER reads instead of two 57 ns windows and theta falls with them
// (`BinEntropyOpts::leafMinBits`, derived from the same cost model - see the
// design doc §5 and RESULTS_ENTROPY_RULE.md for the measured ratio).
//
// `src`/`dst` are disjoint equal-length slices and the array holding the ordered
// slice is returned (bin_sort_rec's own convention), so the caller memcpy's only
// when the answer landed in the partner. With the rule OFF this function returns
// bin_leaf_cmp_sort(src) - the same bytes, in the same order, with the same
// counters as today, because that is literally the code it calls.
static U* bin_leaf_order(const CorpusText& t, bool dollar, U* src, U* dst, U m,
                         U depth, U targetSize, U depthCap, U levelBytes,
                         BinWork& w) {
    if (m <= 1) return src;
    if (!bin_entropy_now.armed || m <= bin_entropy_now.leafFloor || depth >= t.n)
        return bin_leaf_cmp_sort(t, dollar, src, m, depth, targetSize, depthCap, w);
    // The counting pass over the leaf's own position array - in RAM, and the
    // SAME pass that measures the entropy (no extra traversal).
    const U codes = bin_codes(levelBytes);
    std::vector<U> count(codes, 0), start(codes, 0);
    for (U i = 0; i < m; ++i)
        ++count[bin_code(t, dollar, src[i], depth, levelBytes)];
    ++w.levelPasses; w.levelElems += m;
    U sum = 0;
    for (U c = 0; c < codes; ++c) { start[c] = sum; sum += count[c]; }
    require(sum == m, "bin leaf bucket coverage");
    U biggest = count[0];
    for (U c = 1; c < codes; ++c) biggest = std::max(biggest, count[c]);
    const double H = bin_hist_entropy(count.data(), codes, m);
    bin_entropy_journal(w, depth, H);
    ++w.entropyLeafEvals;
    if (biggest < m && H > bin_entropy_now.leafMinBits) {
        ++w.entropyLeafTaken;
        std::vector<U> fill = start;
        for (U i = 0; i < m; ++i) {
            const U p = src[i];
            dst[fill[bin_code(t, dollar, p, depth, levelBytes)]++] = p;
        }
        ++w.binPasses;
        w.depthMax = std::max<U>(w.depthMax, depth + levelBytes);
        // Code 0 holds the dollar suffixes exhausted at this depth: ascending
        // length (descending tail) is their exact order, no comparison needed.
        if (count[0] > 1)
            std::sort(dst, dst + count[0],
                      [](U a, U b) { return bin_word_tail(a) > bin_word_tail(b); });
        for (U c = 1; c < codes; ++c) {
            if (count[c] <= 1) continue;
            U* r = bin_leaf_order(t, dollar, dst + start[c], src + start[c], count[c],
                                  depth + levelBytes, targetSize, depthCap,
                                  levelBytes, w);
            if (r != dst + start[c])
                std::memcpy(dst + start[c], r, count[c] * sizeof(U));
        }
        return dst;
    }
    // H = 0 (no progress: a cyclic corpus of identical rotations must stop here
    // or grind to depth n) and H <= theta both hand the residue to the exact
    // comparator, whose own tie-break IS the answer for equal rotations.
    ++w.entropyLeafExits; w.entropyLeafRows += m;
    return bin_leaf_cmp_sort(t, dollar, src, m, depth, targetSize, depthCap, w);
}
// THE DFS WORK LIST LIVES ON THE HEAP, NOT ON THE C STACK.
//
// bin_sort_rec is a pre-order depth-first walk of a tree whose DEPTH IS THE DATA's
// depth: every level certifies one more byte, and a node whose next byte is
// uniform (one bucket, `noProgressLevels`) STILL has to consume that byte, so a
// chain of such levels is exactly as long as the shared prefix it walks. Measured
// on the 10 GB pile: 13,121 levels reachable, and the only ceiling below the
// BIN_NO_PROGRESS_SANITY_BOUND (2^20) is the length of the corpus itself. As a
// self-recursive function each level cost one C frame (~620 B at -O2), so the walk
// ran out of the 8 MB thread stack long before any guard could fire, and the
// symptom was a SIGSEGV inside the comparator of the leaf at the bottom of the
// chain - found at 10 GB + `--bin-target-size 65536`, reproduced on the UNMODIFIED
// base binary at 88a7cb0, gdb backtrace `bin_tail_cmp <- bin_sort_rec x 13,121`,
// and it disappears when the thread stack is raised to 1 GB. The successor
// charter's direction (smaller leaves -> deeper chains) walks straight into it.
//
// RAISING THE THREAD STACK IS NOT THE FIX: the phase is budgeted by RLIMIT_AS
// (bin_derive_live_cap / bin_check_live_cap) and a per-thread stack reservation is
// charged against exactly that budget - 64 threads x 64 MB of address space is
// 4 GB the live-cap arithmetic would not know it had spent.
//
// So the work list is explicit. The ALGORITHM IS UNCHANGED: the visit ORDER
// (children pushed in descending bucket so they pop in ascending bucket, exactly
// as the recursion visited them), every counter, every comparison and every byte
// of the output are identical - only the storage of the pending work moves. That
// is what the oracle suite and the BIN_WORK counters (identical at 100 MB, 1 GB
// and 10 GB) assert.
//
// THE FRAME CONTRACT is the recursive call's own: on entry the node's m rows are
// in `src`, and when the node is finished its ordered rows are in `src` again. A
// node that splits scatters into `dst`, hands each child `(src = dst + start[c],
// dst = src + start[c])`, and pushes a COLLECT frame that copies the assembled
// result back into its own `src` once the children are done. The function returns
// the top-level `src` (the callers normalize with `if (r != canon) memcpy(...)`).
//
// THE COLLECT COPY IS THE ONE TERM THIS SHAPE ADDS, and it is priced: a splitting
// node now costs three passes over its rows (count, scatter, collect) where the
// recursion cost two (count, scatter). At the deepest measured point on the pile
// that is 68.8e9 level elements = ~470 GB of copy traffic, i.e. under 1% of the
// 15,603 CPU-s that run's construction took (ANALYTIC) - paid for an invariant
// simple enough to review, since "the answer always lands back in the frame's own
// src" makes the buffer discipline local to one frame instead of a return value
// threaded through 55,570 of them.
struct BinSortFrame {
    U* src; U* dst; U m; U depth; bool collect;
};

static U* bin_sort_rec(const CorpusText& t, bool dollar, U* src, U* dst, U m,
                       U depth, U depthCap, U targetSize, U levelBytes,
                       BinWork& w) {
    const U codes = bin_codes(levelBytes);
    // Reused across nodes (the recursion malloc'd three of these per level).
    std::vector<U> count(codes, 0), start(codes, 0), fill(codes, 0);
    std::vector<BinSortFrame> stack;
    stack.reserve(256);
    stack.push_back(BinSortFrame{src, dst, m, depth, false});
    while (!stack.empty()) {
    const BinSortFrame f = stack.back();
    stack.pop_back();
    if (f.collect) {                       // children assembled into `dst`
        if (f.m && f.src != f.dst) std::memcpy(f.src, f.dst, f.m * sizeof(U));
        continue;
    }
    if (f.m <= 1) continue;
    // A walk deeper than the no-progress sanity bound is a BUG, the same verdict
    // the periodic probe below reaches for a node that cannot divide; refusing
    // here keeps a pathological corpus from growing this heap list without end.
    require(stack.size() < size_t(bin_no_progress_sanity_bound),
            "bin-bwt DFS work list past the no-progress sanity bound (a walk deeper "
            "than the sanity bound is a BUG, not a tuning question)");
    U* csrc = f.src;                    // the node's rows, read side
    U* cdst = f.dst;                    // the scatter side
    const U cm = f.m;
    U cdepth = f.depth;
    // Two DIFFERENT reasons can make a node a leaf besides size, and they are
    // counted apart: the depth CAP (the backstop - loudly journalled, because a
    // leaf oversize of the target is a serial stretch nothing bounds) and the
    // TEXT ITSELF (`depth >= t.n`: the suffixes inside the node agree all the way
    // to their end, so there is no byte left to divide on - exact and expected).
    // `depthCap == 0` is the PRODUCT default: no policy cap at all, i.e. the
    // target is the only rule and BIN_NO_PROGRESS_SANITY_BOUND is the bug guard
    // (PART 4). An explicit cap remains for the oracle, which proves the order
    // is exact at cap 1, 4 and 32 on purpose.
    const bool capHit = depthCap && cdepth >= depthCap;
    const bool structHit = cdepth >= t.n;
    if (cm <= targetSize || capHit || structHit) {
        // THE LEAF (its own machine: bin_leaf_cmp_sort, and the entropy rule's
        // in-leaf arm when the rule is armed - see bin_leaf_order). The frame
        // contract: the answer must land in the frame's own src.
        U* r = bin_leaf_order(t, dollar, csrc, cdst, cm, cdepth, targetSize,
                              depthCap, levelBytes, w);
        if (r != csrc) std::memcpy(csrc, r, cm * sizeof(U));
        continue;                          // the leaf sorted `src` in place
    }
    // ---- PHRASE RADIX: the one trigger -------------------------------------
    // A node that is DEEP (>= probeDepth certified bytes) and HEAVY (>= the row
    // floor) is where the corpus's long shared prefixes live: at 10 GB the census
    // reaches depth_max=854 with a 1,048,416-row leaf and NO node that fails to
    // divide, so a "cannot divide" trigger would never fire on the corpus this
    // exists for. The stride damps a failed probe inside a long chain (see
    // PhraseOpts::probeStride): <= 15 extra byte levels, and nothing is lost
    // because the dictionary has an entry at every offset of every block.
    if (const PhraseDict* pd = bin_phrase_now) {
        if (pd->armed() && cm >= pd->o.skipMinRows && cdepth >= pd->o.probeDepth &&
            (pd->o.probeStride <= 1 || cdepth % pd->o.probeStride == 0)) {
            if (const U skipLen = bin_phrase_skip(t, dollar, cdepth, csrc, cm, w)) {
                stack.push_back(BinSortFrame{csrc, cdst, cm, cdepth + skipLen, false});
                continue;
            }
        }
    }
    std::fill(count.begin(), count.end(), U(0));
    for (U i = 0; i < cm; ++i)
        ++count[bin_code(t, dollar, csrc[i], cdepth, levelBytes)];
    ++w.levelPasses; w.levelElems += cm;
    w.depthMax = std::max<U>(w.depthMax, cdepth + levelBytes);
    {
        U sum = 0;
        for (U c = 0; c < codes; ++c) { start[c] = sum; sum += count[c]; }
        require(sum == cm, "bin bucket coverage");
    }
    {
        for (U c = 0; c < codes; ++c) fill[c] = start[c];
        for (U i = 0; i < cm; ++i) {
            U p = csrc[i];
            cdst[fill[bin_code(t, dollar, p, cdepth, levelBytes)]++] = p;
        }
    }
    ++w.binPasses;
    // A split that leaves every row in one bucket has bought one symbol of
    // certified depth and nothing else; it is legal (the next byte still has to
    // be consumed) but it is counted, because a long run of them is the one
    // shape that makes bucketing no cheaper than a leaf sort.
    //
    // PART 4: and it is the ONLY shape in which subdivision cannot progress, so
    // it is also where the periodic tie-break is probed: if every rotation in
    // this node is provably identical (bin_node_period), the exact order is
    // ascending source position and the node is finished with zero comparisons.
    U biggest = count[0];
    for (U c = 1; c < codes; ++c) biggest = std::max(biggest, count[c]);
    {
        U biggest = count[0];
        for (U c = 1; c < codes; ++c) biggest = std::max(biggest, count[c]);
        if (biggest == cm) {
            ++w.noProgressLevels;
            const U nd = cdepth + levelBytes;
            // THE ENTROPY RULE runs the period probe UNCONDITIONALLY when armed:
            // this node is about to be handed to the comparison sort (H = 0 <=
            // theta), so one O(m) gcd pass here buys the zero-comparison answer
            // for the periodic case and costs a single pass for the rest.
            const U per = (nd % bin_period_probe_stride == 0 ||
                           nd >= bin_no_progress_sanity_bound ||
                           bin_entropy_now.armed)
                              ? bin_node_period(t, dollar, csrc, cm, cdepth)
                              : U(0);
            if (per) {
                const double tp = now_sec();
                bin_periodic_sort(csrc, cm, t.n);
                ++w.periodicLeaves;
                w.periodicRows += cm;
                w.periodicPeriodMax = std::max(w.periodicPeriodMax, per);
                w.tLeaf += now_sec() - tp;
                continue;                  // sorted in place, in `src`
            }
            // Not provably periodic and still not dividing: with the rule armed
            // this is the degenerate regime and the LCP-carrying comparison sort
            // is the EXACT answer (the entropy exit below), so the sanity
            // refusal - a bug detector for "nothing can explain this" - is
            // deliberately unreachable on the armed path.
            if (cdepth + levelBytes >= bin_no_progress_sanity_bound &&
                !bin_entropy_now.armed) {
                std::fprintf(stderr,
                    "BIN_PERIOD_SANITY depth=%llu level_bytes=%llu rows=%llu n=%llu "
                    "dollar=%u (a split that cannot progress and no period found at the "
                    "sanity bound is a BUG, not a tuning question)\n",
                    (unsigned long long)(cdepth + levelBytes),
                    (unsigned long long)levelBytes, (unsigned long long)cm,
                    (unsigned long long)t.n, unsigned(dollar));
                require(false, "bin-bwt no-progress past the sanity bound");
            }
        }
    }
    // ---- THE ENTROPY RULE: ONE decision, from the histogram just computed ---
    // tools/binbwt/ENTROPY_RULE_DESIGN.md §2-§3: the counting pass above IS the
    // measurement, so the rule adds O(codes) flops per node and nothing per row.
    // `true` means COMPARISON-BOUND: the children below go to the LCP-carrying
    // leaf rather than into another level.
    const bool entExit = bin_entropy_now.armed
                             ? bin_entropy_exit(w, count.data(), codes, cm, cdepth)
                             : false;
    // Bucket 0 holds the dollar suffixes exhausted at this depth: they all share
    // the node prefix up to their own end, so ascending length (== descending
    // position) is their exact order and no comparison is needed.
    if (count[0] > 1)
        std::sort(cdst, cdst + count[0],
                  [](U a, U b) { return bin_word_tail(a) > bin_word_tail(b); });
    // The children assemble the node in `dst`; the COLLECT frame lands after them
    // (LIFO) and copies it back into `src`, which is the frame contract.
    stack.push_back(BinSortFrame{csrc, cdst, cm, cdepth, true});
    // Descending, so the pops visit buckets in ASCENDING order exactly as the
    // recursion did (the visit order is counters-neutral, but it is kept).
    for (U c = codes; c-- > 1;) {
        if (count[c] <= 1) continue;
        // THE EXIT: the scatter above is already paid for and it certified
        // `cdepth + level_bytes` for every child, so the residue is ordered by
        // the leaf machine at that certified depth - one leaf sort of this
        // child instead of a whole subtree of levels under it. The child's rows
        // live in cdst after the scatter; the collect frame copies them back.
        if (entExit) {
            U* r = bin_leaf_order(t, dollar, cdst + start[c], csrc + start[c],
                                  count[c], cdepth + levelBytes, targetSize,
                                  depthCap, levelBytes, w);
            if (r != cdst + start[c])
                std::memcpy(cdst + start[c], r, count[c] * sizeof(U));
            continue;
        }
        stack.push_back(BinSortFrame{cdst + start[c], csrc + start[c],
                                     count[c], cdepth + levelBytes, false});    }
    }
    return src;
}

// A refinement task: order the m tails of [src, src+m) that share their first
// `depth` symbols, and leave the result in the caller's canonical arena slice
// [canon, canon+m). src/dst are the basement pair of the bin slice; a task that
// splits writes the children's regions into dst and hands them out, so leaf
// tasks are independent and a bin's own tree is worked by many threads.
struct BinTask { U* src; U* dst; U* canon; U m; U depth; unsigned bin; U key; };

// One claimable unit of the window queue. Either the LEVEL-2 split of a byte-1
// bin (`split`, one segment = the whole bin slice) or one to four independent
// segments that all share `depth` (a batched group of small 2-gram sub-bins, or
// a single sub-bin). Segments are disjoint equal-length slice pairs, so a batch
// is just a loop of independent refinements on the claiming thread.
struct BinSeg { U* src; U* dst; U* canon; U m; };
struct BinJob {
    U key = 0;
    unsigned bin = 0;
    U depth = 0;
    unsigned segs = 0;
    bool split = false;
    BinSeg seg[4];
};

// One sequential 1-byte level of the TOP split, over a byte-1 bin's arena slice.
// NEVER a 65,536-code table (the v1 measurement: 66,049 codes per coarse task
// cost 10x CPU): 257 codes, one byte, exactly the code bin_sort_rec computes at
// `depth`. Codes and offsets come back so the caller can dispatch the sub-bins
// (and rank them when the queue is frequency scheduled). The scatter goes
// arena -> scratch; a sub-bin's refinement then reads scratch and writes the
// arena cell back, so this split does the same two passes the refinement's first
// level would have done - no extra traffic, no copy back.
static void bin_split1(const CorpusText& t, bool dollar, const U* src, U* dst,
                       U m, U depth, std::array<U, 257>& count,
                       std::array<U, 258>& start, BinWork& w) {
    count.fill(0);
    for (U i = 0; i < m; ++i) ++count[bin_code(t, dollar, src[i], depth, 1)];
    ++w.levelPasses; w.levelElems += m;
    U sum = 0;
    for (U c = 0; c < 257; ++c) { start[c] = sum; sum += count[c]; }
    require(sum == m, "bin top-level bucket coverage");
    start[257] = m;
    std::array<U, 257> fill;
    for (U c = 0; c < 257; ++c) fill[c] = start[c];
    for (U i = 0; i < m; ++i) {
        const U word = src[i];
        dst[fill[bin_code(t, dollar, word, depth, 1)]++] = word;
    }
    ++w.binPasses;
    // Code 0 holds a dollar suffix that ends inside this node: at most one
    // position here, but keep the exact rule bin_sort_rec uses (ascending length
    // == descending tail) so the sub-bin is canonical whatever happens.
    if (count[0] > 1)
        std::sort(dst, dst + count[0],
                  [](U a, U b) { return bin_word_tail(a) > bin_word_tail(b); });
}

template <class Push>
static void bin_task_run(const CorpusText& t, bool dollar, const BinTask& tk,
                         U splitSize, U targetSize, U depthCap, U levelBytes,
                         BinWork& w, Push push) {
    if (tk.m <= 1) {
        if (tk.m && tk.src != tk.canon)
            std::memcpy(tk.canon, tk.src, tk.m * sizeof(U));
        return;
    }
    if (tk.m <= splitSize || (depthCap && tk.depth >= depthCap) || tk.depth >= t.n) {
        // Below the dispatch threshold one thread refines the whole subtree - and
        // with splitSize == targetSize that subtree IS one leaf (or a handful of
        // them), so the longest a single thread can be busy is one leaf sort of
        // at most `targetSize` rows plus this node's own two passes.
        U* r = bin_sort_rec(t, dollar, tk.src, tk.dst, tk.m, tk.depth, depthCap,
                            targetSize, levelBytes, w);
        if (r != tk.canon) std::memcpy(tk.canon, r, tk.m * sizeof(U));
        return;
    }
    // PHRASE RADIX: the same single trigger as the inline recursion (see the
    // comment there). A skip must land in `canon`.
    if (const PhraseDict* pd = bin_phrase_now) {
        if (pd->armed() && tk.m >= pd->o.skipMinRows &&
            tk.depth >= pd->o.probeDepth &&
            (pd->o.probeStride <= 1 || tk.depth % pd->o.probeStride == 0)) {
            if (const U skipLen =
                    bin_phrase_skip(t, dollar, tk.depth, tk.src, tk.m, w)) {
                U* r = bin_sort_rec(t, dollar, tk.src, tk.dst, tk.m,
                                    tk.depth + skipLen, depthCap, targetSize,
                                    levelBytes, w);
                if (r != tk.canon) std::memcpy(tk.canon, r, tk.m * sizeof(U));
                return;
            }
        }
    }
    const U codes = bin_codes(levelBytes);
    std::vector<U> count(codes, 0), start(codes, 0);
    for (U i = 0; i < tk.m; ++i)
        ++count[bin_code(t, dollar, tk.src[i], tk.depth, levelBytes)];
    ++w.levelPasses; w.levelElems += tk.m;
    w.depthMax = std::max<U>(w.depthMax, tk.depth + levelBytes);
    {
        U sum = 0;
        for (U c = 0; c < codes; ++c) { start[c] = sum; sum += count[c]; }
        require(sum == tk.m, "bin coarse bucket coverage");
    }
    {
        std::vector<U> fill = start;
        for (U i = 0; i < tk.m; ++i) {
            U p = tk.src[i];
            tk.dst[fill[bin_code(t, dollar, p, tk.depth, levelBytes)]++] = p;
        }
    }
    ++w.binPasses;
    U biggest = count[0];
    for (U c = 1; c < codes; ++c) biggest = std::max(biggest, count[c]);
    {
        if (biggest == tk.m) {
            ++w.noProgressLevels;
            const U nd = tk.depth + levelBytes;
            // THE ENTROPY RULE probes unconditionally (see the inline twin): the
            // node is about to exit to the comparison sort anyway, so the exact
            // zero-comparison answer costs one O(m) pass and is always tried first.
            const U per = (nd % bin_period_probe_stride == 0 ||
                           nd >= bin_no_progress_sanity_bound ||
                           bin_entropy_now.armed)
                              ? bin_node_period(t, dollar, tk.src, tk.m, tk.depth)
                              : U(0);
            // PART 4: a dispatched node that cannot divide is probed for
            // periodicity exactly like an inline one; the answer must land in
            // `canon`, and for a periodic node it is ascending source position.
            if (per) {
                const double tp = now_sec();
                bin_periodic_sort(tk.src, tk.m, t.n);
                ++w.periodicLeaves;
                w.periodicRows += tk.m;
                w.periodicPeriodMax = std::max(w.periodicPeriodMax, per);
                w.tLeaf += now_sec() - tp;
                if (tk.src != tk.canon) std::memcpy(tk.canon, tk.src, tk.m * sizeof(U));
                return;
            }
            if (tk.depth + levelBytes >= bin_no_progress_sanity_bound &&
                !bin_entropy_now.armed) {
                std::fprintf(stderr,
                    "BIN_PERIOD_SANITY depth=%llu level_bytes=%llu rows=%llu n=%llu "
                    "dollar=%u (a split that cannot progress and no period found at the "
                    "sanity bound is a BUG, not a tuning question)\n",
                    (unsigned long long)(tk.depth + levelBytes),
                    (unsigned long long)levelBytes, (unsigned long long)tk.m,
                    (unsigned long long)t.n, unsigned(dollar));
                require(false, "bin-bwt no-progress past the sanity bound");
            }
        }
    }
    // ---- THE ENTROPY RULE: one decision, from the histogram above -----------
    const bool entExit = bin_entropy_now.armed
                             ? bin_entropy_exit(w, count.data(), codes, tk.m,
                                                tk.depth)
                             : false;
    // Bucket 0: dollar suffixes exhausted at this depth. They share the node
    // prefix up to their own end, so ascending length (descending tail) is
    // their exact order and no comparison is needed.
    if (count[0]) {
        if (count[0] > 1)
            std::sort(tk.dst, tk.dst + count[0],
                      [](U a, U b) { return bin_word_tail(a) > bin_word_tail(b); });
        if (tk.dst != tk.canon) std::memcpy(tk.canon, tk.dst, count[0] * sizeof(U));
    }
    for (U c = 1; c < codes; ++c) {
        if (!count[c]) continue;
        // THE EXIT: this child is comparison-bound, so it is ordered HERE (by the
        // leaf machine at the certified depth) instead of being dispatched into
        // another level. The answer must land in `canon`.
        if (entExit) {
            U* r = bin_leaf_order(t, dollar, tk.dst + start[c], tk.src + start[c],
                                  count[c], tk.depth + levelBytes, targetSize,
                                  depthCap, levelBytes, w);
            if (r != tk.canon + start[c])
                std::memcpy(tk.canon + start[c], r, count[c] * sizeof(U));
            continue;
        }
        push(BinTask{tk.dst + start[c], tk.src + start[c], tk.canon + start[c],
                     count[c], tk.depth + levelBytes, tk.bin, tk.key});
    }
}

// One bin start-to-finish on the calling thread (used for --threads 1). `depth`
// is the prefix the slice's members already share (0 for a whole byte-1 bin, 1
// for a 2-gram sub-bin of the top split).
static void bin_build_serial(const CorpusText& t, bool dollar, U* slice, U* scratch,
                             U m, U depth, U splitSize, U targetSize, U depthCap,
                             U levelBytes, BinWork& w) {
    if (m <= 1) { ++w.singletonBins; ++w.builtBins; return; }
    std::vector<BinTask> stack;
    std::vector<BinTask> children;
    stack.push_back(BinTask{slice, scratch, slice, m, depth, 0, 0});
    while (!stack.empty()) {
        BinTask tk = stack.back();
        stack.pop_back();
        children.clear();
        bin_task_run(t, dollar, tk, splitSize, targetSize, depthCap, levelBytes, w,
                     [&](const BinTask& c) { children.push_back(c); });
        for (const auto& c : children) stack.push_back(c);
    }
    ++w.builtBins;
    w.peakLiveSlots = std::max<U>(w.peakLiveSlots, 2 * m);
    w.peakLiveBytes = std::max<U>(w.peakLiveBytes, 2 * m * sizeof(U));
}

// `fillOnly` is a MEASUREMENT INSTRUMENT, not a product path: the windows are
// counted and filled exactly as the product does, and then the refinement and
// the emission are skipped (rows are accounted, nothing is produced). It exists
// so the fill term can be timed ALONE on a corpus bigger than the RAM budget,
// which is the pile's regime; every other gate drives fillOnly=false.
template <class Emit>
static void bin_order(const CorpusText& text, bool dollar, U threads, U targetSize,
                      U depthCap, U liveCap, U levelBytes, bool freqSched,
                      U splitSize, BinWork& total, Emit&& emit,
                      bool fillOnly = false,
                      const BinWindowOpts& wopt = BinWindowOpts()) {
    const U n = text.n;
    // PHRASE RADIX: publish this call's dictionary for the build threads (they
    // only read it) and take it back down on the way out. Nothing else about this
    // function changes; with `wopt.phrase == nullptr` the pointer is null and
    // bin_phrase_skip returns immediately without touching a counter.
    bin_phrase_now = wopt.phrase;
    struct PhraseUnset { ~PhraseUnset() { bin_phrase_now = nullptr; } } phraseUnset;
    // THE ENTROPY RULE: the same discipline for the rule's policy (the build
    // threads only ever READ it). With the flag absent `armed` is false and no
    // line of the rule executes - not one counter moves, not one log2 is taken.
    bin_entropy_now = wopt.entropyOpts;
    struct EntropyUnset {
        ~EntropyUnset() { bin_entropy_now = BinEntropyOpts(); }
    } entropyUnset;
    // The sink's ascending capture-target list, joined against the corpus in the
    // fill pass. Empty for the per-row oracle path.
    const std::vector<U>* joinTargets = nullptr;
    if constexpr (bin_publish_sink<Emit>::value) joinTargets = &emit.joinTargets();
    const U joinCount = joinTargets ? joinTargets->size() : 0;
    require(n && threads >= 1 && (!depthCap || depthCap >= levelBytes) &&
            levelBytes >= 1 && levelBytes <= 2, "bin-bwt parameters");
    // ---- THE TARGET SIZE: the leaf bound, automatic by default -------------
    // Depth is an OUTPUT of the data (see bin_sort_rec), so the only shape knob
    // is how many rows a single comparison sort may take. It is priced from the
    // two measured rates of this machine:
    //   * a bucketing level costs one symbol coded per row (a random byte read
    //     plus the scatter write) - the cheapest thing the machine does;
    //   * a leaf comparison sort costs log2(m) LCE walks per row from the
    //     certified depth, and log2(m) is exactly what a bigger leaf buys back
    //     in bucketing levels.
    // A leaf must also leave the queue enough units to fill the machine: the tree
    // produces about n/S leaves, so S is chosen to keep n/S well above the thread
    // count on the SMALL fixtures while pinning the largest serial stretch (one
    // leaf sort) to a bounded, cache-scale working set on the big ones. The
    // measured ladder is in tools/binbwt/RESULTS_AUTO_RADIX.md; the default below
    // is the value that ladder picked, clamped so that it is never smaller than a
    // handful of L2-sized runs nor larger than the 32M rows where a leaf sort's
    // log2 term starts to dominate the bucketing it saves.
    //
    // MEASURED (tools/binbwt/RESULTS_AUTO_RADIX.md: the 1 GB and 10 GB ladders,
    // 96 threads, construction only):
    //   1 GB, one window:   target 262144 -> build 17.29 s, 1e6 -> 18.16,
    //                       4e6 -> 42.62, 8e6 -> 45.08, 16e6 -> 57.65,
    //                       32e6 -> 57.60; the retired 24-row rule -> 20.55.
    //   10 GB, six windows: 24 (old) -> 481.7 s wall, 262144 -> 458.5,
    //                       1e6 -> 444.1, 4e6 -> 485.1, 16e6 -> ..., 32e6 -> ...
    // So the optimum is a FLAT FLOOR between 256K and ~1.3M rows at both scales,
    // and the operator's candidate range of 8-32M rows measures 2-3x WORSE at
    // 1 GB (the leaf sort's log2(m) LCE walks per row cost more than the
    // bucketing levels they save; a level is one coded symbol per row).
    // The automatic rule therefore scales DOWN for small corpora (so a 100 MB
    // fixture still produces thousands of leaves to fill the queue) and saturates
    // at the measured 1M floor, which is the value both ladders picked.
    if (!targetSize) {
        const U share = std::max<U>(1, n / (8 * std::max<U>(1, threads)));
        targetSize = std::min<U>(1u << 20, std::max<U>(1u << 18, share));
    }
    require(targetSize >= 1, "bin-bwt target size");
    total.targetSize = std::max(total.targetSize, targetSize);
    // ---- the TOP level's depth in bytes -------------------------------------
    // levelBytes == 1: the fill pass alone is the top level (byte-1 bins), and a
    // bin's refinement starts at depth 0 (the byte after the bin symbol).
    // levelBytes == 2: the top level is TWO SEQUENTIAL 1-BYTE LEVELS - the
    // existing byte-1 bins (level 1, the fill pass, 256 streams) plus one more
    // 257-code split of each bin by its SECOND tail byte (level 2). The 2-gram
    // sub-bins are then first-class queue units refined from depth 1, so every
    // bin starts 256x smaller and one level shallower, and no 65,536-entry code
    // table exists anywhere: the interior refinement always codes ONE byte per
    // level (innerBytes). The partitioning is exactly the true 2-gram grouping
    // and the concat order stays (byte1, byte2, tail), so the global consuming
    // identity is unchanged.
    const U topSplit = levelBytes - 1;          // extra 1-byte top levels
    const U innerBytes = 1;                     // code tables are always 257
    // THE DISPATCH THRESHOLD. A node above it is split on the queue (one job per
    // child, so the children of a huge bin are worked in parallel); a node at or
    // below it is refined by the claiming thread - and with the default (== the
    // target) that refinement IS one leaf sort, so the longest a single job can
    // hold a thread is one leaf sort of at most `targetSize` rows plus the two
    // passes of the node that produced it. There is no upper bound to escape to:
    // the old default (`n/(threads*64)`, i.e. 1.63M at 10 GB/96t) is smaller than
    // a real target, so the tree below it was already depth-capped and the depth
    // cap, not the target, decided where the leaves landed.
    // FLOORED by the target and never below a thread's fair share of the window:
    // a job is dispatched per node above this, so a threshold as small as a tiny
    // target turns the queue itself into the bottleneck (measured: target 24 with
    // splitSize 24 = one job per 24 rows = 41M jobs at 1 GB, and the 1 GB probe
    // did not finish in 40 minutes). The bound that matters runs the OTHER way:
    // splitSize >= target means a dispatched node is always split, never leaf-
    // sorted whole on the claiming thread.
    if (!splitSize)
        splitSize = std::max<U>({targetSize, U(4096), n / (threads * 64)});
    // Chunks: the counting sweep counts per chunk, and the fill's scatter is one
    // pass per chunk (see the fill pass), so this is both the count width and the
    // fill width. It was capped at 64 when only the count used it; the fill wants
    // the whole machine, so the cap is the histogram table's size (256 chunks =
    // 256 x 256 counts = 512 KB), not a performance guard.
    const U NT = std::max<U>(1, std::min<U>(threads, 256));
    // Corpus chunk split for the counting sweep; the fill scatter reuses the exact
    // same boundaries so per-chunk bin counts ARE the fill offsets.
    std::vector<U> chunk(NT + 1, 0);
    for (U t = 0; t <= NT; ++t) chunk[t] = n / NT * t;
    chunk[NT] = n;
    std::vector<std::array<U, 256>> hists(NT);
    for (auto& h : hists) h.fill(0);
    // ONE decision for every sequential corpus reader in this function: a
    // RAM-cached view hands out a pointer (zero copy), a pooled view must be
    // read in bulk (one pread per span) - span() on a pooled view is the
    // comparator's 256-byte page path and would degrade a megabyte span into
    // one page fill per 256 bytes, which is what the pile's 1.31 TB corpus sees
    // (it can never be RAM cached).
    const bool ramCached = text.ramCached();
    auto seqSpan = [&](U at, uint8_t* buf, U len) {
        if (ramCached) return text.span(at, buf, len);
        text.read(at, buf, len);
        return const_cast<const uint8_t*>(buf);
    };
    {
        const double t0 = now_sec();
        auto countChunk = [&](U t) {
            auto& h = hists[t];
            constexpr U W = 1u << 20;
            std::vector<uint8_t> buf(std::min(W, std::max<U>(1, chunk[t + 1] - chunk[t])));
            for (U at = chunk[t]; at < chunk[t + 1];) {
                U len = std::min(W, chunk[t + 1] - at);
                const uint8_t* b = seqSpan(at, buf.data(), len);
                for (U j = 0; j < len; ++j) ++h[b[j]];
                at += len;
            }
        };
        if (NT == 1) countChunk(0);
        else {
            std::vector<std::thread> pool;
            for (U t = 0; t < NT; ++t) pool.emplace_back(countChunk, t);
            for (auto& th : pool) th.join();
        }
        total.tCount += now_sec() - t0;
        total.tSweep += now_sec() - t0;
        total.sweepBytes += n; ++total.sweepPasses;
    }
    std::array<U, 256> bins{};
    bins.fill(0);
    for (U t = 0; t < NT; ++t)
        for (unsigned c = 0; c < 256; ++c) bins[c] += hists[t][c];
    U histogram = 0, largest = 0;
    for (unsigned c = 0; c < 256; ++c) {
        histogram += bins[c];
        largest = std::max(largest, bins[c]);
    }
    require(histogram == n, "bin-bwt first-byte histogram coverage");
    if (liveCap < largest) {
        std::fprintf(stderr, "BIN_CAP_RAISED requested=%llu largest_bin=%llu\n",
                     (unsigned long long)liveCap, (unsigned long long)largest);
        liveCap = largest;
    }
    U row = 0;
    // ---- THE SAMPLED-SA ANCHOR STREAM (option B), OFF by default -----------
    // Opened before the window plan so a bad path fails before a byte of work,
    // and finished after the last bin so the header's count is the arithmetic
    // one. `k == 0` leaves this inert: no fd, no branch taken per row beyond a
    // predictable `if (saAnchor.on())` per BIN.
    SaAnchorWriter saAnchor;
    if (wopt.saSampleK) {
        require(!fillOnly, "--sa-sample-k cannot be used with --bin-fill-only "
                           "(nothing is emitted there)");
        saAnchor.open(wopt.saSamplePath, wopt.saSampleK, n);
    }
    // ---- THE WINDOW PLAN, computed up front -------------------------------
    // The fill may run ahead of the build by a bounded CREDIT of windows, so the
    // partition has to exist before either phase starts: whole bins packed to
    // `liveCap` slots per window, and every window's bin offsets (the arena
    // layout) precomputed once. Nothing here is per-position.
    struct WinPlan { unsigned b0, b1; U live; };
    std::vector<WinPlan> plan;
    for (unsigned b0 = 0; b0 < 256;) {
        unsigned b1 = b0;
        U live = 0;
        while (b1 < 256 && live + bins[b1] <= liveCap) { live += bins[b1]; ++b1; }
        require(b1 > b0, "bin window admits at least one bin");
        plan.push_back(WinPlan{b0, b1, live});
        b0 = b1;
    }
    std::vector<std::array<U, 257>> winOff(plan.size());
    for (size_t k = 0; k < plan.size(); ++k) {
        U sum = 0;
        for (unsigned c = plan[k].b0; c < plan[k].b1; ++c) {
            winOff[k][c] = sum; sum += bins[c];
        }
        winOff[k][plan[k].b1] = sum;
        require(sum == plan[k].live, "bin window arena sizing");
    }
    // ---- THE FILL PASS: one window, into a DESTINATION --------------------
    // The destination is the window image: a rolling window FILE's slot array
    // (the product, written by buffered pwrite) or a RAM arena (the A/B arm,
    // written by a plain store). The scatter is identical either way - the same
    // slot for the same position - so no counter can move.
    auto fillInto = [&](auto& sink, unsigned b0, unsigned b1,
                        const std::array<U, 257>& off,
                        BinWork& w) -> U {
        U joinedOut = 0;
        // Fill pass: place every position of this window into its bin slice,
        // PARALLEL over the same corpus chunks the counting sweep used - the
        // classic counting-sort scatter, and this is the term that decided the
        // pile (a serial full-corpus sweep per window: 655 windows x 1.15 h at
        // 2e9 slots).
        //
        // The counting sweep already counted, per chunk t and per byte c,
        // exactly `hists[t][c]` positions. ONE prefix sum over (chunk x bin)
        // therefore gives every (bin, chunk) pair its first arena slot:
        //
        //     slot0(t, c) = off[c] + sum_{t' < t} hists[t'][c]
        //
        // The (bin, chunk) regions are DISJOINT, so every thread scatters its
        // own chunk's positions into its own regions with no lock, no atomic
        // and no shared cursor - and concatenated in chunk order they hold the
        // bin's positions in ASCENDING POSITION order, which is exactly the
        // canonical order the serial pass produced. The arena layout is
        // identical slot for slot (asserted below), so nothing downstream of
        // the fill - the refinement, the row order, the sink - can tell the
        // difference. The v1 note that many fill threads were 2-5x SLOWER
        // measured many threads hammering 256 SHARED stream heads; here each
        // thread owns all of its streams.
        //
        // The same pass also does the two things that make parallel publication
        // possible, so that NEITHER costs a random access:
        //  * it writes the row's BWT symbol M[p-1] into the parallel symbol
        //    column (the byte is already in the window buffer, one behind);
        //  * it merge-joins the sink's ascending capture-target list against the
        //    ascending positions, so each target's bin slice index IS its row and
        //    no per-row hash lookup survives anywhere. Each chunk owns the
        //    target sub-range of its own position range (two bisects) and walks
        //    it in step with its positions, so the join stays lock-free and
        //    strictly ascending; a match is flagged only when THIS window places
        //    the position, i.e. every target gets its hit in the one window whose
        //    bin range admits it (asserted at the last window).
        {
            const double t0 = now_sec();
            // slot0[t][c]: this chunk's first slot in bin c's slice.
            std::vector<std::array<U, 256>> slot0(NT);
            for (unsigned c = b0; c < b1; ++c) {
                U sum = off[c];
                for (U t = 0; t < NT; ++t) {
                    slot0[t][c] = sum;
                    sum += hists[t][c];
                }
                require(sum == off[c] + bins[c], "bin fill chunk offsets");
            }
            std::atomic<U> joined{0};
            auto fillChunk = [&](U t) {
                std::array<U, 256> cur{};
                for (unsigned c = b0; c < b1; ++c) cur[c] = slot0[t][c];
                // This chunk's per-bin destination streams: the RAM arena's
                // pointer (one store per row) or the window file's per-(thread,
                // bin) append buffers (one buffered pwrite per flush). The
                // scatter below is IDENTICAL on both arms.
                auto dst = sink.view(b0, b1, slot0[t], hists[t]);
                const U lo = chunk[t], hi = chunk[t + 1];
                const bool allBins = (b1 - b0 == 256);
                // This chunk's slice of the ascending capture-target list.
                U ti = 0, tiEnd = 0;
                if (joinCount) {
                    ti = U(std::lower_bound(joinTargets->begin(),
                                            joinTargets->end(), lo) -
                           joinTargets->begin());
                    tiEnd = U(std::lower_bound(joinTargets->begin(),
                                               joinTargets->end(), hi) -
                             joinTargets->begin());
                }
                std::vector<uint8_t> buf(
                    std::min<U>(1u << 20, std::max<U>(1, hi - lo)));
                // M[p-1] for the first byte of a span: one byte behind the
                // chunk, never inside this thread's own range.
                uint8_t prev = lo ? text.at(lo - 1) : 0;
                U hits = 0;
                for (U at = lo; at < hi;) {
                    U len = std::min<U>(1u << 20, hi - at);
                    const uint8_t* b = seqSpan(at, buf.data(), len);
                    for (U j = 0; j < len; ++j) {
                        U p = at + j;
                        unsigned c = b[j];
                        // The join is over POSITIONS, not over this window's
                        // bins: the cursor matches every target of the chunk in
                        // ascending order (which is why a plain in-step merge is
                        // enough), and the hit is carried into the arena only if
                        // this window is the one that places the position.
                        bool hit = ti < tiEnd && (*joinTargets)[ti] == p;
                        if (hit) ++ti;
                        if (!allBins && (c < b0 || c >= b1)) continue;
                        const U slot = cur[c]++;
                        // The BWT symbol of this row: M[p-1], already in the
                        // window buffer one byte behind (p == 0 is the padding
                        // row whose symbol is 2, exactly SequentialV2's
                        // `p ? at(p-1) : 2`).
                        const uint8_t sym = p ? (j ? b[j - 1] : prev) : uint8_t(2);
                        dst.put(c, slot,
                                bin_word(bin_tail_of(p, n, dollar), sym, hit));
                        hits += hit;
                    }
                    prev = b[len - 1];
                    at += len;
                }
                // Every target of this chunk's position range was matched, in
                // ascending order, exactly once.
                require(ti == tiEnd, "bin fill chunk target join coverage");
                // Flush this thread's remaining buffers and prove each stream
                // wrote exactly its own (bin, chunk) region - the write path's
                // own slot-wise coverage check, on top of the scatter's.
                dst.finish();
                // Every slot of this chunk's regions is written exactly once,
                // and no slot of any other chunk's region is touched.
                for (unsigned c = b0; c < b1; ++c)
                    require(cur[c] == slot0[t][c] + hists[t][c],
                            "bin fill chunk coverage");
                joined.fetch_add(hits, std::memory_order_relaxed);
            };
            if (NT == 1) fillChunk(0);
            else {
                std::vector<std::thread> fillPool;
                fillPool.reserve(NT);
                for (U t = 0; t < NT; ++t) fillPool.emplace_back(fillChunk, t);
                for (auto& th : fillPool) th.join();
            }
            // Every capture target of the corpus is flagged in exactly one
            // window - the one whose bin range admits its position - so the
            // per-window counts must add up to the target list. (This is the
            // fill's own check, kept out of BinWork::targetHits, which the sink
            // owns.)
            // The corpus-wide coverage check ("every target joined exactly
            // once across windows") runs in the driver, after the last window
            // has been filled; here only this window's count is bounded.
            joinedOut = joined.load(std::memory_order_relaxed);
            require(joinedOut <= joinCount, "bin fill target join overrun");
            // What the write path itself cost: the staging buffers handed out
            // and the pwrite calls issued (0/0 on the RAM arena arm).
            w.winFillBufBytes = std::max(w.winFillBufBytes, sink.stageBytes());
            w.winFillWrites += sink.writes();
            w.tFill += now_sec() - t0;
            w.tSweep += now_sec() - t0;
            w.sweepBytes += n; ++w.sweepPasses;
        }
        return joinedOut;
    };
    // ---- THE BUILD: one memory GROUP of one window's bins ------------------
    // The window file is the whole window; the build's RAM is the group it is
    // working on (arena + scratch slice pair, `live` slots here), loaded bin by
    // bin with one sequential bulk read per bin. With `buildCap == 0` the group
    // is the whole window, i.e. exactly the RAM arena's working set.
    auto buildGroup = [&](unsigned b0, unsigned b1, U live,
                          const std::array<U, 257>& off,
                          BinArena& arena, BinArena& scratch) {
        // Bin claim order. EDF (ascending byte = emission order) keeps the
        // emitter fed; `freq` claims the largest bins first instead.
        std::vector<unsigned> order;
        for (unsigned c = b0; c < b1; ++c) if (bins[c]) order.push_back(c);
        if (freqSched)
            std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
                return bins[a] > bins[b];
            });
        auto emitBin = [&](unsigned c) {
            const double t0 = now_sec();
            const U* slice = arena.data() + off[c];
            const U baseRow = row;
            if constexpr (bin_publish_sink<Emit>::value) {
                // The sink publishes the whole ordered slice itself (parallel
                // band writers at run boundaries); it owns nothing of the
                // construction, and the construction counters do not move.
                emit.publishBin(c, slice, bins[c], row);
                row += bins[c];
            } else {
                for (U i = 0; i < bins[c]; ++i)
                    emit(row++, bin_pos_of(bin_word_tail(slice[i]), n, dollar));
            }
            // THE ANCHOR SIDE-STREAM: the SAME ordered slice, read again with
            // stride k. `baseRow` is this bin's first global rank, so the anchor
            // ranks are baseRow's first multiple of k onward - both arms of the
            // emit produce the identical stream because the slice and the rank
            // base are identical.
            if (saAnchor.on()) saAnchor.emitSlice(slice, bins[c], baseRow, n, dollar);
            total.tEmit += now_sec() - t0;
            total.emittedRows += bins[c];
        };
        std::vector<BinWork> work(std::max<U>(1, threads));
        // ---- per-bin attribution for the hot-bin anatomy journal -----------
        // Billed per thread, per byte-1 bin, only so that the largest bin's
        // construction wall and its leaf work can be told apart from the window
        // totals (the operator's `--bin-level-bytes 2` question). Counting these
        // deltas is a handful of adds per task, not per row.
        struct BinStat {
            U leafSorts = 0, leafElems = 0, leafRowsMax = 0, levelElems = 0;
            U symbols = 0;
            U comparisons = 0, topBins = 0;
            double busy = 0;
        };
        const U nw = std::max<U>(1, threads);
        std::vector<std::array<BinStat, 256>> stats(nw);
        std::array<U, 256> subCount{}, subMax{};
        std::array<double, 256> claimT{}, doneT{};
        claimT.fill(0); doneT.fill(0);
        if (threads == 1) {
            for (unsigned c = b0; c < b1; ++c) {
                if (!bins[c]) continue;
                const double t0 = now_sec();
                U* slice = arena.data() + off[c];
                U* scr = scratch.data() + off[c];
                const U m = bins[c];
                U depth = 0;
                std::array<U, 257> count{};
                std::array<U, 258> start{};
                if (topSplit && m > 1) {
                    // Level 2 of the top level, inline: one 257-code byte, then
                    // the sub-bins start one symbol deeper.
                    bin_split1(text, dollar, slice, scr, m, 0, count, start, work[0]);
                    std::memcpy(slice, scr, m * sizeof(U));
                    depth = topSplit;
                    U liveSub = 0;
                    for (U cc = 0; cc < 257; ++cc)
                        if (count[cc]) { ++liveSub; subMax[c] = std::max(subMax[c], count[cc]); }
                    subCount[c] = liveSub;
                    work[0].topBins += liveSub;
                }
                if (!topSplit)
                    bin_build_serial(text, dollar, slice, scr, m, 0, splitSize,
                                     targetSize, depthCap, innerBytes, work[0]);
                else
                    for (U cc = 0; cc < 257; ++cc)
                        if (count[cc])
                            bin_build_serial(text, dollar, slice + start[cc],
                                             scr + start[cc], count[cc], depth,
                                             splitSize, targetSize, depthCap,
                                             innerBytes, work[0]);
                work[0].cpuBuild += now_sec() - t0;
                emitBin(c);
            }
            total.tBuild += work[0].cpuBuild;
        } else {
            // Task queue over BIN SUB-TREES, keyed so the claim order is the
            // emitter's need: with the two-byte top level every 2-gram sub-bin
            // of a byte-1 bin is its own queue unit (key = byte1*257 + rank), so
            // the queue load-balances across all ~65K 2-gram bins while the
            // lowest incomplete byte-1 bin is still always worked first. Small
            // sub-bins batch into one job (up to binBatchSegs segments) to keep
            // the queue's fixed cost off the tiny units.
            constexpr unsigned binBatchSegs = 4;
            struct Shared {
                std::mutex mtx;
                std::condition_variable cv;
                std::multimap<U, BinJob> queue;
                std::unique_ptr<std::atomic<unsigned>[]> pending;
                std::atomic<U> outstanding{0};
                Shared() : pending(new std::atomic<unsigned>[256]) {
                    for (unsigned c = 0; c < 256; ++c) pending[c].store(0);
                }
            } sh;
            std::array<std::atomic<unsigned>, 256> done;
            for (auto& d : done) d.store(0, std::memory_order_relaxed);
            sh.outstanding.store(order.size());
            for (unsigned c : order) {
                sh.pending[c].store(1);
                BinJob job;
                job.key = U(c) * 257u;
                job.bin = c;
                job.depth = 0;
                job.split = topSplit > 0;
                job.segs = 1;
                job.seg[0] = BinSeg{arena.data() + off[c], scratch.data() + off[c],
                                    arena.data() + off[c], bins[c]};
                claimT[c] = now_sec();
                sh.queue.emplace(job.key, job);
            }
            std::vector<std::thread> pool;
            pool.reserve(threads - 1);
            for (U t = 0; t + 1 < threads; ++t)
                pool.emplace_back([&, t] {
                    BinWork& w = work[t];
                    for (;;) {
                        BinJob job;
                        {
                            std::unique_lock<std::mutex> lk(sh.mtx);
                            sh.cv.wait(lk, [&] {
                                return !sh.queue.empty() ||
                                       sh.outstanding.load() == 0;
                            });
                            if (sh.queue.empty()) return;
                            auto it = sh.queue.begin();
                            job = it->second;
                            sh.queue.erase(it);
                        }
                        const U ls0 = w.leafSorts, sy0 = w.symbols, cp0 = w.comparisons;
                        const U lr0 = w.leafRowsMax;
                        // Per-JOB leaf attribution: reset the running max so the
                        // largest leaf THIS job made is what the bin is billed.
                        w.leafRowsMax = 0;
                        const U ll0 = w.leafElems, le0 = w.levelElems, tb0 = w.topBins;
                        const double t0 = now_sec();
                        if (job.split) {
                            // ---- level 2 of the two-byte top level ----------
                            const U* as = job.seg[0].src;   // arena slice
                            U* aw = job.seg[0].canon;       // arena slice, writable
                            U* sc = job.seg[0].dst;         // scratch slice
                            const U m = job.seg[0].m;
                            std::array<U, 257> count{};
                            std::array<U, 258> start{};
                            if (m > 1)
                                bin_split1(text, dollar, as, sc, m, 0, count, start, w);
                            else if (m == 1) sc[0] = as[0];
                            // Dispatch the sub-bins. rank[i] is the i-th claimed
                            // sub-bin: ascending second byte (EDD, the emission
                            // order) or largest first (frequency). Either order
                            // leaves the ARENA layout in byte order - only the
                            // claim order changes - so the sink is untouched.
                            std::array<unsigned, 257> rank{};
                            unsigned live = 0;
                            for (unsigned cc = 0; cc < 257; ++cc)
                                if (count[cc]) rank[live++] = cc;
                            if (freqSched)
                                std::stable_sort(rank.begin(), rank.begin() + live,
                                                 [&](unsigned a, unsigned b) {
                                                     return count[a] > count[b];
                                                 });
                            subCount[job.bin] = live;
                            for (unsigned i = 0; i < live; ++i)
                                subMax[job.bin] = std::max(subMax[job.bin], count[rank[i]]);
                            w.topBins += live;
                            std::lock_guard<std::mutex> lk(sh.mtx);
                            for (unsigned i = 0; i < live;) {
                                BinJob sub;
                                sub.key = U(job.bin) * 257u + i;
                                sub.bin = job.bin;
                                sub.depth = topSplit;
                                sub.split = false;
                                sub.segs = 0;
                                // Batch only units that are small against the
                                // target, so a batch is at most one leaf's worth
                                // of work and the claim order still load-balances.
                                const bool small = count[rank[i]] * binBatchSegs <= targetSize;
                                do {
                                    const unsigned cc = rank[i];
                                    sub.seg[sub.segs++] = BinSeg{sc + start[cc],
                                                                 aw + start[cc],
                                                                 aw + start[cc],
                                                                 count[cc]};
                                    ++i;
                                } while (small && i < live &&
                                         sub.segs < binBatchSegs &&
                                         count[rank[i]] * binBatchSegs <= targetSize);
                                ++sh.pending[job.bin];
                                sh.queue.emplace(sub.key, sub);
                            }
                            sh.cv.notify_all();
                        } else {
                            for (unsigned z = 0; z < job.segs; ++z) {
                                const BinTask tk{job.seg[z].src, job.seg[z].dst,
                                                 job.seg[z].canon, job.seg[z].m,
                                                 job.depth, job.bin, job.key};
                                bin_task_run(text, dollar, tk, splitSize, targetSize,
                                             depthCap, innerBytes, w,
                                             [&](const BinTask& c) {
                                    BinJob child;
                                    child.key = c.key;
                                    child.bin = c.bin;
                                    child.depth = c.depth;
                                    child.split = false;
                                    child.segs = 1;
                                    child.seg[0] = BinSeg{c.src, c.dst, c.canon, c.m};
                                    std::lock_guard<std::mutex> lk(sh.mtx);
                                    ++sh.pending[c.bin];
                                    sh.queue.emplace(child.key, child);
                                    sh.cv.notify_one();
                                });
                            }
                        }
                        const double dt = now_sec() - t0;
                        w.cpuBuild += dt;
                        {
                            BinStat& st = stats[t][job.bin];
                            st.busy += dt;
                            st.leafSorts += w.leafSorts - ls0;
                            st.symbols += w.symbols - sy0;
                            st.comparisons += w.comparisons - cp0;
                            st.leafElems += w.leafElems - ll0;
                            st.leafRowsMax = std::max(st.leafRowsMax, w.leafRowsMax);
                            w.leafRowsMax = std::max(w.leafRowsMax, lr0);
                            st.levelElems += w.levelElems - le0;
                            st.topBins += w.topBins - tb0;
                        }
                        if (--sh.pending[job.bin] == 0) {
                            ++w.builtBins;
                            doneT[job.bin] = now_sec();
                            done[job.bin].store(1, std::memory_order_release);
                            if (--sh.outstanding == 0) {
                                std::lock_guard<std::mutex> lk(sh.mtx);
                                sh.cv.notify_all();
                            }
                        }
                    }
                });
            // Emit in global row order as bins land; construction runs ahead.
            for (unsigned c = b0; c < b1; ++c) {
                if (!bins[c]) continue;
                const double tw = now_sec();
                while (!done[c].load(std::memory_order_acquire))
                    std::this_thread::yield();
                total.tWait += now_sec() - tw;
                emitBin(c);
            }
            for (auto& th : pool) th.join();
            // Build CPU = sum of per-thread busy seconds; the critical path is
            // the slowest thread. absorb() sums cpuBuild, so collapse the sum
            // into one entry and keep the path separately.
            double slowest = 0, cpu = 0;
            for (auto& w : work) {
                slowest = std::max(slowest, w.cpuBuild);
                cpu += w.cpuBuild;
                w.cpuBuild = 0.0;
            }
            work.front().cpuBuild = cpu;
            total.tBuild += slowest;
            // ---- hot-bin anatomy: the window's largest byte-1 bin ----------
            // Journaled apart from the window totals: this is the bin whose
            // leaf sorts decide whether the top split paid off, and its span is
            // a floor on the window's critical path.
            unsigned hot = 256;
            U hotRows = 0;
            for (unsigned c = b0; c < b1; ++c)
                if (bins[c] > hotRows) { hotRows = bins[c]; hot = c; }
            if (hot < 256 && hotRows) {
                BinStat agg{};
                double busyMax = 0;
                for (U t = 0; t < nw; ++t) {
                    const BinStat& st = stats[t][hot];
                    agg.leafSorts += st.leafSorts;
                    agg.leafElems += st.leafElems;
                    agg.leafRowsMax = std::max(agg.leafRowsMax, st.leafRowsMax);
                    agg.levelElems += st.levelElems;
                    agg.symbols += st.symbols;
                    agg.comparisons += st.comparisons;
                    agg.topBins += st.topBins;
                    agg.busy += st.busy;
                    busyMax = std::max(busyMax, st.busy);
                }
                BinWork& hw = work.front();
                if (hotRows > hw.hotRows) {
                    hw.hotRows = hotRows;
                    hw.hotBin = hot;
                    hw.hotTopBins = subCount[hot] ? subCount[hot] : agg.topBins;
                    hw.hotSubMax = subMax[hot];
                    hw.hotLeafSorts = agg.leafSorts;
                    hw.hotLeafElems = agg.leafElems;
                    hw.hotLeafRowsMax = agg.leafRowsMax;
                    hw.hotLevelElems = agg.levelElems;
                    hw.hotSymbols = agg.symbols;
                    hw.hotComparisons = agg.comparisons;
                    hw.hotCpuS = agg.busy;
                    hw.hotThreadS = busyMax;
                    hw.hotSpanS = doneT[hot] > claimT[hot] ? doneT[hot] - claimT[hot] : 0;
                    hw.hotWindows = 1;
                }
            }
        }
        for (auto& w : work) total.absorb(w);
        // The BUILD WORKING SET, which is what has to fit --memory-gb: one
        // arena + scratch slice pair for THIS group, never the whole window.
        const U liveSlots = 2 * live;
        total.peakLiveSlots = std::max<U>(total.peakLiveSlots, liveSlots);
        total.peakLiveBytes = std::max<U>(total.peakLiveBytes, liveSlots * sizeof(U));
    };
    // ======================================================================
    // THE WINDOW PIPELINE: fill ahead by a bounded CREDIT of windows, build
    // behind it, delete each window's file once its bins are built.
    //
    //   filler thread:  window k -> file k            (NT-thread scatter)
    //   builder (this): window k -> bins -> sink      (group by group)
    //
    // Live scratch is therefore O(credit x cap x slot) FILE BYTES and the
    // build's RAM is one group's arena+scratch pair - never the corpus and
    // never the whole window. Window files are named `k % credit`, so the same
    // paths are REUSED across windows (no inode churn) and the header's ordinal
    // is what catches a stale one.
    const U credit = std::max<U>(1, wopt.credit);
    auto winPath = [&](size_t k) {
        return wopt.dir + "/bw-w" + std::to_string(k % credit) + ".pos";
    };
    if (wopt.disc) {
        require(!wopt.dir.empty(), "bin window backing is a FILE but no scratch dir was given");
        std::error_code ec;
        fs::create_directories(wopt.dir, ec);
        require(fs::is_directory(wopt.dir),
                "bin window scratch directory is not usable");
    }
    struct Shard {
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<char> filled;      // 1 = window k's file sealed and readable
        size_t built = 0;              // windows whose build finished (credit gate)
        bool stop = false;
        BinWork fill;                  // the filler's own counters
        U joinedTotal = 0;
        U liveBytes = 0, peakLiveBytes = 0;   // sealed-but-unbuilt window bytes
    } sh;
    sh.filled.assign(plan.size(), 0);
    std::vector<std::unique_ptr<BinArena>> ramArena(plan.size());  // RAM A/B arm

    auto fillerRun = [&]() {
        try {
            for (size_t k = 0; k < plan.size(); ++k) {
                {
                    std::unique_lock<std::mutex> lk(sh.mtx);
                    sh.cv.wait(lk, [&] { return sh.stop || k < sh.built + credit; });
                    if (sh.stop) return;
                }
                BinWork w;
                U joined = 0;
                const double tw0 = now_sec();
                if (wopt.disc) {
                    BinWindow win;
                    win.create(winPath(k), k, plan[k].live, plan[k].b0, plan[k].b1);
                    auto sink = win.fillSink(plan[k].b0, plan[k].b1);
                    joined = fillInto(sink, plan[k].b0, plan[k].b1, winOff[k], w);
                    // Seal: the completion word goes in AFTER every slot, and the
                    // fsync is where the window's write I/O is actually paid.
                    win.seal();
                    win.close();
                    w.tWinWrite += now_sec() - tw0;
                    w.winFileWriteBytes += plan[k].live * bin_window_slot_bytes;
                    ++w.windowFiles;
                } else {
                    std::unique_ptr<BinArena> ra(new BinArena(plan[k].live));
                    BinRamFillSink sink{ra->data()};
                    joined = fillInto(sink, plan[k].b0, plan[k].b1, winOff[k], w);
                    ramArena[k] = std::move(ra);
                }
                {
                    std::lock_guard<std::mutex> lk(sh.mtx);
                    sh.fill.absorb(w);
                    sh.joinedTotal += joined;
                    require(sh.joinedTotal <= joinCount, "bin fill target join overrun");
                    sh.liveBytes += plan[k].live * bin_window_slot_bytes;
                    sh.peakLiveBytes = std::max(sh.peakLiveBytes, sh.liveBytes);
                    sh.filled[k] = 1;
                    // LIVE SCRATCH, PER WINDOW: this is the whole disc footprint
                    // the pipeline is holding right now, and it is bounded by
                    // credit x cap x slot, never by the corpus. `addr_bytes` is
                    // what the window's write path charges the phase's address
                    // space: 0 (pwrite + bounded staging buffers, never a map).
                    std::fprintf(stderr,
                        "BIN_WINDOW index=%llu b0=%u b1=%u live_slots=%llu "
                        "file_bytes=%llu live_scratch_bytes=%llu credit=%llu "
                        "fill_buf_bytes=%llu fill_pwrites=%llu addr_bytes=0\n",
                        (unsigned long long)k, plan[k].b0, plan[k].b1,
                        (unsigned long long)plan[k].live,
                        (unsigned long long)(wopt.disc ? plan[k].live * bin_window_slot_bytes : 0),
                        (unsigned long long)sh.liveBytes, (unsigned long long)credit,
                        (unsigned long long)w.winFillBufBytes,
                        (unsigned long long)w.winFillWrites);
                }
                sh.cv.notify_all();
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(sh.mtx);
            sh.stop = true;
            sh.cv.notify_all();
            throw;
        }
    };
    std::thread filler(fillerRun);
    auto drainFiller = [&]() {
        if (!filler.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(sh.mtx);
            sh.stop = true;
        }
        sh.cv.notify_all();
        filler.join();
    };
    try {
        for (size_t k = 0; k < plan.size(); ++k) {
            const unsigned b0 = plan[k].b0, b1 = plan[k].b1;
            const U live = plan[k].live;
            const std::array<U, 257>& off = winOff[k];
            ++total.windows; total.bins += b1 - b0;
            {
                // The credit gate, on BOTH arms: the build never looks at a
                // window the fill has not sealed.
                std::unique_lock<std::mutex> lk(sh.mtx);
                const double t0 = now_sec();
                sh.cv.wait(lk, [&] { return sh.stop || sh.filled[k]; });
                total.tCreditWait += now_sec() - t0;
                require(sh.filled[k],
                        "bin fill stopped before this window was ready");
            }
            if (fillOnly) {
                // Fill-only probe: the fill and the window write are the whole
                // measurement; nothing is read back, refined or emitted, and no
                // output is written anywhere.
                row += live;
                total.emittedRows += live;
                if (wopt.disc) {
                    bin_window_release_cache(winPath(k));
                    ::unlink(winPath(k).c_str());
                } else {
                    ramArena[k].reset();
                }
                {
                    std::lock_guard<std::mutex> lk(sh.mtx);
                    sh.liveBytes -= live * bin_window_slot_bytes;
                    ++sh.built;
                }
                sh.cv.notify_all();
                if constexpr (bin_publish_sink<Emit>::value) emit.windowDone();
                continue;
            }
            // The build working set: one arena+scratch group. `--bin-build-cap`
            // is the RAM budget in slots; a bin is INDIVISIBLE (its slice is
            // sorted in place), so a single bin larger than the budget RAISES it
            // and says so - the front door treats that as the same
            // misconfiguration BIN_CAP_RAISED is.
            U groupCap = wopt.buildCap ? wopt.buildCap : live;
            if (wopt.buildCap) {
                U winMax = 0;
                for (unsigned c = b0; c < b1; ++c) winMax = std::max(winMax, bins[c]);
                if (winMax > groupCap) {
                    std::fprintf(stderr,
                        "BIN_BUILD_CAP_RAISED requested=%llu largest_bin_in_window=%llu "
                        "using=%llu (a bin slice is sorted in place and cannot be split; "
                        "the RAM working set is 2 x this many 8-byte words)\n",
                        (unsigned long long)wopt.buildCap, (unsigned long long)winMax,
                        (unsigned long long)winMax);
                    groupCap = winMax;
                }
            }
            BinWindow win;
            if (wopt.disc) {
                win.path = winPath(k);
                win.openRead(k, live, b0, b1);
            }
            for (unsigned g0 = b0; g0 < b1;) {
                unsigned g1 = g0;
                U gLive = 0;
                while (g1 < b1 && (g1 == g0 || gLive + bins[g1] <= groupCap)) {
                    gLive += bins[g1]; ++g1;
                }
                require(g1 > g0, "bin window build group admits at least one bin");
                std::array<U, 257> goff{};
                {
                    U sum = 0;
                    for (unsigned c = g0; c < g1; ++c) { goff[c] = sum; sum += bins[c]; }
                    goff[g1] = sum;
                    require(sum == gLive, "bin build group sizing");
                }
                const double tAlloc0 = now_sec();
                BinArena arena(gLive), scratch(gLive);
                total.tAlloc += now_sec() - tAlloc0;
                {
                    // One sequential bulk read per bin, spread over the threads
                    // that are about to refine them (disjoint slices, no lock).
                    const double tr0 = now_sec();
                    const U lw = std::max<U>(1, std::min<U>(threads, U(g1 - g0)));
                    auto loadRun = [&](U z) {
                        for (unsigned c = g0 + unsigned(z); c < g1; c += unsigned(lw))
                            if (bins[c]) {
                                if (wopt.disc)
                                    win.read(off[c], arena.data() + goff[c], bins[c]);
                                else
                                    std::memcpy(arena.data() + goff[c],
                                                ramArena[k]->data() + off[c],
                                                size_t(bins[c] * bin_window_slot_bytes));
                            }
                    };
                    if (lw == 1) loadRun(0);
                    else {
                        std::vector<std::thread> lp;
                        for (U z = 1; z < lw; ++z) lp.emplace_back(loadRun, z);
                        loadRun(0);
                        for (auto& th : lp) th.join();
                    }
                    total.tWinRead += now_sec() - tr0;
                    total.winFileReadBytes += gLive * bin_window_slot_bytes;
                    total.windowLoads += g1 - g0;
                }
                buildGroup(g0, g1, gLive, goff, arena, scratch);
                g0 = g1;
            }
            if (wopt.disc) { win.erase(); }
            else { ramArena[k].reset(); }
            {
                std::lock_guard<std::mutex> lk(sh.mtx);
                sh.liveBytes -= live * bin_window_slot_bytes;
                ++sh.built;
                std::fprintf(stderr,
                    "BIN_WINDOW_DONE index=%llu bins=%u built_windows=%llu "
                    "live_scratch_bytes=%llu\n",
                    (unsigned long long)k, b1 - b0, (unsigned long long)sh.built,
                    (unsigned long long)sh.liveBytes);
            }
            sh.cv.notify_all();
            if constexpr (bin_publish_sink<Emit>::value) emit.windowDone();
        }
    } catch (...) {
        drainFiller();
        for (size_t k = 0; k < plan.size(); ++k)
            if (wopt.disc) ::unlink(winPath(k).c_str());
        throw;
    }
    drainFiller();
    {
        std::lock_guard<std::mutex> lk(sh.mtx);
        total.absorb(sh.fill);
        total.peakWindowFileBytes = std::max(total.peakWindowFileBytes, sh.peakLiveBytes);
        if (sh.joinedTotal != joinCount && !fillOnly)
            require(false, "bin fill target join coverage");
        require(sh.joinedTotal <= joinCount, "bin fill target join overrun");
    }
    require(row == n, "bin-bwt row coverage");
    saAnchor.finish(total);
}

// ------------------------------------------ bin-bwt v2: the parallel sink
// Byte for byte the SequentialV2 output of v1 (the same BWT run stream, the
// same run bands, pieces, BPG1 progress records, meta and terminal marker),
// produced without the serial per-row sink term:
//
//  * no per-row corpus read and no per-row hash probe. The fill pass already
//    wrote every row's BWT symbol M[p-1] into the parallel symbol column and
//    merge-joined the capture targets to their rows, so a row here costs one
//    sequential byte read (binbwt_slice.h, the fill pass);
//  * the ordered pass (ONE thread, sequential memory) does only the work a run
//    index requires: coalesce adjacent equal symbols into runs, number the runs
//    (a prefix sum), close each 65536-run band, and build that band's progress
//    record. That is O(rows) streaming plus O(runs) bookkeeping instead of
//    172.6 ns/row of random-latency sink work;
//  * P concurrent writer threads then publish the bands out of order - piece
//    (rename), .rlebwt extent at that band's run-boundary offset (pwrite),
//    then progress record (rename) - the sharded-writers-at-run-boundaries
//    shape the merge header documents, over the transport that already
//    tolerates P out-of-order producers. The wire does not change; 472
//    adversarial schedules are modelled in tools/binbwt/model_sink2.py.
//
// The class progress record is the other term worth naming: v1 rescanned every
// class ever captured once per band (2e9 tests at 1 GB; at pile 3.6e15 - i.e.
// most of the projected 62.8 h was THIS, not the rows). firstRun is
// non-decreasing in class id and lastRun only ever moves from MAX to its final
// value, so a monotone lower bound plus the head test yields the identical
// reference set in O(classes + refs).
struct BinBandSink {
    struct Class { CapturedSeamClass value; U firstRun = 0, lastRun = UINT64_MAX; };
    // A run is ONE record. Four separate vectors meant four push_backs per run
    // (chars, lens, heads, tails), each growing independently: measured 60 ns per
    // ROW in the publish loop, which was 100% of publish's 1,024 s at 10 GB with
    // slot_wait at 0.00 s. One 24-byte record is one append.
    struct BandRun {
        uint8_t c;
        uint32_t len;
        U head, tail;
    };
    struct Band {
        std::vector<BandRun> runs;
        U size() const { return runs.size(); }
    };
    struct BandJob {
        U band = 0, firstRun = 0, words = 0, rlebwtOff = 0;
        std::vector<BandRun> runs;
        std::vector<char> gmark;
    };
    // ---- fixed shape -----------------------------------------------------
    const CorpusText* text = nullptr;
    SparseSeamCapture seam;
    std::unordered_map<U, U> firstSizes;
    std::vector<U> targets;             // ascending capture-target positions
    U n = 0, window = 0, width = 0;
    bool dollar = true;
    std::string prefix, dir;
    // ---- ordered pass state ---------------------------------------------
    U next = 0, rows = 0, index = 0, bandStart = 0, wordsDone = 0, bandWords = 0;
    U prevLastPos = 0;                  // position of the previous row; n+9 first
    SparseSeamCapture::Hit* boundaryHit = nullptr;   // a hit awaiting its `after`
    SparseRun pending;                  // the BWT run being extended
    // ---- RUN RANKS BY PREFIX SUM -------------------------------------------
    // xsa_finish's Kasai arm needs the RANK of each run's head and tail POSITION.
    // A full ISA is 10.5 TB at pile (80 GB at 10 GB) and breaks the 2n law, but
    // ranks are MONOTONIC in run order, so the finish carries a counter and
    // prefix-sums the run lengths. The only exceptions are "holes" left by the
    // class/dollar repair, and THIS sink knows the true ranks (the row index when
    // the run opened and closed) - so it records exactly those deviations.
    U pendHeadRank = 0;                 // row index where the pending run opened
    U bandBaseRank = 0, bandCumLen = 0; // band's first row index / run-length prefix
    U excRuns = 0;
    std::vector<U> runExc;              // flat triples (run, head rank, tail rank)
    std::array<U, 256> counts{}, frequencies{};
    std::vector<Class> classes;
    U active = UINT64_MAX, publishedClasses = 0, refFrom = 0;
    Band band;
    // ---- writer pool -----------------------------------------------------
    std::mutex mu;
    std::condition_variable cvJob, cvSlot;
    std::deque<BandJob> jobs;
    std::vector<std::thread> writers;
    U slotsFree = 0, writersUsed = 0;
    bool stop = false;
    double tWrite = 0, tFlow = 0;
    int rleFd = -1;
    BinWork own;                        // the sink's OWN work, journalled apart

    BinBandSink(const std::string& p, const CorpusText& t, U inputRuns, bool dlr,
                U threadCount)
        : text(&t), n(t.n), window(v2_piece_window()),
          width(std::max<unsigned>(1, 64 - __builtin_clzll(t.n + 9))),
          dollar(dlr), prefix(p), dir(p + ".pieces") {
        require(::mkdir(dir.c_str(), 0777) == 0, "create band pieces directory");
        seam.discover(t, n, std::min<U>(1u << 26, std::max<U>(1000, inputRuns / 1000)));
        for (const auto& cl : seam.raw) firstSizes.emplace(cl.first, cl.hi);
        targets.reserve(seam.targets.size());
        for (const auto& entry : seam.targets) targets.push_back(entry.first);
        std::sort(targets.begin(), targets.end());
        require(targets.size() == seam.targets.size(), "bin-bwt target list");
        {
            OutFile f(dir + "/start.tmp"); f.raw("BPS1", 4);
            f.w64(n); f.w64(width); f.w64(seam.depth); f.w64(window);
        }
        require(::rename((dir + "/start.tmp").c_str(), (dir + "/start").c_str()) == 0,
                "publish band stream start");
        prevLastPos = n + 9;            // seam.previous: the flank before row 0
        require(!fs::exists(prefix + ".rlebwt"), "band rlebwt exists; refusing to clobber");
        rleFd = ::open((prefix + ".rlebwt").c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
        require(rleFd >= 0, "create band rlebwt");
        // THE PUBLISH BOTTLENECK: 8 writers for ~53,000 band files at 10 GB,
        // with publish blocking on only 16 slots. Measured: publish 852-1034 s
        // of a 1161-1271 s emit, i.e. syscall-bound, not compute-bound (a
        // per-row target bitmap made it WORSE, which is how this was found).
        writersUsed = std::min<U>(32, std::max<U>(1, threadCount / 3));
        slotsFree = std::max<U>(2, 2 * writersUsed);
        if (slotsFree > window) slotsFree = window;
        require(slotsFree >= 1 && writersUsed >= 1, "band writer pool");
        for (U i = 0; i < writersUsed; ++i) writers.emplace_back([this] { writerLoop(); });
        // The padding rows, seeded exactly as SequentialV2 seeds them: the first
        // pad row carries M[n-1], the next nine the padding symbol 2.
        addRun(SparseRun{uint8_t(t.at(n - 1)), 1, n, n});
        addRun(SparseRun{2, 9, n + 1, n + 9});
    }
    ~BinBandSink() {
        stop = true;
        cvJob.notify_all();
        for (auto& th : writers) if (th.joinable()) th.join();
        if (rleFd >= 0) ::close(rleFd);
    }
    const std::vector<U>& joinTargets() const { return targets; }
    void windowDone() {}
    U runs() const { return index; }

    // ---- ordered publication -------------------------------------------
    // One sequential pass over the bin's sorted arena words. Every row costs an
    // 8-byte streaming read plus part of a run; the position, the symbol and the
    // capture-target flag are all already in the word (bin_word), so this pass
    // never reads the corpus at random and never hashes a row.
    void publishBin(unsigned bin, const U* words, U m, U baseRow) {
        (void)bin;
        require(baseRow == next, "bin-bwt sink row coverage");
        const double t0 = now_sec();
        const double trow0 = t0;
        auto posOf = [&](U i) { return bin_pos_of(bin_word_tail(words[i]), n, dollar); };
        for (U i = 0; i < m; ++i) {
            const U row = baseRow + i;
            const U word = words[i];
            const uint8_t c = bin_word_sym(word);
            const bool hit = bin_word_hit(word);
            // This row's position, computed only when something needs it: a
            // previous row's seam flank, this row's own hit, or a class close.
            U p = 0;
            bool haveP = false;
            if (boundaryHit) {                     // the previous row was a hit
                p = posOf(i); haveP = true;
                boundaryHit->after = p;
                boundaryHit = nullptr;
            }
            if (hit) {
                const U hp = haveP ? p : posOf(i);
                ++own.targetHits;
                auto foundTarget = seam.targets.find(hp);
                require(foundTarget != seam.targets.end(),
                        "bin-bwt hit is not a sparse seam target");
                SparseSeamCapture::Hit& target = foundTarget->second;
                require(target.row == UINT64_MAX, "duplicate sparse seam target");
                target.before = i ? posOf(i - 1) : prevLastPos;
                target.row = row; target.after = n;
                boundaryHit = &target;
            }
            // A class closes on the row after its last target; its `after`
            // flank is this row's position and its `lastRun` is the run that
            // holds the previous row (pending, not yet numbered).
            if (active != UINT64_MAX && row == classes[active].value.hi) {
                auto& cl = classes[active];
                cl.value.after = haveP ? p : posOf(i);
                cl.lastRun = index;
                active = UINT64_MAX;
            }
            if (pending.len && pending.c != c) flushRun();
            if (hit) {
                const U hp = posOf(i);
                auto found = firstSizes.find(hp);
                if (found != firstSizes.end()) {
                    if (active == UINT64_MAX) {
                        Class cl;
                        cl.firstRun = index;
                        cl.value.lo = row; cl.value.hi = row + found->second;
                        cl.value.first = hp;
                        cl.value.before = seam.targets.at(hp).before;
                        require(classes.empty() || classes.back().firstRun <= cl.firstRun,
                                "bin-bwt class run order");
                        classes.push_back(std::move(cl));
                        active = classes.size() - 1;
                    } else require(row + found->second <= classes[active].value.hi,
                                   "crossing sequential seam class");
                }
                if (active != UINT64_MAX) classes[active].value.occ.push_back(hp);
            }
            if (!pending.len) {
                pending = SparseRun{c, 1, posOf(i), posOf(i)};
                pendHeadRank = row;
            }
            else pending.tail = posOf(i), ++pending.len;
            ++next; ++rows;
        }
        if (m) prevLastPos = posOf(m - 1);
        const double trow = now_sec() - trow0;
        own.tStitch += trow;
    }
    void addRun(const SparseRun& run) {
        if (pending.len && pending.c != run.c) flushRun();
        if (!pending.len) pending = run;
        else { pending.len += run.len; pending.tail = run.tail; }
        rows += run.len;
    }
    void flushRun() {
        if (!pending.len) return;
        {
            const U predicted = bandBaseRank + bandCumLen;
            if (predicted != pendHeadRank) {
                // KEYED BY HEAD POSITION, not run index: the finish's runs are
                // POST-REPAIR (RepairStream merges/splits downstream of this sink),
                // so a run index does not survive the repair - a position does.
                runExc.push_back(pending.head);
                runExc.push_back(pendHeadRank);
                runExc.push_back(pendHeadRank + pending.len - 1);
                ++excRuns;
            }
            bandCumLen += pending.len;
        }
        counts[pending.c] += pending.len; ++frequencies[pending.c];
        require(pending.len == U(uint32_t(pending.len)),
                "bin-bwt run length exceeds the piece length field");
        band.runs.push_back(BandRun{pending.c, uint32_t(pending.len), pending.head, pending.tail});
        bandWords += (pending.len + 0x7fffff - 1) / 0x7fffff;
        ++index; ++own.runsPublished;
        pending = SparseRun{};
        if (index % 65536 == 0) closeBand();
    }
    static void push64(std::vector<char>& b, U v) {
        for (int i = 0; i < 8; ++i) b.push_back(char(v >> (8 * i)));
    }
    void closeBand() {
        const U count = band.size();
        require(count && count <= 65536, "bin-bwt band count");
        const U bandNo = bandStart / 65536;
        require(bandStart == bandNo * 65536, "bin-bwt band alignment");
        require(bandStart + count == index, "bin-bwt band run coverage");
        if (active != UINT64_MAX)
            require(bandNo + 1 - classes[active].firstRun / 65536 < window,
                    "active seam class exceeds W-1 run bands");
        // Class references for this band, exactly the set the serial sink's
        // O(classes) scan produces, in O(classes + refs) total.
        while (refFrom < classes.size()) {
            const auto& cl = classes[refFrom];
            if (cl.lastRun == UINT64_MAX || cl.lastRun >= bandStart) break;
            ++refFrom;
        }
        std::vector<U> refs;
        for (U id = refFrom; id < classes.size(); ++id) {
            const auto& cl = classes[id];
            if (cl.firstRun >= bandStart + count) break;
            if (cl.lastRun == UINT64_MAX || cl.lastRun >= bandStart) refs.push_back(id);
        }
        const U complete = classes.size() - (active != UINT64_MAX ? 1u : 0u);
        std::vector<char> gmark;
        gmark.reserve(64 + 8 * refs.size() + 8 * 8 * (complete - publishedClasses));
        gmark.insert(gmark.end(), {'B', 'P', 'G', '1'});
        push64(gmark, bandNo); push64(gmark, count);
        push64(gmark, refs.size()); push64(gmark, complete - publishedClasses);
        for (U id : refs) push64(gmark, id);
        for (U id = publishedClasses; id < complete; ++id) {
            const auto& cl = classes[id]; const auto& v = cl.value;
            push64(gmark, id); push64(gmark, cl.firstRun); push64(gmark, cl.lastRun);
            push64(gmark, v.lo); push64(gmark, v.hi); push64(gmark, v.first);
            push64(gmark, v.before); push64(gmark, v.after);
            for (U position : v.occ) push64(gmark, position);
        }
        publishedClasses = complete;
        BandJob job;
        job.band = bandNo; job.firstRun = bandStart;
        job.words = bandWords; job.rlebwtOff = wordsDone;
        job.runs.swap(band.runs);
        bandBaseRank = next;      // the next row index starts the next band
        bandCumLen = 0;
        job.gmark = std::move(gmark);
        band = Band();
        {   // bounded in-flight bands: one slot per queued or running band
            const double tw0 = now_sec();
            std::unique_lock<std::mutex> lk(mu);
            cvSlot.wait(lk, [&] { return slotsFree > 0; });
            --slotsFree;
            own.tSlotWait += now_sec() - tw0;
        }
        // The transport's flow credit, unchanged (v2_flow_wait): band bandNo is
        // published only while the consumer is within `window` bands of it.
        const double t0 = now_sec();
        v2_flow_wait(prefix, bandNo, window);
        tFlow += now_sec() - t0;
        wordsDone += bandWords; bandWords = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            jobs.push_back(std::move(job));
            ++own.bandJobs;
        }
        cvJob.notify_one();
        bandStart = index;
        ++own.bands;
    }
    // One writer thread: piece, then the run-boundary extent of the BWT run
    // stream, then the progress record that makes the band visible.
    void writerLoop() {
        for (;;) {
            BandJob job;
            {
                std::unique_lock<std::mutex> lk(mu);
                cvJob.wait(lk, [&] { return stop || !jobs.empty(); });
                if (jobs.empty()) { require(stop, "band writer wake without work"); return; }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            const double t0 = now_sec();
            const U rc = job.runs.size();
            std::vector<uint8_t> jchars(rc);
            std::vector<uint32_t> jlens(rc);
            std::vector<U> jheads(rc), jtails(rc);
            for (U i = 0; i < rc; ++i) {
                jchars[i] = job.runs[i].c;
                jlens[i] = job.runs[i].len;
                jheads[i] = job.runs[i].head;
                jtails[i] = job.runs[i].tail;
            }
            v2_write_piece(dir, job.band, 0, job.firstRun, jchars.data(),
                           jlens.data(), jheads.data(), jtails.data(), rc,
                           width);
            std::vector<uint32_t> w32;
            w32.reserve(rc + 16);
            for (U i = 0; i < rc; ++i) {
                const uint32_t c = job.runs[i].c;
                U left = job.runs[i].len;
                require(left, "bin-bwt band run length");
                while (left) {
                    U take = std::min<U>(left, 0x7fffff); left -= take;
                    w32.push_back(c | uint32_t(take << 8) | (left ? 0x80000000u : 0u));
                }
            }
            require(w32.size() == job.words, "bin-bwt band rlebwt word count");
            xput(rleFd, w32.data(), 4 * w32.size(), 4 * job.rlebwtOff, "band rlebwt extent");
            require(job.gmark.size() >= 36, "band progress payload");
            const std::string tmp = dir + "/g-tmp-" + std::to_string(job.band);
            const std::string fin = dir + "/g-" + std::to_string(job.band);
            require(!fs::exists(fin), "band progress exists; refusing to clobber");
            int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            require(fd >= 0, "create band progress");
            xput(fd, job.gmark.data(), job.gmark.size(), 0, "band progress write");
            require(::close(fd) == 0, "close band progress");
            require(::rename(tmp.c_str(), fin.c_str()) == 0, "publish band progress");
            {
                std::lock_guard<std::mutex> lk(mu);
                tWrite += now_sec() - t0;
                ++slotsFree;
            }
            cvSlot.notify_one();
        }
    }
    // ---- terminal, exactly SequentialV2::endShard's validations ---------
    // The run-rank exception list: u32 magic 'SXRE', u32 version, u64 count, then
    // count triples (run index, true head rank, true tail rank).
    U writeRunExc(const std::string& path) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        require(fd >= 0, "create run-rank exception list");
        uint64_t h[2] = {0x45525853ull, U(runExc.size() / 3)};
        xput(fd, h, 16, 0, "runexc header");
        if (!runExc.empty())
            xput(fd, runExc.data(), runExc.size() * sizeof(U), 16, "runexc body");
        require(::fsync(fd) == 0, "sync runexc");
        ::close(fd);
        return U(runExc.size() / 3);
    }
    void finish() {
        require(next == n, "incomplete bin-bwt walk");
        if (active != UINT64_MAX) {
            auto& cl = classes[active];
            require(cl.value.hi == n, "incomplete final class");
            cl.value.after = n; cl.lastRun = index; active = UINT64_MAX;
        }
        seam.nextRow = n;               // the seam's own row accounting
        auto expected = seam.finish();
        require(expected.size() == classes.size(), "bin-bwt class count");
        for (U i = 0; i < expected.size(); ++i)
            require(expected[i] == classes[i].value, "bin-bwt class capture mismatch");
        flushRun();
        if (band.size()) closeBand();
        require(publishedClasses == classes.size() && rows == n + 10 && counts[2] == 10,
                "bin-bwt terminal coverage");
        const U limit = std::max<U>(1000, index / 1000);
        U targetRows = 0; for (const auto& cl : classes) targetRows += cl.value.occ.size();
        require(targetRows <= limit && seam.depth <= limit, "bin-bwt repair budget");
        for (const auto& cl : classes)
            require(cl.lastRun / 65536 - cl.firstRun / 65536 < window,
                    "class exceeds W-1 bands");
        {   // O5: no terminal marker and no meta before every band is published
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
        }
        cvJob.notify_all();
        for (auto& th : writers) th.join();
        writers.clear();
        require(::close(rleFd) == 0, "close band rlebwt");
        rleFd = -1;
        own.tBandWrite = tWrite;
        own.tFlowWait = tFlow;
        {
            OutFile f(prefix + ".rlebwt.meta");
            f.w64(n + 10); f.w64(index);
            for (U value : counts) f.w64(value);
            for (U value : frequencies) f.w64(value);
        }
        {
            OutFile f(dir + "/terminal.tmp"); f.raw("BPT1", 4);
            f.w64((index - 1) / 65536); f.w64(index); f.w64(targetRows);
            f.w64(classes.size());
        }
        require(::rename((dir + "/terminal.tmp").c_str(), (dir + "/terminal").c_str()) == 0,
                "publish validated terminal marker");
    }
};

// The BIN_WORK journal, shared by the producer (with a sink) and by
// `--bin-probe` (construction only, no output at all), so a 1-byte and a 2-byte
// top level diff line for line. Every counter kept anywhere above appears here.
struct BinJournal {
    const char* sink = "parallel-v2";
    U serialRuns = 0, writers = 0;
    double tSinkOpen = 0, tOrder = 0, tEnds = 0;
};
static void bin_journal(const BinWork& total, bool dollar, U threads,
                        U levelBytes, U targetSize, U depthCap, bool freqSched,
                        U splitSize, double wall, const BinJournal& j,
                        const BinEntropyOpts& ent = BinEntropyOpts()) {
    std::fprintf(stderr,
        "BIN_WORK dollar=%u threads=%llu level_bytes=%llu target=%llu depth_cap=%llu "
        "freq_sched=%u split=%llu bins=256 top_bins=%llu windows=%llu "
        "sweep_bytes=%llu sweep_passes=%llu level_passes=%llu level_elems=%llu "
        "bin_passes=%llu comparisons=%llu symbols=%llu cert_decided=%llu "
        "ties=%llu leaf_sorts=%llu leaf_elems=%llu peak_live_slots=%llu "
        "peak_live_bytes=%llu emitted_rows=%llu sweep_s=%.3f build_s=%.3f "
        "emit_s=%.3f total_s=%.3f build_cpu_s=%.3f ns_per_symbol_thread=%.3f "
        "ns_per_comparison_thread=%.3f sink_open_s=%.3f order_s=%.3f "
        "endshard_s=%.3f alloc_s=%.3f wait_s=%.3f count_s=%.3f fill_s=%.3f "
        "sink=%s serial_runs=%llu runs=%llu bands=%llu target_hits=%llu "
        "band_jobs=%llu writers=%llu stitch_s=%.3f band_write_s=%.3f "
        "flow_wait_s=%.3f ns_per_row_stitch=%.3f "
        "win_file_bytes=%llu win_file_read_bytes=%llu peak_window_file_bytes=%llu "
        "window_files=%llu window_loads=%llu win_write_s=%.3f win_read_s=%.3f "
        "win_fill_buf_bytes=%llu win_fill_pwrites=%llu "
        "credit_wait_s=%.3f "
        "depth_max=%llu leaf_rows_max=%llu over_target_leaves=%llu "
        "over_target_rows=%llu structural_leaves=%llu structural_rows=%llu "
        "no_progress_levels=%llu periodic_leaves=%llu periodic_rows=%llu "
        "periodic_period_max=%llu sanity_bound=%llu leaf_s=%.3f "
        "stage_budget_bytes=%llu stage_leaves=%llu stage_members=%llu "
        "stage_bytes=%llu stage_hits=%llu stage_partials=%llu stage_misses=%llu "
        "stage_raw_windows=%llu stage_merged_reads=%llu stage_segs_max=%llu "
        "stage_avg_read_bytes=%.1f stage_skipped_ram=%llu stage_s=%.3f "
        "full_SA=0 persistent_positions=0 "
        "sa_sample_k=%llu sa_samples=%llu sa_sample_bytes=%llu sa_sample_s=%.3f "
        "entropy_armed=%u entropy_min_bits=%.3f entropy_leaf_min_bits=%.3f "
        "entropy_evals=%llu entropy_taken=%llu entropy_exits=%llu "
        "entropy_exit_rows=%llu entropy_noprog_exits=%llu "
        "entropy_leaf_evals=%llu entropy_leaf_taken=%llu entropy_leaf_exits=%llu "
        "entropy_leaf_rows=%llu entropy_over_target_leaves=%llu "
        "entropy_over_target_rows=%llu\n",
        unsigned(dollar), (unsigned long long)threads,
        (unsigned long long)levelBytes, (unsigned long long)targetSize,
        (unsigned long long)depthCap, unsigned(freqSched),
        (unsigned long long)splitSize,
        (unsigned long long)total.topBins,
        (unsigned long long)total.windows,
        (unsigned long long)total.sweepBytes, (unsigned long long)total.sweepPasses,
        (unsigned long long)total.levelPasses, (unsigned long long)total.levelElems,
        (unsigned long long)total.binPasses, (unsigned long long)total.comparisons,
        (unsigned long long)total.symbols, (unsigned long long)total.certDecided,
        (unsigned long long)total.ties, (unsigned long long)total.leafSorts,
        (unsigned long long)total.leafElems, (unsigned long long)total.peakLiveSlots,
        (unsigned long long)total.peakLiveBytes, (unsigned long long)total.emittedRows,
        total.tSweep, total.tBuild, total.tEmit, wall, total.cpuBuild,
        total.symbols ? 1e9 * total.cpuBuild / double(total.symbols) : 0.0,
        total.comparisons ? 1e9 * total.cpuBuild / double(total.comparisons) : 0.0,
        j.tSinkOpen, j.tOrder, j.tEnds, total.tAlloc,
        total.tWait, total.tCount, total.tFill,
        j.sink, (unsigned long long)j.serialRuns,
        (unsigned long long)total.runsPublished,
        (unsigned long long)total.bands, (unsigned long long)total.targetHits,
        (unsigned long long)total.bandJobs, (unsigned long long)j.writers,
        total.tStitch, total.tBandWrite, total.tFlowWait,
        total.emittedRows ? 1e9 * total.tStitch / double(total.emittedRows) : 0.0,
        (unsigned long long)total.winFileWriteBytes,
        (unsigned long long)total.winFileReadBytes,
        (unsigned long long)total.peakWindowFileBytes,
        (unsigned long long)total.windowFiles,
        (unsigned long long)total.windowLoads,
        total.tWinWrite, total.tWinRead,
        (unsigned long long)total.winFillBufBytes,
        (unsigned long long)total.winFillWrites,
        total.tCreditWait,
        (unsigned long long)total.depthMax,
        (unsigned long long)total.leafRowsMax,
        (unsigned long long)total.overTargetLeaves,
        (unsigned long long)total.overTargetRows,
        (unsigned long long)total.structuralLeaves,
        (unsigned long long)total.structuralRows,
        (unsigned long long)total.noProgressLevels,
        (unsigned long long)total.periodicLeaves,
        (unsigned long long)total.periodicRows,
        (unsigned long long)total.periodicPeriodMax,
        (unsigned long long)bin_no_progress_sanity_bound, total.tLeaf,
        (unsigned long long)bin_leaf_stage_budget(),
        (unsigned long long)total.stageLeaves,
        (unsigned long long)total.stageMembers,
        (unsigned long long)total.stageBytes,
        (unsigned long long)total.stageHits,
        (unsigned long long)total.stagePartials,
        (unsigned long long)total.stageMisses,
        (unsigned long long)total.stageRawWindows,
        (unsigned long long)total.stageMergedReads,
        (unsigned long long)bin_leaf_stage_segments(),
        total.stageMergedReads
            ? double(total.stageBytes) / double(total.stageMergedReads) : 0.0,
        (unsigned long long)total.stageSkippedRam, total.tStage,
        (unsigned long long)total.saSampleK, (unsigned long long)total.saSamples,
        (unsigned long long)total.saSampleBytes, total.tSaSample,
        unsigned(ent.armed), ent.minBits, ent.leafMinBits,
        (unsigned long long)total.entropyEvals,
        (unsigned long long)total.entropyTaken,
        (unsigned long long)total.entropyExits,
        (unsigned long long)total.entropyExitRows,
        (unsigned long long)total.entropyNoProgExits,
        (unsigned long long)total.entropyLeafEvals,
        (unsigned long long)total.entropyLeafTaken,
        (unsigned long long)total.entropyLeafExits,
        (unsigned long long)total.entropyLeafRows,
        (unsigned long long)total.entropyOverTargetLeaves,
        (unsigned long long)total.entropyOverTargetRows);
    // The largest byte-1 bin of the largest window, alone: the hot-bin anatomy
    // the operator asked for (`--bin-level-bytes 1` vs `2` on the SAME corpus).
    std::fprintf(stderr,
        "BIN_HOT windows_with_anatomy=%llu bin=%llu rows=%llu top_bins=%llu "
        "sub_max=%llu span_s=%.3f cpu_s=%.3f thread_max_s=%.3f leaf_sorts=%llu "
        "leaf_elems=%llu leaf_rows_max=%llu level_elems=%llu symbols=%llu "
        "comparisons=%llu\n",
        (unsigned long long)total.hotWindows, (unsigned long long)total.hotBin,
        (unsigned long long)total.hotRows, (unsigned long long)total.hotTopBins,
        (unsigned long long)total.hotSubMax, total.hotSpanS, total.hotCpuS,
        total.hotThreadS, (unsigned long long)total.hotLeafSorts,
        (unsigned long long)total.hotLeafElems,
        (unsigned long long)total.hotLeafRowsMax, (unsigned long long)total.hotLevelElems,
        (unsigned long long)total.hotSymbols, (unsigned long long)total.hotComparisons);
    // THE ENTROPY RULE's H distribution by depth (ENTROPY_RULE_DESIGN §7). Its
    // own line for the same reason PHRASE_WORK has one: BIN_WORK's field set is
    // parsed by the gate scripts and must not grow shape. Printed when the rule
    // is armed, or when BIN_ENTROPY_H=1 asks for it explicitly (so an unarmed run
    // can attest that it took no measurement at all).
    if (ent.armed || ::getenv("BIN_ENTROPY_H")) {
        for (U d = 0; d < bin_entropy_depth_slots; ++d) {
            const BinWork::BinHEnt& e = total.hDepth[d];
            if (!e.n) continue;
            std::fprintf(stderr,
                "BIN_ENTROPY_H depth=%llu evals=%llu h_min=%.4f h_mean=%.4f "
                "h_max=%.4f%s\n",
                (unsigned long long)d, (unsigned long long)e.n, e.mn, e.sum / double(e.n),
                e.mx,
                (d + 1 == bin_entropy_depth_slots && total.hDepthOver)
                    ? " (last slot folds every deeper evaluation)" : "");
        }
        std::fprintf(stderr,
            "BIN_ENTROPY_H_TOTAL depth_slots=%llu evals_past_last=%llu "
            "min_bits=%.3f leaf_min_bits=%.3f leaf_floor=%llu\n",
            (unsigned long long)bin_entropy_depth_slots,
            (unsigned long long)total.hDepthOver, ent.minBits, ent.leafMinBits,
            (unsigned long long)ent.leafFloor);
    }
}

// PHRASE_WORK: the census of the OTHER regime, next to BIN_WORK's own. A
// separate line on purpose - BIN_WORK's field set is parsed by the gate scripts
// and by the census diffs, so it must not grow shape.
static void bin_phrase_journal_work(const BinWork& w, const PhraseDict* pd) {
    const bool on = pd && pd->armed();
    std::fprintf(stderr,
        "PHRASE_WORK armed=%u probe_depth=%llu probe_stride=%llu skip_min_rows=%llu "
        "probes=%llu probe_rows=%llu skips=%llu skip_rows=%llu "
        "skip_bytes=%llu skip_max_len=%llu skip_depth_max=%llu verify_fails=%llu "
        "refused_no_entry=%llu refused_not_uniform=%llu "
        "no_progress_levels=%llu level_passes=%llu level_elems=%llu "
        "comparisons=%llu symbols=%llu full_SA=0 persistent_positions=0\n",
        unsigned(on), (unsigned long long)(pd ? pd->o.probeDepth : 0),
        (unsigned long long)(pd ? pd->o.probeStride : 0),
        (unsigned long long)(pd ? pd->o.skipMinRows : 0),
        (unsigned long long)w.phraseProbes,
        (unsigned long long)w.phraseProbeRows, (unsigned long long)w.phraseSkips,
        (unsigned long long)w.phraseSkipRows, (unsigned long long)w.phraseSkipBytes,
        (unsigned long long)w.phraseSkipMaxLen,
        (unsigned long long)w.phraseSkipDepthMax,
        (unsigned long long)w.phraseVerifyFails,
        (unsigned long long)w.phraseProbeNoEnt,
        (unsigned long long)w.phraseProbeDis,
        (unsigned long long)w.noProgressLevels,
        (unsigned long long)w.levelPasses, (unsigned long long)w.levelElems,
        (unsigned long long)w.comparisons, (unsigned long long)w.symbols);
}

// ---- producer entry point: the parallel sink publishes the concatenated bins
// ============================ THE TAPE-EMIT PATH ==========================
// Emit the product's band stream from an EXTERNAL SA stream (the tape machine's
// `scatter_sort` output) instead of from the bin tree. Everything downstream -
// the sink, the band pieces, the .rlebwt extents, the progress records, the
// padding rows, the seam classes - is the EXISTING emission code, untouched:
// this only replaces "where the rank-ordered rows come from".
//
// Per row the sink needs a packed word: (tail = pos+1, sym = the preceding byte,
// hit = a sparse seam target). tail and sym are both cheap; `hit` is a
// membership test against the sink's own (small, ascending) target list, and the
// preceding byte is a single read at pos-1. At pile scale both want to move into
// scatter_sort's sequential pass (which already stands on every position); this
// driver takes the simple correct form first and MEASURES it, so the cost of the
// simple form is known rather than assumed.
static void tape_emit(const std::string& prefix, CorpusText& text, bool dollar,
                      U threads, const std::string& saPath, U chunk) {
    const U n = text.n;
    std::unique_ptr<BinBandSink> sink(new BinBandSink(prefix, text, n, dollar, threads));
    // Sparse membership over the seam targets: a few thousand positions. A tiny
    // open-addressed table (power-of-two, linear probe) keeps the per-row test at
    // one probe instead of a 12-deep binary search; this header deliberately does
    // not include <unordered_set>, and at pile scale the per-row test is 1.3e12
    // of them.
    std::vector<U> tgt(sink->joinTargets().begin(), sink->joinTargets().end());
    U tcap = 16;
    while (tcap < tgt.size() * 4 + 16) tcap <<= 1;
    std::vector<U> tslot(tcap, 0);          // 0 = empty (a target is never position 0 plus 1)
    auto thash = [](U x) { return (x * 0x9e3779b97f4a7c15ull) >> 40; };
    for (U t : tgt) {
        U i = thash(t + 1) & (tcap - 1);
        while (tslot[i]) i = (i + 1) & (tcap - 1);
        tslot[i] = t + 1;
    }
    std::vector<uint64_t> tblock((n >> 6) + 2, 0);
    for (U t : tgt) tblock[t >> 6] |= (1ull << (t & 63));
    auto maybeTarget = [&](U p) {
        return tgt.empty() ? false : ((tblock[p >> 6] >> (p & 63)) & 1ull) != 0;
    };
    auto isTarget = [&](U p) {
        if (tgt.empty()) return false;
        U i = thash(p + 1) & (tcap - 1);
        for (;;) {
            const U v = tslot[i];
            if (!v) return false;
            if (v == p + 1) return true;
            i = (i + 1) & (tcap - 1);
        }
    };
    std::fprintf(stderr, "TAPE_EMIT_START n=%llu targets=%zu chunk=%llu prefix=%s\n",
                 (unsigned long long)n, tgt.size(), (unsigned long long)chunk,
                 prefix.c_str());
    const double t0 = now_sec();
    int fd = ::open(saPath.c_str(), O_RDONLY);
    require(fd >= 0, "tape-emit: cannot open the SA stream");
    struct stat st{};
    require(::fstat(fd, &st) == 0, "tape-emit: stat SA stream");
    // THE SAMPLED-SA ANCHOR HEADER ('SXSA', version 1): magic at 0, version at 4,
    // n at 8, k at 16, count at 24, rows at 32, pad at 40 - the layout the product's
    // own --sa-sample-k writes and kasai_column requires. Reading it as six u64s
    // with n at index 2 was the OLD private layout and refused every stream
    // scatter_sort now produces.
    U hdr[6] = {0, 0, 0, 0, 0, 0};
    require(::pread(fd, hdr, 48, 0) == 48, "tape-emit: SA header");
    const uint32_t saMagic = (uint32_t)(hdr[0] & 0xffffffffu);   // bytes 0..3
    const uint64_t saN = hdr[1];                                  // bytes 8..15 = n
    require(saMagic == 0x41535853u, "tape-emit: SA stream is not an SXSA anchor");
    require(saN == n, "tape-emit: SA stream is for a different corpus");
    const U entries = (U((unsigned long long)st.st_size) - 48) / 8;
    require(entries == n, "tape-emit: SA entry count != n");
    std::vector<U> pos(chunk);
    std::vector<U> words(chunk);
    // THE SYMBOL STREAM: scatter_sort emits one remapped BWT symbol per rank
    // beside the SA (M[p-1], position 0 -> the padding symbol 2), computed during
    // its SEQUENTIAL scatter pass. Using it removes the per-row text.at() call,
    // which is the LOCKED windowed corpus adapter - measured at 113x the lock-free
    // path in the prefetch charter, and the reason the first 10 GB emit burned 34
    // cores for 50 minutes and published nothing.
    std::vector<uint8_t> syms(chunk);
    const std::string symPath = saPath + ".sym";
    int sfd = ::open(symPath.c_str(), O_RDONLY);
    const bool haveSyms = sfd >= 0;
    if (!haveSyms)
        std::fprintf(stderr, "TAPE_EMIT_WARN no %s: falling back to the LOCKED "
                             "per-row corpus read (slow)\n", symPath.c_str());
    U baseRow = 0, hits = 0;
    double tRead = 0, tWord = 0, tPub = 0;
    while (baseRow < n) {
        const U want = std::min<U>(chunk, n - baseRow);
        const double tr = now_sec();
        size_t got = 0, bytes = size_t(want) * 8;
        while (got < bytes) {
            ssize_t r = ::pread(fd, ((char*)pos.data()) + got, bytes - got,
                                off_t(48 + (unsigned long long)baseRow * 8 + got));
            require(r > 0, "tape-emit: short SA read");
            got += size_t(r);
        }
        if (haveSyms) {
            size_t sgot = 0;
            while (sgot < want) {
                ssize_t r = ::pread(sfd, ((char*)syms.data()) + sgot, want - sgot,
                                    off_t((unsigned long long)baseRow + sgot));
                require(r > 0, "tape-emit: short sym read");
                sgot += size_t(r);
            }
        }
        tRead += now_sec() - tr;
        const double tw = now_sec();
        // Contiguous, disjoint word ranges; publication remains ordered on
        // the calling thread after every worker has joined. Count hits locally
        // so the row loop has no shared writes or atomic operations.
        const U workers = std::max<U>(1, std::min<U>(threads, (want + 65535) / 65536));
        std::vector<U> workerHits(workers, 0);
        auto buildWords = [&](U worker) {
            U localHits = 0;
            const U lo = want * worker / workers;
            const U hi = want * (worker + 1) / workers;
            for (U i = lo; i < hi; ++i) {
            const U p = pos[i];
            require(p < n, "tape-emit: SA position out of range");
            // The BWT symbol of this row is M[p-1], and position 0 is the
            // PADDING row whose symbol is 2 - exactly the fill pass's
            // `p ? at(p-1) : 2` (binbwt_slice.h fill loop). Getting this wrong
            // costs the last unit of the `counts[2] == 10` terminal-coverage
            // check: 9 padding symbols instead of 10.
            const uint8_t sym = haveSyms ? syms[i] : (p ? text.at(p - 1) : uint8_t(2));
            const bool hit = maybeTarget(p) && isTarget(p);
            if (hit) ++localHits;
            words[i] = bin_word(p + 1, sym, hit);
        }
            workerHits[worker] = localHits;
        };
        std::vector<std::thread> wordPool;
        for (U worker = 1; worker < workers; ++worker)
            wordPool.emplace_back(buildWords, worker);
        buildWords(0);
        for (auto& worker : wordPool) worker.join();
        for (U count : workerHits) hits += count;
        tWord += now_sec() - tw;
        const double tp = now_sec();
        sink->publishBin(0, words.data(), want, baseRow);
        tPub += now_sec() - tp;
        baseRow += want;
    }
    ::close(fd);
    if (sfd >= 0) ::close(sfd);
    const double tEnd0 = now_sec();
    sink->finish();
    {
        const U nExc = sink->writeRunExc(prefix + ".runexc");
        std::fprintf(stderr, "TAPE_EMIT_RUNEXC path=%s count=%llu\n",
                     (prefix + ".runexc").c_str(), (unsigned long long)nExc);
    }
    const double tEnd = now_sec() - tEnd0;
    std::error_code ec;
    fs::remove(sink->dir, ec);
    std::fprintf(stderr,
        "TAPE_EMIT_STATS slot_wait_s=%.2f flow_wait_s=%.2f row_loop_s=%.2f writers=%llu\n",
        sink->own.tSlotWait, sink->own.tFlowWait, sink->own.tStitch,
        (unsigned long long)sink->writersUsed);
    std::fprintf(stderr,
        "TAPE_EMIT_DONE rows=%llu runs=%llu hits=%llu wall=%.2f read_s=%.2f word_s=%.2f "
        "publish_s=%.2f finish_s=%.2f\n",
        (unsigned long long)n, (unsigned long long)sink->runs(),
        (unsigned long long)hits, now_sec() - t0, tRead, tWord, tPub, tEnd);
}

static int binbwt_main(const std::string& path, const std::string& mapPath,
                       const std::string& prefix, U threads, U targetSize,
                       U depthCap, U liveCap, bool liveCapGiven, U levelBytes,
                       bool dollar, bool freqSched, U splitSize, bool serialSink,
                       bool probe, bool fillOnly, BinWindowOpts wopt,
                       const std::string& tapeEmitSa, U tapeEmitChunk) {
    wopt.tapeEmitSa = tapeEmitSa;
    wopt.tapeEmitChunk = tapeEmitChunk;
    // `--bin-fill-only` implies the probe's "write nothing at all" contract.
    if (fillOnly) probe = true;
    // The window scratch tree: the caller's --bin-window-dir if it gave one,
    // else the sink's own scratch root (beside the output prefix), else /tmp for
    // the probe arms (which write no product at all).
    if (wopt.dir.empty()) {
        if (!prefix.empty())
            wopt.dir = std::filesystem::path(prefix).parent_path().string() +
                       "/binbwt-windows";
        else
            wopt.dir = "/tmp/binbwt-windows-" + std::to_string(::getpid());
    }
    if (!probe)
        require(!prefix.empty() && !fs::exists(prefix + ".rlebwt") &&
                !fs::exists(prefix + ".pieces"), "fresh bin-bwt output required");
    // `--bin-level-bytes` is the TOP level's depth in bytes (1 or 2) and nothing
    // else: the interior refinement always codes one byte per level, so a
    // 2-byte top level never builds a 65,536-code table.
    //
    // `depth_cap == 0` (AUTO) is the PRODUCT default since PART 4: the target-size
    // rule is the only policy and bin_no_progress_sanity_bound is the bug guard,
    // so the only thing this check may reject is a level width. (Until this fix
    // the check still demanded `depthCap >= 1`, i.e. it refused the merge tip's
    // OWN default and every default-shaped producer run died with
    // `CROSS_LCP_FATAL bin-bwt parameters` - found while driving part 2's arms.)
    require(levelBytes >= 1 && levelBytes <= 2, "bin-bwt parameters");
    TextSeg segment; segment.path = path; segment.len = fs::file_size(path);
    std::ifstream map(mapPath, std::ios::binary);
    map.read((char*)segment.remap.data(), 256);
    require(bool(map), "bin-bwt remap read");
    auto permutation = segment.remap;
    std::sort(permutation.begin(), permutation.end());
    for (unsigned c = 0; c < 256; ++c)
        require(permutation[c] == c, "bin-bwt remap bijection");
    require(segment.remap[0x1e] == 0x1e, "bin-bwt separator remap");
    CorpusText text;
    text.open({segment}, true);
    require(text.n, "bin-bwt empty corpus");
    // ---- THE TAPE-EMIT BRANCH (--tape-emit SA): skip the bin tree entirely and
    // emit the product from an external SA stream. Everything downstream of the
    // sink is the existing emission path.
    if (!wopt.tapeEmitSa.empty()) {
        tape_emit(prefix, text, dollar, threads, wopt.tapeEmitSa,
                  wopt.tapeEmitChunk ? wopt.tapeEmitChunk : (U(1) << 22));
        return 0;
    }
    // ---- PHRASE RADIX: one parse pass, before any window exists -------------
    // The dictionary is built from the corpus the tree is about to build, off the
    // product path, and journalled so a second run can be diffed (the parse's
    // determinism is one of the oracle's cases). With the flag absent this block
    // does nothing at all and `wopt.phrase` stays null.
    PhraseDict phraseDict;
    if (wopt.phraseOpts.enabled) {
        phraseDict.parse(text, dollar, wopt.phraseOpts);
        phrase_journal_dict(phraseDict, text.n);
    }
    if (phraseDict.armed()) wopt.phrase = &phraseDict;
    // ---- THE LIVE CAP: derived from the phase budget, or checked against it --
    // The default is no longer unbounded (see BinCapTerms): a window is sized so
    // that the corpus cache + the fill staging + the arena pair it needs all fit
    // inside the phase's RLIMIT_AS with 10% headroom. An EXPLICIT cap that does
    // not fit is refused right here, loudly, instead of dying mid-window.
    if (!liveCapGiven) {
        BinCapTerms ct = bin_derive_live_cap(text.n, threads);
        liveCap = ct.cap;
        std::fprintf(stderr,
            "BIN_LIVE_CAP source=auto cap=%llu n=%llu threads=%llu rlimit_as=%llu "
            "usable=%llu cache=%llu stage=%llu group=%llu headroom_pct=%d (%s)\n",
            (unsigned long long)ct.cap, (unsigned long long)text.n,
            (unsigned long long)threads, (unsigned long long)ct.limit,
            (unsigned long long)ct.usable, (unsigned long long)ct.cache,
            (unsigned long long)ct.stage, (unsigned long long)ct.group,
            int(bin_phase_headroom_pct), ct.source.c_str());
    } else {
        bin_check_live_cap(liveCap, text.n, threads, "--bin-live-cap");
        BinCapTerms ct = bin_derive_live_cap(text.n, threads);
        // `--bin-live-cap` is ALWAYS explicit on the product route: the front
        // door derives the default and passes it, so the producer cannot tell a
        // plan pick from an operator pick by looking at the number. The front
        // door sets CROSS_BIN_CAP_SOURCE=auto when it derived the value, and the
        // journal names the source instead of guessing.
        const char* src = ::getenv("CROSS_BIN_CAP_SOURCE");
        std::fprintf(stderr,
            "BIN_LIVE_CAP source=%s cap=%llu n=%llu threads=%llu rlimit_as=%llu "
            "usable=%llu cache=%llu stage=%llu group=%llu headroom_pct=%d fits=1 "
            "(the largest cap this phase could hold is %llu)\n",
            (src && *src) ? src : "operator",
            (unsigned long long)liveCap, (unsigned long long)text.n,
            (unsigned long long)threads, (unsigned long long)ct.limit,
            (unsigned long long)ct.usable, (unsigned long long)ct.cache,
            (unsigned long long)ct.stage, (unsigned long long)ct.group,
            int(bin_phase_headroom_pct), (unsigned long long)ct.cap);
    }
    const double tStart = now_sec();
    BinWork total;
    if (probe) {
        // MEASUREMENT INSTRUMENT: the construction alone, no sink, no output
        // file, no output directory. Emits every row to a no-op callback (the
        // per-row oracle path), so the construction counters and walls are the
        // same ones the producer reports and a 10 GB build term can be measured
        // in minutes instead of after the finish. Nothing else is written.
        bin_order(text, dollar, threads, targetSize, depthCap, liveCap, levelBytes,
                  freqSched, splitSize, total, [](U, U) {}, fillOnly, wopt);
        BinJournal j;
        j.sink = fillOnly ? "probe-fill-only" : "probe-none";
        bin_journal(total, dollar, threads, levelBytes, targetSize, depthCap,
                    freqSched, splitSize, now_sec() - tStart, j, wopt.entropyOpts);
        bin_phrase_journal_work(total, phraseDict.armed() ? &phraseDict : nullptr);
        return 0;
    }
    // `serialSink` runs the UNCHANGED v1 sink (SequentialV2 fed one row at a
    // time) so that the two sinks can be diffed byte for byte on any corpus:
    // it is the equivalence oracle for the parallel publisher, and it keeps the
    // v1 product reproducible from this tree.
    const double tSink0 = now_sec();
    std::string sinkDir;
    U serialRuns = 0;
    std::unique_ptr<BinBandSink> parallel;
    std::unique_ptr<SequentialV2> serial;
    if (serialSink) {
        serial.reset(new SequentialV2(prefix, text, text.n, text.n));
        serial->beginShard(0, 0, text.n);
        sinkDir = serial->dir;
    } else {
        parallel.reset(new BinBandSink(prefix, text, text.n, dollar, threads));
        sinkDir = parallel->dir;
    }
    const double tSink1 = now_sec();
    if (serialSink)
        bin_order(text, dollar, threads, targetSize, depthCap, liveCap, levelBytes,
                  freqSched, splitSize, total,
                  [&](U row, U p) { serial->row(row, p); }, false, wopt);
    else
        bin_order(text, dollar, threads, targetSize, depthCap, liveCap, levelBytes,
                  freqSched, splitSize, total, *parallel, false, wopt);
    const double tOrder = now_sec();
    if (serialSink) { serial->endShard(); serialRuns = serial->index; }
    else { parallel->finish(); total.absorb(parallel->own); }
    const double tEnds = now_sec();
    std::error_code ec;
    fs::remove(sinkDir, ec);
    const double wall = now_sec() - tStart;
    BinJournal j;
    j.sink = serialSink ? "serial-v1" : "parallel-v2";
    j.serialRuns = serialRuns;
    j.writers = parallel ? parallel->writersUsed : 0;
    j.tSinkOpen = tSink1 - tSink0;
    j.tOrder = tOrder - tSink1;
    j.tEnds = tEnds - tOrder;
    bin_journal(total, dollar, threads, levelBytes, targetSize, depthCap, freqSched,
                splitSize, wall, j, wopt.entropyOpts);
    bin_phrase_journal_work(total, phraseDict.armed() ? &phraseDict : nullptr);
    return 0;
}
