// PILE_FINISH - the pile-builder v2 fused banded finish.
//
// Consumes the final merge's v2 band pieces ({char u8, len u32, head SA
// packed, tail SA packed} per run band; the CROSS_V2_PIECES=1 emission)
// while the merge emits them, reads the corpus through the read-only
// remap adapter T[i]=REMAP[corpus[i]] (THE LAW: the text lives exactly once,
// in the corpus; no text is ever materialized to disk - supervisor decision
// 2A, 2026-10-10; the adapter identity is gated byte-exact against the
// banked 1GB pftext, and the PFCK built here is byte-identical to the
// merge's --emit-pf sidecar), and produces the PILE LITE container {m1
// runs, m4 anchors, m5 chi EF, m7 remap, m10 LF, m11 sparse 1/1024 tails}
// (members 2/8/9 belong to the WARM container; supervisor decision 1A).
//
// Bounded scaffolding throughout - no whole-file agg/ssa/ssa_t/head_sa and
// no v1 record/seed columns: the endpoints' cyclic seam repair (the
// fixtures are NON-certified: 4 classes at 100MB, 30 at 1GB) is recomputed
// with RAM-sampled starts + per-char samples and monotone binary searches
// over the piece head/tail columns (heads/tails increase with the run
// index - rows are in SA order - so the pieces ARE the sorted seed
// columns). The slim's per-band topLCP/interiorMin hand off to the sweep
// IN PROCESS (no agg file ever exists). Witnesses go to VALUE-RANGE
// buckets (RAM until the budget, then sorted delta-varint runs); the m5
// EF is encoded bucket-by-bucket in value order with prefix ordinals, so
// the witness set never exists as a dense file. m1 is Huffman-encoded
// from a final rlebwt re-scan (the repair replayed sample-free - the
// normalized chars/lens are a deterministic function of the raw runs and
// the class table). rlebwt is deleted after the container publishes
// (unless --keep-rlebwt).
#include "sxi_format.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <utility>
#include <vector>
#include <string>
#include <stdexcept>
#include <functional>
#include <filesystem>

static constexpr uint64_t INF = UINT64_MAX;
static double tnow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static double G_T0 = tnow();
#define SLIM_LCE_CORE_ONLY
#include "slim_lce.hpp"
using namespace sxi;

static void pf_fail(const std::string& msg) {
    fprintf(stderr, "FATAL pile_finish: %s\n", msg.c_str());
    exit(2);
}
static void pf_check(bool ok, const std::string& msg) {
    if (!ok) pf_fail(msg);
}
static void phase(const char* name, double& last) {
    fprintf(stderr, "PILE_FINISH_PHASE %s wall=%.3f cumulative=%.3f\n", name, tnow() - last, tnow() - G_T0);
    last = tnow();
}
static void pf_read_full(int fd, void* p, uint64_t len, const std::string& what) {
    char* q = (char*)p;
    while (len) {
        ssize_t z = ::read(fd, q, len);
        if (z <= 0) pf_fail("read " + what);
        q += z; len -= uint64_t(z);
    }
}
static void pf_pread_full(int fd, void* p, uint64_t len, uint64_t off, const std::string& what) {
    char* q = (char*)p;
    while (len) {
        ssize_t z = ::pread(fd, q, len, off);
        if (z <= 0) pf_fail("pread " + what);
        q += z; off += uint64_t(z); len -= uint64_t(z);
    }
}
static void pf_write_full(int fd, const void* p, uint64_t len, const std::string& what) {
    const char* q = (const char*)p;
    while (len) {
        ssize_t z = ::write(fd, q, len);
        if (z <= 0) pf_fail("write " + what);
        q += z; len -= uint64_t(z);
    }
}

// ---------------------------------------------------------------- remap
static void load_remap_table(const std::string& path, uint8_t out[256]) {
    int fd = ::open(path.c_str(), O_RDONLY);
    pf_check(fd >= 0, "open remap");
    uint8_t buf[256];
    pf_read_full(fd, buf, 256, "remap");
    ::close(fd);
    memcpy(out, buf, 256);
    uint8_t sorted[256];
    memcpy(sorted, buf, 256);
    std::sort(sorted, sorted + 256);
    for (unsigned i = 0; i < 256; ++i) pf_check(sorted[i] == uint8_t(i), "remap must be a bijection");
}

// ------------------------------------------------------- corpus adapter
struct CorpusText {
    int fd = -1;
    uint64_t size_ = 0;
    uint8_t remap[256];
    explicit CorpusText(const std::string& path, const uint8_t map[256]) {
        fd = ::open(path.c_str(), O_RDONLY);
        pf_check(fd >= 0, "open corpus (read-only adapter)");
        struct stat st {};
        pf_check(fstat(fd, &st) == 0, "stat corpus");
        size_ = uint64_t(st.st_size);
        memcpy(remap, map, 256);
        bool identity = true;
        for (unsigned i = 0; i < 256; ++i) identity = identity && remap[i] == uint8_t(i);
        fprintf(stderr, "PILE_FINISH_CORPUS path=%s bytes=%llu remap_identity=%d read_only=1 rotation=0 derivation=T[i]=REMAP[corpus[i]]\n",
                path.c_str(), (unsigned long long)size_, int(identity));
    }
    ~CorpusText() { if (fd >= 0) ::close(fd); }
    CorpusText(const CorpusText&) = delete;
    CorpusText& operator=(const CorpusText&) = delete;
    uint64_t size() const { return size_; }
    uint8_t operator[](uint64_t i) const {
        if (i >= size_) pf_fail("corpus adapter bounds");
        constexpr uint64_t B = 65536;
        struct Cache { uint64_t next = 0; const CorpusText* owner = nullptr;
                       std::array<uint64_t, 4> tags{{INF, INF, INF, INF}};
                       std::array<std::array<uint8_t, B>, 4> data; };
        thread_local Cache c;
        if (c.owner != this) { c.owner = this; c.tags.fill(INF); }
        uint64_t page = i / B, slot = 0;
        while (slot < 4 && c.tags[slot] != page) ++slot;
        if (slot == 4) {
            slot = c.next++ % 4;
            uint64_t start = page * B, need = std::min<uint64_t>(B, size_ - start), done = 0;
            while (done < need) {
                ssize_t z = ::pread(fd, c.data[slot].data() + done, need - done, start + done);
                pf_check(z > 0, "pread corpus page");
                done += uint64_t(z);
            }
            for (uint64_t k = 0; k < need; ++k) c.data[slot][k] = remap[c.data[slot][k]];
            c.tags[slot] = page;
        }
        return c.data[slot][i % B];
    }
};

// ------------------------------------------------------------- PFCK build
// tau-spaced suffix-hash checkpoints over the corpus adapter, byte-identical
// to cross_lcp_merge --emit-pf's sidecar (same recurrence over the same
// bytes). tau = (8n + R - 1)/R.
static void build_pfck(const CorpusText& corpus, uint64_t n, uint64_t runs, const std::string& out) {
    double t0 = tnow();
    uint64_t tau = std::max<uint64_t>(1, (8 * n + runs - 1) / runs);
    uint64_t count = n / tau + 1;
    pf_check(::access(out.c_str(), F_OK) != 0, "pfck exists; refusing to clobber");
    int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    pf_check(fd >= 0, "create pfck");
    auto wfull = [&](const void* d, uint64_t len, uint64_t off) {
        const char* q = (const char*)d;
        while (len) {
            ssize_t z = ::pwrite(fd, q, len, off);
            pf_check(z > 0, "pfck write");
            q += z; off += uint64_t(z); len -= uint64_t(z);
        }
    };
    char hdr[40];
    uint32_t magic = 0x4B434650;  // "PFCK"
    uint32_t ver = 1; uint64_t rsv = 0;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &ver, 4);
    memcpy(hdr + 8, &n, 8); memcpy(hdr + 16, &tau, 8);
    memcpy(hdr + 24, &count, 8); memcpy(hdr + 32, &rsv, 8);
    wfull(hdr, 40, 0);
    constexpr uint64_t EMB = 1u << 22;
    constexpr uint64_t BW = 1u << 20;
    std::vector<uint64_t> ent;
    std::vector<uint8_t> band(BW);
    uint64_t h = 0;
    for (uint64_t hi = n; hi > 0; ) {
        uint64_t lo = hi > tau * EMB ? hi - tau * EMB : 0;
        ent.clear();
        uint64_t bh = hi;
        while (bh > lo) {
            uint64_t bl = bh > BW ? bh - BW : lo;
            for (uint64_t i = 0; i < bh - bl; ++i) band[i] = corpus[bl + i];
            for (uint64_t i = bh; i-- > bl; ) {
                h = uint64_t(band[i - bl]) + 1 + SlimFingerprint<CorpusText>::BASE * h;
                if (i % tau == 0) ent.push_back(h);
            }
            bh = bl;
        }
        std::reverse(ent.begin(), ent.end());
        uint64_t firstIdx = (lo + tau - 1) / tau;
        if (!ent.empty()) wfull(ent.data(), 8 * ent.size(), 40 + 8 * firstIdx);
        hi = lo;
    }
    if (n % tau == 0) { uint64_t zero = 0; wfull(&zero, 8, 40 + 8 * (n / tau)); }
    struct stat st {};
    pf_check(fstat(fd, &st) == 0 && uint64_t(st.st_size) == 40 + 8 * count, "pfck size");
    pf_check(::close(fd) == 0, "close pfck");
    fprintf(stderr, "PILE_FINISH_PFCK path=%s n=%llu tau=%llu count=%llu bytes=%llu wall=%.3f source=corpus_adapter byte_parity=merge_emit_pf\n",
            out.c_str(), (unsigned long long)n, (unsigned long long)tau, (unsigned long long)count,
            (unsigned long long)(40 + 8 * count), tnow() - t0);
}

