// ext_columns.hpp — external run columns for the parse-free finish.
//
// The finish's run-scale columns (run chars, lens, row starts, LF bases,
// head-SA seeds, SA samples) live ONLY in files. Every reader is a bounded
// pread window: no run-scale vector is ever resident. Two forms:
//   * column-chunked band windows: the query phases sweep run-range bands,
//     bulk-loading each column slice for the active band (the stream-agg
//     sweep pattern applied to the dump and the repair);
//   * two-pass external build: one sequential pass over the run sources
//     derives a per-run RECORD column (starts, lfBase, char) plus a sorted
//     SEED column (head-SA position, run id) via external merge sort, with a
//     small RAM coarse index over starts for run_of() binary searches.
//
// THE LAW holds: only built-structure artifacts are read; no corpus text is
// opened by any of this machinery.
//
// Temp files (all derived, all unlinked on success, all reported on failure):
//   <workPrefix>.xrec    24 B/run  {starts u64, lfBase u64, a u8, pad[7]}
//   <workPrefix>.xseeds  16 B/run  {head-SA pos u64, run u64}, sorted by pos
//   <workPrefix>.xsrunK  transient sorted seed chunks (unlinked while open)
//
// PFCK checkpoint sidecar format (written by the slim's backward pass and by
// cross_lcp_merge --emit-pf; both writers must stay byte-identical):
//   "PFCK" u32 | version u32 (=1) | textLen u64 | tau u64 | count u64 |
//   reserved u64 (=0) | count x u64 suffix-hash checkpoints
//   checkpoint k = polynomial suffix hash (base 0x9e3779b185ebca87, first
//   byte at coefficient BASE^0, h = (T[i]+1) + BASE*h folded backward from
//   the end) of T[k*tau .. textLen), for k in [0, textLen/tau].
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

static inline void ext_fail(const char* s) {
    fprintf(stderr, "FATAL EXT: %s\n", s); exit(2);
}
static inline double ext_tnow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void ext_phase(const char* what, double& last) {
    struct rusage u{}; getrusage(RUSAGE_SELF, &u);
    double now = ext_tnow();
    fprintf(stderr, "SLIM_PHASE %s wall=%.3f cumulative=NA peak_rss_kib=%ld\n",
            what, now - last, u.ru_maxrss);
    fflush(stderr);
    last = now;
}

static inline void ext_pread(int fd, void* buf, uint64_t len, uint64_t off, const char* what) {
    char* p = (char*)buf;
    while (len) {
        ssize_t n = pread(fd, p, len, off);
        if (n <= 0) { fprintf(stderr, "FATAL EXT: %s (pread %llu @ %llu)\n", what,
                              (unsigned long long)len, (unsigned long long)off); exit(2); }
        p += n; len -= (uint64_t)n; off += (uint64_t)n;
    }
}
static inline void ext_pwrite(int fd, const void* buf, uint64_t len, uint64_t off, const char* what) {
    const char* p = (const char*)buf;
    while (len) {
        ssize_t n = pwrite(fd, p, len, off);
        if (n <= 0) { fprintf(stderr, "FATAL EXT: %s (pwrite %llu @ %llu)\n", what,
                              (unsigned long long)len, (unsigned long long)off); exit(2); }
        p += n; len -= (uint64_t)n; off += (uint64_t)n;
    }
}
static inline uint64_t ext_file_size(int fd, const char* what) {
    struct stat st{};
    if (fstat(fd, &st)) ext_fail(what);
    return (uint64_t)st.st_size;
}

// Fixed-width little-endian element column in a file (random access by pread).
struct ExtCol {
    int fd = -1; uint64_t base = 0; uint32_t w = 0; uint64_t count = 0;
    void open_ro(const std::string& path, uint32_t elemBytes, uint64_t elemCount,
                 uint64_t byteBase, const char* what) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) { fprintf(stderr, "FATAL EXT: open %s\n", path.c_str()); exit(2); }
        base = byteBase; w = elemBytes; count = elemCount;
        if (ext_file_size(fd, what) < base + w * count) {
            fprintf(stderr, "FATAL EXT: %s: column %s truncated (%llu < %llu+%llu*%llu)\n",
                    what, path.c_str(), (unsigned long long)ext_file_size(fd, what),
                    (unsigned long long)base, (unsigned long long)w, (unsigned long long)count);
            exit(2);
        }
    }
    inline uint64_t at(uint64_t i) const {
        if (i >= count) ext_fail("column index out of range");
        uint8_t raw[8]{};
        ext_pread(fd, raw, w, base + w * i, "column read");
        uint64_t v = 0;
        for (unsigned b = 0; b < w; ++b) v |= (uint64_t)raw[b] << (8 * b);
        return v;
    }
    inline void read(uint64_t i, void* buf, uint64_t n) const {
        if (i + n > count || n > (1ull << 32)) ext_fail("column bulk read out of range");
        ext_pread(fd, buf, w * n, base + w * i, "column bulk read");
    }
};

