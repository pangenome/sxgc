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
        cpuBuild += o.cpuBuild;
    }
};

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
static inline int bin_tail_cmp(const CorpusText& t, bool dollar, U a, U b,
                               U certified, BinWork& w) {
    const U n = t.n;
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
static inline unsigned bin_code(const CorpusText& t, bool dollar, U tail, U depth,
                                U levelBytes) {
    const U n = t.n;
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

static inline U bin_codes(U levelBytes) {
    U c = 1;
    for (U d = 0; d < levelBytes; ++d) c *= 257u;
    return c;
}

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
        std::sort(dst, dst + count[0], [](U a, U b) { return a > b; });
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

// One bin: refine its tails in place. The bin's scratch slice is private for the
// call, so the live footprint is exactly 2*m position slots for the bin in
// flight and nothing outlives the window.
static void bin_build(const CorpusText& t, bool dollar, U* slice, U* scratch,
                      U m, U depthCap, U leafSize, U levelBytes, BinWork& w) {
    if (m <= 1) { ++w.singletonBins; return; }
    U* r = bin_sort_rec(t, dollar, slice, scratch, m, 0, depthCap, leafSize,
                        levelBytes, w);
    if (r != slice) std::memcpy(slice, r, m * sizeof(U));
    w.peakLiveSlots = std::max<U>(w.peakLiveSlots, 2 * m);
    w.peakLiveBytes = std::max<U>(w.peakLiveBytes, 2 * m * sizeof(U));
}

// The algorithm proper: first-byte count sweep -> windowed fill -> parallel
// per-bin build -> ordered emit. `emit(row, p)` receives the global rows in
// order; that callback is the only place rows leave this file.
template <class Emit>
static void bin_order(const CorpusText& text, bool dollar, U threads, U leafSize,
                      U depthCap, U liveCap, U levelBytes, BinWork& total,
                      Emit emit) {
    const U NT = std::max<U>(1, std::min<U>(threads, 64));
    // Corpus chunk split for the counting sweep; the fill sweep reuses the exact
    // same boundaries so per-thread bin counts ARE the fill offsets.
    std::vector<U> chunk(NT + 1, 0);
    for (U t = 0; t <= NT; ++t) chunk[t] = text.n / NT * t;
    chunk[NT] = text.n;
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
        total.tSweep += now_sec() - t0;
        total.sweepBytes += text.n; ++total.sweepPasses;
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
    require(histogram == text.n, "bin-bwt first-byte histogram coverage");
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
        std::vector<U> arena(live);
        std::array<U, 257> off{};
        {
            U sum = 0;
            for (unsigned c = b0; c < b1; ++c) { off[c] = sum; sum += bins[c]; }
            off[b1] = sum;
            require(sum == live, "bin window arena sizing");
        }
        // Fill pass: place every position of this window into its bin slice.
        {
            const double t0 = now_sec();
            std::vector<std::array<U, 256>> base(NT);
            {
                std::array<U, 256> seen{};
                for (unsigned c = 0; c < 256; ++c) seen[c] = off[c];
                for (U t = 0; t < NT; ++t)
                    for (unsigned c = 0; c < 256; ++c) {
                        base[t][c] = seen[c];
                        seen[c] += hists[t][c];
                    }
                for (unsigned c = b0; c < b1; ++c)
                    require(seen[c] == off[c] + bins[c], "bin window offset coverage");
            }
            auto fillChunk = [&](U t) {
                auto cur = base[t];
                constexpr U W = 1u << 20;
                std::vector<uint8_t> buf(std::min(W, std::max<U>(1, chunk[t + 1] - chunk[t])));
                for (U at = chunk[t]; at < chunk[t + 1];) {
                    U len = std::min(W, chunk[t + 1] - at);
                    const uint8_t* b = text.span(at, buf.data(), len);
                    for (U j = 0; j < len; ++j) {
                        unsigned c = b[j];
                        if (c >= b0 && c < b1) {
                            U p = at + j;
                            arena[cur[c]++] = bin_tail_of(p, text.n, dollar);
                        }
                    }
                    at += len;
                }
                for (unsigned c = b0; c < b1; ++c)
                    require(cur[c] == base[t][c] + hists[t][c], "bin fill coverage");
            };
            if (NT == 1) fillChunk(0);
            else {
                std::vector<std::thread> pool;
                for (U t = 0; t < NT; ++t) pool.emplace_back(fillChunk, t);
                for (auto& th : pool) th.join();
            }
            total.tSweep += now_sec() - t0;
            total.sweepBytes += text.n; ++total.sweepPasses;
        }
        // Frequency-scheduled bin order: largest first, so the tail of a window
        // is a short bin rather than the giant one.
        std::vector<unsigned> order;
        for (unsigned c = b0; c < b1; ++c) if (bins[c]) order.push_back(c);
        std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
            return bins[a] > bins[b];
        });
        auto emitBin = [&](unsigned c) {
            const double t0 = now_sec();
            const U* slice = arena.data() + off[c];
            for (U i = 0; i < bins[c]; ++i) {
                emit(row++, bin_pos_of(slice[i], text.n, dollar));
            }
            total.tEmit += now_sec() - t0;
            total.emittedRows += bins[c];
        };
        std::vector<BinWork> work(std::max<U>(1, threads));
        // One shared scratch arena slice per bin: the recursion ping-pongs
        // between a bin's arena slice and its scratch slice, so the live
        // footprint stays 2*live position slots for the whole window and never
        // grows with the worker count.
        std::vector<U> scratch(live);
        if (threads == 1) {
            for (unsigned c : order) {
                const double t0 = now_sec();
                bin_build(text, dollar, arena.data() + off[c],
                          scratch.data() + off[c], bins[c], depthCap, leafSize,
                          levelBytes, work[0]);
                const double busy = now_sec() - t0;
                total.tBuild += busy; work[0].cpuBuild += busy;
                ++work[0].builtBins;
            }
            for (unsigned c = b0; c < b1; ++c) if (bins[c]) emitBin(c);
        } else {
            std::array<std::atomic<unsigned>, 256> done;
            for (auto& d : done) d.store(0, std::memory_order_relaxed);
            std::atomic<U> cursor(0);
            std::vector<std::thread> pool;
            pool.reserve(threads - 1);
            for (U t = 0; t + 1 < threads; ++t)
                pool.emplace_back([&, t] {
                    BinWork& w = work[t];
                    for (;;) {
                        U i = cursor.fetch_add(1, std::memory_order_relaxed);
                        if (i >= order.size()) break;
                        unsigned c = order[i];
                        const double t0 = now_sec();
                        bin_build(text, dollar, arena.data() + off[c],
                                  scratch.data() + off[c], bins[c], depthCap,
                                  leafSize, levelBytes, w);
                        w.cpuBuild += now_sec() - t0;
                        ++w.builtBins;
                        done[c].store(1, std::memory_order_release);
                    }
                });
            // Emit in global row order as bins land; construction runs ahead.
            for (unsigned c = b0; c < b1; ++c) {
                if (!bins[c]) continue;
                while (!done[c].load(std::memory_order_acquire))
                    std::this_thread::yield();
                emitBin(c);
            }
            for (auto& th : pool) th.join();
        }
        for (auto& w : work) total.absorb(w);
        const U liveSlots = 2 * live;   // arena slice pairs across the window
        total.peakLiveSlots = std::max<U>(total.peakLiveSlots, liveSlots);
        total.peakLiveBytes = std::max<U>(total.peakLiveBytes, liveSlots * sizeof(U));
        b0 = b1;
    }
    require(row == text.n, "bin-bwt row coverage");
}

