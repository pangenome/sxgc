// Brute-force oracle for bin-bwt v1 (operator shape): bin-concat order must
// equal the exact brute suffix/rotation order, in both modes, through the same
// bin_order code path the producer runs. Adversarial fixtures included.
#define main retired_merge_main
#include "../../bit6/cross_lcp_merge.cpp"
#undef main
#include <random>

// exact brute order of the REMAPPED byte string (remap applied before ordering)
static std::vector<U> brute(const std::string& M, bool dollar) {
    const U n = M.size();
    std::vector<std::pair<std::string, U>> v;
    v.reserve(n);
    for (U p = 0; p < n; ++p)
        v.push_back({dollar ? M.substr(p) : M.substr(p) + M.substr(0, p), p});
    std::sort(v.begin(), v.end());
    std::vector<U> o(n);
    for (U i = 0; i < n; ++i) o[i] = v[i].second;
    return o;
}

int main(int argc, char** argv) {
    require(argc == 2, "oracle SCRATCH");
    fs::create_directories(argv[1]);
    setenv("CROSS_POOL_SLOTS", "65536", 1);
    std::vector<std::string> fixtures = {
        std::string("\0", 1), std::string("\0\0", 2),
        std::string("\x00\x05\x1e\x00\x05", 5), std::string("\x1e\x0a\x1e", 3),
        std::string("a", 1), std::string("aa", 2),
        std::string(64, 'a'),
        std::string("abababababababababababababababababababababababababababababababab"),
        std::string("mississippimississippi"),
        std::string("aaaaabaaaabaaaabaaaabaaba"), std::string("\0\0\0\0\x05\0", 6),
        // nested repeats: a repeat inside a repeat, with separator breaks
        std::string("xyxyxyxyxyxyxyxyxy") + std::string("\x1e") +
            std::string("xyxyxyxyxyxyxyxyxy") + std::string("\x1e") + "xyxy",
        // max-byte / min-byte edges around the exhausted-prefix lattice
        std::string("\xff\xff\xfe\xff", 4), std::string("\xff\x00\xff\x00\xff", 5),
    };
    std::mt19937_64 rng(20251017);
    for (U z = 0; z < 120; ++z) {
        std::string t;
        U n = 1 + rng() % 220;
        for (U j = 0; j < n; ++j) t += char(rng() % ((z & 1) ? 256 : 4));
        fixtures.push_back(t);
    }
    // A real nonidentity production-style remap that MOVES bytes the fixtures
    // actually contain, so remapped order is genuinely tested.
    std::array<uint8_t, 256> remap{};
    for (unsigned c = 0; c < 256; ++c) remap[c] = uint8_t(c);
    std::swap(remap[0x00], remap[0x30]);
    std::swap(remap[0x05], remap[0x41]);
    std::swap(remap[0x0a], remap[0x62]);
    std::swap(remap[0x61], remap[0xfe]);
    std::swap(remap[0xff], remap[0x7f]);
    std::swap(remap[0x78], remap[0x79]);   // 'x' <-> 'y': flips periodic order
    std::swap(remap[0x1e], remap[0x20]);
    {
        auto sorted = remap;
        std::sort(sorted.begin(), sorted.end());
        for (unsigned c = 0; c < 256; ++c) require(sorted[c] == c, "oracle remap bijection");
    }
    U cases = 0, windows = 0, multiWindows = 0;
    for (U f = 0; f < fixtures.size(); ++f) {
        const std::string& t = fixtures[f];
        std::string M;
        for (unsigned char c : t) M += char(remap[c]);
        const std::string path = std::string(argv[1]) + "/oracle-corpus";
        TextSeg seg; seg.path = path; seg.len = t.size(); seg.remap = remap;
        { fs::remove(path); OutFile file(path); file.raw(t.data(), t.size()); }
        CorpusText text; text.open({seg}, true);
        for (bool dollar : {false, true}) {
            auto want = brute(M, dollar);
            U n = text.n;
            // Threads {1,3,8}, leaf {1,4,24}, depth cap {1,4,32}, level bytes
            // {1,2} and live caps {1,7,all} drive multi-window paths too.
            for (U threads : {1u, 3u, 8u})
                for (U leaf : {1u, 4u, 24u})
                    for (U dcap : {1u, 4u, 32u})
                        for (U lb : {1u, 2u})
                            for (U lc : std::array<U,3>{1, 7, U(1) << 40}) {
                                if (dcap < lb) continue;
                                BinWork w;
                                std::vector<U> got;
                                got.reserve(n);
                                bin_order(text, dollar, threads, leaf, dcap, lc, lb, w,
                                          [&](U row, U p) {
                                              require(row == got.size(), "oracle row continuity");
                                              got.push_back(p);
                                          });
                                require(got.size() == n, "oracle row coverage");
                                if (got != want) {
                                    std::fprintf(stderr,
                                        "BINBWT_ORACLE_FAIL fixture=%llu dollar=%u threads=%llu "
                                        "leaf=%llu dcap=%llu lb=%llu cap=%llu\n",
                                        (unsigned long long)f, unsigned(dollar),
                                        (unsigned long long)threads, (unsigned long long)leaf,
                                        (unsigned long long)dcap, (unsigned long long)lb,
                                        (unsigned long long)lc);
                                    for (U i = 0; i < n; ++i)
                                        std::fprintf(stderr, " got[%llu]=%llu want[%llu]=%llu\n",
                                            (unsigned long long)i, (unsigned long long)got[i],
                                            (unsigned long long)i, (unsigned long long)want[i]);
                                    return 1;
                                }
                                require(w.sweepBytes == (1 + w.windows) * n, "oracle sweep accounting");
                                require(w.emittedRows == n, "oracle emission accounting");
                                require(w.peakLiveSlots <= 2 * std::max<U>(lc, n),
                                        "oracle live position bound");
                                windows += w.windows;
                                if (w.windows > 1) ++multiWindows;
                                ++cases;
                            }
        }
    }
    std::printf("BINBWT_ORACLE_PASS cases=%llu fixtures=%zu modes=cyclic,dollar "
                "threads=1,3,8 leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 "
                "cap=1,7,all NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic=1 "
                "nested=1 single_byte=1 max_byte=1 nonidentity_remap=1 "
                "exact_brute_SA=1 multi_window_runs=%llu window_instances=%llu\n",
                (unsigned long long)cases, fixtures.size(),
                (unsigned long long)multiWindows, (unsigned long long)windows);
    return 0;
}