// Packed fixed-width bit column (SA samples), LSB-first in little-endian words
// (mirrors sdsl::int_vector<w> packing used by the .ri4 sample array).
struct ExtBits {
    int fd = -1; uint64_t base = 0; uint32_t w = 0; uint64_t count = 0;
    void open_ro(const std::string& path, uint64_t bitBase, uint32_t widthBits,
                 uint64_t elemCount, const char* what) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) { fprintf(stderr, "FATAL EXT: open %s\n", path.c_str()); exit(2); }
        base = bitBase; w = widthBits; count = elemCount;
        if (!w || w > 64 || base % 8) ext_fail("packed column width/alignment");
        uint64_t bytes = ((base + w * count + 7) / 8);
        if (ext_file_size(fd, what) < bytes) {
            fprintf(stderr, "FATAL EXT: %s: packed column %s truncated\n", what, path.c_str());
            exit(2);
        }
    }
    inline uint64_t at(uint64_t i) const {
        if (i >= count) ext_fail("packed column index out of range");
        uint64_t bit0 = base + i * w, word = bit0 >> 6, b0 = bit0 & 63;
        uint64_t r[2]{};
        ext_pread(fd, r, 8, word * 8, "packed column word");
        if (b0 + w > 64) ext_pread(fd, r + 1, 8, word * 8 + 8, "packed column word");
        const uint64_t mask = w == 64 ? ~0ull : ((1ull << w) - 1);
        if (b0 + w <= 64) return (r[0] >> b0) & mask;
        return ((r[0] >> b0) | (r[1] << (64 - b0))) & mask;
    }
    // Bulk read of the whole words covering elements [i0, i1); decode locally
    // with sample_local (bit0 measured from element i0's first bit). The .ri4
    // sample array can END MID-WORD: the final word is read short and the
    // tail of the buffer is zero-filled (the missing bits are beyond saBits).
    void read_words(uint64_t i0, uint64_t i1, std::vector<uint64_t>& words,
                    uint64_t& firstElemBit0) const {
        uint64_t b0 = base + i0 * w, b1 = base + i1 * w;
        uint64_t w0 = b0 >> 6, w1 = (b1 + 63) >> 6;
        words.resize(w1 - w0 + 1);
        uint64_t availEnd = (base + w * count + 7) / 8;   // bytes validated at open
        uint64_t readEnd = std::min<uint64_t>(w1 * 8, availEnd);
        uint64_t want = readEnd > w0 * 8 ? readEnd - w0 * 8 : 0;
        uint64_t done = 0;
        while (done < want) {
            ssize_t n = pread(fd, (char*)words.data() + done, want - done, w0 * 8 + done);
            if (n <= 0) ext_fail("packed column bulk");
            done += (uint64_t)n;
        }
        if (want < words.size() * 8)
            memset((char*)words.data() + want, 0, words.size() * 8 - want);
        firstElemBit0 = b0 - w0 * 64;
    }
};

// Buffered sequential writer over a fresh temp file (no clobber of retained
// artifacts: temps are always new files created by us).
struct ExtWriter {
    int fd = -1; std::vector<char> buf; std::string path; uint64_t wrote = 0;
    void open(const std::string& p, size_t cap = 1 << 22) {
        path = p;
        fd = ::open(p.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd < 0) { fprintf(stderr, "FATAL EXT: create %s (exists?)\n", p.c_str()); exit(2); }
        buf.reserve(cap);
    }
    inline void put(const void* d, uint64_t n) {
        const char* p = (const char*)d;
        if (n >= buf.capacity()) { flush(); ext_pwrite(fd, p, n, wrote, "temp write"); wrote += n; return; }
        if (buf.size() + n > buf.capacity()) flush();
        buf.insert(buf.end(), p, p + n);
    }
    inline void put64(uint64_t v) { char b[8]; for (int i = 0; i < 8; ++i) b[i] = char(v >> (8 * i)); put(b, 8); }
    void flush() {
        if (!buf.empty()) {
            ext_pwrite(fd, buf.data(), buf.size(), wrote, "temp write");
            wrote += buf.size(); buf.clear();
        }
    }
    uint64_t finish() { flush(); return wrote; }
};

