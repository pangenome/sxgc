// m1 STREAM: independent decoder for the lite container's m1 member.
//
// It exists for two jobs, both of them GATES rather than product code:
//
//   --compare OTHER  decode the m1 member of two containers and compare the
//                    decoded NORMALIZED RUN STREAM run by run - (chars,
//                    lengths) in order, byte for byte, with no tolerance and
//                    no sampling. This is the route-B round-trip arbiter: the
//                    one-pass arm must decode to exactly the legacy arm's run
//                    stream even though its Huffman codebook is derived from
//                    the corpus byte histogram instead of the run histogram.
//
//   (default)        report the m1 statistics: run count, per-symbol run and
//                    byte totals, the entropy of each, and the expected head
//                    bits per run under (a) the container's own stored code
//                    and (b) a canonical code built from the container's C
//                    table (the corpus byte histogram). That is the exact
//                    compression cost of the route-B codebook, measured from
//                    a real published container rather than assumed.
//
// The container layout checks reproduce the reader's structure checks (header
// crc, member crc, m1 layout) so a decode here cannot quietly accept a
// container the product reader would reject.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
using U = uint64_t;
using B = uint8_t;
static void require(bool v, const char* m) { if (!v) throw std::runtime_error(m); }
static U le(const B* p, unsigned w = 8) { U v = 0; for (unsigned i = 0; i < w; ++i) v |= U(p[i]) << (8 * i); return v; }
static void put(B* p, U v, unsigned w) { for (unsigned i = 0; i < w; ++i) p[i] = B(v >> (8 * i)); }

// ---------------------------------------------------------------- crc32 (the reader's)
static uint32_t crc32(uint32_t v, const B* p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (unsigned i = 0; i < 256; ++i) { uint32_t x = i; for (int k = 0; k < 8; ++k) x = (x >> 1) ^ (0xedb88320U & -(x & 1)); t[i] = x; }
        return t;
    }();
    for (size_t i = 0; i < n; ++i) v = table[(v ^ p[i]) & 255] ^ (v >> 8);
    return v;
}

struct Map {
    int fd = -1; size_t size = 0; const B* p = nullptr;
    explicit Map(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        struct stat st {};
        require(fd >= 0 && !fstat(fd, &st) && st.st_size > 0, "open/stat container");
        size = size_t(st.st_size);
        p = (const B*)::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        require(p != MAP_FAILED, "mmap container");
    }
    ~Map() { if (p) ::munmap((void*)p, size); if (fd >= 0) ::close(fd); }
};

// ------------------------------------------------------------------ SXI2 m1 view
struct M1 {
    U n = 0, runs = 0, size = 0, headBits = 0, lenBits = 0;
    std::array<U, 64> first{}, count{}, base{};
    std::vector<B> symbols;
    const B* p = nullptr;
    std::array<uint64_t, 256> ctable{};   // prefix sums of the normalized byte totals
    std::array<uint64_t, 256> lens{};
    const B* heads = nullptr; const B* lengths = nullptr;