// ------------------------------------------------------------ piece source
struct RawRun { uint8_t c; uint64_t len, head, tail; };

struct PieceSource {
    std::string prefix, dir;
    uint32_t S = 0;
    uint64_t R = 0, n = 0, B = 65536;
    unsigned w = 0;
    std::vector<std::pair<uint64_t, uint64_t>> shards;
    double pollTimeout = 86400;
    struct View { int fd = -1; uint64_t firstRun = 0, rc = 0; uint64_t offChars = 0, offLens = 0, offHeads = 0, offTails = 0; };
    mutable std::mutex mu;
    mutable std::map<std::pair<uint64_t, uint32_t>, View> views;
    mutable unsigned long opened = 0;

    void load_idx() {
        std::string path = prefix + ".runcols.idx";
        int fd = ::open(path.c_str(), O_RDONLY);
        double waited = 0;
        while (fd < 0) {
            if (waited > pollTimeout) pf_fail("timeout waiting for the merge's runcols idx");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            waited += 0.1;
            fd = ::open(path.c_str(), O_RDONLY);
        }
        char hdr[32];
        pf_read_full(fd, hdr, 32, "idx header");
        uint32_t magic;
        memcpy(&magic, hdr, 4);
        pf_check(magic == 0x31494352, "bad RCI1 magic");
        memcpy(&S, hdr + 4, 4); memcpy(&R, hdr + 8, 8); memcpy(&n, hdr + 16, 8);
        memcpy(&w, hdr + 24, 4); memcpy(&B, hdr + 28, 4);
        pf_check(R && B && w && w <= 64, "idx dims");
        shards.clear();
        uint64_t acc = 0;
        for (uint32_t s = 0; s < S; ++s) {
            char row[16];
            pf_read_full(fd, row, 16, "idx row");
            uint64_t base, cnt;
            memcpy(&base, row, 8); memcpy(&cnt, row + 8, 8);
            pf_check(base == acc, "idx shard bases not contiguous");
            acc += cnt;
            shards.push_back({base, cnt});
        }
        pf_check(acc == R, "idx shard counts != R");
        ::close(fd);
        dir = prefix + ".pieces";
        fprintf(stderr, "PILE_FINISH_IDX shards=%u runs=%llu n=%llu w=%u band=%llu\n",
                S, (unsigned long long)R, (unsigned long long)n, w, (unsigned long long)B);
    }
    uint32_t shard_of(uint64_t r) const {
        for (uint32_t s = 0; s < S; ++s)
            if (shards[s].second && r >= shards[s].first && r < shards[s].first + shards[s].second) return s;
        pf_fail("run outside the shard map");
        return 0;
    }
    const View& view(uint64_t run) const {
        uint64_t band = run / B;
        uint32_t shard = shard_of(run);
        auto key = std::make_pair(band, shard);
        std::lock_guard<std::mutex> lk(mu);
        auto it = views.find(key);
        if (it != views.end()) return it->second;
        std::string name = dir + "/p-" + std::to_string(band) + "-" + std::to_string(shard);
        int fd = ::open(name.c_str(), O_RDONLY);
        pf_check(fd >= 0, "index pread: open piece " + name);
        char hdr[44];
        pf_pread_full(fd, hdr, 44, 0, "piece header " + name);
        View v;
        uint64_t rc, firstRun;
        memcpy(&firstRun, hdr + 16, 8); memcpy(&rc, hdr + 24, 8);
        uint64_t words = (uint64_t(w) * rc + 63) / 64;
        v.fd = fd; v.firstRun = firstRun; v.rc = rc;
        v.offChars = 44; v.offLens = 44 + rc;
        v.offHeads = 44 + 5 * rc; v.offTails = 44 + 5 * rc + 8 * words;
        ++opened;
        return views.emplace(key, v).first->second;
    }
    uint64_t run_len(uint64_t run) const {
        auto& v = view(run);
        uint8_t b[4];
        pf_pread_full(v.fd, b, 4, v.offLens + 4 * (run - v.firstRun), "run len");
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    }
    uint8_t run_char(uint64_t run) const {
        auto& v = view(run);
        uint8_t c;
        pf_pread_full(v.fd, &c, 1, v.offChars + (run - v.firstRun), "run char");
        return c;
    }
    uint64_t samp_read(const View& v, uint64_t off, uint64_t idx) const {
        uint64_t bit = uint64_t(w) * idx, wi = bit >> 6, o = bit & 63;
        uint8_t buf[16];
        unsigned need = (o + w + 7) / 8;
        pf_pread_full(v.fd, buf, need, off + 8 * wi, "sample");
        uint64_t x = 0;
        for (unsigned i = 0; i < need; ++i) x |= uint64_t(buf[i]) << (8 * i);
        uint64_t val = x >> o;
        if (o + w > 64) {
            pf_pread_full(v.fd, buf, 8, off + 8 * (wi + 1), "sample2");
            uint64_t y = 0;
            for (unsigned i = 0; i < 8; ++i) y |= uint64_t(buf[i]) << (8 * i);
            val |= y << (64 - o);
        }
        return val & ((uint64_t(1) << w) - 1);
    }
    uint64_t run_head(uint64_t run) const { auto& v = view(run); return samp_read(v, v.offHeads, run - v.firstRun); }
    uint64_t run_tail(uint64_t run) const { auto& v = view(run); return samp_read(v, v.offTails, run - v.firstRun); }
    void read_band(uint64_t band, std::vector<RawRun>& out) {
        uint64_t lo = band * B, hi = std::min<uint64_t>((band + 1) * B, R);
        out.clear();
        out.reserve(hi - lo);
        uint64_t cursor = lo;
        std::vector<uint32_t> subs;
        for (uint32_t s = 0; s < S; ++s)
            if (shards[s].second && shards[s].first < hi && shards[s].first + shards[s].second > lo)
                subs.push_back(s);
        std::sort(subs.begin(), subs.end());
        for (uint32_t shard : subs) {
            std::string name = dir + "/p-" + std::to_string(band) + "-" + std::to_string(shard);
            double waited = 0;
            int fd = ::open(name.c_str(), O_RDONLY);
            while (fd < 0) {
                if (waited > pollTimeout) pf_fail("timeout waiting for piece " + name);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                waited += 0.02;
                fd = ::open(name.c_str(), O_RDONLY);
            }
            char hdr[44];
            pf_read_full(fd, hdr, 44, "piece header " + name);
            uint32_t magic, sh; uint64_t bnd, firstRun, rc, zero; unsigned ww;
            memcpy(&magic, hdr, 4); memcpy(&bnd, hdr + 4, 8); memcpy(&sh, hdr + 12, 4);
            memcpy(&firstRun, hdr + 16, 8); memcpy(&rc, hdr + 24, 8);
            memcpy(&ww, hdr + 32, 4); memcpy(&zero, hdr + 36, 8);
            pf_check(magic == 0x31435250 && bnd == band && sh == shard && ww == w && zero == 0,
                     "piece header " + name);
            pf_check(firstRun == cursor, "piece coverage: firstRun != cursor at " + name);
            std::vector<uint8_t> chars(rc);
            std::vector<uint32_t> lens(rc);
            uint64_t words = (uint64_t(w) * rc + 63) / 64;
            std::vector<uint64_t> hp(words ? words : 1), tp(words ? words : 1);
            pf_read_full(fd, chars.data(), rc, "piece chars " + name);
            pf_read_full(fd, lens.data(), 4 * rc, "piece lens " + name);
            if (words) {
                pf_read_full(fd, hp.data(), 8 * words, "piece heads " + name);
                pf_read_full(fd, tp.data(), 8 * words, "piece tails " + name);
            }
            ::close(fd);
            {
                std::lock_guard<std::mutex> lk(mu);
                views.erase({band, shard});
            }
            pf_check(::unlink(name.c_str()) == 0, "unlink piece " + name);
            auto unpack = [&](const std::vector<uint64_t>& buf, uint64_t i) {
                uint64_t b = uint64_t(w) * i, wi = b >> 6, o = b & 63;
                uint64_t v = (buf[wi] >> o);
                if (o + w > 64 && wi + 1 < buf.size()) v |= buf[wi + 1] << (64 - o);
                return v & ((uint64_t(1) << w) - 1);
            };
            for (uint64_t i = 0; i < rc; ++i)
                out.push_back({chars[i], lens[i], unpack(hp, i), unpack(tp, i)});
            cursor += rc;
        }
        pf_check(cursor == hi, "band coverage != band end");
        std::string sent = prefix + ".runcols.consumed", tmp = sent + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        pf_check(f, "open consumed tmp");
        fprintf(f, "%llu\n", (unsigned long long)band);
        pf_check(fclose(f) == 0, "close consumed tmp");
        pf_check(::rename(tmp.c_str(), sent.c_str()) == 0, "publish consumed sentinel");
    }
    template <typename F>
    void scan_columns(F&& visit) {
        uint64_t nbands = (R + B - 1) / B;
        for (uint64_t band = 0; band < nbands; ++band) {
            uint64_t lo = band * B, hi = std::min<uint64_t>((band + 1) * B, R);
            std::vector<uint32_t> subs;
            for (uint32_t s = 0; s < S; ++s)
                if (shards[s].second && shards[s].first < hi && shards[s].first + shards[s].second > lo)
                    subs.push_back(s);
            std::sort(subs.begin(), subs.end());
            for (uint32_t shard : subs) {
                std::string name = dir + "/p-" + std::to_string(band) + "-" + std::to_string(shard);
                double waited = 0;
                int fd = ::open(name.c_str(), O_RDONLY);
                while (fd < 0) {
                    if (waited > pollTimeout) pf_fail("timeout waiting for piece " + name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    waited += 0.02;
                    fd = ::open(name.c_str(), O_RDONLY);
                }
                char hdr[44];
                pf_read_full(fd, hdr, 44, "piece header " + name);
                uint64_t rc;
                memcpy(&rc, hdr + 24, 8);
                std::vector<uint8_t> chars(rc);
                std::vector<uint32_t> lens(rc);
                pf_read_full(fd, chars.data(), rc, "piece chars " + name);
                pf_read_full(fd, lens.data(), 4 * rc, "piece lens " + name);
                ::close(fd);
                for (uint64_t i = 0; i < rc; ++i) visit(chars[i], lens[i]);
            }
        }
    }
};

// ------------------------------------------------------- padded frame index
// ----------------------------------------------------------- seam classes
// The v1 chain's cyclic seam repair (rpfbwt_endpoints + seam_repair.hpp)
// recomputed with NO record/seed columns and NO piece random access:
//
//  * DISCOVERY: the backward search from row zero extends the terminal
//    suffix pattern P_d = T[n-d..n). The interval at depth d is exactly the
//    occurrence set occ(P_d) (every suffix with prefix P_d; the pattern's
//    own occurrence at n-d is the interval's leftmost row because its
//    continuation is the 0x02 padding - the smallest byte). The chain
//    refines occ_{d+1} = {p : p+1 in occ_d and T[p] == T[n-d-1]} from the
//    ONE sequential corpus scan for the terminal byte.
//  * ROW INTERVALS: lo = row(first) by ONE binary search over the merge's
//    own WM scratch (the merged SA order; SA(row) = WM[row-10]), hi =
//    lo + |occ|. before/after = SA(lo-1)/SA(hi) by single WM preads.
//  * REBUILD: the occurrence set is LCE-sorted with the v1 comparator and
//    rebuilt into runs - byte-identical class contents to the v1 chain.
struct SeamClass { uint64_t lo, hi, first, before = 0, after = 0;
                   std::vector<uint64_t> occ;     // the class SA set (occ(P_d))
                   std::vector<RawRun> runs; };   // lo/hi in NORMALIZED rows

static void seam_refuse(const char* kind, uint64_t size, uint64_t limit) {
    fprintf(stderr, "CYCLIC_SEAM_REFUSED %s=%llu limit=%llu policy=max(1000,raw_r/1000); no O(n) fallback\n",
            kind, (unsigned long long)size, (unsigned long long)limit);
    pf_fail("cyclic seam repair exceeds compressed-work policy");
}

struct SeamClasses {
    std::vector<SeamClass> classes;
    uint64_t depth = 0, total = 0, maxSeamLce = 0;
    // Binary search over the WM scratch: smallest row index with SA >= target.
    static uint64_t wm_lower_bound(int fd, uint64_t n, uint64_t target) {
        uint64_t lo = 0, hi = n;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            uint8_t b[8];
            pf_pread_full(fd, b, 8, 8 * mid, "wm pread");
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v |= uint64_t(b[i]) << (8 * i);
            if (v < target) lo = mid + 1; else hi = mid;
        }
        return lo;
    }
    static uint64_t wm_at(int fd, uint64_t idx) {
        uint8_t b[8];
        pf_pread_full(fd, b, 8, 8 * idx, "wm pread");
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(b[i]) << (8 * i);
        return v;
    }
    void build(const CorpusText& text, int wmFd, uint64_t n, uint64_t rawR, uint64_t w1,
               const std::function<uint64_t(uint64_t, uint64_t)>& collection_lce,
               const std::function<uint8_t(uint64_t)>& symbol) {
        uint64_t N = n + 10, r = rawR;
        uint64_t limit = std::max<uint64_t>(1000, r / 1000);
        uint8_t terminal = text[n - 1];
        // occ_1: every position of the terminal byte (one sequential scan).
        std::vector<uint64_t> occ;
        {
            constexpr uint64_t RB = 1 << 22;
            std::vector<uint8_t> b(RB);
            uint64_t off = 0;
            while (off < n) {
                uint64_t want = std::min<uint64_t>(RB, n - off), done = 0;
                while (done < want) {
                    ssize_t z = ::pread(text.fd, b.data() + done, want - done, off + done);
                    pf_check(z > 0, "occ scan read");
                    done += uint64_t(z);
                }
                for (uint64_t i = 0; i < want; ++i)
                    if (text.remap[b[i]] == terminal) occ.push_back(off + i);
                off += want;
            }
        }
        fprintf(stderr, "PILE_FINISH_SEAM_OCC terminal=0x%02x depth0_count=%llu scan=corpus\n",
                (unsigned long long)terminal, (unsigned long long)occ.size());
        // The discovery chain: v1 pushes a class per depth while the
        // interval has more than one row, and stops at the unique row.
        std::vector<SeamClass> raw;
        uint64_t d = 0;
        for (;;) {
            ++d;
            if (uint64_t(occ.size()) > limit) seam_refuse("class_size", occ.size(), limit);
            if (occ.size() <= 1) break;
            if (d > limit) seam_refuse("discovery_steps", d, limit);
            uint64_t first = n - d;
            pf_check(first < n, "seam pattern escaped the text");
            uint64_t row = wm_lower_bound(wmFd, n, first) + w1;
            pf_check(wm_at(wmFd, row - w1) == first, "terminal suffix not found in the WM order");
            uint64_t hi = row + occ.size();
            pf_check(row >= w1 && hi - w1 <= n - 1, "terminal suffix interval bounds");
            pf_check(row == wm_lower_bound(wmFd, n, first) + w1, "terminal suffix row");
            raw.push_back({row - w1, hi - w1, first, 0, 0, occ, {}});
            depth = d;
            // refine: occ_{d+1} = {p : p+1 in occ_d and T[p] == T[n-d-1]}
            if (first == 0) seam_refuse("discovery_steps", d + 1, limit);
            uint8_t want = text[first - 1];
            pf_check(want != 2, "unexpected dollar during seam discovery");
            std::vector<uint64_t> next;
            next.reserve(occ.size());
            for (uint64_t q : occ) {
                if (q == 0) continue;   // position 0 has no predecessor byte
                if (text[q - 1] == want) next.push_back(q - 1);
            }
            occ.swap(next);
        }
        // v1: sort by lo, drop nested row intervals, refuse on total rows.
        std::sort(raw.begin(), raw.end(), [](const SeamClass& a, const SeamClass& b) {
            return a.lo < b.lo || (a.lo == b.lo && a.hi > b.hi);
        });
        for (size_t i = 0; i < raw.size(); ++i) {
            if (!classes.empty() && raw[i].lo < classes.back().hi) {
                pf_check(raw[i].hi <= classes.back().hi, "crossing prefix intervals");
                continue;
            }
            classes.push_back(std::move(raw[i]));
        }
        for (auto& cl : classes) total += cl.hi - cl.lo;
        if (total > limit) seam_refuse("total_class_rows", total, limit);
        // before = SA(row(lo)-1) = WM[lo-1]; after = SA(row(hi)) = WM[hi].
        // (row(lo_norm) = lo_norm + w1, and WM[i] = SA(row i+10).)
        for (auto& cl : classes) cl.before = wm_at(wmFd, cl.lo - 1);
        for (auto& cl : classes) cl.after = wm_at(wmFd, cl.hi);
        // rebuild each class: v1's LCE comparator + run rebuild, verbatim.
        uint64_t maxSeam = 0;
        for (auto& cl : classes) {
            std::vector<uint64_t> sa = cl.occ;
            pf_check(sa.size() == cl.hi - cl.lo, "class occurrence count");
            for (uint64_t p : sa) pf_check(p < n, "seam sample outside T");
            std::sort(sa.begin(), sa.end(), [&](uint64_t a, uint64_t b) {
                uint64_t len = collection_lce(a, b);
                maxSeam = std::max(maxSeam, len);
                return len == n ? a < b : symbol((a + len) % n) < symbol((b + len) % n);
            });
            for (uint64_t p : sa) {
                uint8_t c = symbol((p + n - 1) % n);
                if (!cl.runs.empty() && cl.runs.back().c == c) {
                    ++cl.runs.back().len;
                    cl.runs.back().tail = p;
                } else {
                    cl.runs.push_back({c, 1, p, p});
                }
            }
        }
        maxSeamLce = maxSeam;
        fprintf(stderr, "CYCLIC_SEAM_REPAIRED classes=%zu rows=%llu discovery_steps=%llu limit=%llu phi_samples=%llu pattern_chain=1 wm_preads=1\n",
                classes.size(), (unsigned long long)total, (unsigned long long)depth,
                (unsigned long long)limit, (unsigned long long)r);
        fprintf(stderr, "CYCLIC_SEAM_WORK max_seam_lce=%llu exact=1 pattern_chain=1\n", (unsigned long long)maxSeam);
    }
};