// ---- producer entry point: the existing sink consumes the concatenated bins
static int binbwt_main(const std::string& path, const std::string& mapPath,
                       const std::string& prefix, U threads, U leafSize,
                       U depthCap, U liveCap, U levelBytes, bool dollar) {
    require(!prefix.empty() && !fs::exists(prefix + ".rlebwt") &&
            !fs::exists(prefix + ".pieces"), "fresh bin-bwt output required");
    require(threads >= 1 && leafSize >= 1 && depthCap >= levelBytes &&
            levelBytes >= 1 && levelBytes <= 2, "bin-bwt parameters");
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
    SequentialV2 output(prefix, text, text.n, text.n);
    output.beginShard(0, 0, text.n);
    bin_order(text, dollar, threads, leafSize, depthCap, liveCap, levelBytes,
              total, [&](U row, U p) { output.row(row, p); });
    output.endShard();
    std::error_code ec;
    fs::remove(output.dir, ec);
    const double wall = now_sec() - tStart;
    std::fprintf(stderr,
        "BIN_WORK dollar=%u threads=%llu level_bytes=%llu leaf=%llu depth_cap=%llu "
        "bins=256 windows=%llu sweep_bytes=%llu sweep_passes=%llu "
        "level_passes=%llu level_elems=%llu bin_passes=%llu comparisons=%llu "
        "symbols=%llu cert_decided=%llu ties=%llu leaf_sorts=%llu leaf_elems=%llu "
        "peak_live_slots=%llu peak_live_bytes=%llu emitted_rows=%llu "
        "sweep_s=%.3f build_s=%.3f emit_s=%.3f total_s=%.3f build_cpu_s=%.3f "
        "ns_per_symbol_thread=%.3f ns_per_comparison_thread=%.3f "
        "full_SA=0 persistent_positions=0\n",
        unsigned(dollar), (unsigned long long)threads,
        (unsigned long long)levelBytes, (unsigned long long)leafSize,
        (unsigned long long)depthCap, (unsigned long long)total.windows,
        (unsigned long long)total.sweepBytes, (unsigned long long)total.sweepPasses,
        (unsigned long long)total.levelPasses, (unsigned long long)total.levelElems,
        (unsigned long long)total.binPasses, (unsigned long long)total.comparisons,
        (unsigned long long)total.symbols, (unsigned long long)total.certDecided,
        (unsigned long long)total.ties, (unsigned long long)total.leafSorts,
        (unsigned long long)total.leafElems, (unsigned long long)total.peakLiveSlots,
        (unsigned long long)total.peakLiveBytes, (unsigned long long)total.emittedRows,
        total.tSweep, total.tBuild, total.tEmit, wall, total.cpuBuild,
        total.symbols ? 1e9 * total.cpuBuild / double(total.symbols) : 0.0,
        total.comparisons ? 1e9 * total.cpuBuild / double(total.comparisons) : 0.0);
    return 0;
}
