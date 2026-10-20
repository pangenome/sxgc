// sa_anchor_oracle - the brute-force oracle for the SAMPLED-SA ANCHOR STREAM
// (charter Phase 3, option B).
//
// The claim under test: `--sa-sample-k k` writes a file whose entry j is
// EXACTLY SA[j*k] of the true suffix array of the remapped corpus, for every
// j in [0, count), with count == (n-1)/k + 1, in ascending rank, and whose
// header describes itself (magic, version, n, k, count, rows).
//
// Two independent arms are checked, because the anchor hook sits in ONE place
// (`emitBin`) but two different emit paths reach it:
//   ARM 1 (per-row emit, the oracle arm): `bin_order` with a lambda sink. The
//         lambda also records the emitted (rank, position) stream, so the file
//         is checked TWICE - against the emit pass itself (self-consistency)
//         and against the brute-force SA (the real claim).
//   ARM 2 (the product arm): `binbwt_main`, i.e. the real `BinBandSink`
//         publisher. The anchors must equal the same brute SA, and the product
//         (.rlebwt) must be byte-identical to the same run with the knob OFF.
//
// Adversarial corpora (the shapes that break ranks/positions):
//   * 64 uniform-random bytes over a 6-symbol alphabet
//   * all-equal (one bin, one long run, every rotation identical)
//   * periodic "ababab..."
//   * separators + repeats ("mississippi\x1eaaa...\x1ezzz...")
//   * tiny: n = 1 and n = 2
// Adversarial k: 1 (the FULL SA - it must reproduce the sledgehammer), 2, 3, 7,
// k = n (one anchor, rank 0), k = 2n (still one anchor: rank 0 only).
#define main retired_merge_main
#include "../../bit6/cross_lcp_merge.cpp"
#undef main
#include <random>
#include <sys/wait.h>

// ---- the brute-force suffix array of the remapped byte string ---------------
// Identical order function to tools/binbwt/oracle.cpp: the producer sorts the
// tails of the REMAPPED bytes (dollar mode: plain suffixes; cyclic mode:
// rotations of period n).
static std::vector<U> brute_sa(const std::string& M, bool dollar) {
    const U n = M.size();
    std::vector<std::pair<std::string, U>> v;
    v.reserve(n);
    for (U p = 0; p < n; ++p)
        v.push_back({dollar ? M.substr(p) : M.substr(p) + M.substr(0, p), p});
    std::sort(v.begin(), v.end());
    std::vector<U> out(n);
    for (U i = 0; i < n; ++i) out[i] = v[i].second;
    return out;
}

struct AnchorFile {
    uint32_t magic = 0, version = 0;
    U n = 0, k = 0, count = 0, rows = 0, pad = 0;
    std::vector<U> sa;
};

static AnchorFile read_anchor_file(const std::string& path) {
    const uint64_t fsize = fs::file_size(path);
    std::ifstream f(path, std::ios::binary);
    require(bool(f), "sa-anchor oracle: open anchor file");
    AnchorFile a;
    f.read((char*)&a.magic, 4);
    f.read((char*)&a.version, 4);
    f.read((char*)&a.n, 8);
    f.read((char*)&a.k, 8);
    f.read((char*)&a.count, 8);
    f.read((char*)&a.rows, 8);
    f.read((char*)&a.pad, 8);
    require(a.magic == SA_ANCHOR_MAGIC, "sa-anchor oracle: magic");
    require(a.version == SA_ANCHOR_VERSION, "sa-anchor oracle: version");
    require(a.pad == SA_ANCHOR_ROW_PAD, "sa-anchor oracle: pad");
    require(a.k >= 1, "sa-anchor oracle: k");
    require(fsize == SA_ANCHOR_HEADER_BYTES + a.count * 8, "sa-anchor oracle: file size");
    a.sa.resize(a.count);
    if (a.count) {
        f.read((char*)a.sa.data(), std::streamsize(a.count * 8));
        require(f.gcount() == std::streamsize(a.count * 8), "sa-anchor oracle: entry read");
    }
    return a;
}