// Per-run record: everything one LF step of the structural walk reads.
struct ExtRec { uint64_t starts, lfBase; uint8_t a; uint8_t pad[7]; };
static_assert(sizeof(ExtRec) == 24, "record layout");

// External run structure: the two-pass external form shared by the slim walk,
// the slim query phase, and the endpoint seam repair.
struct ExtRuns {
    uint64_t R = 0, textLen = 0, rowsTotal = 0, skippedByte = 256;
    std::array<uint64_t, 256> totals{}, Cless{};
    // Bulk sequential sources (caller-owned; consumed ONLY by build(), which
    // sweeps runs 0..R in blocks — one pread per block per column):
    //   runScan(r, aOut, lenOut, n): n runs' char and length, run ids r..r+n
    //   headScan(r, buf, n) / tailScan(r, buf, n): n u64 SA samples
    // Post-build access goes through the derived record/seed columns, never
    // through these.
    std::function<void(uint64_t, uint8_t*, uint64_t*, uint64_t)> runScan;
    std::function<void(uint64_t, uint64_t*, uint64_t)> headScan;
    std::function<void(uint64_t, uint64_t*, uint64_t)> tailScan;   // optional
    // Optional tail source: enables the INVERSE seed column (.xseedst, sorted
    // by tail) used by the seam repair's phi-inverse row enumeration.
    std::function<uint64_t(uint64_t)> tailAt;
    std::string workPrefix;   // temp files "<prefix>.xrec"/".xseeds"/".xseedst"
    int fdRec = -1, fdSeeds = -1, fdSeedsT = -1;
    uint64_t seedCount = 0;
    static constexpr uint64_t STRIDE = 64;          // coarse index granularity
    std::vector<uint64_t> coarse;                    // starts[r*STRIDE], r*STRIDE<R
    bool built = false;

    inline void rec_at(uint64_t run, ExtRec& out) const {
        uint8_t raw[24];
        ext_pread(fdRec, raw, 24, run * 24, "record column read");
        memcpy(&out.starts, raw, 8); memcpy(&out.lfBase, raw + 8, 8);
        memcpy(&out.a, raw + 16, 1); memset(out.pad, 0, 7);
    }
    inline uint64_t starts_at(uint64_t run) const { ExtRec r; rec_at(run, r); return r.starts; }
    inline uint64_t lfbase_at(uint64_t run) const { ExtRec r; rec_at(run, r); return r.lfBase; }
    inline uint8_t a_at(uint64_t run) const { ExtRec r; rec_at(run, r); return r.a; }
    // run_of(row): the last run with starts[run] <= row, via the RAM coarse
    // index plus one bounded block pread. out receives that run's record.
    inline uint64_t run_of(uint64_t row, ExtRec& out) const {
        if (row >= rowsTotal) ext_fail("run_of: row outside walked rows");
        // coarse: last k with starts[k*STRIDE] <= row
        uint64_t lo = 0, hi = coarse.size();
        while (lo + 1 < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (coarse[mid] <= row) lo = mid; else hi = mid;
        }
        uint64_t b0 = lo * STRIDE, b1 = std::min(b0 + STRIDE, R);
        thread_local std::vector<ExtRec> blk;
        blk.resize((size_t)(b1 - b0));
        ext_pread(fdRec, blk.data(), 24 * (b1 - b0), b0 * 24, "record block read");
        uint64_t rlo = 0, rhi = b1 - b0;   // last r in [0,b1-b0) with starts<=row
        while (rlo + 1 < rhi) {
            uint64_t mid = (rlo + rhi) / 2;
            if (blk[(size_t)mid].starts <= row) rlo = mid; else rhi = mid;
        }
        out = blk[(size_t)rlo];
        return b0 + rlo;
    }
    inline uint64_t run_of(uint64_t row) const { ExtRec r; return run_of(row, r); }
    // Sorted seed k -> (pos, run): one 16-byte pread.
    inline void seed(uint64_t idx, uint64_t& pos, uint64_t& run) const {
        if (idx >= R) ext_fail("seed index out of range");
        uint8_t raw[16];
        ext_pread(fdSeeds, raw, 16, idx * 16, "seed column read");
        memcpy(&pos, raw, 8); memcpy(&run, raw + 8, 8);
    }
    inline uint64_t lf(uint64_t row) const {
        ExtRec r; run_of(row, r);
        return r.lfBase + (row - r.starts);
    }
    inline void seedt(uint64_t idx, uint64_t& pos, uint64_t& run) const {
        if (fdSeedsT < 0) ext_fail("inverse seed column not built (tailAt unset)");
        if (idx >= R) ext_fail("seed index out of range");
        uint8_t raw[16];
        ext_pread(fdSeedsT, raw, 16, idx * 16, "inverse seed column read");
        memcpy(&pos, raw, 8); memcpy(&run, raw + 8, 8);
    }
    inline void close_fds() { if (fdRec >= 0) close(fdRec); if (fdSeeds >= 0) close(fdSeeds);
        if (fdSeedsT >= 0) close(fdSeedsT); fdRec = fdSeeds = fdSeedsT = -1; }
    ~ExtRuns() { close_fds(); }
    ExtRuns() = default;
    ExtRuns(const ExtRuns&) = delete;
    ExtRuns& operator=(const ExtRuns&) = delete;