// ---------------------------------------------------------- repair stream
struct NRun { uint8_t c; uint64_t len, h, t; };
struct RepairStream {
    uint64_t N = 0, n = 0, w1 = 10, terminal = 0;
    SeamClasses* seam = nullptr;
    std::function<void(const NRun&)> emit;
    bool sampleChecks = true;   // false for the m1 replay (samples absent)
    uint64_t rows = 0;
    bool srcHave = false;
    RawRun srcPending{};
    bool certified = true, dollarSeen = false;
    uint64_t zeroes = 0, beforeTerminal = 0;
    uint64_t row = 0;
    size_t next = 0;
    bool have = false;
    NRun pending{};
    uint64_t rNorm = 0, nNorm = 0;
    std::array<uint64_t, 256> totals{}, runFreq{};
    bool firstRunSeen = false;
    void padding_check(bool ok, const char* site, uint64_t row2, uint64_t c, uint64_t len, uint64_t head, uint64_t tail) const {
        if (ok) return;
        fprintf(stderr, "PFP_PADDING_REFUSED site=%s row=%llu char=0x%02llx len=%llu head=%llu tail=%llu terminal=0x%02llx\n",
                site, (unsigned long long)row2, (unsigned long long)c, (unsigned long long)len,
                (unsigned long long)head, (unsigned long long)tail, (unsigned long long)terminal);
        pf_fail("unsupported PFP padding boundary");
    }
    void append(const NRun& v) {
        if (!v.len) return;
        if (have && pending.c == v.c) { pending.len += v.len; pending.t = v.t; }
        else { if (have) emitOne(); pending = v; have = true; }
    }
    void emitOne() {
        ++rNorm; nNorm += pending.len;
        totals[pending.c] += pending.len;
        ++runFreq[pending.c];
        emit(pending);
    }
    RawRun trim_padding(const RawRun& in, uint64_t row2) const {
        RawRun v = in;
        if (row2 >= w1) return v;
        if (sampleChecks)
            padding_check(row2 == 0 ? (v.c == terminal && v.len == 1 && v.head == n && v.tail == n) : v.c == 2,
                          "prefix", row2, v.c, v.len, v.head, v.tail);
        else if (row2 == 0)
            padding_check(v.c == terminal && v.len == 1, "prefix", row2, v.c, v.len, v.head, v.tail);
        uint64_t take = std::min(w1 - row2, v.len);
        v.len -= take;
        if (v.len) {
            if (sampleChecks)
                padding_check(v.c == 2 && v.len == 1 && v.tail == 0, "straddle", row2, v.c, v.len + take, v.head, v.tail);
            v.head = 0;
        }
        return v;
    }
    void feed_raw(const RawRun& raw) {
        if (!firstRunSeen) {
            firstRunSeen = true;
            if (sampleChecks)
                padding_check(raw.c == terminal && raw.len == 1 && raw.head == n && raw.tail == n,
                              "terminal-row", 0, raw.c, raw.len, raw.head, raw.tail);
        }
        pf_check(raw.len && raw.len <= N - rows, "RLE length");
        uint64_t rawLen = raw.len;
        RawRun v = trim_padding(raw, rows);
        rows += rawLen;
        if (!v.len) return;
        if (sampleChecks) pf_check(v.head < n && v.tail < n, "endpoint outside normalized text");
        if (v.c == 2) {
            if (sampleChecks)
                pf_check(v.len == 1 && v.head == 0 && v.tail == 0, "invalid SA-zero dollar row");
            if (beforeTerminal)
                fprintf(stderr, "CYCLIC_SEAM_REPAIR terminal=0x%02llx predecessors_before_zero=%llu\n",
                        (unsigned long long)terminal, (unsigned long long)beforeTerminal);
            certified = beforeTerminal == 0;
            ++zeroes;
            dollarSeen = true;
        }
        if (v.c == terminal) beforeTerminal += v.len;
        if (v.c == 2) v.c = uint8_t(terminal);
        pf_check(v.c >= 6 && v.c < 256, "unsupported text alphabet");
        if (srcHave && srcPending.c == v.c) { srcPending.len += v.len; srcPending.tail = v.tail; }
        else {
            if (srcHave) seamSplice({srcPending.c, srcPending.len, srcPending.head, srcPending.tail});
            srcPending = v; srcHave = true;
        }
    }
    void seamSplice(NRun v) {
        uint64_t end = row + v.len;
        while (row < end) {
            if (seam && next < seam->classes.size() && row >= seam->classes[next].lo) {
                SeamClass& cl = seam->classes[next];
                if (row == cl.lo)
                    for (const auto& piece : cl.runs)
                        append({piece.c, piece.len, piece.head, piece.tail});
                row = std::min(end, cl.hi);
                if (row == cl.hi) { v.h = cl.after; ++next; }
            } else {
                uint64_t stop = (seam && next < seam->classes.size()) ? std::min(end, seam->classes[next].lo) : end;
                NRun piece{v.c, stop - row, v.h, v.t};
                if (stop < end && seam) piece.t = seam->classes[next].before;
                append(piece);
                row = stop;
            }
        }
    }
    void finishStream() {
        if (srcHave) { seamSplice({srcPending.c, srcPending.len, srcPending.head, srcPending.tail}); srcHave = false; }
        if (have) { emitOne(); have = false; }
        pf_check(rows == N, "RLE metadata/trailing rows mismatch");
        pf_check(nNorm == n, "normalized length mismatch");
        pf_check(zeroes == 1 && dollarSeen, "invalid padded cyclic frame");
        fprintf(stderr, "ENDPOINT_STREAM_PASS n=%llu k=1 r=%llu padding_rows=%llu cyclic_seam_certified=%d classes=%zu\n",
                (unsigned long long)n, (unsigned long long)rNorm, (unsigned long long)w1,
                int(certified), seam ? seam->classes.size() : 0);
    }
};

