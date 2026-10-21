// kasai_column - the run-boundary LCP column, built in O(n) by Kasai's ratchet.
//
//   kasai_column --corpus C --remap R --sa SASTREAM --out COLUMN
//                [--threads T] [--invert auto|direct|bucketed]
//                [--ram-budget BYTES] [--buckets N] [--tmp DIR] [--keep-isa]
//   kasai_column --selftest
//   kasai_column --selftest-negative      (must exit 2: the coverage gate fires)
//
// Design + disc plan + gates: tools/binbwt/KASAI_FINISH_DESIGN.md. The pricing
// that commissioned it: tools/binbwt/SAMPLED_SA_DESIGN.md 4.2/5.2.
//
// THE PRODUCT of this tool is the COLUMN `col[r] = LCE(SA[r], SA[r+1])`
// (cyclic LCE, cap n) for every rank r, i.e. exactly the array the finish's two
// LCE call sites consult:
//
//     topLcp[i]   = col[r_i - 1]                        (r_i = the run's head rank,
//     interior[i] = min(col[r_i .. r_i + len_i - 2])     the cumulative sum of
//                                                        the run lengths)
//
// which turns 473,695,414 per-query LCE walks (12.82 verified symbol reads per
// corpus byte, measured at the 1 GB fixture) into sequential column reads.
//
// Column format (48-byte header, all little-endian):
//   0  u32 magic 'SXLC'      4  u32 version
//   8  u64 n                 16 u64 rows (== n)
//   24 u64 escapes           32 u64 max_lcp
//   40 u64 flags (1 = u8+escape, 0 = reserved)
//   48 n bytes: u8 value, 255 means "see the escape list"
//   48+n: escapes x {u64 rank, u64 value}  (16 B each, sorted by rank)
// The escape list is sorted by rank. A rank is escaped iff its value >= 255.
//
// Counters (stderr, exact): invert_s, isa_bytes, isa_dump_bytes, bucket_count,
// kasai_comparisons, kasai_n_ratio, walk_s, column_bytes, max_lcp, mean_lcp,
// escape_count, total_s.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

// ------------------------------------------------------------------ fail loud
[[noreturn]] static void die(const std::string& m) {
    std::fprintf(stderr, "KASAI_COLUMN_FAIL %s\n", m.c_str());
    std::exit(2);
}
static void check(bool ok, const std::string& m) { if (!ok) die(m); }

static double tnow() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}
static void write_full(int fd, const void* p, size_t bytes, const char* what) {
    const char* c = (const char*)p;
    size_t done = 0;
    while (done < bytes) {
        const ssize_t w = ::write(fd, c + done, bytes - done);
        check(w > 0, std::string("write ") + what + ": " + std::strerror(errno));
        done += size_t(w);
    }
}
static void pread_full(int fd, void* p, size_t bytes, uint64_t at, const char* what) {
    char* c = (char*)p;
    size_t done = 0;
    while (done < bytes) {
        const ssize_t g = ::pread(fd, c + done, bytes - done, off_t(at + done));
        check(g > 0, std::string("pread ") + what + ": " + std::strerror(errno));
        done += size_t(g);
    }
}
static void pwrite_full(int fd, const void* p, size_t bytes, uint64_t at, const char* what) {
    const char* c = (const char*)p;
    size_t done = 0;
    while (done < bytes) {
        const ssize_t w = ::pwrite(fd, c + done, bytes - done, off_t(at + done));
        check(w > 0, std::string("pwrite ") + what + ": " + std::strerror(errno));
        done += size_t(w);
    }
}

// ------------------------------------------------------------------ mappings
struct Mapped {
    int fd = -1;
    const uint8_t* p = nullptr;
    uint64_t n = 0;
    void open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        check(fd >= 0, "open " + path + ": " + std::strerror(errno));
        struct stat st{};
        check(::fstat(fd, &st) == 0, "stat " + path);
        n = uint64_t(st.st_size);
        if (!n) { p = (const uint8_t*)""; return; }   // zero-length corpus
        p = (const uint8_t*)::mmap(nullptr, n, PROT_READ, MAP_SHARED, fd, 0);
        check(p != MAP_FAILED, "mmap " + path + ": " + std::strerror(errno));
    }
    ~Mapped() {
        if (p && p != MAP_FAILED && n) ::munmap((void*)p, n);
        if (fd >= 0) ::close(fd);
    }
    Mapped() = default;
    Mapped(const Mapped&) = delete;
    Mapped& operator=(const Mapped&) = delete;
};

// The anchor side-stream's header (bit6/binbwt_slice.h SaAnchorWriter).
static constexpr uint32_t SA_MAGIC = 0x41535853u;   // 'SXSA'
static constexpr uint32_t SA_VERSION = 1;
static constexpr uint32_t SA_HEADER = 48;
static constexpr uint32_t COL_MAGIC = 0x434c5853u;  // 'SXLC'
static constexpr uint32_t ISA_MAGIC = 0x53495853u;  // 'SXIS'
static constexpr uint32_t ISA_VERSION = 1;
static constexpr uint32_t ISA_HEADER = 32;          // magic, version, n, width
static constexpr uint32_t COL_VERSION = 1;
static constexpr uint32_t COL_HEADER = 48;