    M1(const Map& m, const std::string& path) {
        require(m.size >= 64, "short container header");
        const B* h = m.p;
        require(!memcmp(h, "SXI2", 4), "bad container magic");
        require(le(h + 4, 4) == 6, "unsupported container version");
        n = le(h + 8);
        const U r = le(h + 24);
        const U members = le(h + 32, 4), hsize = le(h + 36, 4);
        require(le(h + 40) == m.size, "container length mismatch");
        require(hsize == 64 + 40 * members, "member table size");
        uint32_t want = uint32_t(le(h + 56, 4));
        std::vector<B> copy(h, h + hsize);
        put(copy.data() + 56, 0, 4);
        require(~crc32(~0u, copy.data(), hsize) == want, "container header crc");
        const B* member = nullptr;
        for (U i = 0; i < members; ++i) {
            const B* d = h + 64 + 40 * i;
            if (le(d, 4) != 1) continue;
            require(!member, "duplicate m1 member");
            require(le(d + 4, 4) == 101, "m1 codec");
            const U off = le(d + 8), bytes = le(d + 16), cnt = le(d + 24);
            require(off <= m.size && bytes <= m.size - off && cnt == r, "m1 bounds");
            require(~crc32(~0u, h + off, size_t(bytes)) == uint32_t(le(d + 32, 4)), "m1 member crc");
            member = h + off; size = bytes; runs = cnt;
        }
        require(member, "missing m1 member");
        require(size >= 2320 + 16, "short m1");
        p = member;
        for (unsigned c = 0; c < 256; ++c) ctable[c] = le(p + 8 * c);
        for (unsigned c = 0; c < 256; ++c) lens[c] = p[2048 + c];
        require(ctable[0] == 0, "C table does not start at zero");
        // C table: prefix sums of the normalized byte totals; the last prefix
        // plus its own total must equal n.
        headBits = le(p + 2304); lenBits = le(p + 2312);
        const U hs = (headBits + 7) / 8;
        require(hs <= size - 2320, "head bit size");
        require((lenBits + 7) / 8 == size - 2320 - hs, "length bit size");
        heads = p + 2320; lengths = p + 2320 + hs;
        std::vector<std::pair<unsigned, B>> codes;
        for (unsigned c = 0; c < 256; ++c) if (lens[c]) { require(lens[c] <= 63, "Huffman width"); codes.push_back({unsigned(lens[c]), B(c)}); }
        require(!codes.empty(), "empty Huffman alphabet");
        std::sort(codes.begin(), codes.end());
        U code = 0; unsigned prev = 0;
        for (auto [w, c] : codes) {
            code <<= (w - prev);
            require(code < (U(1) << w), "oversubscribed Huffman code");
            if (!count[w]) { first[w] = code; base[w] = symbols.size(); }
            ++count[w]; symbols.push_back(c); ++code; prev = w;
        }
        (void)path;
    }
    U bytes() const { return size; }
};

struct BitIn {
    const B* p; U size, pos = 0;
    unsigned bit() { require(pos < size, "truncated bits"); unsigned b = (p[pos / 8] >> (pos % 8)) & 1; ++pos; return b; }
    U gamma() { unsigned z = 0; while (!bit()) { require(++z < 64, "gamma overflow"); } U v = 1; while (z--) v = v * 2 + bit(); return v; }
};

// One decoded run at a time, in stream order.
struct Decoder {
    BitIn heads, lengths;
    U left;
    const M1& m;
    explicit Decoder(const M1& v) : m(v) { heads = {m.heads, m.headBits}; lengths = {m.lengths, m.lenBits}; left = m.runs; }
    bool next(uint8_t& c, uint32_t& len) {
        if (!left) return false;
        --left;
        U code = 0;
        for (unsigned w = 1; w < 64; ++w) {
            code = code * 2 + heads.bit();
            if (m.count[w] && code >= m.first[w] && code - m.first[w] < m.count[w]) {
                c = m.symbols[m.base[w] + code - m.first[w]];
                const U L = lengths.gamma();
                require(L <= UINT32_MAX, "run length exceeds u32");
                len = uint32_t(L);
                return true;
            }
        }
        throw std::runtime_error("invalid Huffman code");
    }
    void finish() { require(!left && heads.pos == m.headBits && lengths.pos == m.lenBits, "unused runs or bits"); }
};