// ------------------------------------------------------------- sweep state
// Verbatim port of the production scan-rs state machine (xsa chi-rspace,
// bit6/teralcp_chi.cpp): no walks, no SA, no text access in the sweep
// itself. Cyclic corpora only (the pile class); newline-class corpora are
// refused and use the v1 chain.
struct Sweep {
    bool cyclic = true;
    uint64_t n = 0, bigN = 0, chi = 0;
    std::function<void(uint64_t)> emitWitness;
    static constexpr int64_t IINF = INT64_MAX;
    int64_t rr_len[256];
    uint64_t rr_pos[256];
    bool rr_act[256];
    uint64_t previous_interior = INF;
    int64_t m = IINF;
    int32_t p = -1;
    uint64_t p_sa_last = 0;
    bool first = true;
    explicit Sweep(uint64_t nNorm) : n(nNorm), bigN(nNorm) {
        for (int c = 0; c < 256; ++c) { rr_len[c] = -1; rr_pos[c] = 0; rr_act[c] = false; }
    }
    uint64_t witness(uint64_t pos) const {
        pf_check(pos < n, "chi-rspace: endpoint outside text");
        if (cyclic) return pos == 0 ? 0 : n - pos;
        return bigN - pos;
    }
    void emit(uint64_t value) {
        ++chi;
        emitWitness(value);
    }
    void advance(const NRun& v, uint64_t topLcp, uint64_t interiorMin) {
        int32_t c = v.c;
        int64_t lcp_b = int64_t(topLcp);
        if (first) {
            p = c;
            p_sa_last = v.t;
            previous_interior = interiorMin;
            m = IINF;
            first = false;
            return;
        }
        int64_t m2 = previous_interior == INF ? IINF : int64_t(previous_interior);
        previous_interior = interiorMin;
        int64_t mm = std::min(m, m2);
        if (c != p) {
            int64_t m3 = std::min(mm, lcp_b);
            for (int cc = 1; cc < 256; ++cc) {
                if (m3 < rr_len[cc]) {
                    if (rr_act[cc]) emit(rr_pos[cc]);
                    rr_len[cc] = m3; rr_pos[cc] = 0; rr_act[cc] = false;
                }
            }
            if (lcp_b > rr_len[p]) { rr_len[p] = lcp_b; rr_pos[p] = witness(p_sa_last); rr_act[p] = true; }
            if (lcp_b > rr_len[c]) { rr_len[c] = lcp_b; rr_pos[c] = witness(v.h); rr_act[c] = true; }
            m = IINF;
        } else {
            m = std::min(mm, lcp_b);
        }
        p = c;
        p_sa_last = v.t;
    }
    void finish() {
        for (int cc = 1; cc < 256; ++cc) {
            if (int64_t(-1) < rr_len[cc]) {
                if (rr_act[cc]) emit(rr_pos[cc]);
                rr_len[cc] = -1; rr_pos[cc] = 0; rr_act[cc] = false;
            }
        }
    }
};