struct Anchor {
    uint64_t n = 0, k = 0, count = 0, rows = 0, pad = 0;
    const uint64_t* sa = nullptr;   // valid when k == 1
};
// Parse + prove the k=1 anchor file describes itself (design gate 1).
static Anchor parse_anchor(const Mapped& a, uint64_t corpusN) {
    check(a.n >= SA_HEADER, "anchor file shorter than its header");
    Anchor h;
    uint32_t magic = 0, version = 0;
    std::memcpy(&magic, a.p + 0, 4);
    std::memcpy(&version, a.p + 4, 4);
    std::memcpy(&h.n, a.p + 8, 8);
    std::memcpy(&h.k, a.p + 16, 8);
    std::memcpy(&h.count, a.p + 24, 8);
    std::memcpy(&h.rows, a.p + 32, 8);
    std::memcpy(&h.pad, a.p + 40, 8);
    check(magic == SA_MAGIC, "anchor magic");
    check(version == SA_VERSION, "anchor version");
    check(h.pad == 10, "anchor pad != 10 (container rows are emit ranks + 10)");
    check(h.k == 1, "the column needs the FULL SA stream (--sa-sample-k 1); got k=" +
                    std::to_string(h.k));
    check(h.rows == corpusN, "anchor rows != corpus length");
    check(h.n == corpusN, "anchor n != corpus length");
    check(h.count == h.rows, "anchor count != rows at k=1");
    check(h.count == (h.rows - 1) / h.k + 1, "anchor count != (rows-1)/k+1");
    check(a.n == SA_HEADER + h.count * 8, "anchor file size != 48 + count*8");
    h.sa = (const uint64_t*)(a.p + SA_HEADER);
    for (uint64_t r = 0; r < h.count; ++r)
        check(h.sa[r] < h.n, "anchor entry " + std::to_string(r) + " is not a position");
    return h;
}

// A bijection remap makes raw-byte equality identical to T-byte equality
// (T[i] = REMAP[corpus[i]]), so the walk compares the corpus's own bytes and
// never pays a table lookup per symbol. Proved, not assumed.
static void load_remap(const Mapped& rm, uint8_t out[256]) {
    check(rm.n == 256, "remap must be 256 bytes");
    std::memcpy(out, rm.p, 256);
    bool seen[256] = {};
    for (unsigned i = 0; i < 256; ++i) {
        check(!seen[rm.p[i]], "remap is not a bijection (value collision): the raw-byte "
                              "walk would not equal the remapped-byte walk");
        seen[rm.p[i]] = true;
    }
}

// The ISA file: 32-byte header {magic 'SXIS', version, n u64, width u32}, then
// n entries of `width` bytes, ISA[p] = the rank of the suffix at position p.
// This is the finish's LOCATE TABLE: it is what makes a run's rank exact (a
// cumulative run-length sum is NOT - see KASAI_FINISH_DESIGN.md section 2).
static void write_isa_header(int fd, uint64_t n, unsigned width) {
    uint8_t h[ISA_HEADER] = {};
    const uint32_t magic = ISA_MAGIC, ver = ISA_VERSION, w = width;
    std::memcpy(h + 0, &magic, 4);
    std::memcpy(h + 4, &ver, 4);
    std::memcpy(h + 8, &n, 8);
    std::memcpy(h + 16, &w, 4);
    pwrite_full(fd, h, ISA_HEADER, 0, "ISA header");
}
static void read_isa_header(const std::string& path, uint64_t nExpect, unsigned& width) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    check(fd >= 0, "open ISA " + path + ": " + std::strerror(errno));
    struct stat st{};
    check(::fstat(fd, &st) == 0, "stat ISA");
    uint8_t h[ISA_HEADER];
    pread_full(fd, h, ISA_HEADER, 0, "ISA header");
    uint32_t magic = 0, ver = 0, w = 0;
    uint64_t n = 0;
    std::memcpy(&magic, h + 0, 4);
    std::memcpy(&ver, h + 4, 4);
    std::memcpy(&n, h + 8, 8);
    std::memcpy(&w, h + 16, 4);
    check(magic == ISA_MAGIC, "ISA magic");
    check(ver == ISA_VERSION, "ISA version");
    check(n == nExpect, "ISA n != the corpus length");
    check(w == 4 || w == 8, "ISA width must be 4 or 8");
    check(uint64_t(st.st_size) == ISA_HEADER + n * w, "ISA file size != 32 + n*width");
    ::close(fd);
    width = w;
}

// ISA width in bytes: u32 covers every fixture; the pile packs to w bits
// (design section 3) and this tool refuses rather than guesses.
static unsigned isa_width(uint64_t n) { return n < (uint64_t(1) << 32) ? 4u : 8u; }

// =============================================================== the inversion
// INVERSION-STAGE: position -> rank. The ISA is indexed by POSITION (so the
// Kasai walk reads it sequentially, p ascending) and the buckets are POSITION
// RANGES, so the concatenation of the sorted buckets IS that array.
//
// Two arms, identical output (the gate cmp's the two ISA files):
//   direct    one slot array over the whole corpus in RAM (`--buckets 1`)
//   bucketed  B position-range buckets dumped to disc, each read back and
//             scattered into its own RAM slot array (bounded RAM; the pile arm)
struct InvertResult {
    double wall = 0;
    uint64_t isaBytes = 0, dumpBytes = 0, buckets = 0, span = 0;
    uint64_t peakBucketBytes = 0;
};

struct BucketFile {
    int fd = -1;
    std::string path;
    std::vector<uint8_t> buf;      // buffered appends, flushed whole
    uint64_t entries = 0;
    uint64_t written = 0;
    void flush() {
        if (buf.empty()) return;
        write_full(fd, buf.data(), buf.size(), "isa dump bucket");
        written += buf.size();
        buf.clear();
    }
};

// entry layout in a dump bucket: {u32 p_local, u64 rank}
static constexpr size_t DUMP_ENTRY = 12;