// ------------------------------------------- canonical Huffman from weights (route B)
// The encoder's derivation, reproduced here so the cost can be measured on a
// published container: a Huffman tree over the 256 symbols with the given
// weights (a zero weight is lifted to one pseudo-count so the code is TOTAL and
// no emitted symbol can lack a code), then canonical code lengths by
// (length, symbol) order.
static std::array<uint64_t, 256> canonical_lens(const std::array<uint64_t, 256>& weight) {
    std::array<uint64_t, 256> w = weight;
    for (auto& v : w) v = std::max<uint64_t>(v, 1);
    struct Node { uint64_t w; int left, right, sym; };
    std::vector<Node> tree;
    using Q = std::pair<uint64_t, int>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
    for (int c = 0; c < 256; ++c) { q.push({w[c], int(tree.size())}); tree.push_back({w[c], -1, -1, c}); }
    while (q.size() > 1) {
        auto a = q.top(); q.pop();
        auto b = q.top(); q.pop();
        tree.push_back({a.first + b.first, a.second, b.second, -1});
        q.push({a.first + b.first, int(tree.size() - 1)});
    }
    std::array<uint64_t, 256> lens{};
    std::vector<std::pair<int, unsigned>> stack{{int(q.top().second), 0}};
    while (!stack.empty()) {
        auto [idx, depth] = stack.back(); stack.pop_back();
        if (tree[idx].sym >= 0) { require(depth > 0 && depth <= 63, "Huffman depth"); lens[tree[idx].sym] = depth; continue; }
        stack.push_back({tree[idx].left, depth + 1});
        if (tree[idx].right >= 0) stack.push_back({tree[idx].right, depth + 1});
    }
    return lens;
}