    // Two sequential source passes + external seed sort(s). Fail-loud checks:
    // run-length sum == rowsTotal; head/tail values < rowsTotal; duplicate
    // head positions refuse (mirrors the resident walk's seed checks).
    struct Seed { uint64_t pos, run; };
    struct SeedSorter {
        std::string workPrefix, tag, outPath;
        std::vector<std::string> chunkPaths;
        std::vector<int> chunkFds;
        std::vector<Seed> buf;
        uint64_t chunks = 0;
        void init(const std::string& wp, const char* t, const std::string& out) {
            workPrefix = wp; tag = t; outPath = out;
            buf.reserve(8ull << 20);
        }
        void push(uint64_t pos, uint64_t run) {
            buf.push_back({pos, run});
            if (buf.size() >= buf.capacity()) flush();
        }
        void flush() {
            if (buf.empty()) return;
            std::sort(buf.begin(), buf.end(), [](const Seed& x, const Seed& y) { return x.pos < y.pos; });
            std::string p = workPrefix + ".xs" + tag + "run" + std::to_string(chunks++);
            int fd = open(p.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
            if (fd < 0) ext_fail("create seed chunk");
            unlink(p.c_str());          // blocks free as soon as the merge closes it
            char obuf[1 << 16]; size_t o = 0; uint64_t off = 0;
            for (auto& s : buf) {
                memcpy(obuf + o, &s.pos, 8); memcpy(obuf + o + 8, &s.run, 8); o += 16;
                if (o == sizeof obuf) { ext_pwrite(fd, obuf, o, off, "seed chunk write"); off += o; o = 0; }
            }
            if (o) ext_pwrite(fd, obuf, o, off, "seed chunk write");
            chunkPaths.push_back(p); chunkFds.push_back(fd);
            buf.clear();
        }
        // Multi-pass k-way merge of the sorted chunks into outPath, validating
        // strictly increasing positions (duplicates refuse loudly).
        void finish(uint64_t expected) {
            flush();
            std::vector<int> cur = std::move(chunkFds);
            constexpr size_t MAXF = 200;
            uint64_t pass = 0;
            while (cur.size() > 1) {
                bool lastPass = cur.size() <= MAXF;
                std::vector<int> next;
                for (size_t i = 0; i < cur.size(); i += lastPass ? cur.size() : MAXF) {
                    size_t j = std::min(i + (lastPass ? cur.size() : MAXF), cur.size());
                    std::vector<int> group(cur.begin() + i, cur.begin() + j);
                    if (lastPass) {
                        int out = open(outPath.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
                        if (out < 0) ext_fail("create merged seed column");
                        merge_chunks(group, out, true);
                        close(out);
                    } else {
                        std::string p = workPrefix + ".xs" + tag + "m" + std::to_string(pass) + "_" + std::to_string(i / MAXF);
                        int out = open(p.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
                        if (out < 0) ext_fail("create merge intermediate");
                        merge_chunks(group, out, true);
                        close(out);
                        int ro = open(p.c_str(), O_RDONLY);
                        if (ro < 0) ext_fail("reopen merge intermediate");
                        unlink(p.c_str());
                        next.push_back(ro);
                    }
                }
                for (int f : cur) close(f);
                cur = std::move(next);
                ++pass;
            }
            if (cur.size() == 1) {
                int out = open(outPath.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
                if (out < 0) ext_fail("create merged seed column");
                merge_chunks(cur, out, true);
                close(out);
                close(cur[0]);
            }
            int fd = open(outPath.c_str(), O_RDONLY);
            if (fd < 0) ext_fail("open merged seed column");
            if (ext_file_size(fd, "seeds") != expected * 16) ext_fail("merged seed column size");
            mergedFd = fd;
        }
        int mergedFd = -1;
    };
    void build(int threads) {
        double t0 = ext_tnow(), last = t0;
        if (!R || !rowsTotal || textLen > rowsTotal) ext_fail("ExtRuns dimensions");
        if (!runScan || !headScan) ext_fail("ExtRuns sources unset");
        // pass 1: char totals (Cless follows).
        constexpr uint64_t BLK = 1u << 20;
        std::vector<uint8_t> aB(BLK); std::vector<uint64_t> lB(BLK), hB(BLK), tB(BLK);
        for (uint64_t r = 0; r < R; ) {
            uint64_t n = std::min(BLK, R - r);
            runScan(r, aB.data(), lB.data(), n);
            for (uint64_t k = 0; k < n; ++k) totals[aB[k]] += lB[k];
            r += n;
        }
        { uint64_t acc = 0; for (unsigned c = 0; c < 256; ++c) { Cless[c] = acc; acc += totals[c]; } }
        ext_phase("ext-totals", last);
        // pass 2: record column + seed chunks (buffered, sorted, external).
        ExtWriter rec; rec.open(workPrefix + ".xrec");
        SeedSorter sortH, sortT;
        sortH.init(workPrefix, "h", workPrefix + ".xseeds");
        bool wantT = (bool)tailScan;
        if (wantT) sortT.init(workPrefix, "t", workPrefix + ".xseedst");
        std::array<uint64_t, 256> runAcc{};
        uint64_t acc = 0;
        for (uint64_t r = 0; r < R; ) {
            uint64_t n = std::min(BLK, R - r);
            runScan(r, aB.data(), lB.data(), n);
            headScan(r, hB.data(), n);
            if (wantT) tailScan(r, tB.data(), n);
            for (uint64_t k = 0; k < n; ++k) {
                uint8_t a = aB[k]; uint64_t len = lB[k];
                if (!len) ext_fail("zero-length run");
                ExtRec rec_v{acc, Cless[a] + runAcc[a], a, {0,0,0,0,0,0,0}};
                rec.put(&rec_v, 24);
                runAcc[a] += len; acc += len;
                if (hB[k] >= rowsTotal) ext_fail("head-SA value outside walked rows");
                if ((r + k) % STRIDE == 0) coarse.push_back(acc - len);
                sortH.push(hB[k], r + k);
                if (wantT) {
                    if (tB[k] >= rowsTotal) ext_fail("tail-SA value outside walked rows");
                    sortT.push(tB[k], r + k);
                }
            }
            r += n;
        }
        rec.finish();
        if (acc != rowsTotal) {
            fprintf(stderr, "FATAL EXT: run sum %llu != walked rows %llu\n",
                    (unsigned long long)acc, (unsigned long long)rowsTotal);
            exit(2);
        }
        ext_phase("ext-record+seed-chunks", last);
        sortH.finish(R);
        if (wantT) { sortT.finish(R); ext_phase("ext-seeds-tail-merge", last); }
        fdSeeds = sortH.mergedFd;
        if (wantT) fdSeedsT = sortT.mergedFd;
        fdRec = open((workPrefix + ".xrec").c_str(), O_RDONLY);
        if (fdRec < 0) ext_fail("open record column");
        if (ext_file_size(fdRec, "records") != R * 24) ext_fail("record column size");
        seedCount = R;
        built = true;
        fprintf(stderr, "SLIM_EXT_BUILT R=%llu textLen=%llu rows=%llu records=%llu seeds=%llu "
                "seeds_t=%llu coarse=%zu coarse_stride=%llu work_prefix=%s\n",
                (unsigned long long)R, (unsigned long long)textLen, (unsigned long long)rowsTotal,
                (unsigned long long)(R * 24), (unsigned long long)(R * 16),
                (unsigned long long)(wantT ? R * 16 : 0),
                coarse.size(), (unsigned long long)STRIDE, workPrefix.c_str());
        double now = ext_tnow();
        fprintf(stderr, "SLIM_PHASE ext-build-total wall=%.3f cumulative=NA peak_rss_kib=NA\n", now - t0);
    }

    void cleanup() {
        close_fds();
        if (!workPrefix.empty()) {
            unlink((workPrefix + ".xrec").c_str());
            unlink((workPrefix + ".xseeds").c_str());
            unlink((workPrefix + ".xseedst").c_str());
        }
        // chunk/merge temps were unlinked at creation; nothing else remains
    }
  private:
    // k-way streaming merge of sorted seed chunks into fd. When checkMono,
    // positions must be strictly increasing (duplicate head positions refuse).
    static void merge_chunks(const std::vector<int>& fds, int out, bool checkMono) {
        struct Src {
            int fd; std::vector<uint8_t> buf; uint64_t fileOff = 0, inBuf = 0, bufPos = 0;
            uint64_t pos = 0, run = 0; bool live = true;
            void fill() {
                if (inBuf > bufPos) memmove(buf.data(), buf.data() + bufPos, inBuf - bufPos);
                inBuf -= bufPos; bufPos = 0;
                ssize_t n = pread(fd, buf.data() + inBuf, buf.size() - inBuf, fileOff);
                if (n < 0) ext_fail("seed merge read");
                fileOff += (uint64_t)n; inBuf += (uint64_t)n;
                if (inBuf < 16) live = false;
            }
            bool head() {
                if (!live) return false;
                if (bufPos + 16 > inBuf) fill();
                if (!live) return false;
                memcpy(&pos, buf.data() + bufPos, 8);
                memcpy(&run, buf.data() + bufPos + 8, 8);
                return true;
            }
            void pop() { bufPos += 16; }
        };
        std::vector<std::unique_ptr<Src>> src;
        for (int fd : fds) {
            auto s = std::make_unique<Src>();
            s->fd = fd; s->buf.resize(1 << 20); s->fill();
            src.push_back(std::move(s));
        }
        // min-heap on (pos)
        auto less = [](const std::unique_ptr<Src>& x, const std::unique_ptr<Src>& y) {
            return x->pos > y->pos;   // priority_queue is a max-heap on operator<
        };
        std::vector<Src*> heap;
        for (auto& s : src) if (s->head()) heap.push_back(s.get());
        std::make_heap(heap.begin(), heap.end(), [&](Src* x, Src* y) { return x->pos > y->pos; });
        char obuf[1 << 16]; size_t o = 0; uint64_t off = 0;
        uint64_t lastPos = 0; bool first = true;
        auto flushO = [&]() { if (o) { ext_pwrite(out, obuf, o, off, "seed merge write"); off += o; o = 0; } };
        auto cmp = [&](Src* x, Src* y) { return x->pos > y->pos; };
        while (!heap.empty()) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            Src* s = heap.back(); heap.pop_back();
            if (checkMono && !first && s->pos <= lastPos) {
                fprintf(stderr, "FATAL EXT: duplicate head-SA position %llu (run %llu)\n",
                        (unsigned long long)s->pos, (unsigned long long)s->run);
                exit(2);
            }
            memcpy(obuf + o, &s->pos, 8); memcpy(obuf + o + 8, &s->run, 8); o += 16;
            if (o == sizeof obuf) flushO();
            lastPos = s->pos; first = false;
            s->pop();
            if (s->head()) { heap.push_back(s); std::push_heap(heap.begin(), heap.end(), cmp); }
        }
        flushO();
        (void)less;
    }
};