// ----------------------------------------------------------- witness buckets
// VALUE-RANGE buckets: the sweep appends each witness to its bucket's RAM
// buffer; when the global RAM budget is exceeded the largest bucket is
// sorted and appended to its disc file as a delta-varint run. The EF pass
// walks buckets in value order (ranges are disjoint), merging each
// bucket's RAM part with its sorted runs.
struct WitBuckets {
    uint64_t P = 0, n = 0;
    std::string prefix;
    uint64_t ramBudget = 1ull << 30;
    std::vector<std::vector<uint64_t>> buf;
    std::vector<int> fds;
    uint64_t totalRam = 0, totalValues = 0, spilled = 0;
    WitBuckets(uint64_t nNorm, uint64_t runs, const std::string& pfx, uint64_t budget)
        : n(nNorm), prefix(pfx), ramBudget(budget) {
        P = 4096;
        while (P < 65536 && P * 262144 < runs) P *= 2;
        buf.resize(P);
        fds.assign(P, -1);
    }
    uint64_t bucketOf(uint64_t v) const { return v * P / n; }
    void add(uint64_t v) {
        pf_check(v < n, "witness out of range");
        uint64_t b = bucketOf(v);
        buf[b].push_back(v);
        totalRam += 8;
        ++totalValues;
        while (totalRam > ramBudget) spillLargest();
    }
    static void putVarint(std::vector<uint8_t>& out, uint64_t v) {
        do {
            uint8_t b = v & 127;
            v >>= 7;
            if (v) b |= 128;
            out.push_back(b);
        } while (v);
    }
    void spillLargest() {
        uint64_t best = 0;
        for (uint64_t b = 1; b < P; ++b) if (buf[b].size() > buf[best].size()) best = b;
        if (buf[best].empty()) { totalRam = 0; return; }
        std::vector<uint64_t>& v = buf[best];
        std::sort(v.begin(), v.end());
        std::vector<uint8_t> out;
        out.reserve(v.size() + 16);
        uint32_t cnt = uint32_t(v.size());
        out.insert(out.end(), (const char*)&cnt, (const char*)&cnt + 4);
        putVarint(out, v[0]);
        for (size_t i = 1; i < v.size(); ++i) putVarint(out, v[i] - v[i - 1]);
        if (fds[best] < 0) {
            std::string name = prefix + ".wit-" + std::to_string(best);
            fds[best] = ::open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0666);
            pf_check(fds[best] >= 0, "open witness bucket");
        }
        pf_write_full(fds[best], out.data(), out.size(), "witness bucket");
        totalRam -= 8 * v.size();
        v.clear();
        ++spilled;
    }
    // Iterate all witnesses in ascending order (buckets are value-disjoint,
    // processed in value order; each bucket = RAM part + sorted disc runs).
    template <typename Visit>
    void for_each_sorted(Visit&& visit) {
        std::vector<uint64_t> vals;
        for (uint64_t b = 0; b < P; ++b) {
            vals = buf[b];
            if (fds[b] >= 0) {
                std::string name = prefix + ".wit-" + std::to_string(b);
                int fd = ::open(name.c_str(), O_RDONLY);
                pf_check(fd >= 0, "open witness bucket " + name);
                struct stat st {};
                pf_check(fstat(fd, &st) == 0, "stat witness bucket");
                std::vector<uint8_t> data(uint64_t(st.st_size));
                pf_read_full(fd, data.data(), data.size(), "witness bucket");
                ::close(fd);
                size_t pos = 0;
                while (pos < data.size()) {
                    uint32_t cnt;
                    pf_check(pos + 4 <= data.size(), "witness run truncated");
                    memcpy(&cnt, data.data() + pos, 4); pos += 4;
                    uint64_t prev = 0;
                    for (uint32_t i = 0; i < cnt; ++i) {
                        uint64_t v = 0; unsigned shift = 0;
                        for (;;) {
                            pf_check(pos < data.size(), "witness varint truncated");
                            uint8_t x = data[pos++];
                            v |= uint64_t(x & 127) << shift;
                            if (!(x & 128)) break;
                            shift += 7;
                            pf_check(shift < 64, "witness varint overflow");
                        }
                        vals.push_back(prev + v);
                        prev += v;
                    }
                }
            }
            std::sort(vals.begin(), vals.end());
            for (uint64_t v : vals) visit(v);
        }
    }
    uint64_t discBytes() const {
        uint64_t t = 0;
        for (uint64_t b = 0; b < P; ++b)
            if (fds[b] >= 0) {
                struct stat st {};
                if (fstat(fds[b], &st) == 0) t += uint64_t(st.st_size);
            }
        return t;
    }
    void cleanup() {
        for (uint64_t b = 0; b < P; ++b)
            if (fds[b] >= 0) {
                ::close(fds[b]);
                fds[b] = -1;
                pf_check(::unlink((prefix + ".wit-" + std::to_string(b)).c_str()) == 0, "unlink witness bucket");
            }
    }
};

// --------------------------------------------------------------- bit output
// v1 sxi2_write Bits: LSB-first within bytes; msb(v,w) big-endian inside
// the code; gamma(v) = (w-1) zero bits then w-bit msb.
struct BitsOut {
    std::vector<uint8_t> mem;
    uint64_t bitPos = 0;
    void bit(unsigned b) {
        if (bitPos % 8 == 0) mem.push_back(0);
        if (b & 1) mem[bitPos / 8] |= uint8_t(1) << (bitPos % 8);
        ++bitPos;
    }
    void msb(uint64_t v, unsigned w) { for (unsigned i = w; i--;) bit(unsigned((v >> i) & 1)); }
    void lsb(uint64_t v, unsigned w) { for (unsigned i = 0; i < w; ++i) bit(unsigned((v >> i) & 1)); }
    void gamma(uint64_t v) {
        pf_check(v > 0, "zero gamma");
        unsigned w = 64 - __builtin_clzll(v);
        for (unsigned i = 1; i < w; ++i) bit(0);
        msb(v, w);
    }
    size_t bytes() const { return mem.size(); }
};

// ------------------------------------------------------------ container out
// SXI2 v6 writer (the lite member set). Mirrors bit6/sxi2_write.cpp's Out
// (member crc = ~crc(payload); header crc at +56 over the header) minus the
// fork validator (the lite container has no banked sxi-info path; the gates
// verify it by cross-shape byte-identity and structural self-checks).
static void pf_pwrite_full_or_seek(int fd, const unsigned char* p, size_t z, uint64_t off) {
    while (z) {
        ssize_t w = ::pwrite(fd, p, z, off);
        pf_check(w > 0, "container pwrite");
        p += w; off += uint64_t(w); z -= size_t(w);
    }
}
struct ContainerOut {
    std::string path, temp;
    int fd = -1;
    unsigned count = 0;
    std::vector<sxi::Member> ms;
    bool done = false;
    ContainerOut(const std::string& p, unsigned count_) : path(p), temp(p + ".partial"), count(count_) {
        pf_check(::access(path.c_str(), F_OK) != 0, "output exists; refusing to clobber");
        fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        pf_check(fd >= 0, "create container partial");
        std::vector<char> z(64 + 40 * count, 0);
        pf_write_full(fd, z.data(), z.size(), "container header reserve");
    }
    ~ContainerOut() { if (!done && fd >= 0) { ::close(fd); ::unlink(temp.c_str()); } }
    // begin a member at the next 8-aligned offset after the header reserve
    void begin(unsigned id, unsigned codec, uint64_t cnt) {
        uint64_t here = 64 + 40 * count;
        for (auto& m : ms) here = std::max(here, m.offset + m.bytes);
        here = (here + 7) / 8 * 8;
        ms.push_back({id, codec, here, 0, cnt, ~0u});
        fprintf(stderr, "PILE_CONTAINER_BEGIN id=%u codec=%u count=%llu offset=%llu\n",
                id, codec, (unsigned long long)cnt, (unsigned long long)here);
    }
    void write(const void* p, size_t z) {
        auto& m = ms.back();
        const unsigned char* q = (const unsigned char*)p;
        pf_pwrite_full_or_seek(fd, q, z, m.offset + m.bytes);
        m.bytes += z;
        m.checksum = crc(m.checksum, q, z);
    }
    void num(uint64_t v, unsigned w = 8) {
        unsigned char b[8]{};
        put(b, v, w);
        write(b, w);
    }
    void finish(uint64_t n, uint64_t k, uint64_t r, uint64_t flags) {
        std::sort(ms.begin(), ms.end(), [](const sxi::Member& a, const sxi::Member& b) { return a.id < b.id; });
        uint64_t length = 0;
        for (auto& m : ms) length = std::max(length, m.offset + m.bytes);
        std::vector<unsigned char> h(64 + 40 * ms.size());
        memcpy(h.data(), "SXI2", 4);
        put(h.data() + 4, 6, 4); put(h.data() + 8, n); put(h.data() + 16, k);
        put(h.data() + 24, r); put(h.data() + 32, ms.size(), 4); put(h.data() + 36, h.size(), 4);
        put(h.data() + 40, length); put(h.data() + 48, flags);
        for (size_t i = 0; i < ms.size(); ++i) {
            auto& m = ms[i];
            auto* d = h.data() + 64 + 40 * i;
            put(d, m.id, 4); put(d + 4, m.codec, 4); put(d + 8, m.offset);
            put(d + 16, m.bytes); put(d + 24, m.count); put(d + 32, ~m.checksum, 4);
            fprintf(stderr, "SXI2_MEMBER id=%u codec=%u count=%llu bytes=%llu\n",
                    m.id, m.codec, (unsigned long long)m.count, (unsigned long long)m.bytes);
        }
        put(h.data() + 56, ~crc(~0u, h.data(), h.size()), 4);
        pf_pwrite_full_or_seek(fd, h.data(), h.size(), 0);
        pf_check(fsync(fd) == 0, "fsync container");
        pf_check(::close(fd) == 0, "close container");
        fd = -1;
        pf_check(::link(temp.c_str(), path.c_str()) == 0, "publish container (exists?)");
        pf_check(::unlink(temp.c_str()) == 0, "unlink container partial");
        done = true;
        fprintf(stderr, "SXI2_PASS bytes=%llu lite=1 members=%zu\n", (unsigned long long)length, ms.size());
    }
};