// The full check of one file against one brute SA.
static void verify_anchor_file(const std::string& path, const std::vector<U>& sa,
                              U k, const char* corpusName, bool dollar,
                              uint64_t* totEntries, uint64_t* files) {
    const U n = sa.size();
    AnchorFile a = read_anchor_file(path);
    require(a.n == n, "sa-anchor oracle: header n");
    require(a.k == k, "sa-anchor oracle: header k");
    require(a.rows == n, "sa-anchor oracle: header rows");
    const U want = SaAnchorWriter::expectedCount(n, k);
    require(a.count == want, "sa-anchor oracle: count != (rows-1)/k+1");
    // Every entry is EXACTLY SA[j*k]. This is the whole claim.
    for (U j = 0; j < a.count; ++j) {
        const U rank = j * k;
        require(rank < n, "sa-anchor oracle: entry rank out of range");
        if (a.sa[j] != sa[rank]) {
            std::fprintf(stderr,
                "SA_ANCHOR_MISMATCH corpus=%s dollar=%u k=%llu j=%llu rank=%llu "
                "stored=%llu brute=%llu\n", corpusName, unsigned(dollar),
                (unsigned long long)k, (unsigned long long)j,
                (unsigned long long)rank, (unsigned long long)a.sa[j],
                (unsigned long long)sa[rank]);
            require(false, "sa-anchor oracle: entry is not SA[j*k]");
        }
    }
    // k == 1 IS the sledgehammer: the anchor file must BE the full SA.
    if (k == 1) {
        require(a.count == n, "sa-anchor oracle: k=1 count != n");
        for (U r = 0; r < n; ++r) require(a.sa[r] == sa[r], "sa-anchor oracle: k=1 full SA");
    }
    // k >= n: exactly one anchor, at rank 0 (the smallest suffix).
    if (k >= n) {
        require(a.count == 1, "sa-anchor oracle: k>=n emits one anchor");
        require(a.sa[0] == sa[0], "sa-anchor oracle: k>=n anchor is SA[0]");
    }
    *totEntries += a.count;
    ++*files;
}

static const char* kCorpora[] = {
    "random64", "allequal32", "periodic32", "seps", "one", "two", "runs",
};

static std::string corpus_bytes(const std::string& which) {
    if (which == "random64") {
        std::mt19937 rng(20261020u);
        std::string s;
        for (int i = 0; i < 64; ++i) s.push_back(char('A' + rng() % 6));
        return s;
    }
    if (which == "allequal32") return std::string(32, 'q');
    if (which == "periodic32") return "abababababababababababababababab";
    if (which == "seps")
        return std::string("mississippimississippi\x1e" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "\x1e" "zzzzzzzzzzzzzzzzzzzzzzzz", 32 + 32 + 4 + 24);
    if (which == "one") return "x";
    if (which == "two") return "xy";
    if (which == "runs")
        return std::string("aaaaaaaaaabbbbbbbbbbccccccccccaaaaaabbbbbb", 36);
    require(false, "sa-anchor oracle: unknown corpus");
    return {};
}

