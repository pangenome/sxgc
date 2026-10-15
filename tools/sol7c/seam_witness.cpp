// Test the surviving rank planner's row handoff directly, before any band
// protocol promotion. This is deliberately NOT a production sink.
#define main cross_merge_program_main
#include "../../bit6/cross_lcp_merge.cpp"
#undef main

struct RowWitness : SparseWalkSink {
    struct Range : SparseWalkSink {
        std::vector<U>& rows;
        U lo = 0, hi = 0, next = 0;
        bool started = false, done = false;
        explicit Range(std::vector<U>& r) : rows(r) {}
        void beginShard(U, U a, U b) override {
            require(!started && a <= b && b <= rows.size(), "witness range bounds");
            started = true; lo = next = a; hi = b;
        }
        void row(U rank, U position) override {
            require(started && !done && rank == next && rank < hi, "witness exact row owner");
            rows[next++] = position;
        }
        void endShard() override {
            require(started && !done && next == hi, "witness range completion");
            done = true;
        }
    };
    std::vector<U> rows;
    std::vector<std::unique_ptr<Range>> ranges;
    RowWitness(U n, U p) : rows(n, n + 1) {
        for (U i = 0; i < p; ++i) ranges.emplace_back(new Range(rows));
    }
    U shardCount(U) const override { return ranges.size(); }
    U admission() const override { return ranges.size(); }
    SparseWalkSink* shardSink(U i) override { return ranges.at(i).get(); }
    void beginShard(U, U, U) override { fail("witness parent begin"); }
    void row(U, U) override { fail("witness parent row"); }
    void endShard() override { fail("witness parent end"); }
    U check(const std::vector<U>& want) {
        require(rows == want, "witness complete native brute row identity");
        U next = 0, seams = 0;
        for (auto& r : ranges) {
            require(r->started && r->done && r->lo == next, "witness no gaps or overlap");
            if (r->lo && r->lo < rows.size()) {
                require(rows[r->lo - 1] == want[r->lo - 1] && rows[r->lo] == want[r->lo],
                        "witness neighboring seam flanks");
                ++seams;
            }
            next = r->hi;
        }
        require(next == rows.size(), "witness global coverage");
        return seams;
    }
};

int main() {
    const std::string root = "/mnt/nvme2n1/erikg/sol7c-seams-" + std::to_string(getpid());
    fs::create_directories(root);
    U cases = 0, seams = 0;
    for (U p : {1, 4, 8, 16}) for (bool dollar : {false, true}) {
        std::string text;
        std::vector<SideExt> sides(8);
        for (U j = 0; j < sides.size(); ++j) {
            // Repeated local rotations force strict-position tie handling
            // and duplicated split fronts / empty ranges at large P.
            const std::string part = "\x1e\n\x1e";
            auto pos = brute_cyclic(part);
            auto& side = sides[j];
            side.n = part.size(); side.offset = text.size(); side.period = part.size();
            side.sxsPath = root + "/side-" + std::to_string(j) + ".sxs";
            side.ownsSxs = false; side.refText = false; side.posBase = 20 + side.n;
            fs::remove(side.sxsPath);
            {
                OutFile f(side.sxsPath);
                f.raw("SXS2", 4); f.w64(side.offset); f.w64(side.n);
                f.raw(part.data(), part.size()); f.raw(pos.data(), pos.size() * 8);
            }
            text += part;
        }
        PairScratch sc; sc.init(root + "/case", 0, sides.size());
        Budget budget; CoreStats stats;
        RowWitness sink(text.size(), p);
        core_merge_kway(sides, dollar, 16, budget, stats, sc, 4, false, &sink);
        seams += sink.check(dollar ? brute_dollar(text) : brute_cyclic(text));
        sc.cleanup(); ++cases;
    }
    fs::remove_all(root);
    std::printf("SHARD_ROW_SEAM_WITNESS_PASS cases=%llu seams=%llu P=1,4,8,16 modes=cyclic,dollar exact_rows=1 exact_flanks=1 gaps=0 duplicated_rows=0 production_bands=unvalidated\n",
                (unsigned long long)cases, (unsigned long long)seams);
}