// ------------------------------------------------------------- LCE wrapper
// Cyclic collection_lce over the corpus adapter (SlimLCEParseFree's cyclic
// branch, byte-for-byte), with its own fail-loud work journal.
struct LceWrap {
    SlimFingerprint<CorpusText>* fh = nullptr;
    SlimSharded seedQueries;
    uint64_t n = 0;
    void flush_thread() const { fh->flushThread(); seedQueries.flushThread(); }
    uint64_t operator()(uint64_t i, uint64_t j) const {
        uint64_t size = n, answer = 0;
        if (i >= size || j >= size) pf_fail("cyclic LCE bounds");
        if (i == j) { seedQueries.add(1); return size; }
        while (answer < size) {
            uint64_t cap = std::min({size - answer, size - i, size - j});
            uint64_t got = std::min(fh->lce(i, j, cap), cap);
            answer += got;
            if (got < cap) break;
            i = (i + got) % size; j = (j + got) % size;
        }
        return answer;
    }
};
static void fp_self_check(SlimFingerprint<CorpusText>& fh, const CorpusText& text, uint64_t n, uint64_t tau) {
    uint64_t checks = std::min<uint64_t>(256, n);
    for (uint64_t k = 0; k < checks; ++k) {
        uint64_t p = n ? (k * 2654435761ULL + 0x9e3779b9ULL) % n : 0;
        uint64_t len = std::min<uint64_t>(tau, n - p);
        if (!len) continue;
        uint64_t direct = 0, pk = 1;
        for (uint64_t i = 0; i < len; ++i) { direct += (uint64_t(text[p + i]) + 1) * pk; pk *= SlimFingerprint<CorpusText>::BASE; }
        uint64_t fromFp = fh.suffix(p) - SlimFingerprint<CorpusText>::power(len) * fh.suffix(p + len);
        pf_check(direct == fromFp, "corpus fingerprint self-check mismatch");
    }
    fprintf(stderr, "SLIM_PF_SELF_CHECK checks=%llu passed=1 source=corpus_adapter\n", (unsigned long long)checks);
}