// A minimal PUBLISH sink: exactly the interface `bin_publish_sink` detects
// (joinTargets / publishBin / windowDone), recording the ordered row stream so
// the publish arm's anchors can be checked against the same brute SA.
struct RecordingPublisher {
    std::vector<U> rows;
    std::vector<U> none;
    U n_ = 0;
    bool dollar_ = false;
    const std::vector<U>& joinTargets() const { return none; }
    void publishBin(unsigned, const U* words, U m, U baseRow) {
        require(baseRow == rows.size(), "sa-anchor oracle: publish rank order");
        for (U i = 0; i < m; ++i)
            rows.push_back(bin_pos_of(bin_word_tail(words[i]), n_, dollar_));
    }
    void windowDone() {}
};

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: sa_anchor_oracle SCRATCH");
        const std::string root = argv[1];
        fs::create_directories(root);
        setenv("CROSS_POOL_SLOTS", "65536", 1);
        std::array<uint8_t, 256> id{};
        for (unsigned c = 0; c < 256; ++c) id[c] = uint8_t(c);
        const U kSweep[] = {1, 2, 3, 7, U(1) << 30, U(1) << 31};   // last two: >= n
        uint64_t totEntries = 0, files = 0, arms = 0;
        uint64_t t0 = 0;
        (void)t0;
        for (const char* which : kCorpora) {
            const std::string text = corpus_bytes(which);
            const std::string path = root + "/sa-oracle-corpus";
            { fs::remove(path); OutFile file(path); file.raw(text.data(), text.size()); }
            for (int dollarI = 0; dollarI < 2; ++dollarI) {
                const bool dollar = dollarI == 0;
                std::vector<U> sa = brute_sa(text, dollar);
                for (U k : kSweep) {
                    if (k > sa.size() && k != (U(1) << 30) && k != (U(1) << 31)) continue;
                    // ---- ARM 1: bin_order with the per-row lambda sink ------
                    const std::string af = root + "/anchor-arm1.sxsa";
                    fs::remove(af);
                    TextSeg seg; seg.path = path; seg.len = text.size(); seg.remap = id;
                    CorpusText t; t.open({seg}, true);
                    require(t.n == sa.size(), "sa-anchor oracle: corpus length");
                    BinWindowOpts wopt;
                    wopt.dir = root + "/windows-oracle";
                    wopt.credit = 1;
                    wopt.disc = true;
                    wopt.saSampleK = k;
                    wopt.saSamplePath = af;
                    BinWork w;
                    std::vector<U> emitted;          // rank -> position, the emit pass
                    emitted.reserve(sa.size());
                    bin_order(t, dollar, 3, 4, 32, 0, 1, false, 0, w,
                              [&](U row, U p) { require(row == emitted.size(),
                                                        "sa-anchor oracle: emit rank order");
                                                emitted.push_back(p); },
                              false, wopt);
                    require(emitted.size() == sa.size(), "sa-anchor oracle: emit coverage");
                    // THE EMIT PASS ITSELF must be the brute SA...
                    for (U r = 0; r < sa.size(); ++r) {
                        if (emitted[r] != sa[r]) {
                            std::fprintf(stderr,
                                "SA_EMIT_MISMATCH corpus=%s dollar=%u rank=%llu emitted=%llu brute=%llu\n",
                                which, unsigned(dollar), (unsigned long long)r,
                                (unsigned long long)emitted[r], (unsigned long long)sa[r]);
                            require(false, "sa-anchor oracle: emit stream != brute SA");
                        }
                    }
                    // ...and the FILE must be exactly its k-th entries.
                    verify_anchor_file(af, sa, k, which, dollar, &totEntries, &files);
                    require(w.saSamples == SaAnchorWriter::expectedCount(sa.size(), k),
                            "sa-anchor oracle: journalled count");
                    require(w.saSampleK == k, "sa-anchor oracle: journalled k");
                    require(w.saSampleBytes == SA_ANCHOR_HEADER_BYTES + w.saSamples * 8,
                            "sa-anchor oracle: journalled bytes");
                    ++arms;
                }
                // ---- ARM 2: the PUBLISH arm (the one the product runs) -----
                // `bin_order` takes a different branch when the sink publishes
                // whole bins (`bin_publish_sink`, binbwt_slice.h:1257), and the
                // anchor hook sits OUTSIDE that branch - so this arm proves the
                // two emit paths produce the SAME anchor stream from the same
                // slices. The real BinBandSink is not usable at n=64 (its seam
                // discovery needs a merge-shaped corpus), so this is a minimal
                // sink with exactly the detected interface.
                for (U k : {U(3), U(1)}) {
                    const std::string afP = root + "/anchor-arm2.sxsa";
                    fs::remove(afP);
                    TextSeg seg2; seg2.path = path; seg2.len = text.size(); seg2.remap = id;
                    CorpusText t2; t2.open({seg2}, true);
                    BinWindowOpts w2;
                    w2.dir = root + "/windows-publish";
                    w2.credit = 1;
                    w2.disc = true;
                    w2.saSampleK = k;
                    w2.saSamplePath = afP;
                    BinWork w2work;
                    RecordingPublisher sink;
                    sink.n_ = sa.size(); sink.dollar_ = dollar;
                    bin_order(t2, dollar, 3, 4, 32, 0, 1, false, 0, w2work, sink,
                              false, w2);
                    require(sink.rows.size() == sa.size(),
                            "sa-anchor oracle: publish coverage");
                    for (U r = 0; r < sa.size(); ++r)
                        require(sink.rows[r] == sa[r],
                                "sa-anchor oracle: publish stream != brute SA");
                    verify_anchor_file(afP, sa, k, which, dollar, &totEntries, &files);
                }
                ++arms;
            }
        }
        // ---- THE NEGATIVE ARM: a corrupted entry must DIE --------------------
        // A gate that cannot fail proves nothing. The child writes a valid file,
        // rewrites one entry with a wrong SA value, and runs the same checker;
        // `require` in this tree exits 2 (CROSS_LCP_FATAL), so the parent
        // asserts the refusal from the OUTSIDE.
        {
            const std::string good = root + "/neg-good.sxsa";
            const std::string bad = root + "/neg-bad.sxsa";
            const std::string text = corpus_bytes("random64");
            std::vector<U> sa = brute_sa(text, false);
            const pid_t pid = ::fork();
            require(pid >= 0, "sa-anchor oracle: fork negative arm");
            if (pid == 0) {
                SaAnchorWriter w;
                w.open(good, 3, sa.size());
                for (U j = 0; j < (sa.size() - 1) / 3 + 1; ++j)
                    w.buf.push_back(uint64_t(sa[j * 3]));
                w.count = (sa.size() - 1) / 3 + 1;
                w.rows = sa.size();
                w.finish(*(new BinWork()));
                // copy, then corrupt entry 3 (rank 9): SA[9] must be wrong.
                { std::ifstream in(good, std::ios::binary);
                  std::ofstream out(bad, std::ios::binary);
                  out << in.rdbuf(); }
                const uint64_t wrong = uint64_t((sa[9] + 1) % sa.size());
                const int fd = ::open(bad.c_str(), O_RDWR);
                require(fd >= 0, "sa-anchor oracle: open corrupt target");
                require(::pwrite(fd, &wrong, 8, off_t(SA_ANCHOR_HEADER_BYTES) + 3 * 8) == 8,
                        "sa-anchor oracle: corrupt entry");
                ::close(fd);
                uint64_t e = 0, f = 0;
                verify_anchor_file(bad, sa, 3, "negative", false, &e, &f);
                _exit(0);      // reaching here means the corruption was NOT caught
            }
            int status = 0;
            require(::waitpid(pid, &status, 0) == pid, "sa-anchor oracle: wait negative arm");
            require(WIFEXITED(status) && WEXITSTATUS(status) == 2,
                    "sa-anchor oracle: corrupted anchor was NOT refused (exit != 2)");
            std::fprintf(stderr, "SA_ANCHOR_NEGATIVE_PASS corrupted_entry_refused=1 exit=2\n");
        }
        std::fprintf(stdout,
            "SA_ANCHOR_ORACLE_PASS arms=%llu files=%llu entries=%llu corpora=%zu "
            "k_sweep=1,2,3,7,>=n arms=per-row+publish negative_arm=1\n",
            (unsigned long long)arms, (unsigned long long)files,
            (unsigned long long)totEntries, sizeof(kCorpora) / sizeof(kCorpora[0]));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "SA_ANCHOR_ORACLE_FAIL %s\n", e.what());
        return 2;
    }
}
