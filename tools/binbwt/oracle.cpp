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

    std::printf("BINBWT_ORACLE_FREQ_PASS cases=%llu fixtures=%zu modes=cyclic,dollar "
                "threads=1,3,8 leaf=1,24 depth_cap=16 level_bytes=1,2 cap=7,all "
                "freq_sched=1 exact_brute_SA=1 multi_window_runs=%llu "
                "window_instances=%llu\n",
                (unsigned long long)freqCases, fixtures.size(),
                (unsigned long long)freqMulti, (unsigned long long)freqWindows);
    return 0;
}
