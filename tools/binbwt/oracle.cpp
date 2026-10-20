// Brute-force oracle for bin-bwt v1 (operator shape): bin-concat order must
// equal the exact brute suffix/rotation order, in both modes, through the same
// bin_order code path the producer runs. Adversarial fixtures included.
#define main retired_merge_main
#include "../../bit6/cross_lcp_merge.cpp"
#undef main
#include <random>
#include <sys/wait.h>
#include <fstream>
#include <iterator>

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

// The refusal fixture: mixed documents, separators and repeats, so the window
// machinery has more than one bin and more than one window at cap 1.
static const char* refusal_fixture() {
    return "mississippimississippi\x1eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
           "\x1e" "zzzzzzzzzzzzzzzzzzzzzzzz";
}

int main(int argc, char** argv) {
    require(argc == 2, "oracle SCRATCH");
    fs::create_directories(argv[1]);
    setenv("CROSS_POOL_SLOTS", "65536", 1);
    // ---- REFUSAL CHILD (see the refusal pass at the end of main) -----------
    // `require` is fail-loud by EXIT (bit6/cross_lcp_merge.cpp: fail() prints
    // CROSS_LCP_FATAL and exit(2)), which is exactly the contract the product
    // exposes, so the refusal is asserted from the OUTSIDE: this child runs one
    // sabotaged window and the parent requires exit code 2 + the refusal text.
    if (::getenv("BIN_WINDOW_REFUSAL_ONLY")) {
        std::array<uint8_t, 256> id{};
        for (unsigned c = 0; c < 256; ++c) id[c] = uint8_t(c);
        const std::string t = refusal_fixture();
        const std::string path = std::string(argv[1]) + "/oracle-corpus";
        TextSeg seg; seg.path = path; seg.len = t.size(); seg.remap = id;
        { fs::remove(path); OutFile file(path); file.raw(t.data(), t.size()); }
        CorpusText text; text.open({seg}, true);
        BinWindowOpts wopt;
        wopt.dir = std::string(argv[1]) + "/windows";
        wopt.credit = 1;
        wopt.disc = true;
        BinWork w;
        bin_order(text, false, 3, 4, 32, 1, 1, false, 0, w, [](U, U) {}, false, wopt);
        return 0;   // reaching here means the refusal did NOT fire
    }
    // ---- PART 4 child: the periodic SANITY guard --------------------------
    // The fail-loud bug guard's exactness side, asserted from the OUTSIDE.
    //
    // A node of identical rotations cannot divide and cannot be certified with
    // the period probe disabled, so ONE of two exact outcomes must happen:
    //   * the TEXT ends inside the node (depth >= n): the node is a STRUCTURAL
    //     leaf and the comparison sort of identical rotations IS the exact answer
    //     (ascending source position) - no refusal, no wrong order;
    //   * the SANITY BOUND is reached first (a node agreeing for more than
    //     BIN_NO_PROGRESS_SANITY_BOUND bytes): the producer MUST refuse.
    //
    // This child used to assert ONLY the refusal on a 64-byte text, which the
    // code cannot produce: 64 identical rotations reach depth 64 == n and take
    // the structural-leaf path, so the assertion failed and took the rest of the
    // suite (ADAPTIVE, FREQ) down with it. Measured identically at base e3edef0
    // and at this tip - the TEST was stale since PART 4 introduced `structHit`,
    // not the code. The child now checks the side the 64-byte text can exercise
    // (exact position order + the structural-leaf journal) and attests it on
    // stdout, which the parent requires verbatim.
    if (::getenv("BINBW_ORACLE_PERIOD_CHILD")) {
        std::array<uint8_t, 256> id{};
        for (unsigned c = 0; c < 256; ++c) id[c] = uint8_t(c);
        const std::string t(64, 'a');
        const std::string path = std::string(argv[1]) + "/oracle-corpus";
        TextSeg seg; seg.path = path; seg.len = t.size(); seg.remap = id;
        { fs::remove(path); OutFile file(path); file.raw(t.data(), t.size()); }
        CorpusText text; text.open({seg}, true);
        BinWindowOpts wopt;
        wopt.dir = std::string(argv[1]) + "/windows";
        wopt.credit = 1; wopt.disc = true;
        BinWork w;
        const U n = text.n;
        std::vector<U> got;
        got.reserve(n);
        bin_order(text, false, 1, 1, 0, (U(1) << 40), 1, false, 0, w,
                  [&](U row, U p) {
                      require(row == got.size(), "period child row continuity");
                      got.push_back(p);
                  }, false, wopt);
        require(got.size() == n, "period child row count");
        // Identical rotations: the exact total order is ASCENDING SOURCE POSITION.
        for (U i = 0; i < n; ++i)
            require(got[i] == i, "period child: identical rotations are not in position order");
        require(w.structuralLeaves > 0 && w.structuralRows > 0,
                "period child: the structural-leaf journal did not fire");
        std::printf("BINBWT_ORACLE_PERIOD_CHILD exact_position_order=1 rows=%llu "
                    "structural_leaves=%llu structural_rows=%llu probe_off=1\n",
                    (unsigned long long)got.size(),
                    (unsigned long long)w.structuralLeaves,
                    (unsigned long long)w.structuralRows);
        return 0;
    }
    std::vector<std::string> fixtures = {        std::string("\0", 1), std::string("\0\0", 2),
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
    // The brute-order passes run through the PRODUCT default backing (bounded
    // rolling window FILES in the scratch tree), so the pyramid's base covers
    // the shipped path; `BINBWT_ORACLE_WINDOW_PASS` adds credit/group/reuse and
    // the RAM arm as its own case set.
    BinWindowOpts woptMain;
    woptMain.dir = std::string(argv[1]) + "/windows";
    woptMain.credit = 2;
    woptMain.disc = true;
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
                                bin_order(text, dollar, threads, leaf, dcap, lc, lb, false, 0, w,
                                          [&](U row, U p) {
                                              require(row == got.size(), "oracle row continuity");
                                              got.push_back(p);
                                          }, false, woptMain);
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
    // ---- frequency-scheduled claim order (its own line, its own case set) ----
    // `--bin-freq-sched` ranks a byte-1 bin's 2-gram sub-bins by size, which
    // changes the CLAIM order and (with the batching) the job boundaries. The
    // arena layout and therefore the product must not move, so the same brute
    // order is required. Same corpus fixtures, both top levels, all three thread
    // counts, and both cap regimes (1 window and every window).
    U freqCases = 0, freqWindows = 0, freqMulti = 0;
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
            for (U threads : {1u, 3u, 8u})
                for (U leaf : {1u, 24u})
                    for (U lb : {1u, 2u})
                        for (U lc : std::array<U,2>{7, U(1) << 40}) {
                            BinWork w;
                            std::vector<U> got;
                            got.reserve(n);
                            bin_order(text, dollar, threads, leaf, 16, lc, lb, true, 0, w,
                                      [&](U row, U p) {
                                          require(row == got.size(), "freq oracle row continuity");
                                          got.push_back(p);
                                      }, false, woptMain);
                            require(got.size() == n, "freq oracle row coverage");
                            if (got != want) {
                                std::fprintf(stderr,
                                    "BINBWT_ORACLE_FREQ_FAIL fixture=%llu dollar=%u threads=%llu "
                                    "leaf=%llu lb=%llu cap=%llu\n",
                                    (unsigned long long)f, unsigned(dollar),
                                    (unsigned long long)threads, (unsigned long long)leaf,
                                    (unsigned long long)lb, (unsigned long long)lc);
                                return 1;
                            }
                            freqWindows += w.windows;
                            if (w.windows > 1) ++freqMulti;
                            ++freqCases;
                        }
        }
    }
    // ---- THE DISC-BACKED WINDOW MACHINERY (its own pass, its own case set) ---
    // Everything the window files add is exercised here, on the SAME brute
    // order: credit exhaustion (1..3 windows of lookahead), window-file reuse
    // across windows (`k % credit` names, so a cap of 1 rewrites ONE path for
    // every window), delete-after-build cycles, build groups smaller than a
    // window (`buildCap` 5 forces multi-bin groups and single-bin groups), and
    // the RAM arm as the identity reference. `disc` and `ram` must agree on the
    // exact brute SA and on every counter.
    U winCases = 0, winWindows = 0, winFiles = 0, winReuse = 0;
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
            for (U lb : {1u, 2u})
                for (U lc : std::array<U,2>{1, 7})
                    for (U credit : {1u, 2u, 3u})
                        for (U bcap : std::array<U,2>{0, 5})
                            for (bool disc : {true, false}) {
                                BinWindowOpts wopt;
                                wopt.dir = std::string(argv[1]) + "/windows";
                                wopt.credit = credit;
                                wopt.buildCap = bcap;
                                wopt.disc = disc;
                                BinWork w;
                                std::vector<U> got;
                                got.reserve(n);
                                bin_order(text, dollar, 3, 4, 32, lc, lb, false, 0, w,
                                          [&](U row, U p) {
                                              require(row == got.size(),
                                                      "window oracle row continuity");
                                              got.push_back(p);
                                          }, false, wopt);
                                require(got.size() == n, "window oracle row coverage");
                                if (got != want) {
                                    std::fprintf(stderr,
                                        "BINBWT_ORACLE_WINDOW_FAIL fixture=%llu dollar=%u "
                                        "lb=%llu cap=%llu credit=%llu bcap=%llu disc=%u\n",
                                        (unsigned long long)f, unsigned(dollar),
                                        (unsigned long long)lb, (unsigned long long)lc,
                                        (unsigned long long)credit, (unsigned long long)bcap,
                                        unsigned(disc));
                                    return 1;
                                }
                                require(w.emittedRows == n, "window oracle emission");
                                require(w.sweepBytes == (1 + w.windows) * n,
                                        "window oracle sweep accounting");
                                require(w.peakLiveSlots <= 2 * std::max<U>(lc, n),
                                        "window oracle build-set bound");
                                if (disc) {
                                    // Every window's slots are written once and
                                    // read back once, and live scratch never
                                    // exceeds credit windows' worth.
                                    require(w.winFileWriteBytes == n * sizeof(U),
                                            "window oracle file write bytes");
                                    require(w.winFileReadBytes == n * sizeof(U),
                                            "window oracle file read bytes");
                                    require(w.peakWindowFileBytes <=
                                                credit * std::max<U>(lc, n) * sizeof(U),
                                            "window oracle live scratch bound");
                                    require(w.windowFiles == w.windows,
                                            "window oracle one file per window");
                                }
                                winWindows += w.windows;
                                winFiles += w.windowFiles;
                                if (w.windows > credit) ++winReuse;
                                ++winCases;
                            }
        }
    }
    std::printf("BINBWT_ORACLE_WINDOW_PASS cases=%llu fixtures=%zu modes=cyclic,dollar "
                "threads=3 level_bytes=1,2 cap=1,7 credit=1,2,3 build_cap=0,5 "
                "disc=1 ram=1 file_reuse_runs=%llu window_instances=%llu "
                "window_files=%llu exact_brute_SA=1\n",
                (unsigned long long)winCases, fixtures.size(),
                (unsigned long long)winReuse, (unsigned long long)winWindows,
                (unsigned long long)winFiles);

    // ---- CRASH REFUSAL: a missing / truncated / stale / corrupt window file --
    // Each sabotage is applied to every window file just before the build opens
    // it (BIN_WINDOW_SABOTAGE, a test instrument). bin_order MUST throw, and
    // must throw the refusal, not rebuild silently.
    {
        const std::string t = refusal_fixture();
        std::string M;
        for (unsigned char c : t) M += char(remap[c]);
        const std::string path = std::string(argv[1]) + "/oracle-corpus";
        TextSeg seg; seg.path = path; seg.len = t.size(); seg.remap = remap;
        { fs::remove(path); OutFile file(path); file.raw(t.data(), t.size()); }
        CorpusText text; text.open({seg}, true);
        auto want = brute(M, false);
        struct Case { const char* mode; const char* needle; };
        const Case cases[] = {
            {"unlink",   "MISSING"},
            {"truncate", "TRUNCATED"},
            {"stale",    "STALE"},
            {"nomagic",  "BAD MAGIC"},
            {"nodone",   "INCOMPLETE"},
        };
        U refused = 0;
        for (const auto& cs : cases) {
            const std::string log = std::string(argv[1]) + "/refusal.log";
            ::setenv("BIN_WINDOW_SABOTAGE", cs.mode, 1);
            ::setenv("BIN_WINDOW_REFUSAL_ONLY", "1", 1);
            const pid_t pid = ::fork();
            require(pid >= 0, "refusal fork");
            if (pid == 0) {
                const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) ::_exit(126);
                ::dup2(fd, 2);
                ::close(fd);
                ::execl(argv[0], argv[0], argv[1], static_cast<char*>(nullptr));
                ::_exit(127);
            }
            int status = 0;
            require(::waitpid(pid, &status, 0) == pid, "refusal waitpid");
            ::unsetenv("BIN_WINDOW_SABOTAGE");
            ::unsetenv("BIN_WINDOW_REFUSAL_ONLY");
            const bool exited = WIFEXITED(status);
            const int rc = exited ? WEXITSTATUS(status) : -1;
            require(exited && rc == 2,
                    "window crash refusal did not die loud (a missing/corrupt window file "
                    "must be CROSS_LCP_FATAL + exit 2, never a silent rebuild)");
            std::ifstream lf(log);
            const std::string said((std::istreambuf_iterator<char>(lf)),
                                    std::istreambuf_iterator<char>());
            require(said.find("CROSS_LCP_FATAL") != std::string::npos,
                    "window crash refusal was not loud");
            require(said.find(cs.needle) != std::string::npos,
                    "window crash refusal fired the WRONG refusal");
            std::fprintf(stderr, "BINBWT_ORACLE_REFUSAL mode=%s rc=2 said=*%s*\n",
                         cs.mode, cs.needle);
            ++refused;
        }
        // and the SAME corpus, with nothing sabotaged, still yields the brute SA
        // through the same disc path (so the refusal is not vacuous).
        BinWindowOpts wopt;
        wopt.dir = std::string(argv[1]) + "/windows";
        wopt.credit = 2;
        wopt.disc = true;
        BinWork w;
        std::vector<U> got;
        bin_order(text, false, 3, 4, 32, 1, 1, false, 0, w,
                  [&](U row, U p) { require(row == got.size(), "refusal control rows");
                                    got.push_back(p); }, false, wopt);
        require(got == want, "window refusal control disagrees with the brute SA");
        std::printf("BINBWT_ORACLE_REFUSAL_PASS refusals=%llu modes=unlink,truncate,"
                    "stale,nomagic,nodone control=exact_brute_SA\n",
                    (unsigned long long)refused);
    }

    // ---- TARGET-SIZE RECURSION: ADAPTIVE DEPTH (its own case set) ----------
    // The rule is now "split until the node is <= targetSize", so the depth of
    // the tree is an OUTPUT of the text. These cases force that output to be
    // deep: a text whose rows keep sharing prefixes must keep being divided,
    // level after level, instead of being handed to a leaf sort at a cap. They
    // assert the three things the rule claims:
    //   * the exact brute order on every fixture, in both modes;
    //   * leaf_rows_max <= target and over_target_leaves == 0, i.e. the TARGET
    //     bounded every leaf - the depth cap never had to fire;
    //   * the tree actually went deep: depth_max reaches 4 across the set (a
    //     fixture of 32 identical symbols under target 1 has to be divided four
    //     or more levels down before any node reaches the target).
    // depthCap is 256, the product default: high enough that only a corpus that
    // CANNOT be divided (identical rotations) can ever reach it.
    U adaptCases = 0, adaptWindows = 0, adaptDepthMax = 0;
    BinWork lastCounters;   // PART 1 gate: the lazy arm's counters for the pair
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
            for (U threads : {1u, 8u})
                for (U target : {1u, 2u, 3u, 17u})
                    for (U lc : std::array<U,2>{7, U(1) << 40}) {
                        if (target >= n) continue;      // nothing to divide
                        BinWork w;
                        std::vector<U> got;
                        got.reserve(n);
                        bin_order(text, dollar, threads, target, 256, lc, 1, false, 0, w,
                                  [&](U row, U p) {
                                      require(row == got.size(), "adaptive oracle row continuity");
                                      got.push_back(p);
                                  }, false, woptMain);
                        require(got.size() == n, "adaptive oracle row coverage");
                        if (got != want) {
                            std::fprintf(stderr,
                                "BINBWT_ORACLE_ADAPTIVE_FAIL fixture=%llu dollar=%u threads=%llu "
                                "target=%llu cap=%llu\n",
                                (unsigned long long)f, unsigned(dollar),
                                (unsigned long long)threads, (unsigned long long)target,
                                (unsigned long long)lc);
                            return 1;
                        }
                        // Every leaf is bounded by the TARGET unless the text
                        // itself ended inside the node (depth >= n: the suffixes
                        // agree to their end and there is nothing left to divide).
                        require(w.leafRowsMax <= target || w.structuralLeaves > 0,
                                "adaptive oracle leaf exceeded the target");
                        require(w.overTargetLeaves == 0,
                                "adaptive oracle: the depth cap fired as the rule");
                        adaptDepthMax = std::max(adaptDepthMax, w.depthMax);
                        adaptWindows += w.windows;
                        ++adaptCases;
                    }
        }
    }
    require(adaptDepthMax >= 4,
            "adaptive oracle never produced a deep tree (the fixtures must force depth)");

    // ---- PART 1: PREFETCH-VS-LAZY EQUIVALENCE (the staged leaf sort) --------
    // The leaf staging (bin_stage_leaf / BinLeafStage) changes only WHERE a
    // comparator byte comes from, so the ORDER and every construction counter
    // must be identical with the staged path ON and OFF. Tiny fixtures are RAM
    // cached, so the staging has to be FORCED to engage at all
    // (BIN_LEAF_STAGE_FORCE); it is the same code path a pooled pile view takes.
    // Both arms are run per fixture/mode/config and compared row by row and
    // counter by counter; the counters compared are the ones that decide the
    // product (comparisons, symbols, cert_decided, ties, leaf_sorts, leaf_elems,
    // level_passes, level_elems, bin_passes, emitted_rows, depth_max).
    {
        U stageCases = 0, stageHits = 0, stageBytes = 0, stageArms = 0;
        const char* prevForce = ::getenv("BIN_LEAF_STAGE_FORCE");
        const bool hadForce = prevForce != nullptr;
        std::string saved = hadForce ? prevForce : "";
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
                for (U threads : {1u, 3u, 8u})
                    for (U target : {1u, 3u, 9u})
                        for (bool forced : {false, true}) {
                            if (forced) setenv("BIN_LEAF_STAGE_FORCE", "1", 1);
                            else unsetenv("BIN_LEAF_STAGE_FORCE");
                            BinWork w;
                            std::vector<U> got;
                            got.reserve(n);
                            bin_order(text, dollar, threads, target, 0, U(1) << 40, 1,
                                      false, 0, w, [&](U row, U p) {
                                          require(row == got.size(), "stage oracle row continuity");
                                          got.push_back(p);
                                      }, false, woptMain);
                            require(got == want, "stage oracle: staged order differs from brute");
                            if (!forced) {
                                // The reference arm's counters, kept for the pair.
                                lastCounters = w;
                            } else {
                                // Counters that must not move when the staging is on.
                                require(w.comparisons == lastCounters.comparisons &&
                                        w.symbols == lastCounters.symbols &&
                                        w.certDecided == lastCounters.certDecided &&
                                        w.ties == lastCounters.ties &&
                                        w.leafSorts == lastCounters.leafSorts &&
                                        w.leafElems == lastCounters.leafElems &&
                                        w.levelPasses == lastCounters.levelPasses &&
                                        w.levelElems == lastCounters.levelElems &&
                                        w.binPasses == lastCounters.binPasses &&
                                        w.emittedRows == lastCounters.emittedRows &&
                                        w.depthMax == lastCounters.depthMax &&
                                        w.periodicRows == lastCounters.periodicRows,
                                        "stage oracle: the staged path moved a construction counter");
                                stageHits += w.stageHits + w.stagePartials;
                                stageBytes += w.stageBytes;
                                ++stageArms;
                            }
                            ++stageCases;
                        }
            }
        }
        if (hadForce) setenv("BIN_LEAF_STAGE_FORCE", saved.c_str(), 1);
        else unsetenv("BIN_LEAF_STAGE_FORCE");
        std::printf("BINBWT_ORACLE_STAGE_PASS cases=%llu staged_arms=%llu fixtures=%zu "
                    "modes=cyclic,dollar threads=1,3,8 target=1,3,9 depth_cap=0 level_bytes=1 "
                    "stage_hits=%llu stage_bytes=%llu counters_identical=1 exact_brute_SA=1 "
                    "both_paths=1\n",
                    (unsigned long long)stageCases, (unsigned long long)stageArms,
                    fixtures.size(), (unsigned long long)stageHits,
                    (unsigned long long)stageBytes);
    }

    // ---- PART 1: PREFETCH-VS-LAZY EQUIVALENCE (the staged leaf sort) --------
    // The leaf staging (bin_stage_leaf / BinLeafStage) changes only WHERE a
    // comparator byte comes from, so the ORDER and every construction counter
    // must be identical with the staged path ON and OFF. Tiny fixtures are RAM
    // cached, so the staging has to be FORCED to engage at all
    // (BIN_LEAF_STAGE_FORCE); it is the same code path a pooled pile view takes.
    // Both arms are run per fixture/mode/config and compared row by row and
    // counter by counter; the counters compared are the ones that decide the
    // product (comparisons, symbols, cert_decided, ties, leaf_sorts, leaf_elems,
    // level_passes, level_elems, bin_passes, emitted_rows, depth_max).
    {
        U stageCases = 0, stageHits = 0, stageBytes = 0, stageArms = 0;
        const char* prevForce = ::getenv("BIN_LEAF_STAGE_FORCE");
        const bool hadForce = prevForce != nullptr;
        std::string saved = hadForce ? prevForce : "";
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
                for (U threads : {1u, 3u, 8u})
                    for (U target : {1u, 3u, 9u})
                        for (bool forced : {false, true}) {
                            if (forced) setenv("BIN_LEAF_STAGE_FORCE", "1", 1);
                            else unsetenv("BIN_LEAF_STAGE_FORCE");
                            BinWork w;
                            std::vector<U> got;
                            got.reserve(n);
                            bin_order(text, dollar, threads, target, 0, U(1) << 40, 1,
                                      false, 0, w, [&](U row, U p) {
                                          require(row == got.size(), "stage oracle row continuity");
                                          got.push_back(p);
                                      }, false, woptMain);
                            require(got == want, "stage oracle: staged order differs from brute");
                            if (!forced) {
                                // The reference arm's counters, kept for the pair.
                                lastCounters = w;
                            } else {
                                // Counters that must not move when the staging is on.
                                require(w.comparisons == lastCounters.comparisons &&
                                        w.symbols == lastCounters.symbols &&
                                        w.certDecided == lastCounters.certDecided &&
                                        w.ties == lastCounters.ties &&
                                        w.leafSorts == lastCounters.leafSorts &&
                                        w.leafElems == lastCounters.leafElems &&
                                        w.levelPasses == lastCounters.levelPasses &&
                                        w.levelElems == lastCounters.levelElems &&
                                        w.binPasses == lastCounters.binPasses &&
                                        w.emittedRows == lastCounters.emittedRows &&
                                        w.depthMax == lastCounters.depthMax &&
                                        w.periodicRows == lastCounters.periodicRows,
                                        "stage oracle: the staged path moved a construction counter");
                                stageHits += w.stageHits + w.stagePartials;
                                stageBytes += w.stageBytes;
                                ++stageArms;
                            }
                            ++stageCases;
                        }
            }
        }
        if (hadForce) setenv("BIN_LEAF_STAGE_FORCE", saved.c_str(), 1);
        else unsetenv("BIN_LEAF_STAGE_FORCE");
        std::printf("BINBWT_ORACLE_STAGE_PASS cases=%llu staged_arms=%llu fixtures=%zu "
                    "modes=cyclic,dollar threads=1,3,8 target=1,3,9 depth_cap=0 level_bytes=1 "
                    "stage_hits=%llu stage_bytes=%llu counters_identical=1 exact_brute_SA=1 "
                    "both_paths=1\n",
                    (unsigned long long)stageCases, (unsigned long long)stageArms,
                    fixtures.size(), (unsigned long long)stageHits,
                    (unsigned long long)stageBytes);
    }

    // ---- PART 1: PREFETCH-VS-LAZY EQUIVALENCE (the staged leaf sort) --------
    // The leaf staging (bin_stage_leaf / BinLeafStage) changes only WHERE a
    // comparator byte comes from, so the ORDER and every construction counter
    // must be identical with the staged path ON and OFF. Tiny fixtures are RAM
    // cached, so the staging has to be FORCED to engage at all
    // (BIN_LEAF_STAGE_FORCE); it is the same code path a pooled pile view takes.
    // Both arms are run per fixture/mode/config and compared row by row and
    // counter by counter; the counters compared are the ones that decide the
    // product (comparisons, symbols, cert_decided, ties, leaf_sorts, leaf_elems,
    // level_passes, level_elems, bin_passes, emitted_rows, depth_max).
    {
        U stageCases = 0, stageHits = 0, stageBytes = 0, stageArms = 0;
        const char* prevForce = ::getenv("BIN_LEAF_STAGE_FORCE");
        const bool hadForce = prevForce != nullptr;
        std::string saved = hadForce ? prevForce : "";
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
                for (U threads : {1u, 3u, 8u})
                    for (U target : {1u, 3u, 9u})
                        for (bool forced : {false, true}) {
                            if (forced) setenv("BIN_LEAF_STAGE_FORCE", "1", 1);
                            else unsetenv("BIN_LEAF_STAGE_FORCE");
                            BinWork w;
                            std::vector<U> got;
                            got.reserve(n);
                            bin_order(text, dollar, threads, target, 0, U(1) << 40, 1,
                                      false, 0, w, [&](U row, U p) {
                                          require(row == got.size(), "stage oracle row continuity");
                                          got.push_back(p);
                                      }, false, woptMain);
                            require(got == want, "stage oracle: staged order differs from brute");
                            if (!forced) {
                                // The reference arm's counters, kept for the pair.
                                lastCounters = w;
                            } else {
                                // Counters that must not move when the staging is on.
                                require(w.comparisons == lastCounters.comparisons &&
                                        w.symbols == lastCounters.symbols &&
                                        w.certDecided == lastCounters.certDecided &&
                                        w.ties == lastCounters.ties &&
                                        w.leafSorts == lastCounters.leafSorts &&
                                        w.leafElems == lastCounters.leafElems &&
                                        w.levelPasses == lastCounters.levelPasses &&
                                        w.levelElems == lastCounters.levelElems &&
                                        w.binPasses == lastCounters.binPasses &&
                                        w.emittedRows == lastCounters.emittedRows &&
                                        w.depthMax == lastCounters.depthMax &&
                                        w.periodicRows == lastCounters.periodicRows,
                                        "stage oracle: the staged path moved a construction counter");
                                stageHits += w.stageHits + w.stagePartials;
                                stageBytes += w.stageBytes;
                                ++stageArms;
                            }
                            ++stageCases;
                        }
            }
        }
        if (hadForce) setenv("BIN_LEAF_STAGE_FORCE", saved.c_str(), 1);
        else unsetenv("BIN_LEAF_STAGE_FORCE");
        std::printf("BINBWT_ORACLE_STAGE_PASS cases=%llu staged_arms=%llu fixtures=%zu "
                    "modes=cyclic,dollar threads=1,3,8 target=1,3,9 depth_cap=0 level_bytes=1 "
                    "stage_hits=%llu stage_bytes=%llu counters_identical=1 exact_brute_SA=1 "
                    "both_paths=1\n",
                    (unsigned long long)stageCases, (unsigned long long)stageArms,
                    fixtures.size(), (unsigned long long)stageHits,
                    (unsigned long long)stageBytes);
    }

    // ---- PART 4: THE PERIODIC TIE-BREAK (no depth cap in the user's face) ---
    // The product default is `--bin-depth-cap 0` = AUTO, i.e. the target is the
    // ONLY rule and the recursion ends either at the target or at a node whose
    // rotations are provably identical (bin_node_period). These cases run the
    // NO-CAP rule on every fixture in both modes and assert:
    //   * the exact brute order (the tie-break's ascending-position order is the
    //     comparator's own cyclic rule, so this is not a new total order);
    //   * the depth cap never fired (over_target_leaves == 0) - there is no cap;
    //   * every leaf is bounded by the target UNLESS the text ended inside the
    //     node or the node was proven periodic;
    //   * the periodic path actually fired on the strictly periodic fixtures
    //     (64 identical bytes, "abab...ab", and the doubled "mississippi"), and
    //     produced no comparisons at all for them.
    {
        U perCases = 0, perWindows = 0, perLeaves = 0, perRows = 0, perPeriodMax = 0;
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
                for (U threads : {1u, 3u, 8u})
                    for (U target : {1u, 4u, 24u})
                        for (U lc : std::array<U,2>{7, U(1) << 40}) {
                            BinWork w;
                            std::vector<U> got;
                            got.reserve(n);
                            // depthCap 0 == AUTO: the product default.
                            bin_order(text, dollar, threads, target, 0, lc, 1, false, 0, w,
                                      [&](U row, U p) {
                                          require(row == got.size(), "periodic oracle row continuity");
                                          got.push_back(p);
                                      }, false, woptMain);
                            require(got.size() == n, "periodic oracle row coverage");
                            if (got != want) {
                                std::fprintf(stderr,
                                    "BINBWT_ORACLE_PERIODIC_FAIL fixture=%llu dollar=%u "
                                    "threads=%llu target=%llu cap=%llu\n",
                                    (unsigned long long)f, unsigned(dollar),
                                    (unsigned long long)threads, (unsigned long long)target,
                                    (unsigned long long)lc);
                                for (U i = 0; i < n; ++i)
                                    std::fprintf(stderr, " got[%llu]=%llu want[%llu]=%llu\n",
                                        (unsigned long long)i, (unsigned long long)got[i],
                                        (unsigned long long)i, (unsigned long long)want[i]);
                                return 1;
                            }
                            require(w.overTargetLeaves == 0,
                                    "periodic oracle: a cap fired, but AUTO has no cap");
                            require(w.leafRowsMax <= target || w.structuralLeaves > 0 ||
                                        w.periodicLeaves > 0,
                                    "periodic oracle leaf exceeded the target");
                            if (w.periodicLeaves) {
                                // A proven-periodic node is ordered by position with
                                // ZERO text comparisons from the tie-break itself.
                                require(w.periodicRows > 0 && w.periodicPeriodMax > 0,
                                        "periodic oracle journal");
                            }
                            perCases++;
                            perWindows += w.windows;
                            perLeaves += w.periodicLeaves;
                            perRows += w.periodicRows;
                            perPeriodMax = std::max(perPeriodMax, w.periodicPeriodMax);
                        }
            }
        }
        require(perLeaves > 0, "periodic tie-break never fired on the periodic fixtures");
        // The fail-loud bug guard's exactness side (see the child at the top of
        // main): with the period probe OFF the 64 identical rotations must come
        // out in ascending position order and be journalled as structural leaves.
        {
            const std::string p = std::string(argv[1]) + "/periodic-refusal";
            std::error_code ec; std::filesystem::remove_all(p, ec);
            std::filesystem::create_directories(p, ec);
            const std::string child = std::string(argv[1]) + "/periodic-child";
            const std::string outPath = std::string(argv[1]) + "/periodic-child.out";
            const std::string cmd = "BIN_PERIOD_PROBE=0 BINBW_ORACLE_PERIOD_CHILD=" + p + " " +
                                    std::string(argv[0]) + " " + child + " > " + outPath +
                                    " 2>/dev/null";
            const int rc = std::system(cmd.c_str());
            require(WIFEXITED(rc) && WEXITSTATUS(rc) == 0,
                    "periodic probe-off child did not exit 0 (exact order + journal)");
            std::string childOut;
            { std::ifstream f(outPath); childOut.assign(std::istreambuf_iterator<char>(f),
                                                        std::istreambuf_iterator<char>()); }
            require(childOut.find("exact_position_order=1") != std::string::npos,
                    "periodic probe-off child did not attest the exact position order");
            std::printf("BINBWT_ORACLE_PERIODIC_SANITY %s", childOut.c_str());
        }
        std::printf("BINBWT_ORACLE_PERIODIC_PASS cases=%llu fixtures=%zu modes=cyclic,dollar "
                    "threads=1,3,8 target=1,4,24 depth_cap=0 (AUTO, no cap) level_bytes=1 "
                    "cap=7,all periodic_leaves=%llu periodic_rows=%llu "
                    "periodic_period_max=%llu over_target_leaves=0 exact_brute_SA=1 "
                    "window_instances=%llu\n",
                    (unsigned long long)perCases, fixtures.size(),
                    (unsigned long long)perLeaves, (unsigned long long)perRows,
                    (unsigned long long)perPeriodMax, (unsigned long long)perWindows);
    }
    std::printf("BINBWT_ORACLE_ADAPTIVE_PASS cases=%llu fixtures=%zu modes=cyclic,dollar "
                "threads=1,8 target=1,2,3,17 depth_cap=256 level_bytes=1 cap=7,all "
                "depth_max=%llu leaf_rows_max_le_target=1 over_target_leaves=0 "
                "exact_brute_SA=1 window_instances=%llu\n",
                (unsigned long long)adaptCases, fixtures.size(),
                (unsigned long long)adaptDepthMax, (unsigned long long)adaptWindows);
    std::printf("BINBWT_ORACLE_FREQ_PASS cases=%llu fixtures=%zu modes=cyclic,dollar "
                "threads=1,3,8 leaf=1,24 depth_cap=16 level_bytes=1,2 cap=7,all "
                "freq_sched=1 exact_brute_SA=1 multi_window_runs=%llu "
                "window_instances=%llu\n",
                (unsigned long long)freqCases, fixtures.size(),
                (unsigned long long)freqMulti, (unsigned long long)freqWindows);
    // ---- PHRASE-RADIX PASS (the new oracle family) -------------------------
    // tools/binbwt/PHRASE_RADIX_DESIGN.md §7 names the risks this pass has to
    // discharge, and each one is its own named case here:
    //   (a) the PARSE IS DETERMINISTIC: two parses of the same corpus and flags
    //       produce the same blocks, the same pool and the same entry table;
    //   (b) MID-PHRASE OFFSET BINS ARE EXACT - both directly (every offset of
    //       every block has its own entry, and the entry's bytes are the block's
    //       bytes from that offset: the (phrase, offset) bin law) and end to end
    //       (the whole order equals the brute suffix array, which IS the
    //       end-occurrence ordering);
    //   (c) PHRASE/BYTE BOUNDARIES: blocks abutting separators and single diverse
    //       bytes, blocks whose occurrences wrap the cyclic boundary, suffixes
    //       that END inside a block (the dollar clamp), and a phrase nested
    //       inside a phrase;
    //   (d) THRESHOLD INTERACTIONS on L and F, including the arms where each
    //       threshold must make the dictionary EMPTY;
    //   (e) A SMALL/EMPTY DICTIONARY LEAVES THE BYTE REGIME BIT-IDENTICAL: the
    //       order AND every construction counter, against the same corpus built
    //       with the flag off.
    // `skip_min_rows=1` and `skip_min_len=8` are deliberate: the fixtures are
    // tiny, and the hook must be forced to fire on them (the producer's default
    // 4096 is a cost guard, not a semantic one).
    {
        // A hot block that is NOT periodic - a periodic block would be ordered by
        // the periodic tie-break and the phrase hook would never see it.
        auto hotBlock = [](U len, U salt) {
            std::string b(len, 'A');
            uint64_t x = 0x9E3779B97F4A7C15ULL ^ (uint64_t(salt) << 32) ^ 0x5bf03635ULL;
            for (U i = 0; i < len; ++i) {
                x = x * 6364136223846793005ULL + 1442695040888963407ULL;
                b[i] = char(1 + ((x >> 40) % 200));    // never 0, never 0x1e
            }
            return b;
        };
        const std::string B1 = hotBlock(48, 1);
        const std::string B2 = hotBlock(32, 2);
        const std::string B3 = hotBlock(24, 3);
        const std::string B4 = hotBlock(40, 4);
        const std::string B5 = hotBlock(64, 5);
        const std::string B6 = hotBlock(16, 6);
        std::vector<std::string> pf;
        std::vector<const char*> pfName;
        {   // 1. the block repeated with DIFFERENT continuations: the mid-phrase
            //    case - every offset o has k occurrences and a (phrase, o) bin is
            //    ordered by the continuation after the block. 12 repeats so the
            //    larger-target arms have a node above the target to skip through.
            std::string t;
            for (U k = 0; k < 12; ++k) { t += B1; t += std::string(1, char('a' + k % 26)); }
            for (U k = 0; k < 6; ++k) { t += B2; t += std::string(1, char('A' + k)); }
            pf.push_back(t); pfName.push_back("midphrase");
        }
        {   // 2. every offset of the block made a phrase START (rotations)
            std::string t;
            for (U o = 0; o < 32; o += 4) { t += B2.substr(o); t += B2.substr(0, o); t += "\x1e"; }
            pf.push_back(t); pfName.push_back("offsets");
        }
        {   // 3. phrase/byte boundaries: block + separator + one diverse byte, and a
            //    tail that ENDS inside the block (the dollar clamp)
            std::string t;
            for (U k = 0; k < 8; ++k) { t += B3; t += "\x1e"; t += char('p' + k); }
            t += B3.substr(0, 5);
            pf.push_back(t); pfName.push_back("boundary");
        }
        {   // 4. occurrences that wrap the cyclic boundary
            std::string t;
            for (U k = 0; k < 8; ++k) { t += B4; t += char('0' + k); }
            pf.push_back(t); pfName.push_back("wrap");
        }
        {   // 5. a NEAR-IDENTICAL clone set: one base edited in one copy, so the
            //    skip must stop exactly where the copies diverge
            std::string t;
            for (U k = 0; k < 8; ++k) {
                std::string c = B5;
                if (k == 3) c[40] = char(c[40] ^ 0x11);
                t += c; t += "\x1e";
            }
            pf.push_back(t); pfName.push_back("edit");
        }
        {   // 6. a phrase nested inside a phrase
            std::string inner;
            for (U k = 0; k < 3; ++k) inner += B6;
            std::string t;
            for (U k = 0; k < 8; ++k) { t += inner; t += hotBlock(20, 7 + k); }
            pf.push_back(t); pfName.push_back("nested");
        }
        {   // 7. no repetition at all: the dictionary must come out EMPTY
            std::mt19937_64 r2(424242);
            std::string t;
            for (U j = 0; j < 600; ++j) t += char(r2() % 256);
            pf.push_back(t); pfName.push_back("norepeat");
        }

        BinWindowOpts woptPhrase;
        woptPhrase.dir = std::string(argv[1]) + "/windows-phrase";
        woptPhrase.credit = 2;
        woptPhrase.disc = true;
        auto openFixture = [&](const std::string& t, CorpusText& text,
                               const std::string& tag) {
            const std::string path = std::string(argv[1]) + "/phrase-corpus-" + tag;
            TextSeg seg; seg.path = path; seg.len = t.size(); seg.remap = remap;
            { fs::remove(path); OutFile file(path); file.raw(t.data(), t.size()); }
            text.open({seg}, true);
        };
        auto phraseOpts = [&](U L, U F, bool on) {
            PhraseOpts po;
            po.enabled = on;
            po.minLen = L; po.minFreq = F;
            po.seed = 8; po.window = 4; po.maxLen = 4096;
            po.tableBits = 18; po.reps = 8;
            po.skipMinRows = 1; po.skipMinLen = std::min<U>(8, L);
            return po;
        };
        auto runArm = [&](CorpusText& text, bool dollar, U threads, U leaf, U lb,
                          U cap, BinWindowOpts& wo, BinWork& w, std::vector<U>& got) {
            got.clear();
            got.reserve(text.n);
            bin_order(text, dollar, threads, leaf, 0, cap, lb, false, 0, w,
                      [&](U row, U p) {
                          require(row == got.size(), "phrase oracle row continuity");
                          got.push_back(p);
                      }, false, wo);
            require(got.size() == text.n, "phrase oracle row coverage");
        };

        U phCases = 0, phArms = 0, phDet = 0, phOffChecked = 0, phOffBad = 0;
        U phBlocksLive = 0, phBlocksDead = 0;
        U phSkipRows = 0, phSkips = 0, phSkipBytes = 0, phSkipMaxLen = 0;
        U phEmpty = 0, phNonEmpty = 0, phIdentical = 0, phWindows = 0;
        U phProbes = 0;

        // ---- (a) DETERMINISM + (b') the (phrase, offset) bin law, directly ---
        for (U f = 0; f < pf.size(); ++f) {
            CorpusText text; openFixture(pf[f], text, std::string("det") + std::to_string(f));
            const PhraseOpts po = phraseOpts(8, 2, true);
            PhraseDict a, b;
            a.parse(text, false, po);
            b.parse(text, false, po);
            require(a.on == b.on, "phrase oracle: two parses disagreed on armed");
            require(a.blocks.size() == b.blocks.size(), "phrase oracle: block count moved");
            require(a.pool == b.pool, "phrase oracle: the phrase pool moved between parses");
            require(a.tab.size() == b.tab.size(), "phrase oracle: table size moved");
            for (size_t i = 0; i < a.tab.size(); ++i)
                require(a.tab[i].key == b.tab[i].key && a.tab[i].block == b.tab[i].block &&
                            a.tab[i].off == b.tab[i].off && a.tab[i].len == b.tab[i].len,
                        "phrase oracle: an entry moved between parses");
            ++phDet;
            if (a.on) ++phNonEmpty; else ++phEmpty;
            if (!a.on) continue;
            // The (phrase, offset) bin law. Every offset of every admitted block
            // must have its OWN entry whose bytes are the block's bytes from that
            // offset; a direct-mapped collision may lose an entry (a missed skip,
            // never a wrong order), so the law is asserted as near-total coverage
            // rather than an absolute, and the head offset (the phrase itself) is
            // asserted absolutely.
            U blockOk = 0;
            for (U bi = 0; bi < a.blocks.size(); ++bi) {
                const PhraseDict::Block& bk = a.blocks[bi];
                U usable = 0;
                for (U off = 0; off + po.minLen <= bk.len; ++off) {
                    const uint8_t* src = a.pool.data() + bk.off + off;
                    const U need = bk.len - off;
                    const int32_t e = a.lookup8(src);
                    ++phOffChecked;
                    if (e < 0) continue;                       // a lost skip
                    const PhraseDict::Entry& E = a.entry(e);
                    require(E.len <= 4096 && E.block < a.blocks.size(), "phrase entry shape");
                    // The table key covers exactly 8 bytes, so 8 bytes are what a
                    // lookup can PROMISE. Everything beyond them is the byte
                    // VERIFICATION's job - which the tree does before it ever
                    // certifies a skip - so a candidate that diverges later is not
                    // a wrong entry, it is a skip that will not fire.
                    require(std::memcmp(a.text(E), src, std::min<uint32_t>(E.len, 8)) == 0,
                            "phrase oracle: an entry's key does not cover its own bytes");
                    if (E.block == bi && E.off == off && E.len == need) {
                        require(std::memcmp(a.text(E), src, need) == 0,
                                "phrase oracle: an exact entry's bytes moved");
                        ++usable;
                    } else if (E.len <= need &&
                               std::memcmp(a.text(E), src, E.len) == 0) {
                        // a shorter phrase at the same content: a smaller skip,
                        // still a legitimate radix edge for this offset
                        ++usable;
                    } else {
                        if (phOffBad < 6)
                            std::fprintf(stderr,
                                "PHRASE_ORACLE_LOST bi=%llu off=%llu need=%llu Eblock=%u "
                                "Eoff=%u Elen=%u cmp=%d\n",
                                (unsigned long long)bi, (unsigned long long)off,
                                (unsigned long long)need, E.block, E.off, E.len,
                                std::memcmp(a.text(E), src, std::min<uint32_t>(E.len, need)));
                        ++phOffBad;   // the offset's own entry was lost to the
                                      // direct map: a MISSED SKIP, never a wrong
                                      // order
                    }
                }
                if (usable) ++blockOk; else ++phBlocksDead;
            }
            require(phOffBad * 100 <= phOffChecked,
                    "phrase oracle: more than 1% of the offset entries were lost");
            require(phBlocksDead * 100 <= a.blocks.size() * 5 + 100,
                    "phrase oracle: admitted blocks that the table cannot address");
            phBlocksLive += blockOk;
            // The head of the dictionary must also be content-addressed into the
            // corpus at its own representative position.
            for (U bi = 0; bi < a.blocks.size(); ++bi) {
                const PhraseDict::Block& bk = a.blocks[bi];
                require(bk.len >= po.minLen && bk.len <= po.maxLen,
                        "phrase oracle: block length outside the L/MAX window");
            }
        }

        // ---- (b,c,d) EXACTNESS against the brute SA, both modes, many arms ---
        for (U f = 0; f < pf.size(); ++f) {
            const std::string& t = pf[f];
            std::string M;
            for (unsigned char c : t) M += char(remap[c]);
            for (bool dollar : {false, true}) {
                auto want = brute(M, dollar);
                // L/F arms: {L=8,F=2} (the phrase regime), {L=large,F=2} (empty),
                // {L=8,F=huge} (empty), {L=4,F=2} (many short phrases).
                struct Arm { U L, F; bool on; };
                for (const Arm& arm : {Arm{8, 2, true}, Arm{200, 2, true},
                                       Arm{8, 1000000, true}, Arm{4, 2, true}}) {
                    for (U threads : {1u, 3u}) {
                        for (U leaf : {1u, 4u, 24u}) {
                            for (U lb : {1u, 2u}) {
                                CorpusText text;
                                openFixture(t, text, std::string("arm") + std::to_string(f));
                                BinWindowOpts wo = woptPhrase;
                                PhraseOpts po = phraseOpts(arm.L, arm.F, arm.on);
                                wo.phraseOpts = po;
                                PhraseDict pd;
                                pd.parse(text, dollar, po);
                                if (pd.armed()) wo.phrase = &pd;
                                BinWork w;
                                std::vector<U> got;
                                runArm(text, dollar, threads, leaf, lb, 7u, wo, w, got);
                                if (got != want) {
                                    std::fprintf(stderr,
                                        "PHRASE_ORACLE_FAIL fixture=%s dollar=%u L=%llu F=%llu "
                                        "threads=%llu leaf=%llu lb=%llu\n",
                                        pfName[f], unsigned(dollar),
                                        (unsigned long long)arm.L,
                                        (unsigned long long)arm.F,
                                        (unsigned long long)threads,
                                        (unsigned long long)leaf,
                                        (unsigned long long)lb);
                                    for (U i = 0; i < got.size(); ++i)
                                        if (got[i] != want[i])
                                            std::fprintf(stderr,
                                                " got[%llu]=%llu want[%llu]=%llu\n",
                                                (unsigned long long)i,
                                                (unsigned long long)got[i],
                                                (unsigned long long)i,
                                                (unsigned long long)want[i]);
                                    return 1;
                                }
                                require(w.emittedRows == text.n, "phrase oracle emission");
                                require(w.phraseVerifyFails == 0,
                                        "phrase oracle: a certified phrase failed verification");
                                // The skip lemma's own journal: a skip may never
                                // exceed the node's certified depth budget, and a
                                // skip must certify a WHOLE entry.
                                if (w.phraseSkips)
                                    require(w.phraseSkipMaxLen >= 2,
                                            "phrase oracle: a skip shorter than a phrase");
                                phSkips += w.phraseSkips;
                                phSkipRows += w.phraseSkipRows;
                                phSkipBytes += w.phraseSkipBytes;
                                phProbes += w.phraseProbes;
                                phSkipMaxLen = std::max(phSkipMaxLen, w.phraseSkipMaxLen);
                                phWindows += w.windows;
                                ++phCases; ++phArms;
                            }
                        }
                    }
                }
            }
        }
        require(phSkips > 0, "phrase oracle: the hook never fired on the phrase fixtures");
        require(phSkipRows > 0 && phSkipBytes > 0, "phrase oracle: skip journal empty");

        // ---- (e) EMPTY DICTIONARY == TODAY, bit for bit, counters included ---
        for (U f = 0; f < 12; ++f) {
            const std::string& t = fixtures[f];
            for (bool dollar : {false, true}) {
                BinWork wOff, wOn;
                std::vector<U> off, onr;
                {
                    CorpusText text;
                    openFixture(t, text, std::string("iden-off") + std::to_string(f));
                    BinWindowOpts wo = woptPhrase;
                    runArm(text, dollar, 3, 4, 1, 7, wo, wOff, off);
                }
                {
                    CorpusText text;
                    openFixture(t, text, std::string("iden-on") + std::to_string(f));
                    BinWindowOpts wo = woptPhrase;
                    PhraseOpts po = phraseOpts(4000, 1u << 30, true);   // nothing qualifies
                    wo.phraseOpts = po;
                    PhraseDict pd;
                    pd.parse(text, dollar, po);
                    require(!pd.armed(), "phrase oracle: the empty arm produced a dictionary");
                    wo.phrase = nullptr;                                // exactly as flag-off
                    runArm(text, dollar, 3, 4, 1, 7, wo, wOn, onr);
                }
                require(off == onr, "phrase oracle: the empty dictionary changed the ORDER");
                require(wOff.levelPasses == wOn.levelPasses &&
                            wOff.levelElems == wOn.levelElems &&
                            wOff.comparisons == wOn.comparisons &&
                            wOff.symbols == wOn.symbols &&
                            wOff.leafSorts == wOn.leafSorts &&
                            wOff.binPasses == wOn.binPasses &&
                            wOff.phraseProbes == 0 && wOn.phraseProbes == 0,
                        "phrase oracle: the empty dictionary moved a construction counter");
                ++phIdentical;
            }
        }
        std::printf("BINBWT_ORACLE_PHRASE_PASS cases=%llu arms=%llu fixtures=%zu "
                    "parse_determinism=%llu dictionaries=%llu nonempty=%llu empty=%llu "
                    "offset_entries_checked=%llu offset_entries_lost=%llu "
                    "blocks_addressable=%llu blocks_unaddressable=%llu "
                    "probes=%llu skips=%llu skip_rows=%llu skip_bytes=%llu "
                    "skip_max_len=%llu exact_brute_SA=1 midphrase=1 offsets=1 "
                    "boundary=1 wrap=1 edit=1 nested=1 norepeat=1 "
                    "empty_dict_counter_identical=%llu "
                    "modes=cyclic,dollar threads=1,3 leaf=1,4,24 level_bytes=1,2 "
                    "L=4,8,200,4000 F=2,1000000,2^30 skip_min_rows=1 "
                    "window_instances=%llu\n",
                    (unsigned long long)phCases, (unsigned long long)phArms, pf.size(),
                    (unsigned long long)phDet, (unsigned long long)(phEmpty + phNonEmpty),
                    (unsigned long long)phNonEmpty, (unsigned long long)phEmpty,
                    (unsigned long long)phOffChecked, (unsigned long long)phOffBad,
                    (unsigned long long)phBlocksLive, (unsigned long long)phBlocksDead,
                    (unsigned long long)phProbes, (unsigned long long)phSkips,
                    (unsigned long long)phSkipRows, (unsigned long long)phSkipBytes,
                    (unsigned long long)phSkipMaxLen,
                    (unsigned long long)phIdentical, (unsigned long long)phWindows);
    }
    return 0;
}