int main(int argc, char** argv) {
    try {
        std::string prefix, corpus, remap, out, bridgeDir, sampleCheck, wmPath;
        uint64_t chiExpect = 0, sampleCount = 1024, pollTimeout = 86400, ramBudget = 1ull << 30;
        int threads = int(std::thread::hardware_concurrency());
        bool keepRlebwt = false, keepPfck = false;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto need = [&](const char* what) -> std::string {
                pf_check(i + 1 < argc, std::string(what) + " requires a value");
                return argv[++i];
            };
            if (a == "--prefix") prefix = need("--prefix");
            else if (a == "--corpus") corpus = need("--corpus");
            else if (a == "--remap") remap = need("--remap");
            else if (a == "--output") out = need("--output");
            else if (a == "--bridge-dir") bridgeDir = need("--bridge-dir");
            else if (a == "--witness-sample-check") sampleCheck = need("--witness-sample-check");
            else if (a == "--wm") wmPath = need("--wm");
            else if (a == "--chi-expect") chiExpect = strtoull(need("--chi-expect").c_str(), nullptr, 10);
            else if (a == "--sample-count") sampleCount = strtoull(need("--sample-count").c_str(), nullptr, 10);
            else if (a == "--poll-timeout") pollTimeout = strtoull(need("--poll-timeout").c_str(), nullptr, 10);
            else if (a == "--threads") threads = atoi(need("--threads").c_str());
            else if (a == "--wit-ram-budget") ramBudget = strtoull(need("--wit-ram-budget").c_str(), nullptr, 10);
            else if (a == "--keep-rlebwt") keepRlebwt = true;
            else if (a == "--keep-pfck") keepPfck = true;
            else pf_fail("unknown argument " + a);
        }
        pf_check(!prefix.empty() && !corpus.empty() && !remap.empty() && !out.empty(),
                 "usage: pile_finish --prefix MERGED/frag --corpus CORPUS --remap MAP.bin --output CONTAINER.sxi2 "
                 "[--bridge-dir DIR] [--witness-sample-check BANKED.sA] [--chi-expect N] [--threads N] "
                 "[--keep-rlebwt] [--keep-pfck] [--poll-timeout S]");
        double last = tnow();
        uint8_t remapTable[256];
        load_remap_table(remap, remapTable);
        PieceSource src;
        src.prefix = prefix;
        src.pollTimeout = pollTimeout;
        src.load_idx();
        const uint64_t n = src.n, N = n + 10, R = src.R;
        CorpusText text(corpus, remapTable);
        pf_check(text.size() == n, "corpus size != the merge's text length (adapter: T[i]=REMAP[corpus[i]])");

        // ---- cyclic determination (v1 parity: has_rs || !has_nl over the
        // normalized text, which is the corpus byte stream - identity-gated)
        bool hasRS = false, hasNL = false;
        {
            constexpr uint64_t RB = 1 << 22;
            std::vector<uint8_t> b(RB);
            uint64_t off = 0;
            while (off < n && !hasRS) {
                uint64_t want = std::min<uint64_t>(RB, n - off), done = 0;
                while (done < want) {
                    ssize_t z = ::pread(text.fd, b.data() + done, want - done, off + done);
                    pf_check(z > 0, "cyclic scan read");
                    done += uint64_t(z);
                }
                for (uint64_t i = 0; i < want; ++i) {
                    if (b[i] == 0x1e) { hasRS = true; break; }
                    if (b[i] == 0x0a) hasNL = true;
                }
                off += want;
            }
        }
        bool cyclic = hasRS || !hasNL;
        pf_check(cyclic, "newline-class corpora (no 0x1E, newlines present) use the v1 chain; the v2 finish refuses");
        fprintf(stderr, "PILE_FINISH_CYCLIC=1 has_rs=%d has_nl=%d k=1\n", int(hasRS), int(hasNL));

        // ---- PFCK from the corpus; two fingerprint instances (seam 16n
        // budget, slim 128n budget - the v1 discipline), both corpus-backed
        std::string pfckPath = prefix + ".pfck2";
        build_pfck(text, n, R, pfckPath);
        phase("pfck-corpus", last);
        uint64_t tau = std::max<uint64_t>(1, (8 * n + R - 1) / R);
        uint64_t ckptCount = n / tau + 1;
        int pfckFd = ::open(pfckPath.c_str(), O_RDONLY);
        pf_check(pfckFd >= 0, "reopen pfck");
        SlimFingerprint<CorpusText> fhSeam(text, tau, false, pfckFd, 40, ckptCount);
        SlimSeamWork workSeam(std::max<uint64_t>(100000000ULL, 16 * n), UINT64_MAX);
        fhSeam.seam_policy(workSeam, tau, "pile-finish-seam");
        fp_self_check(fhSeam, text, n, tau);
        SlimFingerprint<CorpusText> fhSlim(text, tau, false, pfckFd, 40, ckptCount);
        SlimSeamWork workSlim(std::max<uint64_t>(1000000000ULL, 128 * n), UINT64_MAX);
        fhSlim.seam_policy(workSlim, tau, "pile-finish-corpus");
        fp_self_check(fhSlim, text, n, tau);
        phase("fingerprints", last);

        // ---- seam classes: pattern chain + the merge's own WM scratch
        // (NO record/seed columns; the discovery is the occurrence chain of
        // P_d = T[n-d..n), row intervals/before/after by WM binary search)
        int wmFd = -1;
        {
            std::string glob = wmPath.empty() ? prefix + ".xsc-*/wm.bin" : wmPath;
            double waited = 0;
            for (;;) {
                if (!wmPath.empty()) {
                    struct stat st {};
                    if (::stat(wmPath.c_str(), &st) == 0 && uint64_t(st.st_size) == 8 * n) {
                        wmFd = ::open(wmPath.c_str(), O_RDONLY);
                        break;
                    }
                } else {
                    for (const auto& e : std::filesystem::directory_iterator(
                             std::filesystem::path(prefix).parent_path())) {
                        if (e.path().string().rfind(prefix + ".xsc-", 0) != 0) continue;
                        std::string cand = e.path().string() + "/wm.bin";
                        struct stat st {};
                        if (::stat(cand.c_str(), &st) == 0 && uint64_t(st.st_size) == 8 * n) {
                            wmFd = ::open(cand.c_str(), O_RDONLY);
                            break;
                        }
                    }
                    if (wmFd >= 0) break;
                }
                if (waited > pollTimeout) pf_fail("timeout waiting for the merge's WM scratch");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                waited += 0.1;
            }
            pf_check(wmFd >= 0, "open WM scratch");
        }
        LceWrap seamLce{&fhSeam, {}, n};
        SeamClasses seam;
        auto seamSymbol = [&](uint64_t pos) -> uint8_t {
            if (pos >= n) pf_fail("seam symbol outside text");
            return text[pos];
        };
        seam.build(text, wmFd, n, R, 10,
                   [&](uint64_t a, uint64_t b) { return seamLce(a, b); },
                   seamSymbol);
        ::close(wmFd);
        phase("seam-classes", last);

        // ---- scratch writers
        WitBuckets buckets(n, R, prefix, ramBudget);
        std::string sparsePath = prefix + ".sparse2";
        pf_check(::access(sparsePath.c_str(), F_OK) != 0, "sparse scratch exists");
        int sparseFd = ::open(sparsePath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        pf_check(sparseFd >= 0, "create sparse scratch");
        bool bridge = !bridgeDir.empty();
        int bCharsFd = -1, bLensFd = -1, bTailsFd = -1, bSAFd = -1;
        unsigned w4 = 1;
        while (w4 < 64 && (uint64_t(1) << w4) <= n - 1) ++w4;
        __uint128_t bTailRes = 0;
        unsigned bTailAvail = 0;
        if (bridge) {
            bCharsFd = ::open((bridgeDir + "/frag.b-chars").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            bLensFd = ::open((bridgeDir + "/frag.b-lens").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            bTailsFd = ::open((bridgeDir + "/frag.b-tails").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            bSAFd = ::open((bridgeDir + "/frag.sA").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            pf_check(bCharsFd >= 0 && bLensFd >= 0 && bTailsFd >= 0 && bSAFd >= 0, "create bridge temps");
        }
        auto bridgeTailWrite = [&](uint64_t tail) {
            bTailRes |= __uint128_t(n - 1 - tail) << bTailAvail;
            bTailAvail += w4;
            if (bTailAvail >= 64) {
                uint64_t word = uint64_t(bTailRes);
                pf_write_full(bTailsFd, &word, 8, "bridge tails");
                bTailRes >>= 64; bTailAvail -= 64;
            }
        };

        Sweep sweep(n);
        LceWrap slimLce{&fhSlim, {}, n};
        std::vector<NRun> nbuf;
        std::vector<uint64_t> gidx;   // global normalized run index per nbuf entry
        std::vector<uint64_t> topLcp, interior;
        uint64_t prevTail = 0;
        RepairStream rs;
        rs.N = N; rs.n = n; rs.terminal = 0; rs.seam = &seam;
        rs.emit = [&](const NRun& v) {
            uint64_t idx = rs.rNorm;   // this run's global index (pre-increment)
            if (idx % 1024 == 0) pf_write_full(sparseFd, &v.t, 8, "sparse tail");
            if (bridge) {
                pf_write_full(bCharsFd, &v.c, 1, "bridge chars");
                uint32_t len32 = uint32_t(v.len);
                pf_write_full(bLensFd, &len32, 4, "bridge lens");
                bridgeTailWrite(v.t);
            }
            nbuf.push_back(v);
            gidx.push_back(idx);
        };
        sweep.emitWitness = [&](uint64_t value) {
            buckets.add(value);
            if (bridge) pf_write_full(bSAFd, &value, 8, "bridge sA");
        };

        // ---- main band loop: pieces -> repair -> LCE -> sweep -> witnesses
        double lceWall = 0, repairWall = 0;
        std::vector<RawRun> raws;
        uint64_t nbands = (R + src.B - 1) / src.B;
        for (uint64_t band = 0; band < nbands; ++band) {
            double t0 = tnow();
            src.read_band(band, raws);
            if (band == 0) {
                pf_check(!raws.empty() && raws[0].len == 1, "run zero is not the terminal padding row");
                rs.terminal = raws[0].c;
                pf_check(rs.terminal >= 6 && rs.terminal < 256, "unsupported terminal byte");
                fprintf(stderr, "PFP_TERMINAL byte=0x%02llx source=padding-row raw_n=%llu raw_r=%llu\n",
                        (unsigned long long)rs.terminal, (unsigned long long)N, (unsigned long long)R);
            }
            for (const auto& raw : raws) rs.feed_raw(raw);
            repairWall += tnow() - t0;
            if (nbuf.empty()) continue;
            size_t cnt = nbuf.size();
            topLcp.assign(cnt, 0);
            interior.assign(cnt, INF);
            t0 = tnow();
            {
                std::atomic<size_t> next{0};
                int tc = std::max(1, std::min(threads, 64));
                std::vector<std::thread> ts;
                for (int t = 0; t < tc; ++t)
                    ts.emplace_back([&, t] {
                        slim_set_thread_shard(unsigned(t));
                        for (;;) {
                            size_t i = next.fetch_add(1);
                            if (i >= cnt) break;
                            const NRun& v = nbuf[i];
                            uint64_t first = v.h, tail = v.t;
                            pf_check(first < n, "position resolution failed");
                            if (v.len == 1) pf_check(first == tail, "head-SA value/range or singleton mismatch");
                            pf_check(tail < n, "position resolution failed");
                            uint64_t prev = gidx[i] == 0 ? 0 : (i ? nbuf[i - 1].t : prevTail);
                            pf_check(prev < n, "previous position resolution failed");
                            topLcp[i] = gidx[i] ? slimLce(prev, first) : 0;
                            interior[i] = v.len == 1 ? INF : slimLce(first, tail);
                        }
                        slimLce.flush_thread();
                    });
                for (auto& t : ts) t.join();
            }
            lceWall += tnow() - t0;
            for (size_t i = 0; i < cnt; ++i) sweep.advance(nbuf[i], topLcp[i], interior[i]);
            prevTail = nbuf.back().t;
            nbuf.clear();
            gidx.clear();
            if ((band & 63) == 0)
                fprintf(stderr, "PILE_FINISH_PROGRESS band=%llu/%llu runs=%llu chi_so_far=%llu repair_wall=%.1f lce_wall=%.1f wit_spills=%llu\n",
                        (unsigned long long)(band + 1), (unsigned long long)nbands,
                        (unsigned long long)rs.rNorm, (unsigned long long)sweep.chi,
                        repairWall, lceWall, (unsigned long long)buckets.spilled);
        }
        rs.finishStream();
        sweep.finish();
        pf_check(::close(sparseFd) == 0, "close sparse scratch");
        if (bridge) {
            if (bTailAvail) {
                uint64_t word = uint64_t(bTailRes);
                pf_write_full(bTailsFd, &word, 8, "bridge tails final");
                bTailRes = 0; bTailAvail = 0;
            }
            pf_check(::close(bCharsFd) == 0 && ::close(bLensFd) == 0 &&
                     ::close(bTailsFd) == 0 && ::close(bSAFd) == 0, "close bridge temps");
        }
        phase("sweep", last);
        const uint64_t chi = sweep.chi, rNorm = rs.rNorm;
        fprintf(stderr, "xsa chi-rspace: chi = %llu (N=%llu, R=%llu) stream-agg=1 fused=1 corpus_lce=1\n",
                (unsigned long long)chi, (unsigned long long)n, (unsigned long long)rNorm);
        if (chiExpect) pf_check(chi == chiExpect, "chi mismatch: got " + std::to_string(chi) + " want " + std::to_string(chiExpect));
        fprintf(stderr, "SLIM_SEEDS queries=%llu lce_wall=%.1f repair_wall=%.1f wit_ram_values=%llu wit_spills=%llu wit_disc_bytes=%llu\n",
                (unsigned long long)slimLce.seedQueries.read(), lceWall, repairWall,
                (unsigned long long)buckets.totalValues, (unsigned long long)buckets.spilled,
                (unsigned long long)buckets.discBytes());

        // ---- the PFCK's work is done: delete it before the container pass
        if (!keepPfck) {
            pf_check(::close(pfckFd) == 0, "close pfck fd");
            pf_check(::unlink(pfckPath.c_str()) == 0, "unlink pfck scratch");
        }

        // ---- witness sample check (banked sA: count + sampled membership)
        std::vector<uint64_t> wantSamples;
        if (!sampleCheck.empty()) {
            int fd = ::open(sampleCheck.c_str(), O_RDONLY);
            pf_check(fd >= 0, "open banked sA");
            struct stat st {};
            pf_check(fstat(fd, &st) == 0, "stat banked sA");
            pf_check(st.st_size % 8 == 0, "banked sA not u64-aligned");
            uint64_t bankedChi = uint64_t(st.st_size) / 8;
            pf_check(bankedChi == chi, "banked sA chi count mismatch");
            uint64_t stride = std::max<uint64_t>(1, bankedChi / std::max<uint64_t>(1, sampleCount));
            for (uint64_t i = 0; i < bankedChi; i += stride) {
                uint8_t b[8];
                pf_pread_full(fd, b, 8, 8 * i, "banked sA sample");
                uint64_t v = 0;
                for (int k = 0; k < 8; ++k) v |= uint64_t(b[k]) << (8 * k);
                wantSamples.push_back(v);
            }
            ::close(fd);
            std::sort(wantSamples.begin(), wantSamples.end());
            wantSamples.erase(std::unique(wantSamples.begin(), wantSamples.end()), wantSamples.end());
        }

        // ---- m5 EF over the sorted bucket stream
        ContainerOut container(out, 6);
        {
            unsigned l = chi ? unsigned(std::max(0, int(std::log2(double(n + 1) / double(chi))))) : 0;
            if (l > 63) l = 63;
            uint64_t lowbits = chi * l, highbits = chi ? (n >> l) + chi + 1 : 0;
            BitsOut low, high;
            uint64_t i = 0, hpos = 0, prev = ~0ull;
            size_t sidx = 0;
            buckets.for_each_sorted([&](uint64_t v) {
                pf_check(prev == ~0ull || v > prev, "witnesses not strictly increasing");
                prev = v;
                low.lsb(v, l);
                uint64_t mark = (v >> l) + i;
                while (hpos < mark) { high.bit(0); ++hpos; }
                high.bit(1); ++hpos;
                ++i;
                while (sidx < wantSamples.size() && wantSamples[sidx] < v)
                    pf_fail("sampled banked witness missing from the v2 stream: " + std::to_string(wantSamples[sidx]));
                if (sidx < wantSamples.size() && wantSamples[sidx] == v) ++sidx;
            });
            while (hpos < highbits) { high.bit(0); ++hpos; }
            pf_check(i == chi, "EF count");
            pf_check(low.bitPos == lowbits && high.bitPos == highbits, "EF dimensions");
            pf_check(sidx == wantSamples.size(), "witness sample check incomplete");
            container.begin(5, 105, chi);
            container.num(l); container.num(lowbits); container.num(highbits);
            container.write(low.mem.data(), low.mem.size());
            container.write(high.mem.data(), high.mem.size());
        }
        if (!wantSamples.empty())
            fprintf(stderr, "WITNESS_SAMPLE_CHECK samples=%zu passed=1\n", wantSamples.size());
        phase("m5-ef", last);

        // ---- small members: m4 anchors (none), m7 remap, m10 LF (derived)
        container.begin(4, 4, 0);
        container.num(0x434e4158, 4);
        container.num(0);
        container.begin(7, 7, 256);
        container.write(remapTable, 256);
        container.begin(10, 110, rNorm);

        // ---- m11 sparse 1/1024 tails
        {
            int fd = ::open(sparsePath.c_str(), O_RDONLY);
            pf_check(fd >= 0, "reopen sparse scratch");
            struct stat st {};
            pf_check(fstat(fd, &st) == 0, "stat sparse");
            uint64_t cnt = uint64_t(st.st_size) / 8;
            pf_check(cnt == (rNorm + 1023) / 1024, "sparse count");
            container.begin(11, 111, cnt);
            container.num(10); container.num(cnt);
            std::vector<uint64_t> vals(cnt);
            pf_read_full(fd, vals.data(), 8 * cnt, "sparse values");
            for (uint64_t v : vals) container.num(v);
            ::close(fd);
        }

        // ---- m1 runs: Huffman over the normalized run chars (frequencies
        // accumulated during the sweep), encoded from a final rlebwt re-scan
        // (the repair replayed sample-free: the normalized chars/lens are a
        // deterministic function of the raw runs and the class table).
        {
            std::array<uint64_t, 256> freqC{};
            uint64_t freqTotal = 0;
            for (unsigned c = 0; c < 256; ++c) { freqC[c] = rs.runFreq[c]; freqTotal += rs.runFreq[c]; }
            pf_check(freqTotal == rNorm, "run frequency total");
            struct Node { uint64_t w; int left, right, sym; };
            std::vector<Node> tree;
            using Q = std::pair<uint64_t, int>;
            std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
            for (int c = 0; c < 256; ++c)
                if (freqC[c]) { q.push({freqC[c], int(tree.size())}); tree.push_back({freqC[c], -1, -1, c}); }
            pf_check(!q.empty(), "empty alphabet");
            if (q.size() == 1) {
                auto one = q.top(); q.pop();
                tree.push_back({one.first, one.second, -1, -1});
                q.push({one.first, int(tree.size() - 1)});
            }
            while (q.size() > 1) {
                auto a = q.top(); q.pop();
                auto b = q.top(); q.pop();
                uint64_t w = a.first + b.first;
                tree.push_back({w, a.second, b.second, -1});
                q.push({w, int(tree.size() - 1)});
            }
            std::array<unsigned char, 256> lens{};
            std::function<void(int, unsigned)> visit = [&](int idx, unsigned depth) {
                if (tree[idx].sym >= 0) { pf_check(depth && depth <= 63, "Huffman depth"); lens[tree[idx].sym] = uint8_t(depth); return; }
                visit(tree[idx].left, depth + 1);
                if (tree[idx].right >= 0) visit(tree[idx].right, depth + 1);
            };
            visit(int(q.top().second), 0);
            std::array<uint64_t, 256> codes{};
            std::vector<std::pair<unsigned, unsigned>> order;
            for (unsigned c = 0; c < 256; ++c) if (lens[c]) order.push_back({lens[c], c});
            std::sort(order.begin(), order.end());
            uint64_t code = 0; unsigned prevLen = 0;
            for (auto& [len, sym] : order) {
                code <<= (len - prevLen);
                codes[sym] = code++;
                prevLen = len;
            }
            BitsOut headBits, lenBits;
            headBits.mem.reserve(rNorm / 2);
            lenBits.mem.reserve(rNorm);
            uint64_t replayRuns = 0;
            {
                RepairStream rs2;
                rs2.N = N; rs2.n = n; rs2.terminal = rs.terminal; rs2.seam = &seam;
                rs2.sampleChecks = false;
                rs2.emit = [&](const NRun& v) {
                    headBits.msb(codes[v.c], lens[v.c]);
                    lenBits.gamma(v.len);
                    ++replayRuns;
                };
                int fd = ::open((prefix + ".rlebwt").c_str(), O_RDONLY);
                pf_check(fd >= 0, "open rlebwt for the m1 replay");
                uint8_t buf[1 << 20];
                uint64_t got = 0;
                size_t pos = 0, have = 0;
                auto nextWord = [&](uint32_t& w32) -> bool {
                    if (pos == have) {
                        ssize_t z = ::read(fd, buf, sizeof buf);
                        if (z <= 0) return false;
                        have = size_t(z); pos = 0;
                    }
                    w32 = uint32_t(buf[pos]) | (uint32_t(buf[pos + 1]) << 8) |
                          (uint32_t(buf[pos + 2]) << 16) | (uint32_t(buf[pos + 3]) << 24);
                    pos += 4;
                    ++got;
                    return true;
                };
                (void)got;
                for (;;) {
                    uint32_t w32;
                    if (!nextWord(w32)) break;
                    uint8_t c = uint8_t(w32 & 255);
                    uint64_t len = (w32 >> 8) & 0x7fffff;
                    while (w32 >> 31) {
                        if (!nextWord(w32)) pf_fail("truncated rlebwt");
                        pf_check(uint8_t(w32 & 255) == c, "RLE continuation character");
                        len += (w32 >> 8) & 0x7fffff;
                    }
                    rs2.feed_raw({c, len, 0, 0});
                }
                ::close(fd);
                rs2.finishStream();
                pf_check(replayRuns == rNorm, "m1 replay run count");
                for (unsigned c = 0; c < 256; ++c)
                    pf_check(rs2.totals[c] == rs.totals[c] && rs2.runFreq[c] == rs.runFreq[c],
                             "m1 replay divergence on char " + std::to_string(c));
            }
            container.begin(1, 101, rNorm);
            // C table = prefix sums of the normalized byte totals (the ri4 C)
            uint64_t acc = 0;
            for (unsigned c = 0; c < 256; ++c) { uint64_t v = acc; acc += rs.totals[c]; container.num(v); }
            container.write(lens.data(), 256);
            container.num(headBits.bitPos);
            container.num(lenBits.bitPos);
            container.write(headBits.mem.data(), headBits.mem.size());
            container.write(lenBits.mem.data(), lenBits.mem.size());
        }
        phase("m1-runs", last);

        // ---- publish the lite container (flags: complete | text | remapped)
        container.finish(n, 1, rNorm, 1 | 8 | 16);
        phase("publish", last);

        // ---- cleanup: scratch only. rlebwt (+meta) go after the container.
        buckets.cleanup();
        pf_check(::unlink(sparsePath.c_str()) == 0, "unlink sparse scratch");
        {
            std::string sent = prefix + ".runcols.consumed";
            ::unlink(sent.c_str());
            ::unlink((prefix + ".runcols.idx").c_str());
            if (::rmdir(src.dir.c_str()) != 0)
                fprintf(stderr, "PILE_FINISH_PIECES_DIR_NONEMPTY %s (left for inspection)\n", src.dir.c_str());
        }
        if (bridge) {
            ::unlink((bridgeDir + "/frag.b-chars").c_str());
            ::unlink((bridgeDir + "/frag.b-lens").c_str());
            ::unlink((bridgeDir + "/frag.b-tails").c_str());
            // frag.sA is a GATE artifact (the emission-order witness bridge):
            // keep it in the bridge dir.
        }
        if (!keepRlebwt) {
            pf_check(::unlink((prefix + ".rlebwt").c_str()) == 0, "unlink rlebwt");
            pf_check(::unlink((prefix + ".rlebwt.meta").c_str()) == 0, "unlink rlebwt.meta");
            fprintf(stderr, "PILE_FINISH_RLEBWT_DELETED\n");
        }
        fprintf(stderr, "PILE_FINISH_DONE chi=%llu n=%llu r=%llu container=%s\n",
                (unsigned long long)chi, (unsigned long long)n, (unsigned long long)rNorm, out.c_str());
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "FATAL pile_finish: %s\n", e.what());
        return 1;
    }
}