static void invert_direct(const Anchor& an, unsigned width, uint64_t n,
                          const std::string& isaPath, InvertResult& out) {
    const double t0 = tnow();
    check(n <= UINT32_MAX, "the direct arm indexes p_local as u32; use --invert bucketed");
    const uint64_t slotBytes = n * width;
    void* slots = nullptr;
    check(::posix_memalign(&slots, 1 << 21, slotBytes ? slotBytes : 8) == 0, "alloc ISA slots");
    // The scatter is one random store per position: 2 MiB pages keep the TLB
    // from becoming the cost (measured: 30.4 s -> see the work journal).
    ::madvise(slots, slotBytes, MADV_HUGEPAGE);
    std::memset(slots, 0, slotBytes ? slotBytes : 8);
    const uint64_t seenBytes = (n + 7) / 8;
    uint8_t* seen = (uint8_t*)std::calloc(seenBytes ? seenBytes : 1, 1);
    check(seen != nullptr, "alloc coverage bitmap");
    // One sequential read of the SA stream: the only pass this arm needs.
    for (uint64_t r = 0; r < n; ++r) {
        const uint64_t p = an.sa[r];
        check(p < n, "SA entry out of range");
        check(!(seen[p >> 3] & uint8_t(1u << (p & 7))),  "SA stream is not a permutation: position " +
              std::to_string(p) + " appears twice");
        seen[p >> 3] |= uint8_t(1u << (p & 7));
        if (width == 4) ((uint32_t*)slots)[p] = uint32_t(r);
        else            ((uint64_t*)slots)[p] = r;
    }
    for (uint64_t p = 0; p < n; ++p)
        check(seen[p >> 3] & uint8_t(1u << (p & 7)),
              "SA stream is not a permutation: position " + std::to_string(p) + " is missing");
    std::free(seen);
    const int fd = ::open(isaPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    check(fd >= 0, "create ISA " + isaPath + ": " + std::strerror(errno));
    write_isa_header(fd, n, width);
    pwrite_full(fd, slots, slotBytes, ISA_HEADER, "ISA body");
    ::close(fd);
    std::free(slots);
    out.wall = tnow() - t0;
    out.isaBytes = ISA_HEADER + slotBytes;
    out.buckets = 1;
    out.span = n;
    out.peakBucketBytes = slotBytes + seenBytes;
}

static void invert_bucketed(const Anchor& an, unsigned width, uint64_t n,
                            const std::string& isaPath, const std::string& tmpDir,
                            uint64_t ramBudget, uint64_t bucketOverride, InvertResult& out) {
    const double t0 = tnow();
    // ---- the plan: positions per bucket, and how many buckets --------------
    uint64_t span = ramBudget / (width + 1);          // slots + the coverage bitmap
    span = std::max<uint64_t>(span, 4096);
    uint64_t B = bucketOverride ? bucketOverride : (n + span - 1) / span;
    if (B < 1) B = 1;
    B = std::min<uint64_t>(B, n);            // never more buckets than positions
    span = (n + B - 1) / B;
    while (span > UINT32_MAX) { B = (B * 3) / 2 + 1; span = (n + B - 1) / B; }
    struct rlimit rl{};
    ::getrlimit(RLIMIT_NOFILE, &rl);
    const uint64_t fdCap = (rl.rlim_cur == RLIM_INFINITY) ? 4096 : uint64_t(rl.rlim_cur) / 2;
    check(uint64_t(B) + 16 <= fdCap,
          "bucket count " + std::to_string(B) + " exceeds the fd budget " + std::to_string(fdCap) +
          " (raise --ram-budget: fewer, larger buckets)");
    // Buffer budget: half the RAM budget across all buckets, clamped per bucket.
    uint64_t bufPer = ramBudget / 2 / B;
    bufPer = std::min<uint64_t>(bufPer, 4u << 20);
    bufPer = std::max<uint64_t>(bufPer, 64u << 10);
    bufPer -= bufPer % DUMP_ENTRY;
    // ---- open the buckets -------------------------------------------------
    std::vector<BucketFile> bk(static_cast<size_t>(B));
    std::string suffix = ("-pid" + std::to_string(::getpid()));
    for (uint64_t b = 0; b < B; ++b) {
        bk[size_t(b)].path = tmpDir + "/dump-" + std::to_string(b) + suffix;
        bk[size_t(b)].fd = ::open(bk[size_t(b)].path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        check(bk[size_t(b)].fd >= 0, "create ISA dump bucket " + bk[size_t(b)].path);
        bk[size_t(b)].buf.reserve(size_t(bufPer));
    }
    // ---- pass 1: one sequential read of the SA stream, bucketed ----------
    for (uint64_t r = 0; r < n; ++r) {
        const uint64_t p = an.sa[r];
        check(p < n, "SA entry out of range");
        const uint64_t b = p / span;
        BucketFile& f = bk[size_t(b)];
        const uint32_t pl = uint32_t(p - b * span);
        f.buf.resize(f.buf.size() + DUMP_ENTRY);
        uint8_t* e = f.buf.data() + f.buf.size() - DUMP_ENTRY;
        std::memcpy(e, &pl, 4);
        std::memcpy(e + 4, &r, 8);
        ++f.entries;
        if (f.buf.size() >= f.buf.capacity()) f.flush();
    }
    for (auto& f : bk) f.flush();
    for (auto& f : bk) { ::close(f.fd); f.fd = -1; out.dumpBytes += f.written; }
    const double tDump = tnow();
    // ---- pass 2: each bucket is a slot array; the scatter IS the sort -----
    const int isaFd = ::open(isaPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    check(isaFd >= 0, "create ISA " + isaPath);
    write_isa_header(isaFd, n, width);
    uint64_t peak = 0;
    for (uint64_t b = 0; b < B; ++b) {
        const uint64_t b0 = b * span;
        const uint64_t live = std::min(span, n - b0);
        const uint64_t got = bk[size_t(b)].written / DUMP_ENTRY;
        check(got == live, "bucket " + std::to_string(b) + " holds " + std::to_string(got) +
              " entries but its position range is " + std::to_string(live) +
              ": the SA stream is not a permutation");
        const uint64_t slotBytes = live * width;
        void* slots = nullptr;
        check(::posix_memalign(&slots, 1 << 21, slotBytes ? slotBytes : 8) == 0, "alloc bucket slots");
        std::memset(slots, 0, slotBytes ? slotBytes : 8);
        const uint64_t seenBytes = (live + 7) / 8;
        uint8_t* seen = (uint8_t*)std::calloc(seenBytes ? seenBytes : 1, 1);
        check(seen != nullptr, "alloc bucket coverage bitmap");
        peak = std::max(peak, slotBytes + seenBytes);
        const int fd = ::open(bk[size_t(b)].path.c_str(), O_RDONLY);
        check(fd >= 0, "reopen ISA dump bucket");
        const uint64_t chunkEntries = std::max<uint64_t>(1, (16u << 20) / DUMP_ENTRY);
        std::vector<uint8_t> stage(size_t(std::min(chunkEntries, got ? got : 1) * DUMP_ENTRY));
        uint64_t done = 0;
        while (done < got) {
            const uint64_t want = std::min(chunkEntries, got - done);
            pread_full(fd, stage.data(), size_t(want * DUMP_ENTRY), done * DUMP_ENTRY, "ISA dump bucket");
            for (uint64_t i = 0; i < want; ++i) {
                const uint8_t* e = stage.data() + i * DUMP_ENTRY;
                uint32_t pl = 0;
                uint64_t r = 0;
                std::memcpy(&pl, e, 4);
                std::memcpy(&r, e + 4, 8);
                check(pl < live, "dump entry outside its bucket range");
                check(!(seen[pl >> 3] & uint8_t(1u << (pl & 7))),
                      "position " + std::to_string(b0 + pl) + " appears twice: not a permutation");
                seen[pl >> 3] |= uint8_t(1u << (pl & 7));
                if (width == 4) ((uint32_t*)slots)[pl] = uint32_t(r);
                else            ((uint64_t*)slots)[pl] = r;
            }
            done += want;
        }
        ::close(fd);
        for (uint64_t p = 0; p < live; ++p)
            check(seen[p >> 3] & uint8_t(1u << (p & 7)),
                  "position " + std::to_string(b0 + p) + " is MISSING: not a permutation");
        std::free(seen);
        pwrite_full(isaFd, slots, slotBytes, ISA_HEADER + b0 * width, "ISA slice");
        std::free(slots);
        check(::unlink(bk[size_t(b)].path.c_str()) == 0, "unlink consumed ISA dump bucket");
    }
    ::close(isaFd);
    check(::rmdir(tmpDir.c_str()) == 0, "rmdir the ISA dump tree (must be empty)");
    out.peakBucketBytes = peak + B * bufPer;
    out.buckets = B;
    out.span = span;
    out.isaBytes = ISA_HEADER + n * width;
    out.wall = tnow() - t0;
    out.dumpBytes = 0;
    for (auto& f : bk) out.dumpBytes += f.written;
    std::fprintf(stderr, "KASAI_COLUMN_INVERT arm=bucketed dump_s=%.3f scatter_s=%.3f\n",
                 tDump - t0, tnow() - tDump);
}

// ================================================================= the Kasai walk
struct ColResult {
    std::vector<uint8_t> col;                        // n entries, 255 = escaped
    std::vector<std::pair<uint64_t, uint64_t>> esc;   // (rank, exact value), sorted
    uint64_t comparisons = 0, maxLcp = 0, queries = 0, sumLcp = 0;
    double wall = 0;
};

// Cyclic LCE from (i,j) starting at known depth `from`, capped at n.
// T is read raw: the remap is a bijection (proved in load_remap).
static inline uint64_t cyclic_extend(const uint8_t* T, uint64_t n, uint64_t i, uint64_t j,
                                     uint64_t from, uint64_t& comps) {
    uint64_t t = from;
    for (;;) {
        if (t >= n) break;
        uint64_t a = i + t; if (a >= n) a -= n;
        uint64_t b = j + t; if (b >= n) b -= n;
        ++comps;
        if (T[a] != T[b]) break;
        ++t;
    }
    return t;
}

// THE WALK. p ascending, the >= h-1 ratchet, cyclic. `chunks` threads each walk
// a contiguous p range starting from h = 0 (the ratchet is not carried across a
// chunk boundary: exact, just amortised slightly less at `chunks` places).
static ColResult kasai(const Anchor& an, const uint8_t* isa, unsigned width, const uint8_t* T,
                      uint64_t n, int threads) {
    ColResult out;
    const double t0 = tnow();
    check(n > 0, "empty corpus");
    // The column needs n bytes in RAM; at the pile this is the bucketed emitter
    // of design section 5. Refuse rather than swap.
    struct rlimit rl{};
    ::getrlimit(RLIMIT_AS, &rl);
    if (rl.rlim_cur != RLIM_INFINITY && uint64_t(rl.rlim_cur) < n + (256u << 20))
        die("the column (" + std::to_string(n) + " B) does not fit this address space: "
            "the pile emitter must bucket by rank (KASAI_FINISH_DESIGN.md section 5)");
    out.col.resize(size_t(n), 0);
    auto isa_at = [&](uint64_t p) -> uint64_t {
        return width == 4 ? uint64_t(((const uint32_t*)isa)[p]) : ((const uint64_t*)isa)[p];
    };
    const int P = std::max(1, std::min(threads, 256));
    std::vector<uint64_t> comps(static_cast<size_t>(P), 0), queries(static_cast<size_t>(P), 0),
        maxlcp(static_cast<size_t>(P), 0), sumlcp(static_cast<size_t>(P), 0);
    std::vector<std::vector<std::pair<uint64_t, uint64_t>>> esc(static_cast<size_t>(P));
    auto worker = [&](int tid) {
        const uint64_t lo = (n * uint64_t(tid)) / uint64_t(P);
        const uint64_t hi = (n * uint64_t(tid + 1)) / uint64_t(P);
        uint64_t h = 0;
        uint64_t& c = comps[size_t(tid)];
        uint64_t& q = queries[size_t(tid)];
        uint64_t& mx = maxlcp[size_t(tid)];
        uint64_t& sm = sumlcp[size_t(tid)];
        auto& e = esc[size_t(tid)];
        for (uint64_t p = lo; p < hi; ++p) {
            const uint64_t r = isa_at(p);
            check(r < n, "ISA entry out of range");
            if (r + 1 == n) { h = 0; continue; }      // no successor in rank space
            const uint64_t nq = an.sa[r + 1];
            check(nq < n, "SA neighbour out of range");
            const uint64_t len = cyclic_extend(T, n, p, nq, h, c);
            ++q;
            mx = std::max(mx, len);
            sm += len;
            if (len < 255) out.col[size_t(r)] = uint8_t(len);
            else { out.col[size_t(r)] = 255; e.emplace_back(r, len); }
            // THE RATCHET'S ONE EXCEPTION: len == n means the two rotations are
            // IDENTICAL, the only case where the sorted order carries no
            // information to shift (the classic Kasai form never sees it because
            // a sentinel-ed text has LCP < n always). Carrying n-1 forward there
            // is wrong - measured by the oracle's periodic arm (col=63, truth 0)
            // - so the ratchet restarts at 0.
            h = (len == n || len == 0) ? 0 : len - 1;
        }
    };
    if (P == 1) {
        worker(0);
    } else {
        std::vector<std::thread> th;
        th.reserve(size_t(P));
        for (int t = 1; t < P; ++t) th.emplace_back(worker, t);
        worker(0);
        for (auto& x : th) x.join();
    }
    // The wrap entry col[n-1] = LCE(SA[n-1], SA[0]) (never consumed by the
    // finish, computed so the column is complete and the oracle can check it).
    {
        uint64_t c = 0;
        const uint64_t i = an.sa[n - 1], j = an.sa[0];
        const uint64_t len = i == j ? n : cyclic_extend(T, n, i, j, 0, c);
        comps[0] += c; queries[0] += (i == j ? 0 : 1);
        maxlcp[0] = std::max(maxlcp[0], len);
        sumlcp[0] += len;
        const uint64_t r = n - 1;
        if (len < 255) out.col[size_t(r)] = uint8_t(len);
        else { out.col[size_t(r)] = 255; esc[0].emplace_back(r, len); }
    }
    for (int t = 0; t < P; ++t) {
        out.comparisons += comps[size_t(t)];
        out.queries += queries[size_t(t)];
        out.maxLcp = std::max(out.maxLcp, maxlcp[size_t(t)]);
        out.sumLcp += sumlcp[size_t(t)];
        for (auto& pr : esc[size_t(t)]) out.esc.push_back(pr);
    }
    std::sort(out.esc.begin(), out.esc.end());
    out.wall = tnow() - t0;
    return out;
}

// =============================================================== column output
static void write_column(const std::string& path, uint64_t n, const ColResult& c) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    check(fd >= 0, "create column " + path + ": " + std::strerror(errno));
    uint8_t h[COL_HEADER] = {};
    const uint32_t magic = COL_MAGIC, ver = COL_VERSION;
    const uint64_t rows = n, escapes = c.esc.size(), maxLcp = c.maxLcp, flags = 1;
    std::memcpy(h + 0, &magic, 4);
    std::memcpy(h + 4, &ver, 4);
    std::memcpy(h + 8, &n, 8);
    std::memcpy(h + 16, &rows, 8);
    std::memcpy(h + 24, &escapes, 8);
    std::memcpy(h + 32, &maxLcp, 8);
    std::memcpy(h + 40, &flags, 8);
    write_full(fd, h, COL_HEADER, "column header");
    write_full(fd, c.col.data(), size_t(n), "column body");
    for (auto& pr : c.esc) {
        uint64_t pair[2] = {pr.first, pr.second};
        write_full(fd, pair, sizeof(pair), "column escapes");
    }
    ::close(fd);
}

// ==================================================================== the oracle
// Brute force over all rotations: the SA by a full lexicographic sort, the
// column by a direct cyclic LCE of every adjacent pair, and then the FINISH's
// two derived families checked as arithmetic:
//     topLcp   = col[r-1]                 interior = min col[r..r+len-2]
// against brute LCE(SA[r-1],SA[r]) and brute LCE(SA[r],SA[r+len-1]).
struct Brute {
    std::vector<uint64_t> sa;
    std::vector<uint64_t> col;
};

static uint64_t brute_lce(const uint8_t* T, uint64_t n, uint64_t i, uint64_t j) {
    if (i == j) return n;
    uint64_t t = 0;
    while (t < n) {
        uint64_t a = i + t; if (a >= n) a -= n;
        uint64_t b = j + t; if (b >= n) b -= n;
        if (T[a] != T[b]) break;
        ++t;
    }
    return t;
}

static Brute brute_build(const std::vector<uint8_t>& T) {
    Brute b;
    const uint64_t n = T.size();
    b.sa.resize(size_t(n));
    for (uint64_t p = 0; p < n; ++p) b.sa[size_t(p)] = p;
    std::sort(b.sa.begin(), b.sa.end(), [&](uint64_t x, uint64_t y) {
        if (x == y) return false;
        // Compare up to n symbols; full ties (periodic corpora) fall back to the
        // position so the order is a permutation (either order is a valid SA).
        for (uint64_t t = 0; t < n; ++t) {
            uint64_t a = x + t, c = y + t;
            if (a >= n) a -= n;
            if (c >= n) c -= n;
            if (T[size_t(a)] != T[size_t(c)]) return T[size_t(a)] < T[size_t(c)];
        }
        return x < y;
    });
    // Verify it is a permutation (the sort above cannot lose an element, but a
    // comparator bug would show here rather than downstream).
    {
        std::vector<uint8_t> seen(static_cast<size_t>(n), 0);
        for (uint64_t p = 0; p < n; ++p) { check(!seen[size_t(b.sa[size_t(p)])], "brute SA dup"); seen[size_t(b.sa[size_t(p)])] = 1; }
    }
    // Verify the sort order is a valid rotation order (adjacent comparisons).
    for (uint64_t r = 0; r + 1 < n; ++r) {
        const uint64_t x = b.sa[size_t(r)], y = b.sa[size_t(r + 1)];
        const uint64_t l = brute_lce(T.data(), n, x, y);
        if (l < n) {
            uint64_t a = x + l, c = y + l;
            if (a >= n) a -= n;
            if (c >= n) c -= n;
            check(T[size_t(a)] < T[size_t(c)], "brute SA is not sorted at rank " + std::to_string(r));
        }
    }
    b.col.resize(size_t(n));
    for (uint64_t r = 0; r < n; ++r)
        b.col[size_t(r)] = brute_lce(T.data(), n, b.sa[size_t(r)], b.sa[size_t((r + 1) % n)]);
    return b;
}

// Run the REAL inversion + REAL walk on a synthetic anchor file for `T` and
// return the column (used by every oracle arm).
static ColResult run_pipeline(const std::vector<uint8_t>& T, const std::vector<uint64_t>& sa,
                              const std::string& tmpDir, unsigned forceBuckets, bool directArm,
                              int threads) {
    const uint64_t n = T.size();
    const std::string ankPath = tmpDir + "/anchor.sxsa";
    {
        const int fd = ::open(ankPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        check(fd >= 0, "create oracle anchor");
        uint8_t h[SA_HEADER] = {};
        const uint32_t magic = SA_MAGIC, ver = SA_VERSION;
        const uint64_t nn = n, k = 1, count = n, rows = n, pad = 10;
        std::memcpy(h + 0, &magic, 4); std::memcpy(h + 4, &ver, 4);
        std::memcpy(h + 8, &nn, 8); std::memcpy(h + 16, &k, 8);
        std::memcpy(h + 24, &count, 8); std::memcpy(h + 32, &rows, 8);
        std::memcpy(h + 40, &pad, 8);
        write_full(fd, h, SA_HEADER, "oracle anchor header");
        write_full(fd, sa.data(), size_t(n * 8), "oracle anchor body");
        ::close(fd);
    }
    Mapped am;
    am.open(ankPath);
    const Anchor an = parse_anchor(am, n);
    const std::string isaPath = tmpDir + "/oracle.isa";
    const unsigned width = isa_width(n);
    InvertResult ir;
    if (directArm) {
        invert_direct(an, width, n, isaPath, ir);
    } else {
        const std::string dumpDir = tmpDir + "/dump";
        check(::mkdir(dumpDir.c_str(), 0777) == 0, "create the oracle's ISA dump tree");
        invert_bucketed(an, width, n, isaPath, dumpDir, /*ramBudget*/ 1u << 20, forceBuckets, ir);
    }
    Mapped im;
    im.open(isaPath);
    unsigned w2 = 0;
    read_isa_header(isaPath, n, w2);
    check(w2 == width && im.n == ISA_HEADER + n * width, "oracle ISA size");
    ColResult c = kasai(an, im.p + ISA_HEADER, width, T.data(), n, threads);
    ::unlink(isaPath.c_str());
    ::unlink(ankPath.c_str());
    ::rmdir(tmpDir.c_str());        // empty in both arms
    return c;
}

static int selftest() {
    struct Case { const char* name; std::vector<uint8_t> t; };
    std::vector<Case> cases;
    auto fromStr = [](const std::string& s) {
        std::vector<uint8_t> v(s.begin(), s.end());
        return v;
    };
    // adversarial corpora: all-equal, periodic, separators + repeats, random,
    // tiny, and a descent (worst case for the ratchet's neighbour).
    { std::vector<uint8_t> v(64, uint8_t('a')); cases.push_back({"all-equal-64", v}); }
    { std::string s; while (s.size() < 64) s += "ab"; cases.push_back({"periodic-ab", fromStr(s)}); }
    { std::string s; while (s.size() < 60) s += "aab"; cases.push_back({"periodic-aab", fromStr(s)}); }
    cases.push_back({"separators", fromStr("hello\x1e" "world\x1e" "hello\x1e" "worlX\x1e")});
    cases.push_back({"dollar-cyclic", fromStr("mississippi$mississippi")});
    cases.push_back({"n1", fromStr("x")});
    cases.push_back({"n2-eq", fromStr("aa")});
    cases.push_back({"n2-ne", fromStr("ba")});
    cases.push_back({"descent", fromStr("zyxwvutsrqponmlkjihgfedcba")});
    cases.push_back({"fib", fromStr("abaababaabaababaababaabaababaabaab")});
    {
        std::vector<uint8_t> v;
        uint64_t s = 12345;
        for (int i = 0; i < 96; ++i) { s = s * 6364136223846793005ull + 1442695040888963407ull; v.push_back(uint8_t('a' + (s >> 33) % 4)); }
        cases.push_back({"random-4sym", v});
    }
    {
        // deep repeats: exercised the ESCAPE path (values >= 255) and a long
        // ratchet over a 300-symbol 'a' run.
        std::vector<uint8_t> v(300, uint8_t('a'));
        for (int i = 0; i < 40; ++i) { v.push_back(uint8_t('a' + (i * 7) % 3)); v.insert(v.end(), 40, uint8_t('a')); }
        cases.push_back({"deep-repeats", v});
    }
    {
        std::vector<uint8_t> v;
        uint64_t s = 777;
        for (int i = 0; i < 1024; ++i) { s = s * 6364136223846793005ull + 1442695040888963407ull; v.push_back(uint8_t('a' + (s >> 41) % 2)); }
        cases.push_back({"random-binary-1k", v});
    }
    {
        std::vector<uint8_t> v;
        uint64_t s = 999;
        for (int i = 0; i < 40; ++i) { s = s * 6364136223846793005ull + 1442695040888963407ull; v.push_back(uint8_t((s >> 40) & 0xff)); }
        cases.push_back({"random-bytes", v});
    }
    const std::string tmpRoot = [] {
        const char* e = std::getenv("KASAI_SELFTEST_DIR");
        return std::string(e ? e : "/tmp") ;
    }();
    uint64_t arms = 0, values = 0, families = 0, escaped = 0;
    for (const auto& cs : cases) {
        const uint64_t n = cs.t.size();
        const Brute b = brute_build(cs.t);
        for (int mode = 0; mode < 3; ++mode) {
            // mode 0: the direct arm; 1: bucketed with many buckets; 2: bucketed, one bucket
            const unsigned buckets = mode == 0 ? 0 : (mode == 1 ? 3 : 1);
            const std::string dir = tmpRoot + "/kasai-selftest-" + std::to_string(::getpid()) +
                                    "-" + std::to_string(arms);
            ::mkdir(dir.c_str(), 0777);
            const std::string inDir = dir + "/in";
            ::mkdir(inDir.c_str(), 0777);
            const ColResult c = run_pipeline(cs.t, b.sa, inDir, buckets, mode == 0, 2);
            // (a) the column, entry by entry, escapes patched in
            std::vector<uint64_t> got(static_cast<size_t>(n));
            size_t ei = 0;
            for (uint64_t r = 0; r < n; ++r) {
                if (c.col[size_t(r)] != 255) got[size_t(r)] = c.col[size_t(r)];
                else {
                    check(ei < c.esc.size() && c.esc[ei].first == r, "escape list is not aligned at rank " + std::to_string(r));
                    check(c.esc[ei].second >= 255, "escape holds a value below the cap");
                    got[size_t(r)] = c.esc[ei].second;
                    ++ei;
                }
            }
            check(ei == c.esc.size(), "unused escape entries");
            escaped += c.esc.size();
            for (uint64_t r = 0; r < n; ++r) {
                check(got[size_t(r)] == b.col[size_t(r)],
                      std::string(cs.name) + ": col[" + std::to_string(r) + "] = " +
                      std::to_string(got[size_t(r)]) + ", brute says " + std::to_string(b.col[size_t(r)]));
                ++values;
            }
            // (b) the FINISH's derivation, as arithmetic: runs = BWT runs
            //     (maximal equal preceding characters), head rank r, length len.
            std::vector<uint8_t> bwt(static_cast<size_t>(n));
            for (uint64_t r = 0; r < n; ++r) bwt[size_t(r)] = cs.t[size_t((b.sa[size_t(r)] + n - 1) % n)];
            uint64_t r0 = 0;
            while (r0 < n) {
                uint64_t len = 1;
                while (r0 + len < n && bwt[size_t(r0 + len)] == bwt[size_t(r0)]) ++len;
                const uint64_t rankStart = r0;
                // topLcp = col[rankStart-1]  (for a non-first run), against brute
                if (rankStart > 0) {
                    const uint64_t gotTop = got[size_t(rankStart - 1)];
                    const uint64_t wantTop = brute_lce(cs.t.data(), n, b.sa[size_t(rankStart - 1)],
                                                       b.sa[size_t(rankStart)]);
                    check(gotTop == wantTop, std::string(cs.name) + ": topLcp at run " +
                          std::to_string(rankStart) + " = " + std::to_string(gotTop) +
                          ", brute says " + std::to_string(wantTop));
                }
                if (len >= 2) {
                    uint64_t m = ~uint64_t(0);
                    for (uint64_t t = rankStart; t + 1 < rankStart + len; ++t) m = std::min(m, got[size_t(t)]);
                    const uint64_t want = brute_lce(cs.t.data(), n, b.sa[size_t(rankStart)],
                                                    b.sa[size_t(rankStart + len - 1)]);
                    check(m == want, std::string(cs.name) + ": interior at run " +
                          std::to_string(rankStart) + " = " + std::to_string(m) +
                          ", brute says " + std::to_string(want));
                }
                ++families;
                r0 += len;
            }
            ::rmdir(dir.c_str());   // inDir was removed by run_pipeline
            ++arms;
        }
    }
    check(escaped > 0, "the escape path was never exercised by the oracle");
    std::printf("KASAI_ORACLE_PASS corpora=%zu arms=%llu column_entries=%llu run_families=%llu "
                "escaped_entries=%llu invert_arms=direct,bucketed3,bucketed1 escape_cap=255\n",
                cases.size(), (unsigned long long)arms, (unsigned long long)values,
                (unsigned long long)families, (unsigned long long)escaped);
    return 0;
}

// A gate that cannot fail proves nothing: a duplicated SA position must be
// refused by the inversion's coverage check (exit 2).
static int selftest_negative() {
    const std::string dir = [] {
        const char* e = std::getenv("KASAI_SELFTEST_DIR");
        return std::string(e ? e : "/tmp");
    }();
    const std::vector<uint8_t> T = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint64_t> sa = {0, 1, 2, 3, 4, 5, 6, 7};
    sa[5] = 4;                                        // duplicate, 5 missing
    const std::string d = dir + "/kasai-neg-" + std::to_string(::getpid());
    ::mkdir(d.c_str(), 0777);
    std::fprintf(stderr, "KASAI_ORACLE_NEGATIVE expecting the coverage check to fire\n");
    (void)run_pipeline(T, sa, d, 0, true, 1);          // must die inside
    std::fprintf(stderr, "KASAI_ORACLE_NEGATIVE_FAIL the corrupted SA was ACCEPTED\n");
    return 1;
}

// ==================================================================== main
int main(int argc, char** argv) {
    std::string corpus, remap, saPath, outPath, tmpDir, invertMode = "auto";
    int threads = int(std::thread::hardware_concurrency());
    uint64_t ramBudget = 8ull << 30, buckets = 0;
    bool keepIsa = false, doSelf = false, doNeg = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&]() -> std::string {
            check(i + 1 < argc, a + " requires a value");
            return std::string(argv[++i]);
        };
        if (a == "--corpus") corpus = need();
        else if (a == "--remap") remap = need();
        else if (a == "--sa") saPath = need();
        else if (a == "--out") outPath = need();
        else if (a == "--tmp") tmpDir = need();
        else if (a == "--invert") invertMode = need();
        else if (a == "--threads") threads = std::atoi(need().c_str());
        else if (a == "--ram-budget") ramBudget = std::strtoull(need().c_str(), nullptr, 10);
        else if (a == "--buckets") buckets = std::strtoull(need().c_str(), nullptr, 10);
        else if (a == "--keep-isa") keepIsa = true;
        else if (a == "--selftest") doSelf = true;
        else if (a == "--selftest-negative") doNeg = true;
        else die("unknown argument " + a);
    }
    if (doSelf) return selftest();
    if (doNeg) return selftest_negative();
    check(!corpus.empty() && !remap.empty() && !saPath.empty() && !outPath.empty(),
          "usage: kasai_column --corpus C --remap R --sa SA --out COLUMN [--threads T] "
          "[--invert auto|direct|bucketed] [--ram-budget BYTES] [--buckets N] [--tmp DIR]");
    check(invertMode == "auto" || invertMode == "direct" || invertMode == "bucketed",
          "--invert must be auto|direct|bucketed");
    check(threads >= 1, "--threads must be >= 1");
    const double tAll = tnow();

    Mapped rm, txt, saMap;
    rm.open(remap);
    uint8_t table[256];
    load_remap(rm, table);
    txt.open(corpus);
    saMap.open(saPath);
    const uint64_t n = txt.n;
    check(n > 0, "empty corpus");
    const Anchor an = parse_anchor(saMap, n);

    const std::string isaPath = outPath + ".isa";
    const std::string dumpDir = outPath + ".isadump";
    unsigned width = isa_width(n);
    // auto: the direct arm when the slot array + bitmap fit a third of the RAM
    // budget, else the bucketed arm.
    bool direct = invertMode == "direct";
    if (invertMode == "auto") direct = (n * (width + 1)) <= (ramBudget / 3);
    InvertResult ir;
    if (direct) {
        std::fprintf(stderr, "KASAI_COLUMN_INVERT arm=direct n=%llu width=%u\n",
                     (unsigned long long)n, width);
        invert_direct(an, width, n, isaPath, ir);
    } else {
        check(::mkdir(dumpDir.c_str(), 0777) == 0,
              "create the ISA dump tree " + dumpDir + " (must not exist)");
        std::fprintf(stderr, "KASAI_COLUMN_INVERT arm=bucketed n=%llu width=%u ram_budget=%llu\n",
                     (unsigned long long)n, width, (unsigned long long)ramBudget);
        invert_bucketed(an, width, n, isaPath, dumpDir, ramBudget, buckets, ir);
    }
    std::fprintf(stderr,
                 "KASAI_COLUMN_INVERT_DONE invert_s=%.3f isa_bytes=%llu dump_bytes=%llu "
                 "buckets=%llu span_slots=%llu peak_bucket_bytes=%llu\n",
                 ir.wall, (unsigned long long)ir.isaBytes, (unsigned long long)ir.dumpBytes,
                 (unsigned long long)ir.buckets, (unsigned long long)ir.span,
                 (unsigned long long)ir.peakBucketBytes);

    Mapped isaMap;
    isaMap.open(isaPath);
    unsigned isaWidth = 0;
    read_isa_header(isaPath, n, isaWidth);
    check(isaWidth == width, "ISA width disagrees with the tool's own width decision");
    // THE RATCHET GATE: a broken amortisation costs ~53n (measured), a working
    // one <= 2n. 3n is the loud line.
    const ColResult c = kasai(an, isaMap.p + ISA_HEADER, width, txt.p, n, threads);
    const double ratio = double(c.comparisons) / double(n);
    if (c.comparisons > 3 * n)
        die("kasai_comparisons = " + std::to_string(c.comparisons) + " = " + std::to_string(ratio) +
            "n, over the 3n ratchet gate: the amortisation is broken");
    write_column(outPath, n, c);
    struct stat st{};
    check(::stat(outPath.c_str(), &st) == 0, "stat column");
    const uint64_t colBytes = uint64_t(st.st_size);
    check(colBytes == COL_HEADER + n + c.esc.size() * 16, "column file size arithmetic");
    if (!keepIsa) ::unlink(isaPath.c_str());
    std::fprintf(stderr,
                 "KASAI_COLUMN_DONE n=%llu threads=%d invert=%s invert_s=%.3f walk_s=%.3f "
                 "total_s=%.3f kasai_comparisons=%llu kasai_n_ratio=%.3f queries=%llu "
                 "max_lcp=%llu column_bytes=%llu escapes=%llu isa_bytes=%llu "
                 "isa_dump_bytes=%llu mean_lcp=%.6f path=%s\n",
                 (unsigned long long)n, threads, direct ? "direct" : "bucketed", ir.wall,
                 c.wall, tnow() - tAll, (unsigned long long)c.comparisons, ratio,
                 (unsigned long long)c.queries, (unsigned long long)c.maxLcp,
                 (unsigned long long)colBytes, (unsigned long long)c.esc.size(),
                 (unsigned long long)ir.isaBytes, (unsigned long long)ir.dumpBytes,
                 double(c.sumLcp) / double(n), outPath.c_str());
    return 0;
}