int main(int argc, char** argv) {
    try {
        std::string primary, other, dump, histOut;
        bool wantFingerprint = false;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto nextArg = [&] { require(i + 1 < argc, "missing option value"); return std::string(argv[++i]); };
            if (a == "--compare") other = nextArg();
            else if (a == "--dump") dump = nextArg();
            else if (a == "--hist") histOut = nextArg();
            else if (a == "--fingerprint") wantFingerprint = true;
            else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
            else primary = a;
        }
        require(!primary.empty(), "usage: m1_stream CONTAINER [--compare OTHER] [--dump RECORDS] [--hist OUT] [--fingerprint]");
        Map mapA(primary);
        M1 A(mapA, primary);
        // Per-symbol run counts and byte totals, and the run-stream fingerprint.
        std::array<uint64_t, 256> runCount{}, byteTotal{};
        uint64_t totalRuns = 0, totalBytes = 0, fnv = 1469598103934665603ull;
        FILE* dumpFile = dump.empty() ? nullptr : std::fopen(dump.c_str(), "wb");
        require(dump.empty() || dumpFile, "create dump");
        Decoder da(A);
        uint8_t c; uint32_t len;
        while (da.next(c, len)) {
            require(len > 0, "zero run length");
            ++runCount[c]; byteTotal[c] += len;
            ++totalRuns; totalBytes += len;
            fnv = (fnv ^ (uint64_t(c) << 32 | len)) * 0x100000001b3ull;
            if (dumpFile) {
                B rec[5]; rec[0] = c; put(rec + 1, len, 4);
                require(std::fwrite(rec, 5, 1, dumpFile) == 1, "write dump");
            }
        }
        da.finish();
        if (dumpFile) { require(std::fclose(dumpFile) == 0, "close dump"); dumpFile = nullptr; }
        // The decoded extent must be exactly n: the normalized run stream is a
        // complete cover of the text.
        std::printf("M1_STREAM container=%s n=%llu runs=%llu bytes=%llu head_bits=%llu len_bits=%llu "
                    "head_bits_per_run=%.6f len_bits_per_run=%.6f fingerprint=%016llx\n",
                    primary.c_str(), (unsigned long long)A.n, (unsigned long long)totalRuns,
                    (unsigned long long)A.bytes(), (unsigned long long)A.headBits,
                    (unsigned long long)A.lenBits,
                    totalRuns ? double(A.headBits) / double(totalRuns) : 0.0,
                    totalRuns ? double(A.lenBits) / double(totalRuns) : 0.0,
                    (unsigned long long)fnv);
        require(totalBytes == A.n, "decoded extent != n");
        require(totalRuns == A.runs, "decoded run count != header");
        // Codebook cost of route B: a canonical code built from the container's
        // own C table (the corpus byte histogram) versus the code the container
        // actually stores (built from the exact run histogram).
        std::array<uint64_t, 256> weight{};
        std::array<uint64_t, 256> byteHist{};
        for (unsigned s = 0; s < 256; ++s) {
            const uint64_t next = s + 1 < 256 ? A.ctable[s + 1] : A.n;
            require(next >= A.ctable[s] && next <= A.n, "C table not monotone");
            byteHist[s] = next - A.ctable[s];
        }
        uint64_t histSum = 0;
        for (unsigned s = 0; s < 256; ++s) histSum += byteHist[s];
        require(histSum == A.n, "C table totals != n");
        weight = byteHist;
        const auto lensB = canonical_lens(weight);
        double bitsStored = double(A.headBits), bitsB = 0;
        double hRun = 0, crossB = 0, hByte = 0;
        uint64_t worst = 0, worstSym = 0;
        for (unsigned s = 0; s < 256; ++s) {
            if (!runCount[s]) continue;
            const double p = double(runCount[s]) / double(totalRuns);
            hRun -= p * std::log2(p);
            bitsB += double(runCount[s]) * double(lensB[s]);
            const double q = byteHist[s] ? double(byteHist[s]) / double(A.n) : 0.0;
            if (q > 0) crossB -= p * std::log2(q);
            if (lensB[s] > worst) { worst = lensB[s]; worstSym = s; }
        }
        for (unsigned s = 0; s < 256; ++s) if (byteHist[s]) { const double q = double(byteHist[s]) / double(A.n); hByte -= q * std::log2(q); }
        double storedAvg = totalRuns ? bitsStored / double(totalRuns) : 0.0;
        std::printf("M1_CODEBOOK runs=%llu stored_bits_per_run=%.6f corpus_hist_bits_per_run=%.6f "
                    "run_entropy=%.6f byte_entropy=%.6f cross_entropy=%.6f growth_bits_per_run=%.6f "
                    "growth_ratio=%.6f max_len=%llu max_len_symbol=%llu\n",
                    (unsigned long long)totalRuns, storedAvg, bitsB / double(totalRuns),
                    hRun, hByte, crossB, bitsB / double(totalRuns) - storedAvg,
                    bitsStored > 0 ? bitsB / bitsStored : 0.0,
                    (unsigned long long)worst, (unsigned long long)worstSym);
        std::printf("M1_STORED_CODE "); // lengths actually published
        for (unsigned s = 0; s < 256; ++s) if (A.lens[s]) std::printf("%u:%llu,", s, (unsigned long long)A.lens[s]);
        std::printf("\nM1_ROUTE_B_CODE ");
        for (unsigned s = 0; s < 256; ++s) if (lensB[s]) std::printf("%u:%llu,", s, (unsigned long long)lensB[s]);
        std::printf("\n");
        if (!histOut.empty()) {
            std::ofstream h(histOut);
            require(bool(h), "create hist file");
            h << "symbol\tcorpus_bytes\trun_starts\n";
            for (unsigned s = 0; s < 256; ++s) h << s << '\t' << byteHist[s] << '\t' << runCount[s] << '\n';
        }
        if (!other.empty()) {
            Map mapB(other);
            M1 Bc(mapB, other);
            require(Bc.n == A.n && Bc.runs == A.runs, "A/B container header mismatch");
            Decoder db(Bc);
            Decoder dc(A);
            uint64_t idx = 0, bad = 0;
            for (;;) {
                uint8_t ca; uint32_t la; uint8_t cb; uint32_t lb;
                const bool ha = dc.next(ca, la), hb = db.next(cb, lb);
                require(ha == hb, "A/B decoded run count differs");
                if (!ha) break;
                if (ca != cb || la != lb) {
                    if (bad < 8) std::printf("M1_AB_MISMATCH run=%llu a=(%u,%u) b=(%u,%u)\n",
                                             (unsigned long long)idx, ca, la, cb, lb);
                    ++bad;
                }
                ++idx;
            }
            dc.finish(); db.finish();            std::printf("M1_AB runs=%llu mismatches=%llu equal=%d\n",
                        (unsigned long long)idx, (unsigned long long)bad, int(bad == 0));
            if (bad) {
                std::printf("M1_AB_RESULT FAIL\n");
                return 1;
            }
            std::printf("M1_AB_RESULT PASS\n");
        }
        (void)wantFingerprint;
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL m1_stream: %s\n", e.what());
        return 2;
    }
}
