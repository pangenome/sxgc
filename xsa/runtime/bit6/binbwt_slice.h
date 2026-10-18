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
struct BinWork {
    U sweepBytes = 0, sweepPasses = 0, windows = 0, bins = 0, binPasses = 0;
    U levelPasses = 0, levelElems = 0;      // bucketing passes / elements coded
    U comparisons = 0, symbols = 0, ties = 0, certDecided = 0;
    U leafSorts = 0, leafElems = 0, singletonBins = 0;
    U peakLiveSlots = 0, peakLiveBytes = 0, emittedRows = 0;
    U builtBins = 0;
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
    void absorb(const BinWork& o) {
        sweepBytes += o.sweepBytes; sweepPasses += o.sweepPasses;
        windows += o.windows; bins += o.bins; binPasses += o.binPasses;
        levelPasses += o.levelPasses; levelElems += o.levelElems;
        comparisons += o.comparisons; symbols += o.symbols; ties += o.ties;
        certDecided += o.certDecided; leafSorts += o.leafSorts;
        leafElems += o.leafElems; singletonBins += o.singletonBins;
        builtBins += o.builtBins;
        peakLiveSlots = std::max(peakLiveSlots, o.peakLiveSlots);
        peakLiveBytes = std::max(peakLiveBytes, o.peakLiveBytes);
        emittedRows += o.emittedRows;
        tSweep += o.tSweep; tBuild += o.tBuild; tEmit += o.tEmit;
        cpuBuild += o.cpuBuild; tAlloc += o.tAlloc; tWait += o.tWait;
        runsPublished += o.runsPublished; bands += o.bands;
        targetHits += o.targetHits; bandJobs += o.bandJobs;
        tCount += o.tCount; tFill += o.tFill;
        tStitch += o.tStitch; tBandWrite += o.tBandWrite; tFlowWait += o.tFlowWait;
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

// Total order on two distinct tails, exact, through the CorpusText view only.
// certified = a provably equal prefix length (the bin's refined depth), so no
// shared prefix inside a bin is ever re-walked by a comparison.
static inline int bin_tail_cmp(const CorpusText& t, bool dollar, U word_a, U word_b,
                               U certified, BinWork& w) {
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
        const uint8_t* A = t.span(a + j, ab, take);
        const uint8_t* B = t.span(b + j, bb, take);
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
static U* bin_sort_rec(const CorpusText& t, bool dollar, U* src, U* dst, U m,
                       U depth, U depthCap, U leafSize, U levelBytes,
                       BinWork& w) {
    if (m <= 1) return src;
    if (m <= leafSize || depth >= depthCap || depth >= t.n) {
        auto less = [&](U a, U b) {
            return bin_tail_cmp(t, dollar, a, b, depth, w) < 0;
        };
        if (m <= 16) {                       // insertion sort for tiny leaves
            for (U i = 1; i < m; ++i) {
                U v = src[i], j = i;
                while (j && less(v, src[j - 1])) { src[j] = src[j - 1]; --j; }
                src[j] = v;
            }
        } else std::sort(src, src + m, less);
        ++w.leafSorts; w.leafElems += m;
        return src;
    }
    const U codes = bin_codes(levelBytes);
    std::vector<U> count(codes, 0);
    std::vector<U> start(codes, 0);
    for (U i = 0; i < m; ++i)
        ++count[bin_code(t, dollar, src[i], depth, levelBytes)];
    ++w.levelPasses; w.levelElems += m;
    {
        U sum = 0;
        for (U c = 0; c < codes; ++c) { start[c] = sum; sum += count[c]; }
        require(sum == m, "bin bucket coverage");
    }
    {
        std::vector<U> fill = start;
        for (U i = 0; i < m; ++i) {
            U p = src[i];
            dst[fill[bin_code(t, dollar, p, depth, levelBytes)]++] = p;
        }
    }
    ++w.binPasses;
    // Bucket 0 holds the dollar suffixes exhausted at this depth: they all share
    // the node prefix up to their own end, so ascending length (== descending
    // position) is their exact order and no comparison is needed.
    if (count[0] > 1)
        std::sort(dst, dst + count[0],
                  [](U a, U b) { return bin_word_tail(a) > bin_word_tail(b); });
    for (U c = 1; c < codes; ++c) {
        if (count[c] <= 1) continue;
        U* child = bin_sort_rec(t, dollar, dst + start[c], src + start[c],
                                count[c], depth + levelBytes, depthCap,
                                leafSize, levelBytes, w);
        if (child != dst + start[c])
            std::memcpy(dst + start[c], child, count[c] * sizeof(U));
    }
    return dst;
}

// A refinement task: order the m tails of [src, src+m) that share their first
// `depth` symbols, and leave the result in the caller's canonical arena slice
// [canon, canon+m). src/dst are the basement pair of the bin slice; a task that
// splits writes the children's regions into dst and hands them out, so leaf
// tasks are independent and a bin's own tree is worked by many threads.
struct BinTask { U* src; U* dst; U* canon; U m; U depth; unsigned bin; };

template <class Push>
static void bin_task_run(const CorpusText& t, bool dollar, const BinTask& tk,
                         U splitSize, U leafSize, U depthCap, U levelBytes,
                         BinWork& w, Push push) {
    if (tk.m <= 1) {
        if (tk.m && tk.src != tk.canon)
            std::memcpy(tk.canon, tk.src, tk.m * sizeof(U));
        return;
    }
    if (tk.m <= splitSize || tk.depth >= depthCap || tk.depth >= t.n) {
        // Below the split threshold one thread refines the whole subtree.
        U* r = bin_sort_rec(t, dollar, tk.src, tk.dst, tk.m, tk.depth, depthCap,
                            leafSize, levelBytes, w);
        if (r != tk.canon) std::memcpy(tk.canon, r, tk.m * sizeof(U));
        return;
    }
    const U codes = bin_codes(levelBytes);
    std::vector<U> count(codes, 0), start(codes, 0);
    for (U i = 0; i < tk.m; ++i)
        ++count[bin_code(t, dollar, tk.src[i], tk.depth, levelBytes)];
    ++w.levelPasses; w.levelElems += tk.m;
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
        push(BinTask{tk.dst + start[c], tk.src + start[c], tk.canon + start[c],
                     count[c], tk.depth + levelBytes, tk.bin});
    }
}

// One bin start-to-finish on the calling thread (used for --threads 1).
static void bin_build_serial(const CorpusText& t, bool dollar, U* slice, U* scratch,
                             U m, U splitSize, U leafSize, U depthCap,
                             U levelBytes, BinWork& w) {
    if (m <= 1) { ++w.singletonBins; ++w.builtBins; return; }
    std::vector<BinTask> stack;
    std::vector<BinTask> children;
    stack.push_back(BinTask{slice, scratch, slice, m, 0, 0});
    while (!stack.empty()) {
        BinTask tk = stack.back();
        stack.pop_back();
        children.clear();
        bin_task_run(t, dollar, tk, splitSize, leafSize, depthCap, levelBytes, w,
                     [&](const BinTask& c) { children.push_back(c); });
        for (const auto& c : children) stack.push_back(c);
    }
    ++w.builtBins;
    w.peakLiveSlots = std::max<U>(w.peakLiveSlots, 2 * m);
    w.peakLiveBytes = std::max<U>(w.peakLiveBytes, 2 * m * sizeof(U));
}

template <class Emit>
static void bin_order(const CorpusText& text, bool dollar, U threads, U leafSize,
                      U depthCap, U liveCap, U levelBytes, bool freqSched,
                      U splitSize, BinWork& total, Emit&& emit) {
    const U n = text.n;
    // The sink's ascending capture-target list, joined against the corpus in the
    // fill pass. Empty for the per-row oracle path.
    const std::vector<U>* joinTargets = nullptr;
    if constexpr (bin_publish_sink<Emit>::value) joinTargets = &emit.joinTargets();
    const U joinCount = joinTargets ? joinTargets->size() : 0;
    require(n && threads >= 1 && leafSize >= 1 && depthCap >= levelBytes &&
            levelBytes >= 1 && levelBytes <= 2, "bin-bwt parameters");
    if (!splitSize) splitSize = std::max<U>(4 * leafSize, n / (threads * 64));
    const U NT = std::max<U>(1, std::min<U>(threads, 64));
    // Corpus chunk split for the counting sweep; the fill sweep reuses the exact
    // same boundaries so per-thread bin counts ARE the fill offsets.
    std::vector<U> chunk(NT + 1, 0);
    for (U t = 0; t <= NT; ++t) chunk[t] = n / NT * t;
    chunk[NT] = n;
    std::vector<std::array<U, 256>> hists(NT);
    for (auto& h : hists) h.fill(0);
    {
        const double t0 = now_sec();
        auto countChunk = [&](U t) {
            auto& h = hists[t];
            constexpr U W = 1u << 20;
            std::vector<uint8_t> buf(std::min(W, std::max<U>(1, chunk[t + 1] - chunk[t])));
            for (U at = chunk[t]; at < chunk[t + 1];) {
                U len = std::min(W, chunk[t + 1] - at);
                const uint8_t* b = text.span(at, buf.data(), len);
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
    for (unsigned b0 = 0; b0 < 256;) {
        unsigned b1 = b0;
        U live = 0;
        while (b1 < 256 && live + bins[b1] <= liveCap) { live += bins[b1]; ++b1; }
        require(b1 > b0, "bin window admits at least one bin");
        ++total.windows; total.bins += b1 - b0;
        const double tAlloc0 = now_sec();
        BinArena arena(live), scratch(live);
        total.tAlloc += now_sec() - tAlloc0;
        std::array<U, 257> off{};
        {
            U sum = 0;
            for (unsigned c = b0; c < b1; ++c) { off[c] = sum; sum += bins[c]; }
            off[b1] = sum;
            require(sum == live, "bin window arena sizing");
        }
        // Fill pass: place every position of this window into its bin slice.
        // Single-threaded on purpose: many threads writing into 256 stream heads
        // measured 2-5x SLOWER than one (per-thread per-bin write streams destroy
        // write combining), and it keeps the arena order canonical - ascending
        // position inside every bin - so the work counters are identical for
        // every thread count.
        //
        // The same pass also does the two things that make parallel publication
        // possible, so that NEITHER costs a random access:
        //  * it writes the row's BWT symbol M[p-1] into the parallel symbol
        //    column (the byte is already in the window buffer, one behind);
        //  * it merge-joins the sink's ascending capture-target list against the
        //    ascending positions, so each target's bin slice index IS its row and
        //    no per-row hash lookup survives anywhere.
        {
            const double t0 = now_sec();
            std::vector<uint8_t> buf(std::min<U>(1u << 20, n));
            std::array<U, 256> cur{};
            for (unsigned c = b0; c < b1; ++c) cur[c] = off[c];
            const bool allBins = (b1 - b0 == 256);
            U ti = 0;                    // cursor in the ascending target list
            uint8_t prev = 0;            // M[p-1] for the first byte of a span
            for (U at = 0; at < n;) {
                U len = std::min<U>(1u << 20, n - at);
                const uint8_t* b = text.span(at, buf.data(), len);
                for (U j = 0; j < len; ++j) {
                    U p = at + j;
                    unsigned c = b[j];
                    bool hit = ti < joinCount && (*joinTargets)[ti] == p;
                    if (hit) ++ti;
                    if (!allBins && (c < b0 || c >= b1)) continue;
                    const U slot = cur[c]++;
                    // The BWT symbol of this row: M[p-1], already in the window
                    // buffer one byte behind (p == 0 is the padding row whose
                    // symbol is 2, exactly SequentialV2's `p ? at(p-1) : 2`).
                    const uint8_t sym = p ? (j ? b[j - 1] : prev) : uint8_t(2);
                    arena.data()[slot] = bin_word(bin_tail_of(p, n, dollar), sym, hit);
                }
                prev = b[len - 1];
                at += len;
            }
            require(ti == joinCount, "bin fill target join coverage");
            for (unsigned c = b0; c < b1; ++c)
                require(cur[c] == off[c] + bins[c], "bin fill coverage");
            total.tFill += now_sec() - t0;
            total.tSweep += now_sec() - t0;
            total.sweepBytes += n; ++total.sweepPasses;
        }
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
            total.tEmit += now_sec() - t0;
            total.emittedRows += bins[c];
        };
        std::vector<BinWork> work(std::max<U>(1, threads));
        if (threads == 1) {
            for (unsigned c = b0; c < b1; ++c) {
                if (!bins[c]) continue;
                const double t0 = now_sec();
                bin_build_serial(text, dollar, arena.data() + off[c],
                                 scratch.data() + off[c], bins[c], splitSize,
                                 leafSize, depthCap, levelBytes, work[0]);
                work[0].cpuBuild += now_sec() - t0;
                emitBin(c);
            }
            total.tBuild += work[0].cpuBuild;
        } else {
            // Task queue over bin sub-trees: a bin's own tree is worked by as
            // many threads as it has coarse nodes, so a single huge bin cannot
            // serialize the window.
            struct Shared {
                std::mutex mtx;
                std::condition_variable cv;
                // Keyed by bin index: the lowest incomplete bin is always
                // worked first, which is exactly the emitter's next need.
                std::multimap<unsigned, BinTask> queue;
                std::unique_ptr<std::atomic<unsigned>[]> pending;
                std::atomic<U> outstanding{0};
                Shared() : pending(new std::atomic<unsigned>[256]) {
                    for (unsigned c = 0; c < 256; ++c) pending[c].store(0);
                }
            } sh;
            std::array<std::atomic<unsigned>, 256> done;
            for (auto& d : done) d.store(0, std::memory_order_relaxed);
            for (unsigned c : order) sh.pending[c].store(1);
            sh.outstanding.store(order.size());
            for (unsigned c : order)
                sh.queue.emplace(c, BinTask{arena.data() + off[c],
                                            scratch.data() + off[c],
                                            arena.data() + off[c], bins[c], 0, c});
            std::vector<std::thread> pool;
            pool.reserve(threads - 1);
            for (U t = 0; t + 1 < threads; ++t)
                pool.emplace_back([&, t] {
                    BinWork& w = work[t];
                    for (;;) {
                        BinTask tk;
                        {
                            std::unique_lock<std::mutex> lk(sh.mtx);
                            sh.cv.wait(lk, [&] {
                                return !sh.queue.empty() ||
                                       sh.outstanding.load() == 0;
                            });
                            if (sh.queue.empty()) return;
                            auto it = sh.queue.begin();
                            tk = it->second;
                            sh.queue.erase(it);
                        }
                        const double t0 = now_sec();
                        bin_task_run(text, dollar, tk, splitSize, leafSize,
                                     depthCap, levelBytes, w, [&](const BinTask& c) {
                            std::lock_guard<std::mutex> lk(sh.mtx);
                            ++sh.pending[c.bin];
                            sh.queue.emplace(c.bin, c);
                            sh.cv.notify_one();
                        });
                        w.cpuBuild += now_sec() - t0;
                        if (--sh.pending[tk.bin] == 0) {
                            ++w.builtBins;
                            done[tk.bin].store(1, std::memory_order_release);
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
        }
        if constexpr (bin_publish_sink<Emit>::value) emit.windowDone();
        for (auto& w : work) total.absorb(w);
        const U liveSlots = 2 * live;   // arena + scratch slice pair per window
        total.peakLiveSlots = std::max<U>(total.peakLiveSlots, liveSlots);
        total.peakLiveBytes = std::max<U>(total.peakLiveBytes, liveSlots * sizeof(U));
        b0 = b1;
    }
    require(row == n, "bin-bwt row coverage");
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
    struct Band {
        std::vector<uint8_t> chars;
        std::vector<uint32_t> lens;
        std::vector<U> heads, tails;
        U size() const { return chars.size(); }
    };
    struct BandJob {
        U band = 0, firstRun = 0, words = 0, rlebwtOff = 0;
        std::vector<uint8_t> chars;
        std::vector<uint32_t> lens;
        std::vector<U> heads, tails;
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
        writersUsed = std::min<U>(8, std::max<U>(1, threadCount / 8));
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
            if (!pending.len) pending = SparseRun{c, 1, posOf(i), posOf(i)};
            else pending.tail = posOf(i), ++pending.len;
            ++next; ++rows;
        }
        if (m) prevLastPos = posOf(m - 1);
        own.tStitch += now_sec() - t0;
    }
    void addRun(const SparseRun& run) {
        if (pending.len && pending.c != run.c) flushRun();
        if (!pending.len) pending = run;
        else { pending.len += run.len; pending.tail = run.tail; }
        rows += run.len;
    }
    void flushRun() {
        if (!pending.len) return;
        counts[pending.c] += pending.len; ++frequencies[pending.c];
        require(pending.len == U(uint32_t(pending.len)),
                "bin-bwt run length exceeds the piece length field");
        band.chars.push_back(pending.c);
        band.lens.push_back(uint32_t(pending.len));
        band.heads.push_back(pending.head); band.tails.push_back(pending.tail);
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
        job.chars.swap(band.chars); job.lens.swap(band.lens);
        job.heads.swap(band.heads); job.tails.swap(band.tails);
        job.gmark = std::move(gmark);
        band = Band();
        {   // bounded in-flight bands: one slot per queued or running band
            std::unique_lock<std::mutex> lk(mu);
            cvSlot.wait(lk, [&] { return slotsFree > 0; });
            --slotsFree;
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
            const U rc = job.chars.size();
            v2_write_piece(dir, job.band, 0, job.firstRun, job.chars.data(),
                           job.lens.data(), job.heads.data(), job.tails.data(), rc,
                           width);
            std::vector<uint32_t> w32;
            w32.reserve(rc + 16);
            for (U i = 0; i < rc; ++i) {
                const uint32_t c = job.chars[i];
                U left = job.lens[i];
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

// ---- producer entry point: the parallel sink publishes the concatenated bins
static int binbwt_main(const std::string& path, const std::string& mapPath,
                       const std::string& prefix, U threads, U leafSize,
                       U depthCap, U liveCap, U levelBytes, bool dollar,
                       bool freqSched, U splitSize, bool serialSink) {
    require(!prefix.empty() && !fs::exists(prefix + ".rlebwt") &&
            !fs::exists(prefix + ".pieces"), "fresh bin-bwt output required");
    require(depthCap >= levelBytes && levelBytes >= 1 && levelBytes <= 2,
            "bin-bwt parameters");
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
    const double tStart = now_sec();
    BinWork total;
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
        bin_order(text, dollar, threads, leafSize, depthCap, liveCap, levelBytes,
                  freqSched, splitSize, total,
                  [&](U row, U p) { serial->row(row, p); });
    else
        bin_order(text, dollar, threads, leafSize, depthCap, liveCap, levelBytes,
                  freqSched, splitSize, total, *parallel);
    const double tOrder = now_sec();
    if (serialSink) { serial->endShard(); serialRuns = serial->index; }
    else { parallel->finish(); total.absorb(parallel->own); }
    const double tEnds = now_sec();
    std::error_code ec;
    fs::remove(sinkDir, ec);
    const double wall = now_sec() - tStart;
    std::fprintf(stderr,
        "BIN_WORK dollar=%u threads=%llu level_bytes=%llu leaf=%llu depth_cap=%llu "
        "freq_sched=%u split=%llu bins=256 windows=%llu sweep_bytes=%llu "
        "sweep_passes=%llu level_passes=%llu level_elems=%llu bin_passes=%llu "
        "comparisons=%llu symbols=%llu cert_decided=%llu ties=%llu "
        "leaf_sorts=%llu leaf_elems=%llu peak_live_slots=%llu peak_live_bytes=%llu "
        "emitted_rows=%llu sweep_s=%.3f build_s=%.3f emit_s=%.3f total_s=%.3f "
        "build_cpu_s=%.3f ns_per_symbol_thread=%.3f "
        "ns_per_comparison_thread=%.3f sink_open_s=%.3f order_s=%.3f "
        "endshard_s=%.3f alloc_s=%.3f wait_s=%.3f "
        "count_s=%.3f fill_s=%.3f "
        "sink=%s serial_runs=%llu runs=%llu bands=%llu target_hits=%llu "
        "band_jobs=%llu writers=%llu stitch_s=%.3f band_write_s=%.3f "
        "flow_wait_s=%.3f ns_per_row_stitch=%.3f "
        "full_SA=0 persistent_positions=0\n",
        unsigned(dollar), (unsigned long long)threads,
        (unsigned long long)levelBytes, (unsigned long long)leafSize,
        (unsigned long long)depthCap, unsigned(freqSched),
        (unsigned long long)splitSize,
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
        tSink1 - tSink0, tOrder - tSink1, tEnds - tOrder, total.tAlloc,
        total.tWait, total.tCount, total.tFill,
        serialSink ? "serial-v1" : "parallel-v2",
        (unsigned long long)serialRuns,
        (unsigned long long)total.runsPublished,
        (unsigned long long)total.bands, (unsigned long long)total.targetHits,
        (unsigned long long)total.bandJobs,
        (unsigned long long)(parallel ? parallel->writersUsed : 0),
        total.tStitch, total.tBandWrite, total.tFlowWait,
        total.emittedRows ? 1e9 * total.tStitch / double(total.emittedRows) : 0.0);
    return 0;
}
