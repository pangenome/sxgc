// PILE_FINISH - the pile-builder v2 fused banded finish.
//
// Consumes the final merge's v2 band pieces ({char u8, len u32, head SA
// packed, tail SA packed} per run band; the CROSS_V2_PIECES=1 emission)
// while the merge emits them, reads the corpus through the read-only
// remap adapter T[i]=REMAP[corpus[i]] (THE LAW: the text lives exactly once,
// in the corpus; no text is ever materialized to disk - supervisor decision
// 2A, 2026-10-10; the adapter identity is gated byte-exact against the
// banked 1GB pftext; PFCK uses an independent, exact suffix-hash grid), and produces the PILE LITE container {m1
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
#include <sys/mman.h>
#include <immintrin.h>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <sys/resource.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <elf.h>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>
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
#include <sys/wait.h>
using namespace sxi;

// PER-UNIT (v3, finish-decomposed): every failure path in the fold and in the
// verification loop is `[[cold]] [[noreturn]]` and OUT OF LINE. The fold's hot
// functions (RepairStream::feed_raw, CorpusText::operator[], the fingerprint
// ladder, Sweep::advance) otherwise each carry a few hundred bytes of
// fprintf/printf-argument setup and a std::string construction in their own
// body, which is instruction-cache and branch-density cost paid on EVERY row.
// Literals take the const char* overload, so a passing check builds nothing.
[[noreturn]] __attribute__((noinline, cold)) static void pf_fail(const char* msg) {
    fprintf(stderr, "FATAL xsa_finish: %s\n", msg);
    exit(2);
}
[[noreturn]] __attribute__((noinline, cold)) static void pf_fail(const std::string& msg) {
    fprintf(stderr, "FATAL xsa_finish: %s\n", msg.c_str());
    exit(2);
}
// MEASURE-FIRST (v3): the per-run/per-symbol checks below are the fold's hot
// path, so the two-argument form MUST NOT build a std::string when it passes.
// The old signature took `const std::string&`, i.e. every call site with a
// literal constructed (and, past 15 chars, malloc'd + freed) a temporary on
// every row - measured as the single largest per-unit cost of the serial
// fold. The const char* overload is now the literal path; the std::string
// overload survives for genuinely dynamic messages only.
static void pf_check(bool ok, const char* msg) {
    if (!ok) pf_fail(msg);
}
static void pf_check(bool ok, const std::string& msg) {
    if (!ok) pf_fail(msg);
}

// ------------------------------------------------ phase attribution + sampler
// The finish is one serial thread plus one LCE pool; the question this run
// exists to answer is WHERE that serial thread's nanoseconds go. Two
// independent instruments, both self-hosted (perf_event_paranoid=4 forbids
// perf(1) counters on this box):
//   (a) a phase tag every instrumented region sets, plus per-phase
//       getrusage//proc deltas (minflt/majflt = the page-cache miss proxy,
//       /proc/self/io read_bytes = the device-read proxy);
//   (b) an ITIMER_PROF sampler: SIGPROF is delivered to a running thread, the
//       handler records the instruction pointer + the phase tag, and the dump
//       resolves PCs to symbols with dladdr (build with -rdynamic). One sample
//       = 1 ms of CPU anywhere in the process, so the histogram is a CPU-time
//       profile of the whole pipeline, per phase.
enum PfPhase { PF_IDLE = 0, PF_REPAIR, PF_READBAND, PF_PIECEWAIT, PF_LCE, PF_ADVANCE,
              PF_M5, PF_M1, PF_SEAM, PF_PFCK, PF_EMIT, PF_NPHASE };
static const char* const pf_phase_name[PF_NPHASE] = {
    "idle", "repair", "readband", "piecewait", "lce", "advance", "m5", "m1", "seam", "pfck", "emit" };
static std::atomic<int> g_pfPhase{PF_IDLE};
struct PfPhaseScope {
    int prev;
    explicit PfPhaseScope(int p) : prev(g_pfPhase.load(std::memory_order_relaxed)) {
        g_pfPhase.store(p, std::memory_order_relaxed);
    }
    ~PfPhaseScope() { g_pfPhase.store(prev, std::memory_order_relaxed); }
};
static bool pf_prof_on = false;
static constexpr unsigned PF_PROF_SLOTS = 1u << 16;
struct PfSample { std::atomic<uint64_t> pc{(uint64_t)-1}; std::atomic<uint64_t> count{0}; };
static PfSample g_pfProf[PF_NPHASE][PF_PROF_SLOTS];
static std::atomic<uint64_t> g_pfProfDrop{0};
static void pf_prof_handler(int, siginfo_t*, void* ctx) {
#if defined(__x86_64__)
    ucontext_t* u = (ucontext_t*)ctx;
    uint64_t pc = (uint64_t)u->uc_mcontext.gregs[REG_RIP];
    int ph = g_pfPhase.load(std::memory_order_relaxed);
    if (ph < 0 || ph >= PF_NPHASE) ph = PF_IDLE;
    unsigned slot = unsigned((pc >> 2) * 2654435761u >> 16) & (PF_PROF_SLOTS - 1);
    for (unsigned probe = 0; probe < 8; ++probe, slot = (slot + 1) & (PF_PROF_SLOTS - 1)) {
        uint64_t cur = g_pfProf[ph][slot].pc.load(std::memory_order_relaxed);
        if (cur == pc) { g_pfProf[ph][slot].count.fetch_add(1, std::memory_order_relaxed); return; }
        if (cur == (uint64_t)-1 &&
            g_pfProf[ph][slot].pc.compare_exchange_strong(cur, pc, std::memory_order_relaxed)) {
            g_pfProf[ph][slot].count.fetch_add(1, std::memory_order_relaxed); return;
        }
    }
    g_pfProfDrop.fetch_add(1, std::memory_order_relaxed);
#else
    (void)ctx;
#endif
}
// Histogram dump: samples are 1 ms of CPU, so count/total is a CPU-time share.
static void pf_prof_start() {
    const char* env = getenv("XSA_FINISH_PROF");
    if (!env || *env == '0') return;
    struct sigaction sa {};
    sa.sa_sigaction = pf_prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    pf_check(sigaction(SIGPROF, &sa, nullptr) == 0, "install SIGPROF sampler");
    struct itimerval it {};
    it.it_interval.tv_usec = 1000;
    it.it_value.tv_usec = 1000;
    pf_check(setitimer(ITIMER_PROF, &it, nullptr) == 0, "arm ITIMER_PROF sampler");
    pf_prof_on = true;
    fprintf(stderr, "XSA_FINISH_PROF on=1 period_ms=1 slots=%u rdynamic_symbols=1\n", PF_PROF_SLOTS);
}
struct PfProfRow { uint64_t pc, count; };
// dladdr only sees GLOBAL symbols, and the finish's hot code is local
// (lambdas, static helpers), so resolve against the ELF .symtab of our own
// image instead: every STT_FUNC with a size, sorted by address.
struct PfSym { uint64_t lo, hi, nameOff; };
static std::vector<PfSym> g_pfSyms;
static std::vector<char> g_pfSymStr;
static void pf_load_symtab() {
    if (!g_pfSyms.empty()) return;
    FILE* f = fopen("/proc/self/exe", "rb");
    if (!f) return;
    Elf64_Ehdr eh {};
    if (fread(&eh, sizeof eh, 1, f) != 1 || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) { fclose(f); return; }
    std::vector<Elf64_Shdr> sh(eh.e_shnum);
    fseek(f, off_t(eh.e_shoff), SEEK_SET);
    if (fread(sh.data(), sizeof(Elf64_Shdr), sh.size(), f) != sh.size()) { fclose(f); return; }
    for (const auto& s : sh) {
        if (s.sh_type != SHT_SYMTAB || !s.sh_entsize || !s.sh_size || s.sh_link >= sh.size()) continue;
        const auto& strsh = sh[s.sh_link];
        g_pfSymStr.resize(strsh.sh_size);
        fseek(f, off_t(strsh.sh_offset), SEEK_SET);
        if (fread(g_pfSymStr.data(), 1, g_pfSymStr.size(), f) != g_pfSymStr.size()) break;
        std::vector<Elf64_Sym> sym(s.sh_size / sizeof(Elf64_Sym));
        fseek(f, off_t(s.sh_offset), SEEK_SET);
        if (fread(sym.data(), sizeof(Elf64_Sym), sym.size(), f) != sym.size()) break;
        for (const auto& y : sym) {
            if (ELF64_ST_TYPE(y.st_info) != STT_FUNC || !y.st_size || !y.st_name) continue;
            if (y.st_name >= g_pfSymStr.size()) continue;
            g_pfSyms.push_back({y.st_value, y.st_value + y.st_size, y.st_name});
        }
        std::sort(g_pfSyms.begin(), g_pfSyms.end(), [](const PfSym& a, const PfSym& b) { return a.lo < b.lo; });
        break;
    }
    fclose(f);
    fprintf(stderr, "XSA_FINISH_PROF_SYMTAB funcs=%zu\n", g_pfSyms.size());
}
static const char* pf_sym_of(uint64_t pc, uint64_t& off) {
    size_t lo = 0, hi = g_pfSyms.size();
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (g_pfSyms[mid].lo <= pc) lo = mid + 1; else hi = mid; }
    if (!lo) { off = 0; return "?"; }
    for (size_t i = lo; i-- > 0 && i + 8 > lo;) {
        if (pc >= g_pfSyms[i].lo && pc < g_pfSyms[i].hi) {
            off = pc - g_pfSyms[i].lo;
            const char* n = g_pfSymStr.data() + g_pfSyms[i].nameOff;
            // Demangle-ish: strip nothing, but prefer the readable tail of
            // an Itanium-mangled name is not attempted; the offset is enough.
            return n;
        }
        if (i == 0) break;
    }
    off = 0;
    Dl_info info {};
    if (dladdr((void*)pc, &info) && info.dli_sname) { off = pc - (uint64_t)info.dli_saddr; return info.dli_sname; }
    return "?";
}
static void pf_prof_dump(const char* label) {
    if (!pf_prof_on) return;
    struct itimerval off {};
    setitimer(ITIMER_PROF, &off, nullptr);
    pf_load_symtab();
    double total = 0;
    std::vector<PfProfRow> rows[PF_NPHASE];
    for (int ph = 0; ph < PF_NPHASE; ++ph) {
        for (unsigned s = 0; s < PF_PROF_SLOTS; ++s) {
            uint64_t c = g_pfProf[ph][s].count.load(std::memory_order_relaxed);
            if (!c) continue;
            rows[ph].push_back({g_pfProf[ph][s].pc.load(std::memory_order_relaxed), c});
            total += double(c);
        }
        std::sort(rows[ph].begin(), rows[ph].end(), [](const PfProfRow& a, const PfProfRow& b) { return a.count > b.count; });
    }
    fprintf(stderr, "XSA_FINISH_PROF_DUMP %s samples=%.0f dropped=%llu\n", label, total,
            (unsigned long long)g_pfProfDrop.load());
    for (int ph = 0; ph < PF_NPHASE; ++ph) {
        uint64_t phTotal = 0;
        for (const auto& r : rows[ph]) phTotal += r.count;
        if (!phTotal) continue;
        fprintf(stderr, "XSA_FINISH_PROF_PHASE phase=%s samples=%llu of_total=%.2f%%\n",
                pf_phase_name[ph], (unsigned long long)phTotal,
                total ? 100.0 * double(phTotal) / total : 0.0);
        for (size_t i = 0; i < rows[ph].size() && i < 8; ++i) {
            uint64_t off = 0;
            const char* name = pf_sym_of(rows[ph][i].pc, off);
            fprintf(stderr, "XSA_FINISH_PROF phase=%s in_phase=%.2f%% of_cpu=%.2f%% samples=%llu sym=%s+%llu\n",
                    pf_phase_name[ph], 100.0 * double(rows[ph][i].count) / double(phTotal),
                    total ? 100.0 * double(rows[ph][i].count) / total : 0.0,
                    (unsigned long long)rows[ph][i].count, name, (unsigned long long)off);
        }
    }
}
// Kernel+user accounting deltas: minor/major faults (page-cache/TLB misses on
// the mmap'd corpus) and bytes actually pulled from devices.
struct PfProcSnap { uint64_t minflt = 0, majflt = 0, readBytes = 0, rchar = 0; };
static PfProcSnap pf_proc_snap() {
    PfProcSnap s;
    struct rusage ru {};
    if (getrusage(RUSAGE_SELF, &ru) == 0) { s.minflt = uint64_t(ru.ru_minflt); s.majflt = uint64_t(ru.ru_majflt); }
    FILE* f = fopen("/proc/self/io", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            unsigned long long v = 0;
            if (sscanf(line, "read_bytes: %llu", &v) == 1) s.readBytes = v;
            else if (sscanf(line, "rchar: %llu", &v) == 1) s.rchar = v;
        }
        fclose(f);
    }
    return s;
}
// One instrumented region: wall + CPU + fault/IO deltas + the caller's own
// per-unit counters. Rates are printed only when the denominator is real.
struct PfRegion {
    const char* name;
    double t0 = 0, cpu0 = 0;
    int prevPhase = PF_IDLE;
    PfProcSnap snap0{};
    PfRegion(const char* n, int p) : name(n) {
        prevPhase = g_pfPhase.load(std::memory_order_relaxed);
        g_pfPhase.store(p, std::memory_order_relaxed);
        struct rusage ru {};
        getrusage(RUSAGE_SELF, &ru);
        cpu0 = double(ru.ru_utime.tv_sec) + 1e-6 * double(ru.ru_utime.tv_usec) +
               double(ru.ru_stime.tv_sec) + 1e-6 * double(ru.ru_stime.tv_usec);
        snap0 = pf_proc_snap();
        t0 = tnow();
    }
    double wall = 0, cpu = 0;
    uint64_t minflt = 0, majflt = 0, readBytes = 0;
    void close() {
        wall = tnow() - t0;
        struct rusage ru {};
        getrusage(RUSAGE_SELF, &ru);
        cpu = double(ru.ru_utime.tv_sec) + 1e-6 * double(ru.ru_utime.tv_usec) +
              double(ru.ru_stime.tv_sec) + 1e-6 * double(ru.ru_stime.tv_usec) - cpu0;
        PfProcSnap s = pf_proc_snap();
        minflt = s.minflt - snap0.minflt; majflt = s.majflt - snap0.majflt;
        readBytes = s.readBytes - snap0.readBytes;
        g_pfPhase.store(prevPhase, std::memory_order_relaxed);
    }
    void report(uint64_t units, const char* unitName) const {
        fprintf(stderr, "XSA_FINISH_REGION %s wall=%.3f cpu=%.3f minflt=%llu majflt=%llu dev_read=%llu units=%llu %s_ns=%.2f\n",
                name, wall, cpu, (unsigned long long)minflt, (unsigned long long)majflt,
                (unsigned long long)readBytes, (unsigned long long)units, unitName,
                units ? 1e9 * wall / double(units) : 0.0);
    }
};
// Sum over many short regions (one per band / one per buffer).
struct PfAcc {
    const char* name;
    double wall = 0, cpu = 0;
    uint64_t minflt = 0, majflt = 0, readBytes = 0;
    explicit PfAcc(const char* n) : name(n) {}
    void add(const PfRegion& r) {
        wall += r.wall; cpu += r.cpu; minflt += r.minflt; majflt += r.majflt; readBytes += r.readBytes;
    }
    void report(const char* unitName, uint64_t units) const {
        fprintf(stderr, "XSA_FINISH_REGION %s wall=%.3f cpu=%.3f minflt=%llu majflt=%llu dev_read=%llu units=%llu %s_ns=%.2f\n",
                name, wall, cpu, (unsigned long long)minflt, (unsigned long long)majflt,
                (unsigned long long)readBytes, (unsigned long long)units, unitName,
                units ? 1e9 * wall / double(units) : 0.0);
    }
};
static void phase(const char* name, double& last) {
    fprintf(stderr, "XSA_FINISH_PHASE %s wall=%.3f cumulative=%.3f\n", name, tnow() - last, tnow() - G_T0);
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
    pf_check(fd >= 0, std::string("open remap ") + path + ": " + std::strerror(errno));
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
// Corpus-access profile (MEASURE-FIRST). Per thread: number of adapter reads
// and number of 4 KiB page TRANSITIONS between consecutive reads. A page hop is
// the honest miss proxy the corpus path can produce without perf counters: one
// hop = a new TLB/page-cache entry to resolve, so hops/calls is the locality of
// what each phase actually asks for. Cache-line padded (adjacent threads must
// not share a line just because they both charge) and thread-indexed through
// slim_tid(), which the finish's LCE pool already registers.
struct alignas(64) PfAccessStat { uint64_t calls = 0, hops = 0, lastPage = ~0ull; };
static constexpr unsigned PF_ACCESS_TIDS = 512;
static std::array<PfAccessStat, PF_ACCESS_TIDS> g_pfAccess{};
static std::atomic<bool> g_pfAccessOn{false};
static inline void pf_access_charge(uint64_t i) {
    PfAccessStat& a = g_pfAccess[slim_tid()];
    ++a.calls;
    uint64_t page = i >> 12;
    if (page != a.lastPage) { ++a.hops; a.lastPage = page; }
}
static void pf_access_totals(uint64_t& calls, uint64_t& hops) {
    calls = hops = 0;
    for (unsigned t = 0; t < PF_ACCESS_TIDS; ++t) { calls += g_pfAccess[t].calls; hops += g_pfAccess[t].hops; }
}
static void pf_access_reset() {
    for (unsigned t = 0; t < PF_ACCESS_TIDS; ++t) { g_pfAccess[t].calls = 0; g_pfAccess[t].hops = 0; g_pfAccess[t].lastPage = ~0ull; }
}
struct CorpusText {
    int fd = -1;
    uint64_t size_ = 0;
    uint8_t remap[256];
    // PILE-WEEK (pile-week-v1): the corpus is IMMUTABLE (read-only adapter,
    // THE LAW: the corpus is never written), so the byte path mmaps it
    // read-only - page-cache backed, zero syscalls per byte. The old path
    // did a 64KB pread + a remap pass over the page on every thread-local
    // cache miss (the sweep's LCE/verification loops are symbol-bound: at
    // the 1GB/96t gate the corpus-LCE wall was ~790s with 99% TL-cache hit
    // rates, i.e. the cost was the miss path + remap churn, not I/O).
    // Byte-identical by construction (T[i]=REMAP[corpus[i]]); the adapter
    // identity gate + the chi/byte gates confirm. A failed mapping keeps
    // the pread path unchanged.
    const uint8_t* cmap = nullptr;
    void* cmapRegion = nullptr;
    struct Window { void* p = nullptr; uint64_t base = INF, len = 0; std::atomic<uint64_t> age{0}; };
    mutable std::array<Window, 4> windows{};
    mutable std::shared_mutex windowMutex;
    mutable std::atomic<uint64_t> windowClock{0};
    mutable bool fallbackLogged = false;
    uint64_t windowBytes = 1ull << 30;
    // >0 only when every window is mapped up front and none can be evicted (see
    // the constructor): the byte path then has no lock, no LRU and no munmap.
    uint64_t pinnedWindows = 0;
    explicit CorpusText(const std::string& path, const uint8_t map[256]) {
        fd = ::open(path.c_str(), O_RDONLY);
        pf_check(fd >= 0, "open corpus (read-only adapter)");
        struct stat st {};
        pf_check(fstat(fd, &st) == 0, "stat corpus");
        size_ = uint64_t(st.st_size);
        memcpy(remap, map, 256);
        struct rlimit limit {};
        pf_check(getrlimit(RLIMIT_AS, &limit) == 0, "read corpus address-space limit");
        // The pin budget WANTS to cover the corpus: the pinned lock-free byte
        // path is ~113x the windowed shared-lock+scan path (RESULTS_PREFETCH2.md
        // 4.4), so the budget is corpus-sized with a 32 GiB floor for tiny
        // corpora, and RLIMIT_AS/3 remains the hard bound. At the pile the
        // finish phase must run with an envelope that permits this (see
        // --finish-as-gb): pinned windows charge ADDRESS SPACE, not RSS - the
        // pages are page-cache backed and reclaimable.
        uint64_t mapBudget = std::max<uint64_t>(32ull << 30, size_);
        if (limit.rlim_cur != RLIM_INFINITY) mapBudget = std::min<uint64_t>(mapBudget, limit.rlim_cur / 3);
        windowBytes = std::max<uint64_t>(4096, (mapBudget / windows.size()) & ~uint64_t(4095));
        // MEASUREMENT KNOB (the product never sets it): force the WINDOWED byte
        // path even when the whole file would fit, with an explicit window size,
        // so the two corpus read paths can be priced against each other at any
        // scale. They are not equivalent in cost: `operator[]` on the whole-file
        // mapping is `remap[cmap[i]]` with NO synchronisation, while the windowed
        // path takes a shared_mutex on EVERY byte and scans the window table
        // (see operator[] below). The 10 GB finish runs windowed because 10 GB
        // exceeds the map budget under the phase ulimit; the 1 GB arms run
        // mmap'd, and that difference alone is what the 1 GB/10 GB wall gap is
        // made of (RESULTS_PREFETCH2.md section 4.4).
        bool forceWindowed = false;
        if (const char* wb = std::getenv("XSA_FINISH_CORPUS_WINDOW_BYTES")) {
            char* end = nullptr; errno = 0;
            const unsigned long long v = std::strtoull(wb, &end, 10);
            pf_check(*wb && *wb != '-' && end && !*end && !errno && v >= 4096,
                     "XSA_FINISH_CORPUS_WINDOW_BYTES must be an integer >= 4096");
            windowBytes = std::max<uint64_t>(4096, uint64_t(v) & ~uint64_t(4095));
            forceWindowed = true;
        }
        void* p = (!forceWindowed && size_ <= std::min<uint64_t>(mapBudget, 1ull << 30))
                      ? ::mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (p != MAP_FAILED) {
            cmap = (const uint8_t*)p;
            cmapRegion = p;
        }
        if (!cmap) {
            // ---- THE NO-EVICTION CASE (the pile's and the 10 GB run's shape) ----
            // If every byte of the corpus fits in the window set, then NO window
            // can ever be evicted, so the byte path needs no lock at all: map
            // every window up front, freeze the table, and let `operator[]` index
            // it directly. That is not an optimisation detail - the locked window
            // path costs a shared_mutex per BYTE, which at 10 GB is the finish's
            // entire wall (measured: 1 GB corpus, ONE 1 GiB window covering
            // everything, no eviction, lazy reads - lce_wall 857.6 s for 257 bands
            // vs 166 s for all 5616 bands on the mmap path, i.e. 110x per band,
            // 97.5% of it pool wait; RESULTS_PREFETCH2.md 4.4).
            const uint64_t need = (size_ + windowBytes - 1) / windowBytes;
            bool all = need <= windows.size();
            if (all) {
                for (uint64_t k = 0; k < need; ++k) {
                    const uint64_t base = k * windowBytes;
                    const uint64_t len = std::min<uint64_t>(windowBytes, size_ - base);
                    void* q = ::mmap(nullptr, size_t(len), PROT_READ, MAP_SHARED, fd, off_t(base));
                    if (q == MAP_FAILED) { all = false; break; }
                    windows[k].p = q; windows[k].base = base; windows[k].len = len;
                }
                if (!all) {   // roll back: leave the locked path in charge
                    for (auto& w : windows) if (w.p) { ::munmap(w.p, w.len); w.p = nullptr; w.base = INF; w.len = 0; }
                } else {
                    pinnedWindows = need;
                }
            }
            fprintf(stderr, "XSA_FINISH_CORPUS_WINDOWED regions=%zu region_bytes=%llu mapped_cap=%llu need_windows=%llu pinned=%d\n",
                    windows.size(), (unsigned long long)windowBytes,
                    (unsigned long long)(windows.size() * windowBytes),
                    (unsigned long long)need, int(pinnedWindows));
        }
        bool identity = true;
        for (unsigned i = 0; i < 256; ++i) identity = identity && remap[i] == uint8_t(i);
        fprintf(stderr, "XSA_FINISH_CORPUS path=%s bytes=%llu remap_identity=%d read_only=1 rotation=0 derivation=T[i]=REMAP[corpus[i]] mmap=%d\n",
                path.c_str(), (unsigned long long)size_, int(identity), int(cmap != nullptr));
    }
    ~CorpusText() {
        if (cmapRegion) { ::munmap(cmapRegion, size_); cmapRegion = nullptr; cmap = nullptr; }
        for (auto& w : windows) if (w.p) ::munmap(w.p, w.len);
        if (fd >= 0) ::close(fd);
    }
    CorpusText(const CorpusText&) = delete;
    CorpusText& operator=(const CorpusText&) = delete;
    uint64_t size() const { return size_; }
    // Bulk reads pin one of the adapter's shared windows for a whole block.
    // Workers share the four-window cap; they never allocate private mappings.
    void read_block(uint64_t off, uint8_t* dst, uint64_t len) const {
        pf_check(off <= size_ && len <= size_ - off, "corpus block bounds");
        while (len) {
            uint64_t take = cmap ? len : std::min(len, windowBytes - off % windowBytes);
            if (cmap) {
                for (uint64_t i = 0; i < take; ++i) dst[i] = remap[cmap[off + i]];
            } else {
                // Populate via the normal adapter miss/fallback machinery, then
                // retry if another worker evicted the window before we pinned it.
                (void)(*this)[off];
                std::shared_lock<std::shared_mutex> lock(windowMutex);
                const Window* hit = nullptr;
                for (const auto& w : windows)
                    if (w.p && w.base == off / windowBytes * windowBytes) hit = &w;
                if (hit) {
                    const uint8_t* src = (const uint8_t*)hit->p + off - hit->base;
                    for (uint64_t i = 0; i < take; ++i) dst[i] = remap[src[i]];
                } else {
                    lock.unlock();
                    // A bounded block fallback avoids cache thrashing when the
                    // worker count exceeds the number of resident windows.
                    pf_pread_full(fd, dst, take, off, "corpus block fallback");
                    for (uint64_t i = 0; i < take; ++i) dst[i] = remap[dst[i]];
                }
            }
            off += take; dst += take; len -= take;
        }
    }
    // ---------------------------------------------- PART 2: THE PREFETCH
    // The verification of a band is a pure function of that band's buffered runs
    // (the sol6b audit), so the adapter offsets it will read are known BEFORE it
    // is dispatched: each run carries its head and tail SA coordinate, i.e. two
    // text positions, and the LCE walks start there. This is the primitive that
    // turns those known offsets into populated page tables.
    //
    // WHY POPULATE AND NOT pread. The corpus is page cache (the corpus is
    // read-only and read many times over by the verification), so the cost a
    // lazy reader pays is the MINOR FAULT that installs the page-table entry,
    // not the device read. MADV_POPULATE_READ (Linux >= 5.14) does exactly that
    // for a range in ONE syscall, in the prefetching thread, and the entries are
    // then present for every verification worker (same mm). Where the kernel has
    // no MADV_POPULATE_READ the fallback touches one byte per page, which is the
    // same faults, paid by the prefetcher instead of by 92 workers.
    //
    // It is bounded and idempotent: a range whose pages are already present
    // costs a short kernel walk and no I/O, so calling it again for a band that
    // shares pages with an earlier band is free (and the RUNS PARTITION the SA
    // rows, so each target is prefetched exactly once in the whole run).
    void prefetch_range(uint64_t off, uint64_t len) const {
        if (!len || off >= size_) return;
        len = std::min<uint64_t>(len, size_ - off);
        const uint64_t page = 4096;
        if (cmap) {
            uint64_t lo = off & ~(page - 1), hi = (off + len + page - 1) & ~(page - 1);
            hi = std::min<uint64_t>(hi, size_);
            pf_populate(static_cast<const char*>(cmapRegion) + lo, hi - lo);
            return;
        }
        // Windowed adapter: the range must live in a mapping this process owns.
        // Pin the covering window under the shared lock (the same lock the byte
        // path uses, so the range cannot be evicted while it is populated).
        std::shared_lock<std::shared_mutex> lock(windowMutex);
        for (const auto& w : windows) {
            if (!w.p) continue;
            uint64_t lo = std::max<uint64_t>(off, w.base);
            uint64_t hi = std::min<uint64_t>(off + len, w.base + w.len);
            if (lo >= hi) continue;
            uint64_t a = lo & ~(page - 1), b = (hi + page - 1) & ~(page - 1);
            b = std::min<uint64_t>(b, w.base + w.len);
            if (a < w.base) a = w.base;
            if (b > a) pf_populate((const uint8_t*)w.p + (a - w.base), b - a);
            return;
        }
        // Not mapped yet: one byte through the normal path installs the window,
        // then the range is populated by the recursive call (which now hits).
        (void)(*this)[off];
        prefetch_range(off, len);
    }
    mutable std::atomic<uint64_t> pfRanges{0}, pfBytes{0}, pfPages{0}, pfMissed{0},
        pfWillneed{0}, pfWillneedBytes{0}, pfWillneedFailed{0},
        pfResidentPages{0}, pfSampledPages{0},
        pfColdDrops{0}, pfColdDropBytes{0};
    mutable std::atomic<double> pfWall{0}, pfColdWall{0};
    // -------------------------------------------- PART A: WILLNEED (assist)
    // MADV_POPULATE_READ (prefetch_range above) BLOCKS the prefetching thread
    // until the pages are resident: one front, one thread, and on a cold corpus
    // (the pile regime) that thread is the device's only queuer of reads.
    // WILLNEED is the cheap half of the same idea: it hands the SAME merged
    // range to the kernel's readahead and returns immediately, so the device
    // reads can be sorted and overlapped with the workers' demand faults. It is
    // advisory (a kernel that refuses is counted, never fatal) and it is
    // page-clamped to the file because posix_fadvise refuses offsets past EOF.
    void willneed_range(uint64_t off, uint64_t len) const {
        if (!len || off >= size_ || fd < 0) return;
        len = std::min<uint64_t>(len, size_ - off);
        const uint64_t lo = off & ~uint64_t(4095);
        const uint64_t hi = std::min<uint64_t>((off + len + 4095) & ~uint64_t(4095), size_);
        if (hi <= lo) return;
        if (::posix_fadvise(fd, off_t(lo), off_t(hi - lo), POSIX_FADV_WILLNEED) != 0) {
            pfWillneedFailed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pfWillneed.fetch_add(1, std::memory_order_relaxed);
        pfWillneedBytes.fetch_add(hi - lo, std::memory_order_relaxed);
    }
    // ---------------------------------------- PART C: THE COLD-CORPUS TRICK
    // At 1 GB the page cache warms the WHOLE corpus, so every arm measures a
    // warm read path and the pile's regime (a corpus that cannot fit the cache)
    // is invisible. Dropping the corpus from the cache before an arm makes the
    // measurement say what it will cost when the text does not fit. Advisory,
    // page-cache only: the file's bytes cannot change, so no gate moves.
    void cold_all() const {
        if (fd < 0 || !size_) return;
        const double t0 = tnow();
        // THE MAPPING MUST BE ZAPPED FIRST. posix_fadvise(DONTNEED) only drops
        // pages that are NOT mapped into a page table, and the whole-file adapter
        // maps the corpus - so fadvise alone is a NO-OP here (measured: 100%
        // resident right after the "drop"). MADV_DONTNEED on the shared mapping
        // zaps the PTEs and, for a clean MAP_SHARED file page, drops it from the
        // page cache as well; the next access faults it back from the file. Bytes
        // cannot change (the corpus is read-only), so no gate can move.
        if (cmapRegion && cmap) ::madvise(cmapRegion, size_t(size_), MADV_DONTNEED);
        for (auto& w : windows) if (w.p) ::madvise(w.p, size_t(w.len), MADV_DONTNEED);
        (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        pfColdDrops.fetch_add(1, std::memory_order_relaxed);
        pfColdDropBytes.fetch_add(size_, std::memory_order_relaxed);
        pfColdWall.store(pfColdWall.load(std::memory_order_relaxed) + (tnow() - t0),
                         std::memory_order_relaxed);
    }
    uint64_t corpusBytes() const { return size_; }
    // -------------------------------------------------- PART B: RESIDENCY VEC
    // Before a merged read is issued, ask the kernel which of its pages are
    // ALREADY resident. A sweep whose batch is half-warm must fetch the half that
    // is not - that is what makes the read-ahead idempotent across passes (and
    // what makes `touched_mass` an upper bound while `fetched_bytes` is the true
    // device cost). Returns false when there is no mapping to ask (a windowed
    // adapter or a kernel that refuses), and the caller must then assume every
    // page is cold - never the other way round.
    bool page_residency(uint64_t off, uint64_t len, std::vector<unsigned char>& out) const {
        const uint64_t page = 4096;
        if (off >= size_) { out.clear(); return true; }
        len = std::min<uint64_t>(len, size_ - off);
        const uint64_t lo = off & ~(page - 1);
        const uint64_t hi = std::min<uint64_t>((off + len + page - 1) & ~(page - 1), size_);
        if (hi <= lo) { out.clear(); return true; }
        out.assign((hi - lo) / page, 1);
        if (!cmap || !cmapRegion) return false;
        if (::mincore((uint8_t*)cmapRegion + lo, size_t(hi - lo), out.data()) != 0) return false;
        return true;
    }
    static void pf_populate(const void* addr, uint64_t len) {
        // Page-aligned by both call sites (madvise refuses anything else).
        int rc = ::madvise(const_cast<void*>(addr), size_t(len), MADV_POPULATE_READ);
        if (rc == 0) return;
        // Fallback: no MADV_POPULATE_READ in this kernel - touch one byte per
        // page; the faults are the same ones the workers would have taken.
        const uint64_t page = 4096;
        volatile uint8_t sink = 0;
        for (uint64_t z = 0; z < len; z += page) sink ^= ((const uint8_t*)addr)[z];
        (void)sink;
    }
    // ------------------------------------------------------ CACHE RESIDENCY
    // Is the corpus still in the page cache? Sampled (never assumed) at the end
    // of the finish: mincore() the adapter's own mappings at a stride of 64 pages
    // and count how many of the sampled pages are resident. A second number the
    // same probe reports is the kernel's own Cached/Shmem counters, read before
    // and after the run by the caller. `mmap=1` needs no sampling - the whole
    // file is one mapping - but the same stride is used so the numbers compare.
    //
    // `stridePages` is the sampling stride (64 pages = 256 KiB by default; the
    // per-phase journal uses a coarser stride because it runs many times and a
    // mincore walk is a syscall per page range sampled).
    struct Residency { uint64_t sampled = 0, resident = 0, corpusPages = 0; };
    Residency sample_residency(uint64_t stridePages = 64) const {
        const uint64_t page = 4096, stride = stridePages * page;
        uint64_t sampled = 0, resident = 0;
        Residency out;
        out.corpusPages = (size_ + page - 1) / page;
        auto count = [&](const void* base, uint64_t len) {
            std::vector<unsigned char> vec(1);
            for (uint64_t off = 0; off < len; off += stride) {
                uint64_t alen = std::min<uint64_t>(page, len - off);
                uint8_t* addr = (uint8_t*)const_cast<void*>(base) + off;
                vec.assign((alen + page - 1) / page, 0);
                if (::mincore(addr, size_t(alen), vec.data()) != 0) { ++sampled; continue; }
                ++sampled;
                resident += vec[0] & 1;
            }
        };
        if (cmap) count(cmap, size_);
        else for (const auto& w : windows) if (w.p) count(w.p, w.len);
        pfSampledPages.store(sampled);
        pfResidentPages.store(resident);
        out.sampled = sampled; out.resident = resident;
        return out;
    }
    uint8_t operator[](uint64_t i) const {
        if (i >= size_) pf_fail("corpus adapter bounds");
        if (g_pfAccessOn.load(std::memory_order_relaxed)) pf_access_charge(i);
        if (cmap) return remap[cmap[i]];
        // NO-EVICTION FAST PATH: every window is mapped and frozen, so the table
        // is immutable and this is a pure index + load - the same cost as the
        // whole-file mapping. This is what makes the 10 GB finish's verification
        // term the same shape as the 1 GB one's.
        if (pinnedWindows) {
            const uint64_t k = i / windowBytes;
            const Window& w = windows[k];
            if (w.p && i >= w.base && i - w.base < w.len) return remap[((const uint8_t*)w.p)[i - w.base]];
            pf_fail("corpus adapter: byte outside every pinned window");
        }
        {
            // Shared readers pin mappings through the byte read; only misses evict.
            uint64_t base = (i / windowBytes) * windowBytes;
            {
                std::shared_lock<std::shared_mutex> lock(windowMutex);
                for (auto& w : windows) if (w.base == base && w.p) {
                    w.age.store(++windowClock, std::memory_order_relaxed);
                    return remap[((uint8_t*)w.p)[i - base]];
                }
            }
            std::unique_lock<std::shared_mutex> lock(windowMutex);
            Window* target = &windows[0];
            for (auto& w : windows) {
                if (w.base == base && w.p) { w.age.store(++windowClock, std::memory_order_relaxed); return remap[((uint8_t*)w.p)[i - base]]; }
                if (w.age.load(std::memory_order_relaxed) < target->age.load(std::memory_order_relaxed)) target = &w;
            }
            if (target->p) ::munmap(target->p, target->len);
            target->p = nullptr; target->base = INF;
            uint64_t len = std::min(windowBytes, size_ - base);
            void* p = ::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, off_t(base));
            if (p != MAP_FAILED) {
                target->p = p; target->base = base; target->len = len;
                target->age.store(++windowClock, std::memory_order_relaxed);
                return remap[((uint8_t*)p)[i - base]];
            }
            if (!fallbackLogged) {
                fprintf(stderr, "XSA_FINISH_CORPUS_FALLBACK pread=1 window_bytes=%llu errno=%d reason=%s\n", (unsigned long long)windowBytes, errno, strerror(errno));
                fallbackLogged = true;
            }
        }
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

// One /proc/meminfo field, in kB (0 when it cannot be read): the residency probe
// reports the kernel's own Cached counter alongside its mincore sample so the
// two can be cross-checked.
static long pf_meminfo_kb(const char* field) {
    FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    long v = 0;
    const size_t n = std::strlen(field);
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, field, n) == 0 && line[n] == ':') { v = std::atol(line + n + 1); break; }
    }
    std::fclose(f);
    return v;
}

// ---------------------------------------------------- CACHE RESIDENCY JOURNAL
// PART A (operator): the corpus's page-cache residency is SAMPLED per phase, not
// assumed once at the end. A single mincore() walk at a coarse stride is a few
// ms at 1 GB and tens of ms at 10 GB, so the per-phase lines are OFF by default
// (XSA_FINISH_CACHE_JOURNAL=1 turns them on) and the end-of-finish line is
// always printed. The numbers are of the ADAPTER's own mappings (the same view
// the workers read through), plus the kernel's own Cached/Dirty/Free counters so
// the two can be cross-checked.
static bool pf_cache_journal_on() {
    const char* e = std::getenv("XSA_FINISH_CACHE_JOURNAL");
    return e && *e && *e != '0';
}
static void pf_cache_journal(const CorpusText& text, const char* label,
                             uint64_t stridePages = 1024) {
    const CorpusText::Residency r = text.sample_residency(stridePages);
    const long cachedKb = pf_meminfo_kb("Cached"), dirtyKb = pf_meminfo_kb("Dirty");
    const long freeKb = pf_meminfo_kb("MemFree"), shmemKb = pf_meminfo_kb("Shmem");
    const uint64_t page = 4096;
    const double residentGb = double(r.resident) * double(stridePages * page) / 1e9;
    std::fprintf(stderr,
        "XSA_FINISH_CACHE_RESIDENCY label=%s sampled_pages=%llu resident_pages=%llu "
        "resident_pct=%.2f stride_pages=%llu corpus_gb=%.3f resident_gb=%.3f "
        "meminfo_cached_gb=%.2f meminfo_dirty_gb=%.2f meminfo_free_gb=%.2f "
        "meminfo_shmem_gb=%.2f populate_ranges=%llu willneed_ranges=%llu "
        "willneed_bytes=%llu willneed_failed=%llu cold_drops=%llu cold_bytes=%llu "
        "cold_wall=%.3f\n",
        label, (unsigned long long)r.sampled, (unsigned long long)r.resident,
        r.sampled ? 100.0 * double(r.resident) / double(r.sampled) : 0.0,
        (unsigned long long)stridePages, double(text.size()) / 1e9, residentGb,
        cachedKb / (1024.0 * 1024.0), dirtyKb / (1024.0 * 1024.0),
        freeKb / (1024.0 * 1024.0), shmemKb / (1024.0 * 1024.0),
        (unsigned long long)text.pfRanges.load(),
        (unsigned long long)text.pfWillneed.load(),
        (unsigned long long)text.pfWillneedBytes.load(),
        (unsigned long long)text.pfWillneedFailed.load(),
        (unsigned long long)text.pfColdDrops.load(),
        (unsigned long long)text.pfColdDropBytes.load(),
        text.pfColdWall.load());
}

// PART A's finish-side hygiene switch: `XSA_FINISH_HYGIENE=0` turns OFF the
// witness-spool release and the rlebwt release, i.e. the finish lets its own
// scratch squat the page cache. The pile-regime A/B's control arm uses it; the
// product default is 1 (hygiene on).
static bool pf_hygiene_on() {
    const char* e = std::getenv("XSA_FINISH_HYGIENE");
    return !(e && *e == '0');
}

// ------------------------------------------------- CACHE HYGIENE (operator)
// Our own scratch I/O is what evicts the corpus from the page cache: every
// window file, witness spool and product write dirties pages the kernel would
// otherwise keep for the text. The corpus is the SECOND-TIER POOL (hundreds of
// GB of free RAM on this box), so a scratch file whose bytes are done with is
// told so explicitly, page by page, at the moment it stops being needed.
//   * DONTNEED rather than O_DIRECT: the pwrite fill's staging is 32 MiB/thread
//     water-filled over 256 interleaved bin streams, so its file offsets are
//     neither 512-byte nor 4096-byte aligned per write - O_DIRECT would need an
//     aligned staging rewrite (and its throughput was measured separately, see
//     RESULTS_PREFETCH.md). DONTNEED needs no alignment and cannot change a byte.
//   * the call is advisory and CANNOT alter the file's bytes, so every
//     byte-identity bank is unaffected by construction.
static void pf_release_cache(int fd) {
    if (fd < 0) return;
    struct stat st {};
    if (fstat(fd, &st) != 0) return;
    if (st.st_size <= 0) return;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

// ------------------------------------------------------------- PFCK build
// Fixed-grid suffix hashes, independent of merge run count. Each segment
// first hashes with a zero suffix prologue; a suffix scan of segment totals
// establishes the real prologues, then workers correct the checkpoint file.
// H(i) = H_segment(i) + BASE^(segment_end-i) * H(segment_end), modulo 2^64.
static uint64_t pfck_spacing() {
    const char* env = std::getenv("PFCK_SPACING");
    if (!env) return 22;
    char* end = nullptr; errno = 0;
    uint64_t tau = strtoull(env, &end, 10);
    pf_check(*env && *env != '-' && end && !*end && !errno && tau > 0 && tau <= UINT64_MAX / 2,
             "PFCK_SPACING must be a positive integer <= UINT64_MAX/2");
    return tau;
}
static void build_pfck(const CorpusText& corpus, uint64_t n, uint64_t tau,
                       int threads, uint64_t ramBudget, const std::string& out) {
    double t0 = tnow();
    uint64_t count = n / tau + 1;
    int fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
    pf_check(fd >= 0, "create pfck (refusing to clobber)");
    auto wfull = [&](const void* d, uint64_t len, uint64_t off) {
        const char* q = (const char*)d;
        while (len) {
            ssize_t z = ::pwrite(fd, q, len, off);
            pf_check(z > 0, "pfck write");
            q += z; off += uint64_t(z); len -= uint64_t(z);
        }
    };
    char hdr[40];
    uint32_t magic = 0x4B434650, ver = 1; uint64_t rsv = 0;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &ver, 4);
    memcpy(hdr + 8, &n, 8); memcpy(hdr + 16, &tau, 8);
    memcpy(hdr + 24, &count, 8); memcpy(hdr + 32, &rsv, 8);
    wfull(hdr, 40, 0);
    constexpr uint64_t BW = 1u << 20, EC = 1u << 16;
    // At most 1.5 MiB per worker, taken from a small fraction of the finish
    // allowance so readiness overlaps merge within the shared envelope.
    const uint64_t workerBytes = BW + 8 * EC;
    int workers = int(std::min<uint64_t>(std::max(1, std::min(threads, 256)),
                       std::max<uint64_t>(1, ramBudget / 16 / workerBytes)));
    uint64_t blocks = n / tau + (n % tau != 0);
    uint64_t segments = std::min<uint64_t>(std::max<uint64_t>(1, blocks), workers * 4);
    workers = std::min<int>(workers, int(segments));
    auto boundary = [&](uint64_t k) {
        if (k == segments) return n;
        return (blocks / segments * k + blocks % segments * k / segments) * tau;
    };
    std::vector<uint64_t> totals(segments), prologue(segments);
    auto parallel = [&](const std::function<void(uint64_t)>& job) {
        std::atomic<uint64_t> next{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < workers; ++t) pool.emplace_back([&] {
            for (;;) { uint64_t k = next.fetch_add(1); if (k >= segments) break; job(k); }
        });
        for (auto& t : pool) t.join();
    };
    parallel([&](uint64_t k) {
        uint64_t lo = boundary(k), hi = boundary(k + 1), h = 0;
        std::vector<uint8_t> band(BW);
        std::vector<uint64_t> ent; ent.reserve(EC);
        auto flush = [&](uint64_t first) {
            std::reverse(ent.begin(), ent.end());
            wfull(ent.data(), ent.size() * 8, 40 + first * 8); ent.clear();
        };
        for (uint64_t bh = hi; bh > lo;) {
            uint64_t bl = bh - std::min<uint64_t>(BW, bh - lo);
            corpus.read_block(bl, band.data(), bh - bl);
            for (uint64_t i = bh; i-- > bl;) {
                h = uint64_t(band[i - bl]) + 1 + SlimFingerprint<CorpusText>::BASE * h;
                if (i % tau == 0) {
                    ent.push_back(h);
                    if (ent.size() == EC) flush(i / tau);
                }
            }
            bh = bl;
        }
        if (!ent.empty()) flush(lo / tau);
        totals[k] = h;
    });
    uint64_t suffix = 0;
    for (uint64_t k = segments; k-- > 0;) {
        prologue[k] = suffix;
        suffix = totals[k] + SlimFingerprint<CorpusText>::power(boundary(k + 1) - boundary(k)) * suffix;
    }
    parallel([&](uint64_t k) {
        uint64_t lo = boundary(k) / tau, hi = boundary(k + 1);
        uint64_t end = hi / tau + (hi % tau != 0);
        std::vector<uint64_t> ent(EC);
        uint64_t stride = SlimFingerprint<CorpusText>::power(tau);
        uint64_t factor = end > lo ? SlimFingerprint<CorpusText>::power(hi - (end - 1) * tau) : 0;
        while (end > lo) {
            uint64_t first = end - std::min<uint64_t>(EC, end - lo);
            pf_pread_full(fd, ent.data(), (end - first) * 8, 40 + first * 8, "pfck correction");
            for (uint64_t j = end - first; j-- > 0;) {
                ent[j] += factor * prologue[k]; factor *= stride;
            }
            wfull(ent.data(), (end - first) * 8, 40 + first * 8); end = first;
        }
    });
    if (n % tau == 0) { uint64_t zero = 0; wfull(&zero, 8, 40 + 8 * (n / tau)); }
    struct stat st {};
    pf_check(fstat(fd, &st) == 0 && uint64_t(st.st_size) == 40 + 8 * count, "pfck size");
    pf_check(::close(fd) == 0, "close pfck");
    fprintf(stderr, "XSA_FINISH_PFCK path=%s n=%llu tau=%llu count=%llu bytes=%llu wall=%.3f workers=%d segments=%llu worker_bytes=%llu source=corpus_adapter fixed_grid=1\n",
            out.c_str(), (unsigned long long)n, (unsigned long long)tau, (unsigned long long)count,
            (unsigned long long)(40 + 8 * count), tnow() - t0, workers,
            (unsigned long long)segments, (unsigned long long)workerBytes);
}

// ------------------------------------------------------------ piece source
struct RawRun { uint8_t c; uint64_t len, head, tail; };

struct PieceSource {
    std::string prefix, dir;
    bool streaming = false, terminal = false;
    int64_t progress = -1, consumed = -1;
    uint64_t window = 0, depth = 0, finalBand = 0, capturedRows = 0;
    std::map<uint64_t, std::vector<uint64_t>> bandRefs;
    std::map<uint64_t, uint64_t> bandCounts, classExtents;
    std::vector<std::vector<uint64_t>> completed;
    uint64_t delivered = 0;
    uint32_t S = 0;
    uint64_t R = 0, n = 0, B = 65536;
    unsigned w = 0;
    std::vector<std::pair<uint64_t, uint64_t>> shards;
    double pollTimeout = 86400;
    struct View { int fd = -1; uint64_t firstRun = 0, rc = 0; uint64_t offChars = 0, offLens = 0, offHeads = 0, offTails = 0; };
    mutable std::mutex mu;
    mutable std::map<std::pair<uint64_t, uint32_t>, View> views;
    mutable unsigned long opened = 0;

    bool read_terminal() {
        int fd = ::open((dir + "/terminal").c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st{}; pf_check(::fstat(fd, &st) == 0 && st.st_size == 36, "terminal marker size");
        char magic[4]; uint64_t values[4];
        pf_read_full(fd, magic, 4, "terminal magic"); pf_read_full(fd, values, 32, "terminal fields"); ::close(fd);
        pf_check(memcmp(magic, "BPT1", 4) == 0 && values[1] > 0 && values[1] <= n+10 &&
                 values[0] == (values[1]-1)/B && values[2] <= (1u<<26), "terminal dimensions");
        terminal = true; finalBand = values[0]; R = values[1];
        pf_check(progress <= int64_t(finalBand), "terminal progress moved backward");
        if (progress == int64_t(finalBand)) {
            pf_check(values[2] == capturedRows && values[3] == completed.size(), "terminal capture assertion");
            pf_check(capturedRows <= std::max<uint64_t>(1000, R/1000) &&
                     depth <= std::max<uint64_t>(1000, R/1000), "terminal product repair budget");
            pf_check(bandCounts.at(finalBand) == R-finalBand*B, "terminal run count");
            for (const auto& cl : completed) pf_check(cl[2] < R, "terminal class run bounds");
        }
        return true;
    }
    void load_progress() {
        uint64_t band = uint64_t(progress+1);
        const std::string path = dir + "/g-" + std::to_string(band);
        double waited = 0; int fd = ::open(path.c_str(), O_RDONLY);
        while (fd < 0) {
            read_terminal();
            if (terminal && band > finalBand) return;
            if (waited > pollTimeout) pf_fail("timeout waiting for band progress " + path);
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); waited += .02;
            fd = ::open(path.c_str(), O_RDONLY);
        }
        struct stat st{}; pf_check(::fstat(fd, &st) == 0 && st.st_size >= 36, "progress size");
        char magic[4]; uint64_t fields[4];
        pf_read_full(fd, magic, 4, "progress magic"); pf_read_full(fd, fields, 32, "progress dimensions");
        pf_check(memcmp(magic, "BPG1", 4) == 0 && fields[0] == band && fields[1] > 0 && fields[1] <= B &&
                 fields[2] <= (1u<<26) && fields[3] <= (1u<<26), "malformed band progress");
        uint64_t offset = 36;
        auto next = [&]() { uint64_t value;
            pf_check(offset <= uint64_t(st.st_size) && uint64_t(st.st_size)-offset >= 8, "truncated band progress");
            pf_read_full(fd, &value, 8, "band progress value"); offset += 8; return value; };
        auto& refs = bandRefs[band];
        for (uint64_t i = 0; i < fields[2]; ++i) {
            uint64_t id = next(); pf_check(id < (1u<<26) && (refs.empty() || refs.back() < id), "progress class references");
            refs.push_back(id);
        }
        for (uint64_t i = 0; i < fields[3]; ++i) {
            std::vector<uint64_t> cl(8); for (auto& value : cl) value = next();
            pf_check(cl[0] == completed.size() && cl[1] <= cl[2] && cl[2]/B <= band &&
                     cl[3] < cl[4] && cl[4] <= n && cl[5] < n && cl[6] < n+10 && cl[7] < n+10,
                     "progress class bounds");
            pf_check(completed.empty() || completed.back()[4] <= cl[3], "progress overlapping classes");
            uint64_t rows = cl[4]-cl[3], extent = cl[2]/B-cl[1]/B;
            pf_check(int64_t(cl[1]/B) > consumed, "class captured after its band was consumed");
            for (uint64_t owner = cl[1]/B; owner <= cl[2]/B; ++owner) {
                const auto& owners = bandRefs.at(owner);
                pf_check(std::find(owners.begin(), owners.end(), cl[0]) != owners.end(),
                         "progress omitted intersecting class reference");
            }
            pf_check(extent < window && rows <= (1u<<26)-capturedRows, "progress class refusal");
            for (uint64_t j = 0; j < rows; ++j) { uint64_t position = next();
                pf_check(position < n, "progress occurrence bounds"); cl.push_back(position); }
            pf_check(cl[8] == cl[5], "progress leftmost occurrence");
            auto unique = std::vector<uint64_t>(cl.begin()+8, cl.end()); std::sort(unique.begin(), unique.end());
            pf_check(std::adjacent_find(unique.begin(), unique.end()) == unique.end(), "progress duplicate occurrence");
            capturedRows += rows; classExtents.emplace(cl[0], extent); completed.push_back(std::move(cl));
        }
        pf_check(offset == uint64_t(st.st_size), "progress trailing data"); ::close(fd);
        pf_check(::unlink(path.c_str()) == 0, "unlink consumed progress");
        bandCounts.emplace(band, fields[1]); progress = int64_t(band);
        if (band) pf_check(bandCounts.at(band-1) == B, "partial band before EOF");
        read_terminal();
    }
    bool ready_band(uint64_t band) {
        if (terminal && band > finalBand) return false;
        while (progress < int64_t(band)) {
            if (read_terminal() && band > finalBand) return false;
            load_progress();
            if (terminal && band > finalBand) return false;
        }
        uint64_t extent = 0;
        for (uint64_t id : bandRefs.at(band)) {
            while (!classExtents.count(id)) {
                pf_check(!terminal || progress < int64_t(finalBand), "terminal missing referenced class");
                load_progress();
            }
            const auto& cl = completed.at(id);
            pf_check(cl[1]/B <= band && cl[2]/B >= band, "class reference outside band");
            extent = std::max(extent, classExtents.at(id));
        }
        pf_check(extent < window, "band extent exceeds W-1");
        while (!terminal && uint64_t(progress) < band+extent) {
            if (read_terminal()) break;
            load_progress();
        }
        // EOF clamps b+E(b) to N. Terminal also attests final flanks.
        while (terminal && uint64_t(progress) < std::min(band+extent, finalBand)) load_progress();
        if (bandCounts.at(band) < B) {
            double waited = 0;
            while (!read_terminal()) {
                if (waited > pollTimeout) pf_fail("timeout waiting for terminal assertion");
                std::this_thread::sleep_for(std::chrono::milliseconds(20)); waited += .02;
            }
            pf_check(band == finalBand, "partial band is not final");
        }
        return true;
    }
    void consumed_band(uint64_t band) {
        if (const char* delay = getenv("XSA_FINISH_BAND_DELAY_MS"))
            std::this_thread::sleep_for(std::chrono::milliseconds(strtoull(delay,nullptr,10)));
        std::string path = prefix + ".runcols.consumed", tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w"); pf_check(f, "open consumed tmp");
        fprintf(f, "%llu\n", (unsigned long long)band);
        pf_check(fclose(f) == 0 && ::rename(tmp.c_str(), path.c_str()) == 0, "publish consumed band");
        consumed = int64_t(band);
        bandRefs.erase(band);
    }
    void read_stream_band(uint64_t band, std::vector<RawRun>& out) {
        uint64_t count = bandCounts.at(band);
        std::string path = dir + "/p-" + std::to_string(band) + "-0";
        int fd = ::open(path.c_str(), O_RDONLY); pf_check(fd >= 0, "published band piece missing");
        char hdr[44]; pf_read_full(fd, hdr, 44, "band piece header");
        uint32_t magic, shard, width; uint64_t actualBand, first, rc, zero;
        memcpy(&magic,hdr,4); memcpy(&actualBand,hdr+4,8); memcpy(&shard,hdr+12,4);
        memcpy(&first,hdr+16,8); memcpy(&rc,hdr+24,8); memcpy(&width,hdr+32,4); memcpy(&zero,hdr+36,8);
        pf_check(magic == 0x31435250 && actualBand == band && shard == 0 && first == band*B &&
                 rc == count && width == w && zero == 0, "band piece dimensions");
        uint64_t words = (uint64_t(w)*count+63)/64;
        struct stat st{}; pf_check(::fstat(fd,&st)==0 && uint64_t(st.st_size)==44+5*count+16*words, "band piece size");
        std::vector<uint8_t> chars(count); std::vector<uint32_t> lens(count);
        std::vector<uint64_t> heads(words), tails(words);
        pf_read_full(fd,chars.data(),count,"band chars"); pf_read_full(fd,lens.data(),4*count,"band lens");
        pf_read_full(fd,heads.data(),8*words,"band heads"); pf_read_full(fd,tails.data(),8*words,"band tails"); ::close(fd);
        auto unpack = [&](const std::vector<uint64_t>& data, uint64_t i) {
            uint64_t bit = uint64_t(w)*i, wi=bit/64, shift=bit%64, value=data[wi]>>shift;
            if (shift+w>64) value |= data[wi+1]<<(64-shift);
            return value & ((uint64_t(1)<<w)-1);
        };
        out.clear(); out.reserve(count);
        for (uint64_t i=0;i<count;++i) {
            uint64_t head=unpack(heads,i), tail=unpack(tails,i);
            pf_check(lens[i]>0 && head<n+10 && tail<n+10,"band record bounds");
            out.push_back({chars[i],lens[i],head,tail});
        }
        pf_check(::unlink(path.c_str())==0,"unlink consumed band piece");
    }
    void load_idx() {
        std::string path = prefix + ".runcols.idx";
        dir = prefix + ".pieces";
        int fd = ::open(path.c_str(), O_RDONLY);
        double waited = 0;
        while (fd < 0) {
            int start = ::open((dir + "/start").c_str(), O_RDONLY);
            if (start >= 0) {
                char magic[4]; uint64_t fields[4]; struct stat st{};
                pf_check(::fstat(start,&st)==0 && st.st_size==36, "stream start size");
                pf_read_full(start,magic,4,"stream start magic"); pf_read_full(start,fields,32,"stream start fields"); ::close(start);
                pf_check(memcmp(magic,"BPS1",4)==0 && fields[0]>0 && fields[1]>0 && fields[1]<64 &&
                         fields[2]>0 && fields[2]<=(1u<<26) && fields[3]>0,"stream start dimensions");
                n=fields[0]; w=fields[1]; depth=fields[2]; window=fields[3]; R=n+10; streaming=true;
                return;
            }
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
        fprintf(stderr, "XSA_FINISH_IDX shards=%u runs=%llu n=%llu w=%u band=%llu\n",
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
        if (streaming) { read_stream_band(band,out); return; }
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

// ----------------------------------------------------------- seam classes
// The v1 chain's cyclic seam repair (rpfbwt_endpoints + seam_repair.hpp)
// recomputed with NO record/seed columns, NO ssa/ssa_t, NO piece random
// access and NO rlebwt rank walk:
//
//  * DISCOVERY: the occurrence chain of the terminal suffix pattern
//    P_d = T[n-d..n) straight from the corpus. occ_1 = the terminal byte's
//    positions (one sequential scan); occ_{d+1} = {p : p+1 in occ_d and
//    T[p] == T[n-d-1]} - the same sets v1's padded-BWT backward search
//    computes (occurrences never reach the 0x02 padding: P_d has no 0x02).
//  * ROWS: the merge's own WM scratch is the merged SA in ROW order
//    (SA(padded row i+w1) = WM[i]; it is NOT sorted, so no binary search
//    over it exists). ONE sequential WM pass collects row(first_d) for
//    the few discovery depths; v1's leftmost-row invariant (check(lo ==
//    row): the pattern's own occurrence at n-d continues into the 0x02
//    padding - the smallest byte - so it is the interval's first row)
//    makes lo = row(first) exactly. hi = lo + |occ_d|.
//  * FLANKS + SA SETS: before = SA[lo-1] (v1's phi; wraps to the longest
//    padding suffix at lo == 0), after = SA[hi] (v1's phi-inverse walk;
//    wraps to SA[0] = n at hi == n), and the class SA sets SA[lo..hi) are
//    single WM preads - no run columns are touched at all.
//  * REBUILD: the class SA set in row order (v1's phi-walk order - the
//    exact sequence v1 feeds its comparator) is LCE-sorted with the v1
//    comparator and rebuilt into runs - byte-identical class contents.
//  * CROSS-CHECK: the WM-read class SA set must equal the corpus chain's
//    occurrence set element-for-element (two independent sources for
//    occ(P_d); any divergence is fatal).
struct SeamClass { uint64_t lo, hi, first, before = 0, after = 0;
                   std::vector<uint64_t> occ;     // the class SA set (occ(P_d))
                   std::vector<RawRun> runs; };   // lo/hi in NORMALIZED rows

// ---------------------------------------------------------------- packed WM
// The merge's WM scratch (the merged SA in ROW order) has two layouts:
// legacy u64-per-row (size == 8n) or, when the merge runs CROSS_WM_PACK=1,
// the PWM1 block format written by cross_lcp_merge's WmPacker: a 32-byte
// header {magic 'PWM1', wbits, count, blockVals=4096, blockBytes, 0} then
// fixed 4096-value blocks packed wbits LSB-first (w = bits(n+9)). This
// reader auto-detects; all seam reads go through it.
struct PfWm {
    int fd = -1;
    uint64_t n = 0, blockBytes = 0;
    unsigned w = 0;
    bool packed = false;
    static constexpr uint64_t VALS = 4096;
    static uint64_t packed_size(uint64_t n_, unsigned w_) {
        uint64_t blocks = (n_ + VALS - 1) / VALS;
        return 32 + blocks * ((VALS * uint64_t(w_) + 7) / 8);
    }
    void open(int openFd, uint64_t count) {
        pf_check(openFd >= 0, "wm fd");
        fd = openFd;
        n = count;
        struct stat st {};
        pf_check(::fstat(fd, &st) == 0, "fstat wm");
        if (uint64_t(st.st_size) == 8 * n) {
            packed = false;
            return;
        }
        uint8_t hdr[32];
        pf_pread_full(fd, hdr, 32, 0, "packed wm header");
        auto rd32 = [&](unsigned off) { uint32_t v; memcpy(&v, hdr + off, 4); return v; };
        auto rd64 = [&](unsigned off) { uint64_t v; memcpy(&v, hdr + off, 8); return v; };
        pf_check(rd32(0) == 0x31574D50, "packed wm magic");
        pf_check(rd64(8) == n, "packed wm count");
        pf_check(rd32(16) == VALS, "packed wm block shape");
        w = rd32(4);
        pf_check(w >= 1 && w < 64, "packed wm width");
        blockBytes = rd32(20);
        pf_check(blockBytes == (VALS * uint64_t(w) + 7) / 8, "packed wm block bytes");
        pf_check(uint64_t(st.st_size) == packed_size(n, w), "packed wm size");
        packed = true;
    }
    // Unpack one fixed block into dst[0..VALS) (values beyond n read as the
    // zero padding - callers are bounds-checked above the row count).
    void load_block(uint64_t block, uint64_t* dst) const {
        static thread_local std::vector<uint8_t> staging;
        if (staging.size() != blockBytes) staging.resize(blockBytes);
        pf_pread_full(fd, staging.data(), blockBytes, 32 + block * blockBytes, "packed wm block");
        __uint128_t res = 0; unsigned avail = 0; uint64_t bi = 0;
        const uint64_t mask = (uint64_t(1) << w) - 1;
        for (uint64_t k = 0; k < VALS; ++k) {
            while (avail < w) { res |= __uint128_t(staging[bi]) << avail; ++bi; avail += 8; }
            dst[k] = uint64_t(res) & mask;
            res >>= w;
            avail -= w;
        }
    }
    uint64_t at(uint64_t idx) const {
        pf_check(idx < n, "wm bounds");
        if (!packed) {
            uint8_t b[8];
            pf_pread_full(fd, b, 8, 8 * idx, "wm pread");
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v |= uint64_t(b[i]) << (8 * i);
            return v;
        }
        uint64_t buf[VALS];
        load_block(idx / VALS, buf);
        return buf[idx % VALS];
    }
    void read(uint64_t idx, uint64_t* out, uint64_t len) const {
        pf_check(idx + len <= n, "wm bulk bounds");
        if (!packed) {
            pf_pread_full(fd, out, 8 * len, 8 * idx, "wm read");
            return;
        }
        uint64_t done = 0;
        while (done < len) {
            uint64_t block = (idx + done) / VALS, in = (idx + done) % VALS;
            uint64_t take = std::min<uint64_t>(len - done, VALS - in);
            take = std::min<uint64_t>(take, n - block * VALS - in);
            uint64_t buf[VALS];
            load_block(block, buf);
            memcpy(out + done, buf + in, 8 * take);
            done += take;
        }
    }
};

static void seam_refuse(const char* kind, uint64_t size, uint64_t limit) {
    fprintf(stderr, "CYCLIC_SEAM_REFUSED %s=%llu limit=%llu policy=max(1000,raw_r/1000); no O(n) fallback\n",
            kind, (unsigned long long)size, (unsigned long long)limit);
    pf_fail("cyclic seam repair exceeds compressed-work policy");
}

struct SeamClasses {
    bool fromSpool = false;
    std::vector<SeamClass> classes;
    uint64_t depth = 0, total = 0, maxSeamLce = 0;
    void build_spool(int fd, uint64_t n, uint64_t rawR,
                     const std::function<uint64_t(uint64_t, uint64_t)>& collection_lce,
                     const std::function<uint8_t(uint64_t)>& symbol) {
        fromSpool = true;
        const uint64_t limit = std::max<uint64_t>(1000, rawR / 1000);
        struct stat st{};
        pf_check(::fstat(fd, &st) == 0 && st.st_size >= 28, "seam spool header size");
        uint64_t offset = 4;
        char magic[4]; pf_pread_full(fd, magic, 4, 0, "seam spool magic");
        pf_check(memcmp(magic, "SCS1", 4) == 0, "seam spool magic");
        // SCS1 classes are already banded in row order. Consume its values
        // through one bounded sequential buffer instead of a pread per word.
        std::array<uint8_t, 65536> input{};
        uint64_t inputBase = 0, inputSize = 0;
        auto next = [&]() {
            pf_check(offset <= uint64_t(st.st_size) && uint64_t(st.st_size) - offset >= 8,
                     "truncated seam spool");
            if (offset < inputBase || offset + 8 > inputBase + inputSize) {
                inputBase = offset;
                inputSize = std::min<uint64_t>(input.size(), uint64_t(st.st_size) - offset);
                pf_pread_full(fd, input.data(), inputSize, offset, "seam spool buffer");
            }
            const uint8_t* b = input.data() + offset - inputBase; offset += 8;
            uint64_t v = 0; for (unsigned j = 0; j < 8; ++j) v |= uint64_t(b[j]) << (8*j);
            return v;
        };
        pf_check(next() == n, "seam spool corpus length");
        depth = next(); uint64_t count = next();
        pf_check(depth > 0 && depth <= limit && count <= limit, "seam spool discovery budget");
        for (uint64_t i = 0; i < count; ++i) {
            SeamClass cl{};
            cl.lo = next(); cl.hi = next(); cl.first = next(); cl.before = next(); cl.after = next();
            pf_check(cl.lo < cl.hi && cl.hi <= n && cl.first < n &&
                     cl.before < n+10 && cl.after < n+10, "seam spool class bounds");
            pf_check(classes.empty() || classes.back().hi <= cl.lo, "seam spool overlapping intervals");
            uint64_t rows = cl.hi - cl.lo;
            pf_check(rows <= limit - total, "seam spool aggregate budget");
            total += rows; cl.occ.reserve(rows);
            for (uint64_t j = 0; j < rows; ++j) {
                uint64_t position = next(); pf_check(position < n, "seam spool occurrence bounds");
                cl.occ.push_back(position);
            }
            pf_check(cl.occ.front() == cl.first, "seam spool leftmost row");
            auto unique = cl.occ; std::sort(unique.begin(), unique.end());
            pf_check(std::adjacent_find(unique.begin(), unique.end()) == unique.end(), "seam spool duplicate occurrence");
            classes.push_back(std::move(cl));
        }
        pf_check(offset == uint64_t(st.st_size), "seam spool trailing data");
        fprintf(stderr, "XSA_FINISH_SEAM_SPOOL classes=%zu rows=%llu discovery_steps=%llu limit=%llu WM=absent\n",
                classes.size(), (unsigned long long)total, (unsigned long long)depth, (unsigned long long)limit);
        rebuild(n, rawR, limit, collection_lce, symbol);
    }
    void build(const CorpusText& text, const PfWm& wm, uint64_t n, uint64_t rawR, uint64_t w1,
               const std::function<uint64_t(uint64_t, uint64_t)>& collection_lce,
               const std::function<uint8_t(uint64_t)>& symbol) {
        const uint64_t N = n + w1, r = rawR;
        const uint64_t limit = std::max<uint64_t>(1000, r / 1000);
        const uint8_t terminal = text[n - 1];
        // ---- discovery: the occurrence chain of P_d = T[n-d..n) from the
        // corpus. occ_1 = the terminal byte's positions (one seq scan);
        // occ_{d+1} = {p : p+1 in occ_d and T[p] == T[n-d-1]}.
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
        fprintf(stderr, "XSA_FINISH_SEAM_OCC terminal=0x%02llx depth0_count=%llu scan=corpus\n",
                (unsigned long long)terminal, (unsigned long long)occ.size());
        // v1 pushes a class per depth while the interval holds more than
        // one row and stops at the unique row (that step counts toward the
        // depth too); sizes and refusals are v1's.
        std::vector<SeamClass> raw;
        uint64_t d = 0;
        for (;;) {
            if (d == limit) seam_refuse("discovery_steps", d + 1, limit);
            ++d;
            depth = d;
            if (occ.size() <= 1) break;
            if (uint64_t(occ.size()) > limit) seam_refuse("class_size", occ.size(), limit);
            uint64_t first = n - d;
            pf_check(first > 0 && first < n, "seam pattern escaped the text");
            raw.push_back({0, 0, first, 0, 0, occ, {}});
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
        // ---- rows: ONE sequential WM scan collects row(first) for every
        // pushed depth (the WM is the SA in row order; a value's row IS its
        // WM index - no search structure over it exists or is needed).
        std::unordered_map<uint64_t, uint64_t> rowOf;
        {
            std::unordered_set<uint64_t> pending;
            for (const auto& cl : raw) pending.insert(cl.first);
            constexpr uint64_t WB = 1 << 20;   // 8 MiB scan windows
            std::vector<uint64_t> buf(WB);
            uint64_t idx = 0;
            while (!pending.empty()) {
                pf_check(idx < n, "seam first suffix missing from the WM order");
                uint64_t want = std::min<uint64_t>(WB, n - idx);
                wm.read(idx, buf.data(), want);
                for (uint64_t k = 0; k < want && !pending.empty(); ++k) {
                    auto it = pending.find(buf[k]);
                    if (it != pending.end()) {
                        pf_check(rowOf.emplace(*it, idx + k).second, "duplicate WM value");
                        pending.erase(it);
                    }
                }
                idx += want;
            }
        }
        // lo = row(first) (v1's check(lo == row)); hi = lo + |occ_d|.
        for (auto& cl : raw) {
            uint64_t lo = rowOf.at(cl.first);
            uint64_t hi = lo + cl.occ.size();
            pf_check(hi <= n, "seam interval escapes the normalized rows");
            cl.lo = lo;
            cl.hi = hi;
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
        // ---- flanks + class SA sets, all single WM preads.
        // before = SA[lo-1] (v1's phi(first); the padded rows below w1 are
        // the padding suffixes SA[i] = n+i, so lo == 0 wraps to N-1).
        // after = SA[hi] (v1's phi-inverse walk; hi == n wraps to SA[0] = n).
        for (auto& cl : classes) {
            cl.before = cl.lo ? wm.at(cl.lo - 1) : N - 1;
            cl.after = cl.hi < n ? wm.at(cl.hi) : n;
            pf_check(cl.before < N && cl.after < N, "seam flank outside the padded text");
            std::vector<uint64_t> sa(cl.hi - cl.lo);
            if (!sa.empty())
                wm.read(cl.lo, sa.data(), sa.size());
            // Cross-check: WM row interval vs the corpus chain - two
            // independent sources for occ(P_d); divergence is fatal.
            std::vector<uint64_t> chainSet = cl.occ;
            std::sort(chainSet.begin(), chainSet.end());
            std::vector<uint64_t> sortedSet = sa;
            std::sort(sortedSet.begin(), sortedSet.end());
            pf_check(sortedSet == chainSet, "seam class SA set != the corpus occurrence set");
            for (uint64_t p : sa) pf_check(p < n, "seam sample outside T");
            pf_check(sa.front() == cl.first, "class leftmost row is not the terminal suffix");
            cl.occ = std::move(sa);   // row order = v1's phi-walk order
        }
        rebuild(n, r, limit, collection_lce, symbol);
    }
    void rebuild(uint64_t n, uint64_t r, uint64_t limit,
                 const std::function<uint64_t(uint64_t, uint64_t)>& collection_lce,
                 const std::function<uint8_t(uint64_t)>& symbol) {
        // rebuild each class: v1's LCE comparator + run rebuild, verbatim.
        uint64_t maxSeam = 0;
        for (auto& cl : classes) {
            std::vector<uint64_t> sa = cl.occ;
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
        if (getenv("SLIM_SEAM_DEBUG"))
            for (auto& cl : classes) {
                fprintf(stderr, "SLIM_SEAM_CLASS2 lo=%llu hi=%llu after=%llu runs=%zu:",
                        (unsigned long long)cl.lo, (unsigned long long)cl.hi,
                        (unsigned long long)cl.after, cl.runs.size());
                for (auto& v : cl.runs)
                    fprintf(stderr, " (%02x,%llu,h=%llu,t=%llu)", v.c,
                            (unsigned long long)v.len, (unsigned long long)v.head, (unsigned long long)v.tail);
                fprintf(stderr, "\n");
            }
        fprintf(stderr, "CYCLIC_SEAM_REPAIRED classes=%zu rows=%llu discovery_steps=%llu limit=%llu phi_samples=%llu pattern_chain=1 wm_scan=%d seam_spool=%d\n",
                classes.size(), (unsigned long long)total, (unsigned long long)depth,
                (unsigned long long)limit, (unsigned long long)r, int(!fromSpool), int(fromSpool));
        fprintf(stderr, "CYCLIC_SEAM_WORK max_seam_lce=%llu exact=1 pattern_chain=1 wm_scan=%d seam_spool=%d\n",
                (unsigned long long)maxSeam, int(!fromSpool), int(fromSpool));
    }
};

// Exercise the production spool reader with row orders unrelated to numeric
// position order, padded flanks, empty discovery and malformed records.
static void selftest_band_progress(const std::string& directory) {
    const std::string prefix=directory+"/progress-oracle", dir=prefix+".pieces";
    auto write=[&](const std::string& name,const char* magic,const std::vector<uint64_t>& words){
        int fd=::open((dir+"/"+name).c_str(),O_CREAT|O_TRUNC|O_WRONLY,0600);
        pf_check(fd>=0,"create progress oracle");pf_write_full(fd,magic,4,"oracle magic");
        pf_write_full(fd,words.data(),words.size()*8,"oracle words");::close(fd);
    };
    for(unsigned mode=0;mode<12;++mode){
        std::filesystem::remove_all(dir);std::filesystem::create_directories(dir);
        write("start","BPS1",{65540,17,3,3});
        std::vector<uint64_t> first{0,65536,1,0,0};
        std::vector<uint64_t> last{1,1,1,1,0,0,65535,65536,0,3,9,65549,4,9,3,0};
        std::vector<uint64_t> terminal{1,65537,3,1};
        if(mode==1)first[1]=0;
        if(mode==2)last.pop_back();
        if(mode==3)last[5]=1;
        if(mode==4)last[10]=65540;
        if(mode==5)last[15]=last[14];
        if(mode==6)terminal[2]=4;
        if(mode==7)terminal[0]=2;
        if(mode==8)last.push_back(0);
        if(mode==9)last[7]=3*65536;
        if(mode==10)first={0,65536,0,0};
        if(mode==11)last[7]=65537;
        write("g-0","BPG1",first);write("g-1","BPG1",last);write("terminal","BPT1",terminal);
        auto run=[&](){PieceSource src;src.prefix=prefix;src.pollTimeout=.1;src.load_idx();
            pf_check(src.streaming&&src.ready_band(0)&&src.ready_band(1)&&!src.ready_band(2),"band-only EOF readiness");
            pf_check(src.R==65537&&src.completed.size()==1&&src.capturedRows==3,"terminal captures");
            pf_check(src.classExtents.at(0)==1&&src.completed[0][8]==9,"band reader row order");};
        if(mode==0)run();
        else {pid_t child=::fork();pf_check(child>=0,"fork progress refusal");
            if(!child){int quiet=::open("/dev/null",O_WRONLY);::dup2(quiet,STDERR_FILENO);::close(quiet);run();_exit(0);}
            int status;pf_check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==2,"malformed progress accepted");}
    }
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    write("start","BPS1",{65536,17,1,1}); write("g-0","BPG1",{0,65536,0,0});
    PieceSource exact; exact.prefix=prefix; exact.pollTimeout=1; exact.load_idx();
    pf_check(exact.ready_band(0),"full final band readiness");
    std::thread delayed([&]{std::this_thread::sleep_for(std::chrono::milliseconds(30));
        write("terminal","BPT1",{0,65536,0,0});});
    pf_check(!exact.ready_band(1),"delayed terminal full-band EOF"); delayed.join();
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    write("start","BPS1",{65540,17,3,3});
    write("g-0","BPG1",{0,65536,1,0,0});
    write("g-1","BPG1",{1,1,1,1,0,0,65535,65536,0,3,9,65549,4,9,3,0});
    pid_t late = ::fork(); pf_check(late >= 0,"fork backward terminal refusal");
    if (!late) {
        int quiet=::open("/dev/null",O_WRONLY); ::dup2(quiet,STDERR_FILENO); ::close(quiet);
        PieceSource src; src.prefix=prefix; src.load_idx(); src.load_progress(); src.load_progress();
        write("terminal","BPT1",{0,65536,3,1}); src.read_terminal(); _exit(0);
    }
    int lateStatus; pf_check(::waitpid(late,&lateStatus,0)==late && WIFEXITED(lateStatus) &&
                            WEXITSTATUS(lateStatus)==2,"backward terminal accepted");
    std::filesystem::remove_all(dir);
    std::printf("BAND_PROGRESS_READER_ORACLE_PASS trivial_bases=1 band_only=1 deferred_class=1 EOF_clamp=1 terminal=1 delayed_full_band_EOF=1 backward_terminal_refused=1 malformed_refusals=12\n");
}

static int selftest_seam_spool(const std::string& directory) {
    std::filesystem::create_directories(directory);
    const std::string file = directory + "/reader-oracle.spool";
    const std::vector<uint8_t> text{0x1e, 0, 5, 5, 0, 0x1e, 9, 0, 5, 0x1e, 1, 0};
    const uint64_t n = text.size();
    auto lce = [&](uint64_t a, uint64_t b) { uint64_t d = 0; while (d < n && text[(a+d)%n] == text[(b+d)%n]) ++d; return d; };
    auto symbol = [&](uint64_t p) { return text[p]; };
    for (unsigned mode = 0; mode < 3; ++mode) {
        std::vector<SeamClass> expected;
        if (mode) expected.push_back({0, 3, 9, n+9, 4, {9, 3, 0}, {}});
        if (mode == 2) expected.push_back({6, 9, 8, 6, n, {8, 1, 7}, {}});
        std::vector<uint8_t> bytes{'S','C','S','1'};
        auto put = [&](uint64_t v) { for (unsigned j = 0; j < 8; ++j) bytes.push_back(uint8_t(v >> (8*j))); };
        put(n); put(3); put(expected.size());
        for (const auto& cl : expected) {
            put(cl.lo); put(cl.hi); put(cl.first); put(cl.before); put(cl.after);
            for (uint64_t p : cl.occ) put(p);
        }
        int fd = ::open(file.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
        pf_check(fd >= 0, "create reader oracle spool");
        pf_write_full(fd, bytes.data(), bytes.size(), "reader oracle bytes");
        SeamClasses actual; actual.build_spool(fd, n, 100, lce, symbol);
        pf_check(actual.classes.size() == expected.size(), "reader oracle class count");
        for (size_t i = 0; i < expected.size(); ++i) {
            auto& a = actual.classes[i]; auto& e = expected[i];
            pf_check(a.lo == e.lo && a.hi == e.hi && a.first == e.first &&
                     a.before == e.before && a.after == e.after && a.occ == e.occ,
                     "reader oracle adversarial row order");
            auto order = e.occ;
            std::sort(order.begin(), order.end(), [&](uint64_t x, uint64_t y) {
                uint64_t d = lce(x,y); return d == n ? x < y : symbol((x+d)%n) < symbol((y+d)%n);
            });
            std::vector<RawRun> reference;
            for (uint64_t position : order) {
                uint8_t c = symbol((position+n-1)%n);
                if (!reference.empty() && reference.back().c == c) { ++reference.back().len; reference.back().tail = position; }
                else reference.push_back({c,1,position,position});
            }
            pf_check(reference.size() == a.runs.size(), "reader oracle rebuilt count");
            for (size_t j = 0; j < reference.size(); ++j)
                pf_check(reference[j].c == a.runs[j].c && reference[j].len == a.runs[j].len &&
                         reference[j].head == a.runs[j].head && reference[j].tail == a.runs[j].tail,
                         "reader oracle rebuilt runs");
        }
        if (mode == 2) {
            for (unsigned bad = 0; bad < 3; ++bad) {
                pf_check(::ftruncate(fd, 0) == 0, "reset negative spool");
                pf_check(::lseek(fd, 0, SEEK_SET) == 0, "reset negative offset");
                auto malformed = bytes;
                if (bad == 0) malformed.pop_back();
                if (bad == 1) { for (unsigned j = 0; j < 8; ++j) malformed[20+j] = 0xff; }
                if (bad == 2) { for (unsigned j = 0; j < 8; ++j) malformed[76+j] = malformed[68+j]; }
                pf_write_full(fd, malformed.data(), malformed.size(), "negative spool");
                pid_t child = ::fork(); pf_check(child >= 0, "fork reader refusal oracle");
                if (!child) {
                    int quiet = ::open("/dev/null", O_WRONLY); ::dup2(quiet, STDERR_FILENO); ::close(quiet);
                    SeamClasses rejected; rejected.build_spool(fd, n, 100, lce, symbol); _exit(0);
                }
                int status; pf_check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 2,
                                     "reader oracle failed to reject malformed spool");
            }
        }
        ::close(fd);
    }
    ::unlink(file.c_str());
    selftest_band_progress(directory);
    std::printf("SEAM_SPOOL_READER_ORACLE_PASS empty=1 adversarial_orders=2 padded_flanks=1 NUL=1 byte05=1 malformed_refusals=3\n");
    return 0;
}

// ---------------------------------------------------------- repair stream
// sol6b dependency audit: repair is NOT intrinsically a corpus-wide serial
// recurrence. rows/row are additive prefix coordinates; next is a cursor in
// sorted, disjoint seam intervals (a chunk may binary-search its entry class).
// srcPending and pending are the two boundary coalescers, with head from the
// left and tail from the right. A seam interval crossing a chunk must retain
// its entry/exit flank and insert cl.runs exactly once at cl.lo. Counts and
// character totals reduce additively after boundary merges. Dollar/padding
// guards need the absolute raw-row prefix and the preceding terminal count.
// Thus independent normalization needs BOTH pending runs and prefix metadata,
// not merely independently invoking feed_raw with a fresh RepairStream.
// In streaming mode class readiness is still required before local repair;
// ready_band's bounded extent/terminal assertion supplies that dependency.
// The default stays serial. The opt-in exact-checkpoint scheduler below
// validates the carry but retains a serial planner; it is not a speed claim.
// `rk` is the run's BWT RANK BASE, i.e. the transducer's own ROW index for the
// run's first row (row 0 == the first non-padding BWT row). It is filled by
// append()/emitOne() and is the ONLY exact rank source in the finish: a running
// sum of the run lengths is NOT exact, because the seam-class splice can advance
// `row` past the rows its reconstructed runs describe (measured at 100 MB: the
// first class block ends one row short, and a cumulative sum silently shifts
// EVERY subsequent rank by one). The KASAI arm indexes the LCP column with this.
struct NRun { uint8_t c; uint64_t len, h, t; uint64_t rk = 0; };
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
    // PER-UNIT (finish-decomposed): the sink is a template parameter on the hot
    // path. `emit` is a std::function, i.e. an indirect call that cannot inline
    // the run's bookkeeping into the transducer; the band loop passes its sink
    // directly, so emitOne/append/seamSplice/feed_raw fold into one specialised
    // straight-line body per call site. The std::function overloads survive for
    // the replay, the chunk planner and the selftests, and are byte-for-byte the
    // same semantics (they pass `emit` as the sink).
    // `base` is the BWT rank of the piece's first row. A new pending run inherits
    // it; a coalesced continuation keeps the run's original base (the rows are
    // contiguous, so the merged run's base is the first piece's).
    template<class Sink> void appendAt(const NRun& v, uint64_t base, Sink& sink) {
        if (!v.len) return;
        if (have && pending.c == v.c) { pending.len += v.len; pending.t = v.t; }
        else { if (have) emitOne(sink); pending = v; pending.rk = base; have = true; }
    }
    template<class Sink> void append(const NRun& v, Sink& sink) { appendAt(v, row, sink); }
    template<class Sink> void emitOne(Sink& sink) {
        ++rNorm; nNorm += pending.len;
        totals[pending.c] += pending.len;
        ++runFreq[pending.c];
        sink(pending);
    }
    void append(const NRun& v) { append(v, emit); }
    void emitOne() { emitOne(emit); }
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
    void feed_raw(const RawRun& raw) { feed_raw(raw, emit); }
    template<class Sink> void feed_raw(const RawRun& raw, Sink& sink) {
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
        // Alphabet invariant: the production remap vacates 0x01-0x04 as control
        // markers (permuted with 0xfc-0xff), so remapped TEXT never contains
        // them; c==2 (the SA-zero dollar row) was rewritten to the terminal
        // above. 0x00 and 0x05 ARE legal text (real pile bytes; measured 18
        // and 23 occurrences at 10GB). Forbid exactly the vacated range.
        pf_check(v.c != 1 && v.c != 2 && v.c != 3 && v.c != 4, "unsupported text alphabet");
        if (srcHave && srcPending.c == v.c) { srcPending.len += v.len; srcPending.tail = v.tail; }
        else {
            if (srcHave) seamSplice({srcPending.c, srcPending.len, srcPending.head, srcPending.tail}, sink);
            srcPending = v; srcHave = true;
        }
    }
    void seamSplice(NRun v) { seamSplice(v, emit); }
    template<class Sink> void seamSplice(NRun v, Sink& sink) {
        uint64_t end = row + v.len;
        while (row < end) {
            if (seam && next < seam->classes.size() && row >= seam->classes[next].lo) {
                SeamClass& cl = seam->classes[next];
                if (row == cl.lo) {
                    // The class's reconstructed runs are contiguous from the
                    // class's first row: stamp each piece's own rank base.
                    uint64_t base = row;
                    for (const auto& piece : cl.runs) {
                        appendAt({piece.c, piece.len, piece.head, piece.tail}, base, sink);
                        base += piece.len;
                    }
                }
                row = std::min(end, cl.hi);
                if (row == cl.hi) { v.h = cl.after; ++next; }
            } else {
                uint64_t stop = (seam && next < seam->classes.size()) ? std::min(end, seam->classes[next].lo) : end;
                NRun piece{v.c, stop - row, v.h, v.t};
                if (stop < end && seam) piece.t = seam->classes[next].before;
                append(piece, sink);
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
    // PILE-WEEK (pile-week-v1): the per-char-change threshold scan over all
    // 256 rr_len entries was the serial advance machine's dominant cost
    // (~368M runs x 256 signed compares at 1GB; measured 83.6s clean at 96t,
    // 139s on the contaminated convergence run). Same emitted SET, computed
    // by an AVX2 compare-mask over the 256-entry column plus a bit-scan
    // action loop (the reset condition `m3 < rr_len[cc]` and the updates are
    // UNCHANGED; signed compares match scalar int64 exactly; the mask path
    // is checked by the standing byte-identity gates). Scalar fallback when
    // AVX2 is absent. rr_len stays int64 - no semantic change at all.
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
    // mask bit i of word w <=> (m3 < rr_len[64*w + i]) - 4 x u64 = 256 bits.
    __attribute__((target("avx2")))
    static void gt_masks(const int64_t* rr, int64_t m3, uint64_t* out4) {
        const __m256i t = _mm256_set1_epi64x(m3);
        for (int w = 0; w < 4; ++w) {
            uint64_t acc = 0;
            for (int q = 0; q < 16; ++q) {
                __m256i x = _mm256_loadu_si256((const __m256i*)(rr + 64 * w + 4 * q));
                // movemask_pd: one bit per 64-bit lane (the cmp result lanes
                // are 0 / all-ones; movemask_ps would yield TWO bits per lane)
                uint32_t mk = uint32_t(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(x, t))));
                acc |= uint64_t(mk) << (4 * q);
            }
            out4[w] = acc;
        }
    }
    static void gt_masks_scalar(const int64_t* rr, int64_t m3, uint64_t* out4) {
        for (int w = 0; w < 4; ++w) {
            uint64_t acc = 0;
            for (int i = 0; i < 64; ++i)
                if (m3 < rr[64 * w + i]) acc |= uint64_t(1) << i;
            out4[w] = acc;
        }
    }
    static bool avx2_ok() {
        static const bool v = __builtin_cpu_supports("avx2") != 0;
        return v;
    }
    // Emit + reset every cc with rr_len[cc] > m3 (bit index = cc; skip cc 0:
    // the scalar loop starts at 1).
    void threshold_reset(int64_t m3, uint64_t* scratch) {
        if (avx2_ok()) gt_masks(rr_len, m3, scratch);
        else gt_masks_scalar(rr_len, m3, scratch);
        scratch[0] &= ~1ull;   // cc 0 is never in the machine
        for (int w = 0; w < 4; ++w) {
            uint64_t bits = scratch[w];
            while (bits) {
                int cc = w * 64 + ctz64(bits);
                bits &= bits - 1;
                if (rr_act[cc]) emit(rr_pos[cc]);
                rr_len[cc] = m3; rr_pos[cc] = 0; rr_act[cc] = false;
            }
        }
    }
    static unsigned ctz64(uint64_t v) { return unsigned(__builtin_ctzll(v)); }
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
            alignas(32) uint64_t scratch[4];
            threshold_reset(m3, scratch);
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
        // final flush: m3 = -1 (emit every active witness); the scalar rule
        // is `int64_t(-1) < rr_len[cc]` with the act flag checked - the
        // threshold_reset at m3=-1 emits exactly the act set.
        alignas(32) uint64_t scratch[4];
        threshold_reset(-1, scratch);
    }
};

// A cold terminal state is not a chunk transducer: it forgets when an
// incoming requirement would have fired. Keep this executable counterexample
// beside the production machine; do not replace the sweep with a fold of
// cold chunk states. The safe associative object is the state TRANSFORMATION
// (including ordered output), not Sweep's terminal rr_* columns.
static int selftest_sweep_monoid() {
    auto same = [](const Sweep& a, const Sweep& b) {
        return a.n == b.n && a.bigN == b.bigN && a.chi == b.chi &&
            a.previous_interior == b.previous_interior && a.m == b.m &&
            a.p == b.p && a.p_sa_last == b.p_sa_last && a.first == b.first &&
            std::equal(a.rr_len, a.rr_len + 256, b.rr_len) &&
            std::equal(a.rr_pos, a.rr_pos + 256, b.rr_pos) &&
            std::equal(a.rr_act, a.rr_act + 256, b.rr_act);
    };
    const std::array<NRun, 3> chunk{{{5,2,9,10}, {6,1,11,11}, {5,1,12,12}}};
    auto chunkRun = [&](Sweep& s, uint64_t firstInterior) {
        s.advance(chunk[0], 1, firstInterior);
        s.advance(chunk[1], 1, INF);
        s.advance(chunk[2], 0, INF);
    };
    std::vector<uint64_t> coldA, coldB, warmA, warmB;
    Sweep a(100), b(100);
    a.emitWitness = [&](uint64_t x) { coldA.push_back(x); };
    b.emitWitness = [&](uint64_t x) { coldB.push_back(x); };
    chunkRun(a, 0); chunkRun(b, 1);
    pf_check(same(a,b) && coldA == coldB, "cold chunk collision");
    Sweep c(100), d(100);
    c.emitWitness = [&](uint64_t x) { warmA.push_back(x); };
    d.emitWitness = [&](uint64_t x) { warmB.push_back(x); };
    for (Sweep* s : {&c, &d}) {
        s->advance({7,1,20,20}, 0, INF);
        s->advance({8,1,21,21}, 1, INF);
    }
    chunkRun(c, 0); chunkRun(d, 1);
    pf_check(coldA == std::vector<uint64_t>({90,89}) &&
             warmA == std::vector<uint64_t>({91,80,79,90,89}) &&
             warmB == std::vector<uint64_t>({91,89,80,79}), "warm chunk counterexample");
    std::printf("SWEEP_COLD_SUMMARY_COUNTEREXAMPLE cold=[90,89] warm_A=[91,80,79,90,89] warm_B=[91,89,80,79] incoming_prefix=7:20,8:21 chunk=5:9-10,6:11,5:12 top=1,1,0 interior_A=0,INF,INF interior_B=1,INF,INF\n");

    // Verify the exact state-carry/replay law at every split. This is the
    // baseline any bounded summary must reproduce, NOT a parallel speedup.
    uint64_t rng = 0x6b20261014ull, cases = 0;
    auto draw = [&]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    for (unsigned trial = 0; trial < 2048; ++trial) {
        size_t count = trial % 33;
        std::vector<NRun> runs;
        std::vector<uint64_t> top, interior;
        const uint8_t alphabet[] = {0,5,6,7,255};
        for (size_t i = 0; i < count; ++i) {
            uint8_t ch = alphabet[draw()%5];
            uint64_t len = 1 + draw()%3, h = draw()%100, t = len == 1 ? h : draw()%100;
            runs.push_back({ch,len,h,t}); top.push_back(draw()%8);
            interior.push_back(len == 1 ? INF : draw()%8);
        }
        std::vector<uint64_t> expected;
        Sweep reference(100);
        reference.emitWitness = [&](uint64_t x) { expected.push_back(x); };
        for (size_t i = 0; i < count; ++i) reference.advance(runs[i],top[i],interior[i]);
        reference.finish();
        for (size_t cut = 0; cut <= count; ++cut) {
            std::vector<uint64_t> actual;
            Sweep left(100);
            left.emitWitness = [&](uint64_t x) { actual.push_back(x); };
            for (size_t i = 0; i < cut; ++i) left.advance(runs[i],top[i],interior[i]);
            Sweep right = left; // full entry state, no intermediate flush
            for (size_t i = cut; i < count; ++i) right.advance(runs[i],top[i],interior[i]);
            right.finish();
            pf_check(actual == expected && same(right,reference), "state carry replay law");
            ++cases;
        }
    }
    std::printf("SWEEP_STATE_CARRY_ORACLE_PASS streams=2048 splits=%llu NUL=1 byte05=1 singleton=1 periodic_colors=1 empty=1 ordered_witnesses=1 final_state=1\n", (unsigned long long)cases);
    return 0;
}

// ----------------------------------------------------------- witness buckets
// VALUE-RANGE buckets: the sweep appends each witness to its bucket's RAM
// buffer; when the global RAM budget is exceeded the largest bucket is
// sorted and appended to its disc file as a delta-varint run. The EF pass
// walks buckets in value order (ranges are disjoint), merging each
// bucket's compressed runs with a bounded 64-way heap.
struct WitBuckets {
    uint64_t P = 0, n = 0;
    std::string prefix;
    uint64_t ramBudget = 1ull << 30;
    std::vector<std::vector<uint64_t>> buf;
    std::vector<int> fds;
    struct Run { uint64_t offset, bytes; uint32_t count; };
    std::vector<std::vector<Run>> runs;
    uint64_t totalRam = 0, totalValues = 0, spilled = 0;
    // CACHE HYGIENE (operator): every witness spool chunk is scratch whose bytes
    // are dead the moment they have been merged into the visit order. Released
    // page by page (DONTNEED) at the one place that knows the chunk is finished,
    // so a spilled witness stream never squats on the corpus's page cache.
    uint64_t cacheReleasedBytes = 0, cacheReleaseCalls = 0;
    void releaseChunk(int fd, const std::string& path) {
        if (fd < 0 || !pf_hygiene_on()) return;
        struct stat st {};
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            if (::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0) {
                cacheReleasedBytes += uint64_t(st.st_size);
                ++cacheReleaseCalls;
            }
        }
        (void)path;
    }
    WitBuckets(uint64_t nNorm, uint64_t runs, const std::string& pfx, uint64_t budget)
        : n(nNorm), prefix(pfx), ramBudget(budget) {
        P = 4096;
        while (P < 65536 && P * 262144 < runs) P *= 2;
        buf.resize(P);
        this->runs.resize(P);
        fds.assign(P, -1);
    }
    uint64_t bucketOf(uint64_t v) const { return v * P / n; }
    void add(uint64_t v) {
        pf_check(v < n, "witness out of range");
        uint64_t b = bucketOf(v);
        size_t oldCapacity = buf[b].capacity();
        buf[b].push_back(v);
        totalRam += 8 * (buf[b].capacity() - oldCapacity);
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
        spillBucket(best);
    }
    void spillBucket(uint64_t best) {
        if (buf[best].empty()) return;
        std::vector<uint64_t>& v = buf[best];
        std::sort(v.begin(), v.end());
        std::vector<uint8_t> out;
        out.reserve(v.size() + 16);
        pf_check(v.size() <= UINT32_MAX, "witness spill run count overflow");
        uint32_t cnt = uint32_t(v.size());
        out.insert(out.end(), (const char*)&cnt, (const char*)&cnt + 4);
        putVarint(out, v[0]);
        for (size_t i = 1; i < v.size(); ++i) putVarint(out, v[i] - v[i - 1]);
        if (fds[best] < 0) {
            std::string name = prefix + ".wit-" + std::to_string(best);
            fds[best] = ::open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0666);
            pf_check(fds[best] >= 0, "open witness bucket");
        }
        off_t end = lseek(fds[best], 0, SEEK_END);
        pf_check(end >= 0, "seek witness bucket");
        uint64_t offset = uint64_t(end);
        runs[best].push_back({offset + 4, out.size() - 4, cnt});
        pf_write_full(fds[best], out.data(), out.size(), "witness bucket");
        totalRam -= 8 * v.capacity();
        std::vector<uint64_t>().swap(v);
        ++spilled;
    }
    void presort_parallel(int) {
        // Spill the remaining bounded RAM before opening the merge cursors.
        for (uint64_t b = 0; b < P; ++b) spillBucket(b);
    }
    template <typename Visit>
    void for_each_sorted(Visit&& visit) {
        struct Cursor {
            Run run; uint64_t pos = 0, value = 0;
            uint32_t left; std::array<uint8_t, 4096> data;
            size_t at = 0, have = 0;
            uint8_t byte(int fd) {
                if (at == have) {
                    have = size_t(std::min<uint64_t>(data.size(), run.bytes - pos));
                    pf_check(have != 0, "witness varint truncated");
                    pf_pread_full(fd, data.data(), have, run.offset + pos, "witness run");
                    pos += have; at = 0;
                }
                return data[at++];
            }
            void advance(int fd) {
                uint64_t delta = 0; unsigned shift = 0;
                for (;;) {
                    uint8_t x = byte(fd);
                    pf_check(shift < 64 && (shift != 63 || (x & 127) <= 1), "witness varint overflow");
                    delta |= uint64_t(x & 127) << shift;
                    if (!(x & 128)) break;
                    shift += 7;
                }
                pf_check(delta <= INF - value, "witness delta overflow");
                value += delta; --left;
            }
        };
        using Item = std::pair<uint64_t, size_t>;
        auto merge = [&](int fd, const std::vector<Run>& input, size_t begin, size_t end, auto emit) {
            std::vector<Cursor> cursors;
            cursors.reserve(end - begin);
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
            for (size_t i = begin; i < end; ++i) {
                auto run = input[i];
                pf_check(run.count != 0, "empty witness run");
                cursors.push_back(Cursor{run, 0, 0, run.count, {}});
                cursors.back().advance(fd);
                heap.push({cursors.back().value, cursors.size() - 1});
            }
            while (!heap.empty()) {
                auto [value, index] = heap.top(); heap.pop();
                emit(value);
                auto& c = cursors[index];
                if (c.left) { c.advance(fd); heap.push({c.value, index}); }
            }
        };
        constexpr size_t FANIN = 64;
        for (uint64_t b = 0; b < P; ++b) {
            if (runs[b].empty()) continue;
            std::string name = prefix + ".wit-" + std::to_string(b);
            int fd = ::open(name.c_str(), O_RDONLY);
            pf_check(fd >= 0, "open witness merge");
            // Reduce oversized heaps in bounded passes, retaining delta-varint scratch.
            while (runs[b].size() > FANIN) {
                std::string nextName = name + ".merge";
                int out = ::open(nextName.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
                pf_check(out >= 0, "create witness merge pass");
                std::vector<Run> next;
                uint64_t offset = 0;
                for (size_t begin = 0; begin < runs[b].size(); begin += FANIN) {
                    size_t end = std::min(runs[b].size(), begin + FANIN);
                    uint64_t count = 0;
                    for (size_t j = begin; j < end; ++j) count += runs[b][j].count;
                    pf_check(count <= UINT32_MAX, "witness merged run count overflow");
                    uint32_t cnt = uint32_t(count);
                    pf_write_full(out, &cnt, 4, "witness merge count");
                    uint64_t bytes = 0, prev = 0;
                    std::vector<uint8_t> encoded;
                    encoded.reserve(65536);
                    auto flush = [&] {
                        pf_write_full(out, encoded.data(), encoded.size(), "witness merge pass");
                        bytes += encoded.size(); encoded.clear();
                    };
                    merge(fd, runs[b], begin, end, [&](uint64_t value) {
                        pf_check(value >= prev, "witness merge order");
                        putVarint(encoded, value - prev); prev = value;
                        if (encoded.size() >= 65536) flush();
                    });
                    flush();
                    next.push_back({offset + 4, bytes, cnt}); offset += 4 + bytes;
                }
                pf_check(::close(out) == 0, "close witness merge pass");
                releaseChunk(fd, name);
                ::close(fd); ::close(fds[b]);
                pf_check(::unlink(name.c_str()) == 0, "delete consumed witness pass");
                pf_check(::rename(nextName.c_str(), name.c_str()) == 0, "replace witness pass");
                fds[b] = ::open(name.c_str(), O_WRONLY);
                pf_check(fds[b] >= 0, "reopen witness scratch accounting");
                fd = ::open(name.c_str(), O_RDONLY);
                pf_check(fd >= 0, "reopen witness merge pass");
                runs[b] = std::move(next);
            }
            merge(fd, runs[b], 0, runs[b].size(), visit);
            releaseChunk(fd, name);
            ::close(fd);
            // This bucket's values will never be visited again.
            ::close(fds[b]); fds[b] = -1;
            pf_check(::unlink(name.c_str()) == 0, "delete consumed witness bucket");
            std::vector<Run>().swap(runs[b]);
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
// PILE-WEEK (pile-week-v1): the old writer cost ~6 ops + a push_back branch
// PER BIT (the m5 EF is ~1e9 bits and m1 another ~5e9 at 1GB: ~50s of the
// window budget). Same byte stream, word-accumulated: bits enter a 128-bit
// accumulator LSB-first and whole bytes flush out in order; msb() packs the
// bit-reversed value (identical bit sequence), and runs of zero bits (the
// EF high stream's gaps, gamma's prefixes) flush in bulk. The container's
// byte-identity gates are the arbiter.
struct BitsOut {
    std::string path;
    int fd = -1;
    std::vector<uint8_t> mem;
    uint64_t byteCount = 0;
    explicit BitsOut(const std::string& path_) : path(path_) {
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        pf_check(fd >= 0, "create bit spool");
        mem.reserve(1 << 20);
    }
    ~BitsOut() { if (fd >= 0) ::close(fd); ::unlink(path.c_str()); }
    void drain() {
        pf_write_full(fd, mem.data(), mem.size(), "bit spool");
        byteCount += mem.size(); mem.clear();
    }
    void byte(uint8_t v) {
        mem.push_back(v);
        if (mem.size() == (1 << 20)) drain();
    }
    template<class Container> void copyTo(Container& out) {
        drain();
        std::array<uint8_t, 1 << 20> block;
        for (uint64_t pos = 0; pos < byteCount;) {
            size_t z = size_t(std::min<uint64_t>(block.size(), byteCount - pos));
            pf_pread_full(fd, block.data(), z, pos, "bit spool");
            out.write(block.data(), z); pos += z;
        }
    }
    uint64_t bitPos = 0;
    unsigned __int128 acc = 0;
    unsigned nbits = 0;
    void flush8() {
        while (nbits >= 8) {
            byte(uint8_t(acc));
            acc >>= 8;
            nbits -= 8;
        }
    }
    void bit(unsigned b) {
        acc |= (__uint128_t)(uint64_t(b & 1)) << nbits;
        ++nbits;
        ++bitPos;
        if (nbits >= 8) flush8();
    }
    void putzeros(uint64_t k) {
        if (!k) return;
        unsigned take = unsigned(std::min<uint64_t>(k, 8 - nbits));
        nbits += take; bitPos += take; k -= take;   // zeros are implicit in acc
        if (nbits >= 8) flush8();
        while (k >= 8) { byte(0); bitPos += 8; k -= 8; }
        if (k) { nbits += unsigned(k); bitPos += k; }
    }
    static uint64_t revbits(uint64_t v, unsigned w) {
        uint64_t r = 0;
        for (unsigned i = 0; i < w; ++i) r |= ((v >> i) & 1) << (w - 1 - i);
        return r;
    }
    void msb(uint64_t v, unsigned w) {
        if (!w) return;
        pf_check(w <= 64, "msb width");
        acc |= (__uint128_t)(revbits(v, w)) << nbits;
        nbits += w; bitPos += w;
        flush8();
    }
    void lsb(uint64_t v, unsigned w) {
        if (!w) return;
        pf_check(w <= 64, "lsb width");
        uint64_t m = w == 64 ? ~0ull : ((uint64_t(1) << w) - 1);
        acc |= (__uint128_t)(v & m) << nbits;
        nbits += w; bitPos += w;
        flush8();
    }
    void gamma(uint64_t v) {
        pf_check(v > 0, "zero gamma");
        unsigned w = 64 - __builtin_clzll(v);
        putzeros(w - 1);
        msb(v, w);
    }
    // The v1 writer's byte count is ALWAYS ceil(bitPos/8) (the group byte is
    // created at its first bit, even if the stream ends mid-group): flush
    // the pending partial byte before the stream is consumed.
    void pad() {
        if (nbits) {
            byte(uint8_t(acc));
            acc = 0;
            nbits = 0;
        }
    }
    size_t bytes() const { return byteCount + mem.size(); }
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

// One pool for all bands; dispatch returns only after every worker flushes its counters.
// ------------------------------------------------- PART 2: THE PREFETCHER
// One background thread that populates the corpus adapter's page tables for the
// NEXT band's verification targets while the serial fold walks the current one.
// The serial fold IS the clock: it is cheap (29 ns/raw run) and strictly
// ordered, so "submit the ranges right after read_band(k+1), before the fold"
// gives the prefetcher the whole fold plus the tail of the pool's previous
// verification to finish the population, and the fan-out then starts against
// warm page tables.
//
// Pipelining: read -> feed(SERIAL) -> prefetch(NEXT band) -> LCE(PARALLEL, warm)
// -> advance. The prefetcher OWNS no state of the fold and touches only the
// immutable corpus, so it cannot reorder anything; it is a pure read-ahead.
//
// ------------------------------------------- PART B: THE READ DISCIPLINE
// The operator's design, and the reason this is not a loop of preads:
//
//   * SCATTERED. Every one of a band's verification targets is a ~256 B window
//     at an arbitrary corpus offset. Issued one by one (in arrival order) that
//     is a scattered 4 KB device read per window - measured at ~1-3 GB/s on this
//     box, and worse once the queue depth is the prefetcher's own one-thread
//     loop.
//   * SWEEP. Sort the batch, COALESCE windows whose gap is under PILE_PREFETCH_GAP
//     into merged reads, and issue those in ONE ASCENDING MONOTONE FRONT, split
//     into skip-chunked pieces (a merged read longer than `chunkBytes` is issued
//     as consecutive chunks, so a long dense run cannot block the front behind a
//     single enormous madvise). Sequential read of the same bytes measures
//     5.24 GB/s - the 2-4x the thesis rests on.
//
// The batch statistics are counted, not assumed: `raw_windows` (targets in),
// `merged_reads` (reads issued), `coalesce_ratio`, `avg_read_bytes`, the fronts
// (disjoint ascending clusters = monotone sweeps), `pages_populated`, and the
// TOUCHED MASS (distinct bytes the batch asked for, as a fraction of the corpus)
// - the sweep's true cost is one pass over that mass.
//
// Bounded: the queue holds at most JOBS jobs of at most `budgetBytes` each
// (default 32 MiB), so the prefetcher's footprint is that budget in RANGES (the
// pages it populates belong to the corpus mapping, which the verification reads
// anyway). When the queue is full the submit is DROPPED and counted (`skipped`),
// never queued: the verification's demand path still reads the bytes, it just
// pays the faults itself. `depth` (JOBS) is the pipeline-depth knob of the
// balance curve: how many bands of targets may be in flight at once.
//
// MODES (`XSA_FINISH_PREFETCH`, product default 0 = the lazy arm):
//   0 / off        - no read-ahead at all (the demand path's faults).
//   1 / sweep      - sort + coalesce + one monotone front (the thesis).
//   2 / scattered  - the same targets issued in ARRIVAL order, no sort, no
//                    coalesce: the "batch-scattered" arm of the three-way.
// `XSA_FINISH_PREFETCH_ASSIST` selects what the front does with each read:
//   populate (default) - MADV_POPULATE_READ (blocking, one front, one thread)
//   willneed           - POSIX_FADV_WILLNEED (non-blocking, the kernel queues)
//   both               - WILLNEED first, then POPULATE (assist + guarantee)
struct PfPrefetcher {
    struct Stats {
        std::atomic<uint64_t> jobs{0}, rawWindows{0}, mergedReads{0}, reads{0},
            readBytes{0}, fronts{0}, pagesPopulated{0}, skipped{0}, droppedBytes{0},
            touchedBytes{0}, fetchedBytes{0}, skipResidentReads{0}, skipResidentBytes{0},
            residencyUnknown{0};
        std::atomic<uint64_t> busyUsec{0}, willneedUsec{0}, mincoreUsec{0};
    } st;
    // Joins/back-pressure: JOBS = how many submits may be in flight (the
    // pipeline depth knob), chunkBytes = the largest single issued read.
    unsigned JOBS = 2;
    const CorpusText* corpus = nullptr;
    std::mutex mtx;
    std::condition_variable wake, idle;
    // The queue holds JOBS *batches of RAW target windows*. ALL the coalescing work
    // - the sort, the merge, the chunking - happens on the prefetch thread, never
    // on the fold thread: measured, sorting 262k window pairs per band on the fold
    // thread cost 84-140 s of serial time on the 1 GB fixture and turned a
    // break-even sweep into a 43% REGRESSION (lazy 137.5 s vs sweep 196.6 s, rep 1).
    // The fold thread's whole cost is now two pushes per run.
    std::vector<std::vector<std::pair<uint64_t, uint64_t>>> qjobs;
    bool have = false, stop = false;
    unsigned inflight = 0;                          // jobs submitted, not done
    std::thread th;
    uint64_t budgetBytes = 32u << 20;
    uint64_t gap = 1u << 16;
    uint64_t windowBytes = 256;
    uint64_t chunkBytes = 4u << 20;
    int mode = 1;                                   // 1 sweep, 2 scattered
    int assist = 0;                                 // 0 populate, 1 willneed, 2 both
    // Skip what is already resident (sweep mode only): the read-ahead becomes
    // idempotent across passes, so its device cost is the corpus's COLD mass, not
    // its touched mass.
    bool skipResident = false;
    uint64_t corpusBytes = 0;
    explicit PfPrefetcher(const CorpusText* c) : corpus(c) {
        corpusBytes = c ? c->size() : 0;
        th = std::thread([this] {
            std::unique_lock<std::mutex> lk(mtx);
            for (;;) {
                wake.wait(lk, [&] { return stop || have; });
                if (stop && !have) return;
                std::vector<std::vector<std::pair<uint64_t, uint64_t>>> jobs = std::move(qjobs);
                qjobs.clear();
                have = false;
                lk.unlock();
                const double t0 = tnow();
                double wil = 0, minc = 0;
                std::vector<unsigned char> res;
                for (auto& raw : jobs) {
                    // ---- PART B: coalesce the batch into a monotone front ----
                    std::vector<std::pair<uint64_t, uint64_t>> m;   // merged, ascending
                    if (mode == 2) {
                        m = std::move(raw);                         // arrival order, no sort
                    } else {
                        std::sort(raw.begin(), raw.end());
                        uint64_t merged = 0;
                        for (const auto& x : raw) {
                            if (!m.empty() && x.first <= m.back().first + m.back().second + gap) {
                                const uint64_t end = std::max(m.back().first + m.back().second,
                                                              x.first + x.second);
                                merged += (end - m.back().first) - m.back().second;
                                m.back().second = end - m.back().first;
                            } else {
                                m.push_back(x);
                                merged += x.second;
                            }
                            // The budget bounds the MERGED bytes (the bytes actually
                            // read); the raw windows are only the request. The rest
                            // of the band keeps its demand faults.
                            if (merged >= budgetBytes) break;
                        }
                        raw.clear();
                    }
                    // Skip-chunked monotone front: split every merged read into
                    // chunks of at most chunkBytes, preserving ascending order, and
                    // count the fronts (a new front starts wherever the next chunk
                    // is not contiguous with the previous one, i.e. where the sweep
                    // must skip).
                    std::vector<std::pair<uint64_t, uint64_t>> job;
                    job.reserve(m.size() * 2);
                    uint64_t lastEnd = INF, touched = 0;
                    for (const auto& x : m) {
                        uint64_t off = x.first, left = x.second;
                        uint64_t chunk = mode == 2 ? left : std::min<uint64_t>(left, chunkBytes);
                        while (left) {
                            const uint64_t take = std::min<uint64_t>(chunk, left);
                            if (off != lastEnd) st.fronts.fetch_add(1, std::memory_order_relaxed);
                            job.emplace_back(off, take);
                            touched += take;
                            lastEnd = off + take;
                            off += take; left -= take;
                            chunk = std::min<uint64_t>(left, chunkBytes);
                        }
                    }
                    st.mergedReads.fetch_add(job.size(), std::memory_order_relaxed);
                    st.touchedBytes.fetch_add(touched, std::memory_order_relaxed);
                    for (const auto& r : job) {
                    // PART B: the sweep skips the pages the kernel already has. A
                    // band's targets largely repeat the previous band's (the SA is
                    // walked in order), so without this the front re-reads the
                    // corpus once per band. The split keeps the front MONOTONE:
                    // the non-resident runs are issued in ascending order.
                    uint64_t issued = 0, skippedBytes = 0;
                    bool asked = false;
                    if (skipResident) {
                        const double tm = tnow();
                        const bool known = corpus->page_residency(r.first, r.second, res);
                        minc += tnow() - tm;
                        asked = known;
                        if (!known) st.residencyUnknown.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (asked && !res.empty()) {
                        const uint64_t page = 4096;
                        const uint64_t base = r.first & ~(page - 1);
                        size_t i = 0;
                        while (i < res.size()) {
                            if (res[i]) { ++i; continue; }
                            size_t j = i;
                            while (j < res.size() && !res[j]) ++j;
                            const uint64_t a = base + uint64_t(i) * page;
                            const uint64_t b = std::min<uint64_t>(base + uint64_t(j) * page,
                                                                 corpusBytes);
                            if (b > a) {
                                if (assist == 1 || assist == 2) {
                                    const double tw = tnow();
                                    corpus->willneed_range(a, b - a);
                                    wil += tnow() - tw;
                                }
                                if (assist == 0 || assist == 2) {
                                    corpus->prefetch_range(a, b - a);
                                    st.pagesPopulated.fetch_add((b - a) / page, std::memory_order_relaxed);
                                }
                                issued += b - a;
                            }
                            i = j;
                        }
                        skippedBytes = r.second - std::min<uint64_t>(r.second, issued);
                        if (!issued) {
                            st.skipResidentReads.fetch_add(1, std::memory_order_relaxed);
                            st.skipResidentBytes.fetch_add(r.second, std::memory_order_relaxed);
                        }
                    } else {
                        if (assist == 1 || assist == 2) {
                            const double tw = tnow();
                            corpus->willneed_range(r.first, r.second);
                            wil += tnow() - tw;
                        }
                        if (assist == 0 || assist == 2) {
                            const uint64_t page = 4096;
                            const uint64_t lo = r.first & ~(page - 1);
                            const uint64_t hi = std::min<uint64_t>((r.first + r.second + page - 1) & ~(page - 1), corpusBytes);
                            if (hi > lo) {
                                corpus->prefetch_range(lo, hi - lo);
                                st.pagesPopulated.fetch_add((hi - lo) / page, std::memory_order_relaxed);
                            }
                        }
                        issued = r.second;
                    }
                    (void)skippedBytes;
                    st.reads.fetch_add(1, std::memory_order_relaxed);
                    st.readBytes.fetch_add(r.second, std::memory_order_relaxed);
                    st.fetchedBytes.fetch_add(issued, std::memory_order_relaxed);
                    }
                }
                st.busyUsec.fetch_add(uint64_t(1e6 * (tnow() - t0)), std::memory_order_relaxed);
                st.willneedUsec.fetch_add(uint64_t(1e6 * wil), std::memory_order_relaxed);
                st.mincoreUsec.fetch_add(uint64_t(1e6 * minc), std::memory_order_relaxed);
                lk.lock();
                // inflight counts SUBMITTED jobs; one drain cycle may carry several.
                inflight -= unsigned(jobs.size());
                idle.notify_all();
            }
        });
    }
    // Build the band's target list: each run's head and tail SA coordinate, one
    // window each. This is the ONLY work the fold thread does - two pushes per raw
    // run; the sort/merge/chunk/issue all happen on the prefetch thread (see the
    // qjobs comment: doing it here cost 84-140 s of serial time at 1 GB).
    void submit(const std::vector<RawRun>& raws, uint64_t n, uint64_t win) {
        if (!corpus || raws.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (inflight >= JOBS) {
                st.skipped.fetch_add(1, std::memory_order_relaxed);
                st.droppedBytes.fetch_add(2 * raws.size() * win, std::memory_order_relaxed);
                return;
            }
        }
        // Per-band window: the submit override wins, else the prefetcher's own.
        if (win) windowBytes = win;
        std::vector<std::pair<uint64_t, uint64_t>> r;
        r.reserve(2 * raws.size());
        for (const auto& raw : raws) {
            for (uint64_t pos : {raw.head, raw.tail}) {
                if (pos >= n) continue;
                r.emplace_back(pos, std::min<uint64_t>(windowBytes, n - pos));
            }
        }
        if (r.empty()) return;
        st.rawWindows.fetch_add(r.size(), std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (inflight >= JOBS) {          // re-checked: the thread may have finished one
                st.skipped.fetch_add(1, std::memory_order_relaxed);
                st.droppedBytes.fetch_add(r.size() * windowBytes, std::memory_order_relaxed);
                return;
            }
            ++inflight;
            qjobs.push_back(std::move(r));
            have = true;
        }
        st.jobs.fetch_add(1, std::memory_order_relaxed);
        wake.notify_one();
    }
    // Wait for the queue to drain (used at the end of the sweep only; the band
    // loop never blocks on the prefetcher).
    void drain() {
        std::unique_lock<std::mutex> lk(mtx);
        idle.wait(lk, [&] { return inflight == 0; });
    }
    void report() const {
        const uint64_t raw = st.rawWindows.load(), merged = st.mergedReads.load();
        const uint64_t reads = st.reads.load(), bytes = st.readBytes.load();
        const uint64_t touched = st.touchedBytes.load();
        std::fprintf(stderr,
            "XSA_FINISH_PREFETCH_POLICY mode=%d (%s) assist=%d (%s) skip_resident=%d "
            "window_bytes=%llu budget_bytes=%llu gap_bytes=%llu chunk_bytes=%llu jobs=%u "
            "source=next_band_targets\n",
            mode, mode == 2 ? "scattered" : "sweep",
            assist, assist == 0 ? "populate" : (assist == 1 ? "willneed" : "both"),
            int(skipResident),
            (unsigned long long)windowBytes, (unsigned long long)budgetBytes,
            (unsigned long long)gap, (unsigned long long)chunkBytes, JOBS);
        const uint64_t fetched = st.fetchedBytes.load();
        std::fprintf(stderr,
            "XSA_FINISH_PREFETCH jobs=%llu raw_windows=%llu merged_reads=%llu "
            "coalesce_ratio=%.3f fronts=%llu reads=%llu avg_read_bytes=%.1f bytes=%llu "
            "touched_bytes=%llu fetched_bytes=%llu fetched_mass_pct=%.4f "
            "skip_resident_reads=%llu skip_resident_bytes=%llu residency_unknown=%llu "
            "corpus_bytes=%llu touched_mass_pct=%.4f "
            "pages_populated=%llu skipped=%llu dropped_bytes=%llu busy_s=%.3f "
            "willneed_s=%.3f mincore_s=%.3f\n",
            (unsigned long long)st.jobs.load(), (unsigned long long)raw,
            (unsigned long long)merged, raw ? double(merged) / double(raw) : 0.0,
            (unsigned long long)st.fronts.load(), (unsigned long long)reads,
            reads ? double(bytes) / double(reads) : 0.0,
            (unsigned long long)bytes, (unsigned long long)touched,
            (unsigned long long)fetched,
            corpusBytes ? 100.0 * double(fetched) / double(corpusBytes) : 0.0,
            (unsigned long long)st.skipResidentReads.load(),
            (unsigned long long)st.skipResidentBytes.load(),
            (unsigned long long)st.residencyUnknown.load(),
            (unsigned long long)corpusBytes,
            corpusBytes ? 100.0 * double(touched) / double(corpusBytes) : 0.0,
            (unsigned long long)st.pagesPopulated.load(),
            (unsigned long long)st.skipped.load(),
            (unsigned long long)st.droppedBytes.load(),
            1e-6 * double(st.busyUsec.load()), 1e-6 * double(st.willneedUsec.load()),
            1e-6 * double(st.mincoreUsec.load()));
    }
    ~PfPrefetcher() {
        { std::lock_guard<std::mutex> lk(mtx); stop = true; }
        wake.notify_all();
        if (th.joinable()) th.join();
    }
};

// The prefetcher's knobs, read ONCE (a per-band getenv would be a syscall in the
// fold). mode 0 disables the prefetcher entirely - the product default.
static int pf_prefetch_mode() {
    const char* e = std::getenv("XSA_FINISH_PREFETCH");
    if (!e || !*e) return 0;
    if (!std::strcmp(e, "sweep") || !std::strcmp(e, "on")) return 1;
    if (!std::strcmp(e, "scattered")) return 2;
    char* end = nullptr; errno = 0;
    long v = std::strtol(e, &end, 10);
    pf_check(*e && end && !*end && !errno && v >= 0 && v <= 2,
             "XSA_FINISH_PREFETCH must be 0|1|2|off|sweep|scattered");
    return int(v);
}
static uint64_t pf_env_u64(const char* name, uint64_t def, uint64_t lo, uint64_t hi) {
    const char* e = std::getenv(name);
    if (!e || !*e) return def;
    char* end = nullptr; errno = 0;
    unsigned long long v = std::strtoull(e, &end, 10);
    pf_check(*e && *e != '-' && end && !*end && !errno && v >= lo && v <= hi,
             std::string(name) + " out of range");
    return uint64_t(v);
}

struct FinishPool {
    std::mutex mutex;
    std::condition_variable work, complete;
    std::vector<std::thread> workers;
    std::function<void(int)> task;
    uint64_t generation = 0;
    size_t remaining = 0;
    bool stopping = false;
    explicit FinishPool(int count) {
        for (int t = 0; t < count; ++t) workers.emplace_back([this, t] {
            uint64_t seen = 0;
            std::unique_lock<std::mutex> lock(mutex);
            for (;;) {
                work.wait(lock, [&] { return stopping || generation != seen; });
                if (stopping) return;
                seen = generation;
                auto job = task;
                lock.unlock(); job(t); lock.lock();
                if (--remaining == 0) complete.notify_one();
            }
        });
    }
    // finish-decomposed: the barrier is split so a caller can DISPATCH a job
    // and keep working (the ordered fold of the next band) before waiting for
    // it. start()/wait() must strictly alternate; a second start() without a
    // wait() would clobber `remaining` and silently drop half the job.
    std::atomic<bool> dispatched{false};
    void start(std::function<void(int)> job) {
        std::unique_lock<std::mutex> lock(mutex);
        pf_check(!dispatched.load(std::memory_order_relaxed), "finish pool dispatched twice without wait");
        task = std::move(job); remaining = workers.size(); ++generation;
        dispatched.store(true, std::memory_order_relaxed);
        work.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        complete.wait(lock, [&] { return remaining == 0; });
        task = {};
        dispatched.store(false, std::memory_order_relaxed);
    }
    void run(std::function<void(int)> job) {
        start(std::move(job));
        wait();
    }
    ~FinishPool() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        work.notify_all();
        for (auto& worker : workers) worker.join();
    }
};

// Exact checkpoints are a correctness baseline for parallel repair. The planner
// deliberately runs the existing transducer: copying it captures BOTH pending
// coalescers, raw/normalized prefixes, dollar/padding guards, class cursor and
// entry/exit flanks (in the immutable, ready seam table). This first version
// duplicates repair work; keep it opt-in until a cheaper planner is measured.
static bool repair_state_equal(const RepairStream& a, const RepairStream& b) {
    return a.rows == b.rows && a.row == b.row && a.next == b.next &&
        a.srcHave == b.srcHave && (!a.srcHave ||
        (a.srcPending.c == b.srcPending.c && a.srcPending.len == b.srcPending.len &&
         a.srcPending.head == b.srcPending.head && a.srcPending.tail == b.srcPending.tail)) &&
        a.have == b.have && (!a.have || (a.pending.c == b.pending.c &&
         a.pending.len == b.pending.len && a.pending.h == b.pending.h && a.pending.t == b.pending.t)) &&
        a.certified == b.certified && a.dollarSeen == b.dollarSeen &&
        a.zeroes == b.zeroes && a.beforeTerminal == b.beforeTerminal &&
        a.firstRunSeen == b.firstRunSeen && a.rNorm == b.rNorm &&
        a.nNorm == b.nNorm && a.totals == b.totals && a.runFreq == b.runFreq;
}
static std::array<double, 3> repair_chunks(RepairStream& rs, const std::vector<RawRun>& raw,
                          FinishPool& pool, size_t chunkRuns) {
    const double begin = tnow();
    pf_check(chunkRuns > 0, "zero repair chunk");
    struct Chunk {
        RepairStream entry, exit;
        size_t lo, hi;
        std::vector<NRun> output;
    };
    std::vector<Chunk> chunks;
    auto emit = rs.emit;
    rs.emit = [](const NRun&) {};
    for (size_t lo = 0; lo < raw.size(); lo += chunkRuns) {
        size_t hi = std::min(raw.size(), lo + chunkRuns);
        RepairStream entry = rs;
        for (size_t i = lo; i < hi; ++i) rs.feed_raw(raw[i]);
        chunks.push_back({std::move(entry), rs, lo, hi, {}});
    }
    // One already-ready band bounds both checkpoint storage and reorder output.
    // No worker reads beyond that band or changes readiness/consumption gating.
    const double planned = tnow();
    std::atomic<size_t> next{0};
    pool.run([&](int) {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= chunks.size()) break;
            auto& chunk = chunks[i];
            RepairStream local = chunk.entry;
            local.emit = [&](const NRun& run) { chunk.output.push_back(run); };
            for (size_t j = chunk.lo; j < chunk.hi; ++j) local.feed_raw(raw[j]);
            pf_check(repair_state_equal(local, chunk.exit), "repair checkpoint replay mismatch");
        }
    });
    const double repaired = tnow();
    const uint64_t finalR = rs.rNorm;
    rs.emit = emit;
    for (const auto& chunk : chunks) {
        rs.rNorm = chunk.entry.rNorm;
        for (const auto& run : chunk.output) { ++rs.rNorm; emit(run); }
        pf_check(rs.rNorm == chunk.exit.rNorm, "repair chunk output count");
    }
    rs.rNorm = finalR;
    return {planned - begin, repaired - planned, tnow() - repaired};
}

static int selftest_repair_chunks() {
    FinishPool pool(4);
    uint64_t cases = 0;
    for (unsigned seed = 0; seed < 128; ++seed) {
        // Separate and straddling padding; adjacent equal raw chars; dollar
        // rewriting into the terminal; legal 0x00/0x05 bytes. Every split is
        // exercised, including both sides of each class and its flanks.
        std::vector<RawRun> raw{{9,1,64,64}, {2,9,65,73}};
        if (seed & 1) raw.back() = {2,10,65,0};
        uint64_t n = (seed & 1) ? 1 : 0;
        if (!(seed & 1)) ++n;
        for (unsigned i = 0; i < 24; ++i) {
            uint8_t alphabet[] = {0,5,9,10};
            uint64_t len = 1 + ((seed * 17 + i * 13) % 4);
            raw.push_back({alphabet[((i / (1 + seed % 3)) + seed) % 4],len,1 + i, len == 1 ? 1 + i : 25 + i});
            if (!(seed & 1) && i == seed % 24) raw.push_back({2,1,0,0});
            n += len;
        }
        raw.front().head = raw.front().tail = n;
        SeamClasses seam;
        if (seed & 2) {
            seam.classes.push_back({2,5,1,17,19,{},{{5,1,7,7},{0,2,8,9}}});
            seam.classes.push_back({8,12,1,21,23,{},{{9,4,11,13}}});
        }
        std::vector<NRun> expected;
        RepairStream initial; initial.N=n+10; initial.n=n; initial.terminal=9; initial.seam=&seam;
        initial.emit = [&](const NRun& r) { expected.push_back(r); };
        RepairStream serial=initial;
        for (const auto& r : raw) serial.feed_raw(r);
        serial.finishStream();
        for (size_t width=1; width<=raw.size(); ++width) {
            std::vector<NRun> actual;
            RepairStream test=initial;
            test.emit = [&](const NRun& r) { actual.push_back(r); };
            repair_chunks(test,raw,pool,width);
            test.finishStream();
            pf_check(repair_state_equal(serial,test), "repair oracle final state");
            pf_check(expected.size()==actual.size(), "repair oracle run count");
            for (size_t i=0; i<actual.size(); ++i)
                pf_check(expected[i].c==actual[i].c && expected[i].len==actual[i].len &&
                         expected[i].h==actual[i].h && expected[i].t==actual[i].t,
                         "repair oracle serialized run bytes");
            ++cases;
        }
    }
    printf("CHUNKED_REPAIR_ORACLE_PASS streams=128 widths=%llu workers=4 both_coalescers=1 class_flanks=1 padding_straddle=1 dollar=1 legal_00_05=1\n", (unsigned long long)cases);
    return 0;
}

// CPU-only attribution: the legacy repair timer also charges producer waits,
// class rebuilds and piece I/O, so it cannot by itself measure this scheduler.
static int benchmark_repair_chunks() {
    constexpr uint64_t count = 1u << 20;
    std::vector<RawRun> raw{{9,1,count+1,count+1},{2,9,0,0},{2,1,0,0}};
    for (uint64_t i=0; i<count; ++i) raw.push_back({uint8_t(i%2 ? 5 : 9),1,1,1});
    RepairStream initial; initial.N=count+11; initial.n=count+1; initial.terminal=9;
    initial.emit=[](const NRun&) {};
    auto serial=initial;
    double begin=tnow();
    for (const auto& r:raw) serial.feed_raw(r);
    double serialWall=tnow()-begin;
    FinishPool pool(16);
    auto parallel=initial;
    auto wall=repair_chunks(parallel,raw,pool,4096);
    pf_check(repair_state_equal(serial,parallel), "repair benchmark state");
    fprintf(stdout,"REPAIR_CHUNK_CPU_BENCH runs=%zu threads=16 chunk_runs=4096 serial=%.6f planner=%.6f workers=%.6f ordered_drain=%.6f duplicated_planner_work=1\n",
            raw.size(),serialWall,wall[0],wall[1],wall[2]);
    return 0;
}

// ------------------------------------------------------------------ PROB-CHI
// The claims list and the checker that make a HASH-TRUSTED chi certain.
// Analysis + the probability model: tools/binbwt/PROB_CHI_ANALYSIS.md.
// The formal statement: lean/ProbChi.lean.
//
// CLAIM SEMANTICS (one line, no exceptions):
//
//   claim(i, j, g, cap)  means   T[i..i+g) == T[j..j+g)
//                          and   (g == cap  or  T[i+g] != T[j+g])
//
// i.e. exactly `g == min(lce(i,j), cap)`. That is the ONLY arithmetic fact the
// sweep consumes (`topLcp` and `interiorMin`, xsa_finish.cpp lceJob), so a
// claims list that the text satisfies makes the sweep's inputs identical to the
// FULL tier's inputs; the sweep is a deterministic function of those inputs and
// therefore emits the identical container. The probability lives in the
// constructor's speed and never reaches the product.
// Bucket count is a function of n, not a constant: bucket b covers the text
// positions [b*n/NB, (b+1)*n/NB), so the checker's i-side sweep stays inside one
// position window at every scale. 1024 buckets at 100 MB..1 GB (5.6 MB of claim
// records per bucket at 1 GB); the cap keeps a pile claims list biddable in
// RAM-per-worker terms and is the place a pile-scale run must switch to
// band-fused checking (see PROB_CHI_ANALYSIS.md 3.2: a pile spool is ~7.4 TB).
static inline uint64_t pf_claim_buckets(uint64_t n) {
    uint64_t b = n >> 20;
    if (b < 1024) b = 1024;
    if (b > 8192) b = 8192;
    return b;
}

static inline uint8_t* pf_varint_put(uint8_t* p, uint64_t v) {
    while (v >= 0x80) { *p++ = uint8_t(v) | 0x80; v >>= 7; }
    *p++ = uint8_t(v);
    return p;
}
static inline const uint8_t* pf_varint_get(const uint8_t* p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (unsigned sh = 0; p < end; sh += 7) {
        uint8_t b = *p++;
        if (sh > 63) pf_fail("claims: varint overflow");
        v |= uint64_t(b & 0x7F) << sh;
        if (!(b & 0x80)) return p;
    }
    pf_fail("claims: truncated varint");
}

// A claim is verified by reading the text AT THE CLAIMED POSITIONS. Nothing here
// searches, probes, hashes or consults the structure: two spans, one boundary.
struct PfClaimView { uint64_t i, j, g; uint8_t truncated; };
// Returns 0 when the claim HOLDS; otherwise the offset of the first violation
// (== g means "the boundary is equal, so the answer was not maximal").
static inline uint64_t pf_claim_violation(const CorpusText& t, const PfClaimView& c) {
    // Bounds: the span is always inside the text (cap = min(limit, n-i, n-j));
    // the BOUNDARY byte is only needed for a non-truncated claim, where
    // g < cap <= n-i, so i+g+1 <= n holds by the claim's construction.
    pf_check(c.i + c.g <= t.size() && c.j + c.g <= t.size(), "claim span outside text");
    if (!c.truncated) pf_check(c.i + c.g + 1 <= t.size() && c.j + c.g + 1 <= t.size(),
                               "claim boundary outside text");
    for (uint64_t k = 0; k < c.g; ++k)
        if (t[c.i + k] != t[c.j + k]) return k;
    if (!c.truncated && t[c.i + c.g] == t[c.j + c.g]) return c.g;
    return UINT64_MAX;
}

// ---- the SPOOL sink: hash-trust + a position-bucketed claims list on scratch.
// Claims are bucketed by `i` (bucket b covers text positions
// [b*n/NB, (b+1)*n/NB)), which is what makes the checker's i-side a monotone
// sweep instead of 4.7e8 independent random starts.
struct PfClaimSpool : SlimClaimSink {
    std::string dir;
    uint64_t n = 0, nb = 0;
    std::atomic<uint64_t> claimCount{0}, spanSymbols{0}, encodedBytes{0};
    std::vector<int> fd;
    std::vector<uint64_t> written;
    std::mutex mu;
    struct Buf { std::vector<std::vector<uint8_t>> d; };
    std::vector<std::unique_ptr<Buf>> bufs;
    explicit PfClaimSpool(const std::string& d, uint64_t textSize)
        : dir(d), n(textSize), nb(pf_claim_buckets(textSize)) {
        std::filesystem::create_directories(dir);
        fd.assign(nb, -1);
        written.assign(nb, 0);
        bufs.resize(512);
        for (uint64_t b = 0; b < nb; ++b) {
            char name[64];
            snprintf(name, sizeof name, "b%04llu.vc", (unsigned long long)b);
            fd[b] = ::open((dir + "/" + name).c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
            pf_check(fd[b] >= 0, std::string("create claims bucket ") + name);
        }
    }
    ~PfClaimSpool() {
        for (int f : fd) if (f >= 0) ::close(f);
    }
    Buf* buf() {
        unsigned t = slim_tid();
        if (bufs[t]) return bufs[t].get();
        std::lock_guard<std::mutex> g(mu);
        if (!bufs[t]) { bufs[t] = std::make_unique<Buf>(); bufs[t]->d.resize(nb); }
        return bufs[t].get();
    }
    void flushBucket(unsigned b, Buf& t) {
        std::vector<uint8_t>& d = t.d[b];
        if (d.empty()) return;
        std::lock_guard<std::mutex> g(mu);
        const char* p = (const char*)d.data();
        uint64_t off = written[b], len = d.size();
        while (len) {
            ssize_t z = ::pwrite(fd[b], p, len, off_t(off));
            pf_check(z > 0, "claims bucket write");
            p += z; off += uint64_t(z); len -= uint64_t(z);
        }
        written[b] = off;
        encodedBytes.fetch_add(d.size(), std::memory_order_relaxed);
        d.clear();
    }
    void claim(uint64_t i, uint64_t j, uint64_t g, uint64_t cap) override {
        pf_check(i < n && j < n && g <= n, "claim outside text");
        unsigned b = unsigned((__uint128_t(i) * nb) / n);
        if (b >= nb) b = unsigned(nb - 1);
        Buf* t = buf();
        std::vector<uint8_t>& d = t->d[b];
        uint8_t tmp[32];
        uint8_t* e = pf_varint_put(tmp, i);
        e = pf_varint_put(e, j);
        e = pf_varint_put(e, g);
        *e++ = uint8_t(g == cap ? 1 : 0);
        d.insert(d.end(), tmp, e);
        claimCount.fetch_add(1, std::memory_order_relaxed);
        spanSymbols.fetch_add(g + (g < cap), std::memory_order_relaxed);
        if (d.size() >= (8u << 10)) flushBucket(b, *t);
    }
    void close() {
        for (uint64_t b = 0; b < nb; ++b)
            for (auto& t : bufs) if (t) flushBucket(unsigned(b), *t);
        std::string meta = "PROBCHI_CLAIMS 1\nbuckets " + std::to_string(nb) +
                           "\nn " + std::to_string(n) +
                           "\nclaims " + std::to_string(claimCount.load()) +
                           "\nspan_symbols " + std::to_string(spanSymbols.load()) +
                           "\nencoded_bytes " + std::to_string(encodedBytes.load()) +
                           "\nbucket_bytes";
        for (uint64_t b = 0; b < nb; ++b) meta += " " + std::to_string(written[b]);
        meta += "\n";
        const std::string path = dir + "/meta";
        int f = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        pf_check(f >= 0, "create claims meta");
        pf_check(::write(f, meta.data(), meta.size()) == ssize_t(meta.size()), "write claims meta");
        pf_check(::close(f) == 0, "close claims meta");
        fprintf(stderr, "XSA_FINISH_CLAIMS_SPOOL dir=%s claims=%llu span_symbols=%llu "
                        "encoded_bytes=%llu buckets=%llu n=%llu\n",
                dir.c_str(), (unsigned long long)claimCount.load(),
                (unsigned long long)spanSymbols.load(),
                (unsigned long long)encodedBytes.load(),
                (unsigned long long)nb, (unsigned long long)n);
    }
};

// ---- the FUSED sink: verify in the worker that proposed the answer.
// No spool, no sorting, the same access pattern as the FULL tier's inline
// confirmation - but no hash probing and no verification-budget accounting. It
// is the pile-scale shape (a pile claims spool would be ~7.4 TB against a ~2 TB
// scratch ceiling) and the honest A/B of "is the position sort worth a spool?".
struct PfClaimFused : SlimClaimSink {
    const CorpusText* text = nullptr;
    std::atomic<uint64_t> claimCount{0}, comparedSymbols{0};
    double t0 = 0, wall = 0;
    std::atomic<uint64_t> badClaims{0};
    void claim(uint64_t i, uint64_t j, uint64_t g, uint64_t cap) override {
        claimCount.fetch_add(1, std::memory_order_relaxed);
        PfClaimView c{i, j, g, uint8_t(g == cap ? 1 : 0)};
        const uint64_t bad = pf_claim_violation(*text, c);
        comparedSymbols.fetch_add(g + (g < cap), std::memory_order_relaxed);
        if (bad != UINT64_MAX) {
            badClaims.fetch_add(1, std::memory_order_relaxed);
            fprintf(stderr, "XSA_FINISH_CLAIM_VIOLATION mode=fused i=%llu j=%llu g=%llu cap=%llu "
                            "truncated=%u at_offset=%llu Ti=%u Tj=%u Ti_g=%u Tj_g=%u\n",
                    (unsigned long long)i, (unsigned long long)j, (unsigned long long)g,
                    (unsigned long long)cap, unsigned(c.truncated), (unsigned long long)bad,
                    unsigned((*text)[i + std::min(bad, g)]), unsigned((*text)[j + std::min(bad, g)]),
                    unsigned((*text)[i + g]), unsigned((*text)[j + g]));
            pf_fail("hash-trusted LCE answer is FALSE (fused checker)");
        }
    }
};

// ---- the FLOOR sink: emit and count, check nothing. This is a MEASUREMENT arm
// only (XSA_FINISH_LCE_TRUST=3): it prices the constructor's floor when it
// verifies nothing at all, so the checker's own price is separable. It is NOT a
// product tier - nothing is certified.
struct PfClaimNull : SlimClaimSink {
    std::atomic<uint64_t> claimCount{0}, spanSymbols{0};
    void claim(uint64_t, uint64_t, uint64_t g, uint64_t cap) override {
        claimCount.fetch_add(1, std::memory_order_relaxed);
        spanSymbols.fetch_add(g + (g < cap), std::memory_order_relaxed);
    }
};

// ---- THE CHECKER PASS. Reads the claims, reads the text at the claimed
// positions, confirms. A worker OWNS a bucket, sorts it by `i` and sweeps it
// from low to high, so every worker's i-side is a monotone front inside one
// position window: the pass is NB sorted sweeps over the text plus the scattered
// `j` side. No searching, no probing, no structure, no history.
static void pf_check_claims(const CorpusText& text, const std::string& dir, int threads) {
    const double t0 = tnow();
    const std::string metaPath = dir + "/meta";
    FILE* f = fopen(metaPath.c_str(), "rb");
    pf_check(f != nullptr, "open claims meta (" + metaPath + ")");
    uint64_t buckets = 0, n = 0, claims = 0, spanSymbols = 0, encodedBytes = 0;
    std::vector<uint64_t> bucketBytes;
    char key[64];
    pf_check(fscanf(f, "%63s", key) == 1 && std::string(key) == "PROBCHI_CLAIMS", "claims meta magic");
    uint64_t version = 0;
    pf_check(fscanf(f, "%llu", (unsigned long long*)&version) == 1 && version == 1, "claims meta version");
    pf_check(fscanf(f, "%63s %llu", key, (unsigned long long*)&buckets) == 2 && std::string(key) == "buckets", "claims meta buckets");
    pf_check(fscanf(f, "%63s %llu", key, (unsigned long long*)&n) == 2 && std::string(key) == "n", "claims meta n");
    pf_check(fscanf(f, "%63s %llu", key, (unsigned long long*)&claims) == 2 && std::string(key) == "claims", "claims meta claims");
    pf_check(fscanf(f, "%63s %llu", key, (unsigned long long*)&spanSymbols) == 2 && std::string(key) == "span_symbols", "claims meta span_symbols");
    pf_check(fscanf(f, "%63s %llu", key, (unsigned long long*)&encodedBytes) == 2 && std::string(key) == "encoded_bytes", "claims meta encoded_bytes");
    pf_check(fscanf(f, "%63s", key) == 1 && std::string(key) == "bucket_bytes", "claims meta bucket_bytes");
    bucketBytes.resize(buckets);
    for (uint64_t b = 0; b < buckets; ++b)
        pf_check(fscanf(f, "%llu", (unsigned long long*)&bucketBytes[b]) == 1, "claims meta bucket size");
    pf_check(fclose(f) == 0, "close claims meta");
    pf_check(buckets > 0 && buckets <= 65536, "claims meta bucket count out of range");
    // FIAT: one bucket is loaded whole (that is what makes the i side sortable
    // and monotone), so a bucket larger than the budget is a claims list that
    // must be checked in a different SHAPE, not a check that silently degrades.
    // At the pile the spool is 1.9-7.1 TB and a bucket can be ~1 GB of records:
    // that run must use the fused sink (XSA_FINISH_LCE_TRUST=2) or a
    // band-consumed spool. Failing loud here is the point.
    const uint64_t bucketMax = pf_env_u64("XSA_FINISH_CLAIM_BUCKET_MAX", 1ull << 30, 1ull << 20, 1ull << 40);
    for (uint64_t b = 0; b < buckets; ++b)
        pf_check(bucketBytes[b] <= bucketMax,
                 "claims bucket " + std::to_string(b) + " is " + std::to_string(bucketBytes[b]) +
                 " B, over XSA_FINISH_CLAIM_BUCKET_MAX=" + std::to_string(bucketMax) +
                 " (raise it, add buckets, or check fused: XSA_FINISH_LCE_TRUST=2)");
    pf_check(n == text.size(), "claims meta n != corpus size (the claims belong to a different text)");

    struct PerThread { std::atomic<uint64_t> claims{0}, symbols{0}, bad{0}, buckets{0}; };
    const int T = std::max(1, std::min(threads, 256));
    std::unique_ptr<PerThread[]> per(new PerThread[unsigned(T)]);
    std::atomic<uint64_t> badSeq{~0ull}, badBucket{0}, checked{0}, maxBucketClaims{0};
    std::mutex badMu;
    std::atomic<uint64_t> nextBucket{0};
    auto worker = [&](int t) {
        slim_set_thread_shard(unsigned(t));
        std::vector<uint8_t> raw;
        std::vector<PfClaimView> view;
        for (;;) {
            uint64_t b = nextBucket.fetch_add(1);
            if (b >= buckets) break;
            if (!bucketBytes[b]) { continue; }
            raw.resize(bucketBytes[b]);
            char name[64];
            snprintf(name, sizeof name, "/b%04llu.vc", (unsigned long long)b);
            int bfd = ::open((dir + name).c_str(), O_RDONLY);
            pf_check(bfd >= 0, std::string("open claims bucket ") + name);
            pf_pread_full(bfd, raw.data(), raw.size(), 0, "claims bucket read");
            pf_check(::close(bfd) == 0, "close claims bucket");
            view.clear();
            const uint8_t* p = raw.data();
            const uint8_t* end = p + raw.size();
            while (p < end) {
                PfClaimView c{};
                p = pf_varint_get(p, end, c.i);
                p = pf_varint_get(p, end, c.j);
                p = pf_varint_get(p, end, c.g);
                if (p >= end) pf_fail("claims: truncated record flag");
                c.truncated = uint8_t(*p++ & 1);
                view.push_back(c);
            }
            // POSITION ORDER: this bucket is one worker's monotone i-front.
            std::sort(view.begin(), view.end(), [](const PfClaimView& a, const PfClaimView& c) { return a.i < c.i; });
            const size_t cnt = view.size();
            {
                uint64_t mx = maxBucketClaims.load(std::memory_order_relaxed);
                while (cnt > mx && !maxBucketClaims.compare_exchange_weak(mx, cnt, std::memory_order_relaxed)) {}
            }
            for (size_t k = 0; k < cnt; ++k) {
                const PfClaimView& c = view[k];
                const uint64_t bad = pf_claim_violation(text, c);
                per[unsigned(t)].claims.fetch_add(1, std::memory_order_relaxed);
                per[unsigned(t)].symbols.fetch_add(c.g + (c.truncated ? 0 : 1), std::memory_order_relaxed);
                if (bad != UINT64_MAX) {
                    per[unsigned(t)].bad.fetch_add(1, std::memory_order_relaxed);
                    if (badSeq.load() == ~0ull) { std::lock_guard<std::mutex> g(badMu);
                        if (badSeq.load() == ~0ull) {
                            badSeq.exchange(view[k].i);
                            badBucket.store(b);
                            fprintf(stderr, "XSA_FINISH_CLAIM_VIOLATION mode=checker bucket=%llu i=%llu j=%llu g=%llu "
                                            "truncated=%u at_offset=%llu Ti=%u Tj=%u\n",
                                    (unsigned long long)b, (unsigned long long)c.i, (unsigned long long)c.j,
                                    (unsigned long long)c.g, unsigned(c.truncated), (unsigned long long)bad,
                                    unsigned(text[c.i + std::min(bad, c.g)]), unsigned(text[c.j + std::min(bad, c.g)]));
                        } }
                }
            }
            per[unsigned(t)].buckets.fetch_add(1, std::memory_order_relaxed);
            checked.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < T; ++t) pool.emplace_back(worker, t);
    for (auto& th : pool) th.join();
    uint64_t done = 0, symbols = 0, bad = 0, swept = 0;
    for (int t = 0; t < T; ++t) { done += per[t].claims.load(); symbols += per[t].symbols.load();
                                  bad += per[t].bad.load(); swept += per[t].buckets.load(); }
    const double wall = tnow() - t0;
    const uint64_t firstBad = badSeq.load();
    fprintf(stderr, "XSA_FINISH_CHECK_CLAIMS claims=%llu claims_declared=%llu symbols_compared=%llu "
                    "buckets=%llu swept=%llu max_bucket_claims=%llu threads=%d wall=%.3f "
                    "violations=%llu first_bad_i=%s source=claim_list independent_of_structure=1\n",
            (unsigned long long)done, (unsigned long long)claims,
            (unsigned long long)symbols, (unsigned long long)buckets, (unsigned long long)swept,
            (unsigned long long)maxBucketClaims.load(), T, wall,
            (unsigned long long)bad, firstBad == ~0ull ? "none" : std::to_string(firstBad).c_str());
    pf_check(done == claims, "claims list is INCOMPLETE: checked " + std::to_string(done) +
                             " of " + std::to_string(claims) + " records");
    pf_check(bad == 0, "claims list FAILED: " + std::to_string(bad) +
                       " claims the text does not satisfy (first at bucket " +
                       std::to_string(badBucket.load()) + ")");
}


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
// ------------------------------------------------------ THE LCP COLUMN (kasai)
// `tools/binbwt/kasai_column`'s product, read as a table. col[r] =
// LCE(SA[r], SA[r+1]) over the same cyclic text, u8 fast path + a sorted escape
// list for values >= 255 (KASAI_FINISH_DESIGN.md). The finish reads it in RANK
// ORDER along the ordered fold - each band stages its own CONTIGUOUS rank slice
// with one pread - so the sweep's two query families cost a column read and a
// range minimum instead of an LCE walk:
//
//     topLcp[i]   = col[r_i - 1]                     r_i = cumulative run length
//     interior[i] = min(col[r_i .. r_i + len_i - 2])
//
// THE MAPPING IS THE CLOSURE CONDITION: the runs are contiguous BWT intervals in
// ascending rank order, so sum(len) over every emitted run == n and r_i is the
// running sum. That is asserted at the end of the sweep, and
// XSA_FINISH_KASAI_AUDIT=1 asserts every value this table returns equals the
// SlimFingerprint answer it replaces.
static std::atomic<uint64_t> g_kasaiDbg{0};

// ------------------------------------------------- THE LOCATE TABLE (ISA, i.e.
// position -> rank). The run's rank CANNOT be derived from a cumulative sum of
// the run lengths: the seam-class splice advances the transducer's row past the
// rows its reconstructed runs describe, so a sum silently shifts every later
// rank (measured at 100 MB: the first class block is one row short of its span
// and 159 consecutive topLcp values came out wrong). `kasai_column`'s ISA is
// therefore the finish's locate table, exactly as the design document prices it:
// one random read per run, in the pool, on a page-cache-resident array.
struct LocateIsa {
    int fd = -1;
    const uint8_t* map = nullptr;
    uint64_t bytes = 0, n = 0;
    unsigned width = 0;
    uint64_t lookups = 0;
    void open(const std::string& path, uint64_t nExpect) {
        static constexpr uint32_t MAGIC = 0x53495853u;   // 'SXIS'
        static constexpr uint32_t VERSION = 1;
        fd = ::open(path.c_str(), O_RDONLY);
        pf_check(fd >= 0, "open --sa-locate " + path + ": " + std::strerror(errno));
        struct stat st {};
        pf_check(::fstat(fd, &st) == 0, "stat --sa-locate");
        bytes = uint64_t(st.st_size);
        uint8_t h[32];
        pf_read_full(fd, h, 32, "locate header");
        uint32_t magic = 0, ver = 0, w = 0;
        std::memcpy(&magic, h + 0, 4);
        std::memcpy(&ver, h + 4, 4);
        std::memcpy(&n, h + 8, 8);
        std::memcpy(&w, h + 16, 4);
        pf_check(magic == MAGIC, "--sa-locate magic");
        pf_check(ver == VERSION, "--sa-locate version");
        pf_check(n == nExpect, "--sa-locate n != the corpus length");
        pf_check(w == 4 || w == 8, "--sa-locate width must be 4 or 8");
        pf_check(bytes == 32 + n * w, "--sa-locate file size != 32 + n*width");
        width = w;
        map = (const uint8_t*)::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
        pf_check(map != MAP_FAILED, "mmap --sa-locate: " + std::string(std::strerror(errno)));
        ::madvise((void*)map, bytes, MADV_RANDOM);
        fprintf(stderr, "XSA_FINISH_LOCATE_ISA path=%s n=%llu width=%u bytes=%llu\n",
                path.c_str(), (unsigned long long)n, width, (unsigned long long)bytes);
    }
    inline uint64_t at(uint64_t p) const {
        pf_check(p < n, "locate lookup outside [0,n)");
        uint64_t r;
        if (width == 4) { uint32_t v; std::memcpy(&v, map + 32 + p * 4, 4); r = v; }
        else std::memcpy(&r, map + 32 + p * 8, 8);
        pf_check(r < n, "locate entry outside [0,n)");
        return r;
    }
};

// ============================ THE SAMPLED INVERSE ===========================
// Anchors are keyed by POSITION, which survives the finish's repair; run indices and
// run lengths do not. The fold resyncs its rank cursor whenever a run's head sits on
// an anchor, so the cursor only has to stay correct for < k positions.
// DEBUG ONLY (XSA_FINISH_ISA_DEBUG=<SA stream>): build the TRUE inverse so the derived
// ranks can be compared run by run. Affordable on fixtures (n x 8 B), never at pile.
static std::string pf_env_str(const char* k) {
    const char* v = ::getenv(k);
    return v ? std::string(v) : std::string();
}

struct TrueIsa {
    std::vector<uint64_t> v;
    void load(const std::string& path, uint64_t n) {
        pf_check(n <= (1ull << 32), "XSA_FINISH_ISA_DEBUG is a fixture-scale diagnostic");
        int fd = ::open(path.c_str(), O_RDONLY);
        pf_check(fd >= 0, "open XSA_FINISH_ISA_DEBUG SA stream");
        struct stat st {};
        pf_check(::fstat(fd, &st) == 0 && uint64_t(st.st_size) == 48 + 8 * n, "debug SA stream size");
        std::vector<uint64_t> sa(n);
        // pread at +48: the stream carries a 48-byte anchor header (reading from 0
        // turned the header into SA entries and indexed out of range - segfault).
        {
            size_t got = 0;
            while (got < n * 8) {
                ssize_t r = ::pread(fd, ((char*)sa.data()) + got, n * 8 - got, off_t(48 + got));
                pf_check(r > 0, "debug SA body read");
                got += size_t(r);
            }
        }
        ::close(fd);
        v.assign(n, 0);
        for (uint64_t r = 0; r < n; ++r) v[sa[r]] = r;
        fprintf(stderr, "XSA_FINISH_ISA_DEBUG n=%llu inverse built (%llu MB)\n",
                (unsigned long long)n, (unsigned long long)(n * 8 >> 20));
    }
    uint64_t at(uint64_t p) const { return v[p]; }
};

struct SampIsa {
    int fd = -1;
    const uint8_t* map = nullptr;
    uint64_t bytes = 0, n = 0, k = 0, entries = 0;
    void open(const std::string& path, uint64_t nExpect) {
        fd = ::open(path.c_str(), O_RDONLY);
        pf_check(fd >= 0, "open --isa-sample " + path + ": " + std::strerror(errno));
        struct stat st {};
        pf_check(::fstat(fd, &st) == 0, "stat --isa-sample");
        bytes = uint64_t(st.st_size);
        uint64_t h[4] = {0, 0, 0, 0};
        pf_read_full(fd, h, 32, "isa-sample header");
        pf_check(h[0] == 1363761235ull, "--isa-sample magic (SXIQ)");
        n = h[2]; k = h[3];
        pf_check(n == nExpect, "--isa-sample n != the corpus length");
        pf_check(k >= 1, "--isa-sample k");
        entries = (n + k - 1) / k;
        pf_check(bytes == 32 + entries * 8, "--isa-sample size != 32 + entries*8");
        map = (const uint8_t*)::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
        pf_check(map != MAP_FAILED, "mmap --isa-sample");
        fprintf(stderr, "XSA_FINISH_ISA_SAMPLE path=%s n=%llu k=%llu entries=%llu bytes=%llu\n",
                path.c_str(), (unsigned long long)n, (unsigned long long)k,
                (unsigned long long)entries, (unsigned long long)bytes);
    }
    bool has(uint64_t p) const { return p % k == 0 && p / k < entries; }
    uint64_t at(uint64_t p) const {   // rank of an anchored position
        uint64_t r;
        std::memcpy(&r, map + 32 + (p / k) * 8, 8);
        pf_check(r < n, "--isa-sample rank outside [0,n)");
        return r;
    }
    uint64_t lookups = 0;
};

// ============================ RUN RANKS WITHOUT AN ISA =======================
// The Kasai arm needs rank(run head) and rank(run tail). A full ISA is 80 GB at
// 10 GB and 10.5 TB at pile - it breaks the 2n law. But ranks are MONOTONIC in run
// order, so the fold carries a row cursor and prefix-sums the run lengths; the
// producer emits ONLY the deviations it knows for certain (the holes left by the
// class/dollar repair) as <prefix>.runexc. Megabytes instead of terabytes.
struct RunRankExc {
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> by;
    // KEYED BY HEAD POSITION: the fold's runs are post-repair, so their indices do
    // not match the producer's; positions survive the renumbering.
    uint64_t head(uint64_t pos, uint64_t predicted) const {
        auto it = by.find(pos);
        return it == by.end() ? predicted : it->second.first;
    }
    uint64_t tail(uint64_t pos, uint64_t predicted) const {
        auto it = by.find(pos);
        return it == by.end() ? predicted : it->second.second;
    }
    void load(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        pf_check(fd >= 0, "open --run-ranks " + path + ": " + std::strerror(errno));
        uint64_t h[2] = {0, 0};
        pf_read_full(fd, h, 16, "runranks header");
        pf_check((h[0] & 0xffffffffull) == 0x45525853ull, "--run-ranks magic (SXRE)");
        const uint64_t count = h[1];
        std::vector<uint64_t> triples(count * 3);
        if (count) pf_read_full(fd, triples.data(), count * 24, "runranks body");
        ::close(fd);
        by.reserve(count * 2);
        for (uint64_t i = 0; i < count; ++i)
            by.emplace(triples[i * 3], std::make_pair(triples[i * 3 + 1], triples[i * 3 + 2]));
        fprintf(stderr, "XSA_FINISH_RUN_RANKS path=%s exceptions=%llu\n",
                path.c_str(), (unsigned long long)count);
    }
};

// The rank-space bookkeeping that must survive a band boundary: the previous
// band's last run's rank and end rank (for the adjacency test) and the counters.
struct KasaiState {
    uint64_t lastHeadRank = 0, lastEndRank = 0;
    uint64_t gaps = 0, disorder = 0, noncontig = 0, bands = 0;
    std::atomic<uint64_t> fallback{0};   // queries that needed the walk (rank holes)
};

struct LcpColumn {
    int fd = -1;
    uint64_t n = 0, escapeCount = 0, maxLcp = 0, flags = 0, fileBytes = 0;
    std::vector<std::pair<uint64_t, uint64_t>> esc;   // (rank, exact value), sorted
    uint64_t preads = 0, stagedBytes = 0, maxEnd = 0;
    static void pread_at(int f, void* p, uint64_t len, uint64_t at, const char* what) {
        char* c = (char*)p;
        uint64_t done = 0;
        while (done < len) {
            const ssize_t g = ::pread(f, c + done, size_t(len - done), off_t(at + done));
            pf_check(g > 0, std::string("pread ") + what);
            done += uint64_t(g);
        }
    }
    struct Band {
        uint64_t base = 0;
        std::vector<uint8_t> u8;
        std::vector<std::pair<uint64_t, uint64_t>> deep;   // escaped ranks in this slice
        uint64_t at(uint64_t rank) const {
            pf_check(rank >= base && rank - base < u8.size(),
                     "column read outside the staged band slice");
            const uint8_t v = u8[size_t(rank - base)];
            if (v != 255) return uint64_t(v);
            size_t lo = 0, hi = deep.size();
            while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                if (deep[mid].first < rank) lo = mid + 1; else hi = mid;
            }
            pf_check(lo < deep.size() && deep[lo].first == rank,
                     "escaped column rank " + std::to_string(rank) + " has no escape entry");
            return deep[lo].second;
        }
    };
    void open(const std::string& path, uint64_t nExpect) {
        static constexpr uint32_t MAGIC = 0x434c5853u;   // 'SXLC'
        static constexpr uint32_t VERSION = 1;
        static constexpr uint32_t HDR = 48;
        fd = ::open(path.c_str(), O_RDONLY);
        pf_check(fd >= 0, "open --lcp-column " + path + ": " + std::strerror(errno));
        struct stat st {};
        pf_check(::fstat(fd, &st) == 0, "stat --lcp-column");
        fileBytes = uint64_t(st.st_size);
        uint8_t h[HDR];
        pf_read_full(fd, h, HDR, "lcp column header");
        uint32_t magic = 0, version = 0;
        std::memcpy(&magic, h + 0, 4);
        std::memcpy(&version, h + 4, 4);
        std::memcpy(&n, h + 8, 8);
        uint64_t rows = 0;
        std::memcpy(&rows, h + 16, 8);
        std::memcpy(&escapeCount, h + 24, 8);
        std::memcpy(&maxLcp, h + 32, 8);
        std::memcpy(&flags, h + 40, 8);
        pf_check(magic == MAGIC, "--lcp-column magic");
        pf_check(version == VERSION, "--lcp-column version");
        pf_check(n == nExpect, "--lcp-column n != the corpus length");
        pf_check(rows == n, "--lcp-column rows != n");
        pf_check(flags == 1, "--lcp-column flags: only u8+escape is defined");
        pf_check(fileBytes == uint64_t(HDR) + n + escapeCount * 16,
                 "--lcp-column file size != 48 + n + escapes*16");
        esc.resize(size_t(escapeCount));
        if (escapeCount) {
            std::vector<uint8_t> buf(size_t(escapeCount * 16));
            pread_at(fd, buf.data(), buf.size(), uint64_t(HDR) + n, "lcp column escapes");
            for (size_t i = 0; i < esc.size(); ++i) {
                std::memcpy(&esc[i].first, buf.data() + i * 16, 8);
                std::memcpy(&esc[i].second, buf.data() + i * 16 + 8, 8);
                pf_check(esc[i].first < n, "--lcp-column escape rank out of range");
                pf_check(esc[i].second >= 255, "--lcp-column escape below the u8 cap");
                if (i) pf_check(esc[i].first > esc[i - 1].first, "--lcp-column escapes not strictly ordered");
            }
        }
        fprintf(stderr,
                "XSA_FINISH_LCP_COLUMN path=%s n=%llu escapes=%llu max_lcp=%llu "
                "bytes=%llu bytes_per_rank=%.4f\n",
                path.c_str(), (unsigned long long)n, (unsigned long long)escapeCount,
                (unsigned long long)maxLcp, (unsigned long long)fileBytes,
                double(fileBytes) / double(n + 1));
    }
    // One pread per band: the b-th band's rank slice, contiguous and ascending.
    void load(Band& b, uint64_t base, uint64_t len) {
        pf_check(base + len <= n, "column band slice outside [0,n) base=" +
                                  std::to_string(base) + " len=" + std::to_string(len) +
                                  " n=" + std::to_string(n));
        b.base = base;
        b.u8.resize(size_t(len ? len : 1));
        if (len) pread_at(fd, b.u8.data(), len, uint64_t(48) + base, "lcp column slice");
        b.deep.clear();
        // patch the escapes that fall inside this slice (sorted by rank, so the
        // slice is one contiguous range of the escape list)
        size_t lo = 0, hi = esc.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (esc[mid].first < base) lo = mid + 1; else hi = mid;
        }
        for (size_t i = lo; i < esc.size() && esc[i].first < base + len; ++i)
            b.deep.push_back(esc[i]);
        ++preads;
        stagedBytes += len;
        if (base + len > maxEnd) maxEnd = base + len;
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
        if (argc == 2 && std::string(argv[1]) == "--benchmark-repair-chunks") return benchmark_repair_chunks();
        if (argc == 2 && std::string(argv[1]) == "--selftest-repair-chunks") return selftest_repair_chunks();
        if (argc == 2 && std::string(argv[1]) == "--selftest-sweep-monoid") return selftest_sweep_monoid();
        if (argc == 3 && std::string(argv[1]) == "--selftest-seam-spool") return selftest_seam_spool(argv[2]);
        uint64_t chiExpect = 0, sampleCount = 1024, pollTimeout = 86400, ramBudget = 1ull << 30;
        int threads = int(std::thread::hardware_concurrency());
        std::string prefix, corpus, remap, out, bridgeDir, sampleCheck, wmPath;
        std::string checkClaimsDir;   // PROB-CHI: standalone checker mode
        std::string lcpColumnPath;    // KASAI: the prebuilt LCP column (--lcp-column)
        std::string locateIsaPath;    // KASAI: the locate table (--sa-locate)
        std::string runRanksPath;     // KASAI: prefix-sum run ranks (--run-ranks)
        std::string isaSamplePath;    // KASAI: sampled inverse anchors (--isa-sample)
        uint64_t repairChunkRuns = 0; // experimental exact-checkpoint planner
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
            else if (a == "--check-claims") checkClaimsDir = need("--check-claims");
            else if (a == "--lcp-column") lcpColumnPath = need("--lcp-column");
            else if (a == "--run-ranks") runRanksPath = need("--run-ranks");
            else if (a == "--isa-sample") isaSamplePath = need("--isa-sample");
            else if (a == "--sa-locate") locateIsaPath = need("--sa-locate");
            else if (a == "--output") out = need("--output");
            else if (a == "--bridge-dir") bridgeDir = need("--bridge-dir");
            else if (a == "--witness-sample-check") sampleCheck = need("--witness-sample-check");
            else if (a == "--wm") wmPath = need("--wm");
            else if (a == "--repair-chunk-runs") repairChunkRuns = strtoull(need("--repair-chunk-runs").c_str(), nullptr, 10);
            else if (a == "--chi-expect") chiExpect = strtoull(need("--chi-expect").c_str(), nullptr, 10);
            else if (a == "--sample-count") sampleCount = strtoull(need("--sample-count").c_str(), nullptr, 10);
            else if (a == "--poll-timeout") pollTimeout = strtoull(need("--poll-timeout").c_str(), nullptr, 10);
            else if (a == "--threads") threads = atoi(need("--threads").c_str());
            else if (a == "--wit-ram-budget") ramBudget = strtoull(need("--wit-ram-budget").c_str(), nullptr, 10);
            else if (a == "--keep-rlebwt") keepRlebwt = true;
            else if (a == "--keep-pfck") keepPfck = true;
            else pf_fail("unknown argument " + a);
        }
        pf_check(!checkClaimsDir.empty() ||
                 (!prefix.empty() && !corpus.empty() && !remap.empty() && !out.empty()),
                 "usage: xsa_finish --prefix MERGED/frag --corpus CORPUS --remap MAP.bin --output CONTAINER.sxi2 "
                 "[--bridge-dir DIR] [--witness-sample-check BANKED.sA] [--chi-expect N] [--threads N] "
                 "[--keep-rlebwt] [--keep-pfck] [--poll-timeout S] [--repair-chunk-runs N] "
                 "[--lcp-column COLUMN.sxlc --sa-locate ISA.sxis]");
        pf_check(!repairChunkRuns || repairChunkRuns >= 1024, "experimental repair chunks require at least 1024 runs");
        double last = tnow();
        pf_prof_start();
        if (const char* acc = getenv("XSA_FINISH_ACCESS"))
            g_pfAccessOn.store(*acc && *acc != '0', std::memory_order_relaxed);
        uint8_t remapTable[256];
        load_remap_table(remap, remapTable);
        if (!checkClaimsDir.empty()) {
            // PROB-CHI STANDALONE CHECKER: `--check-claims DIR --corpus C --remap M`.
            // No pieces, no merge, no rlebwt, no pfck, no structure - only the
            // claims and the text. This is the artifact that must be runnable at
            // the pile, where no banked container exists to cmp against.
            CorpusText text(corpus, remapTable);
            pf_check_claims(text, checkClaimsDir, threads);
            fprintf(stderr, "XSA_FINISH_CHECK_CLAIMS_DONE dir=%s corpus=%s n=%llu threads=%d\n",
                    checkClaimsDir.c_str(), corpus.c_str(), (unsigned long long)text.size(), threads);
            return 0;
        }
        PieceSource src;
        src.prefix = prefix;
        src.pollTimeout = pollTimeout;
        CorpusText text(corpus, remapTable);
        const uint64_t tau = pfck_spacing();
        std::string pfckPath = prefix + ".pfck2";
        // Start before either publication poll: only corpus + remap are needed.
        std::thread readiness([&] {
            build_pfck(text, text.size(), tau, threads, ramBudget, pfckPath);
        });
        src.load_idx();
        const uint64_t n = src.n, N = n + 10;
        uint64_t R = src.R;
        pf_check(text.size() == n, "corpus size != the merge's text length (adapter: T[i]=REMAP[corpus[i]])");

        // ---- KASAI: the prebuilt LCP column (kasai-finish), proved HERE, before
        // a byte of the sweep runs: n identity, the file's own arithmetic, and a
        // strictly ordered escape list. `XSA_FINISH_KASAI=0` keeps the old
        // SlimFingerprint walk on the identical inputs (the A/B arm).
        const bool kasaiOff = pf_env_u64("XSA_FINISH_KASAI", 1, 0, 1) == 0;
        const int kasaiAudit = int(pf_env_u64("XSA_FINISH_KASAI_AUDIT", 0, 0, 2));
        LcpColumn lcpCol;
        LocateIsa locateIsa;
        RunRankExc runRankExc;
        SampIsa sampIsa;
        TrueIsa trueIsa;
        const bool isaDebug = !pf_env_str("XSA_FINISH_ISA_DEBUG").empty();
        if (isaDebug) trueIsa.load(pf_env_str("XSA_FINISH_ISA_DEBUG"), n);
        uint64_t devMax = 0, devRuns = 0, devSum = 0;
        const bool runRankExcEmpty = runRanksPath.empty();   // false => use the prefix sum
        uint64_t rowCursor = 0;      // prefix-sum row cursor carried across bands
        KasaiState kasaiState;
        const bool kasaiOn = !lcpColumnPath.empty() && !kasaiOff;
        if (kasaiOn) {
            // RANKS COME FROM THE PREFIX SUM + EXCEPTIONS, not from a full ISA:
            // --run-ranks is the 2n-compatible supply; --sa-locate remains accepted
            // for the older flow (it is a 10.5 TB object at pile and must not be the
            // only path).
            // Ranks are DERIVED from the finish's own run stream (see PHASE 1), so
            // neither --run-ranks nor --sa-locate is required to run the arm. The ISA
            // is only the audit reference.
            pf_check(!kasaiAudit || !locateIsaPath.empty(),
                     "XSA_FINISH_KASAI_AUDIT=1 needs --sa-locate ISA.sxis as the reference");
            lcpCol.open(lcpColumnPath, n);
            if (!locateIsaPath.empty()) locateIsa.open(locateIsaPath, n);
            if (!runRanksPath.empty()) runRankExc.load(runRanksPath);
            if (!isaSamplePath.empty()) sampIsa.open(isaSamplePath, n);
        }
        else if (!lcpColumnPath.empty())
            fprintf(stderr, "XSA_FINISH_LCP_COLUMN_IGNORED path=%s reason=XSA_FINISH_KASAI=0\n",
                    lcpColumnPath.c_str());
        else
            pf_check(!kasaiAudit, "XSA_FINISH_KASAI_AUDIT=1 requires --lcp-column");
        fprintf(stderr, "XSA_FINISH_KASAI arm=%s audit=%d column=%s\n",
                kasaiOn ? "on" : "off", int(kasaiAudit),
                lcpColumnPath.empty() ? "none" : lcpColumnPath.c_str());

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
        fprintf(stderr, "XSA_FINISH_CYCLIC=1 has_rs=%d has_nl=%d k=1\n", int(hasRS), int(hasNL));

        // Default v2 protocol: bounded captured seam classes. Explicit
        // --wm retains legacy scratch compatibility outside the v2 route.
        PfWm wm;
        int wmFd = -1;
        std::string wmFile;   // captured for delete-as-consumed unlink below
        const bool useSpool = wmPath.empty();
        if (useSpool && !src.streaming) {
            double waited = 0;
            for (;;) {
                for (const auto& e : std::filesystem::directory_iterator(
                         std::filesystem::path(prefix).parent_path())) {
                    if (e.path().string().rfind(prefix + ".xsc-", 0) != 0 || !e.is_directory()) continue;
                    std::string candidate = e.path().string() + "/seam.spool";
                    struct stat st{};
                    if (::stat((candidate + ".done").c_str(), &st) != 0) continue;
                    pf_check(st.st_size == 8, "seam spool done size");
                    wmFd = ::open(candidate.c_str(), O_RDONLY);
                    pf_check(wmFd >= 0, "published seam spool missing");
                    wmFile = candidate; break;
                }
                if (wmFd >= 0) break;
                if (waited > pollTimeout) pf_fail("timeout waiting for the merge's seam spool + .done");
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); waited += 0.1;
            }
        } else if (!src.streaming) {
            const uint64_t packedExpect = PfWm::packed_size(n, src.w);
            // Packed completeness gate: blocks land in arbitrary order, so
            // the finish requires the merge's rename-published .done
            // sentinel (written after the last block) in addition to the
            // full size. The legacy layout keeps the banked size==8n rule.
            double waited = 0;
            for (;;) {
                struct stat st {};
                if (::stat(wmPath.c_str(), &st) == 0) {
                    uint64_t sz = uint64_t(st.st_size);
                    if (sz == 8*n || (sz == packedExpect && ::stat((wmPath + ".done").c_str(), &st) == 0)) {
                        wmFd = ::open(wmPath.c_str(), O_RDONLY); wmFile = wmPath; break;
                    }
                }
                if (waited > pollTimeout) pf_fail("timeout waiting for the merge's WM scratch");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                waited += 0.1;
            }
            pf_check(wmFd >= 0, "open WM scratch");
            wm.open(wmFd, n);
        }
        double joinStart = tnow();
        readiness.join();
        fprintf(stderr, "XSA_FINISH_PFCK_WAIT wall=%.3f overlapped=1\n", tnow() - joinStart);
        phase("readiness-pfck", last);
        if (pf_cache_journal_on()) pf_cache_journal(text, "readiness-pfck", 1024);
        uint64_t ckptCount = n / tau + 1;
        int pfckFd = ::open(pfckPath.c_str(), O_RDONLY);
        pf_check(pfckFd >= 0, "reopen pfck");
        auto fhSeamOwner = std::make_unique<SlimFingerprint<CorpusText>>(text, tau, false, pfckFd, 40, ckptCount);
        auto& fhSeam = *fhSeamOwner;
        SlimSeamWork workSeam(std::max<uint64_t>(100000000ULL, 16 * n), UINT64_MAX);
        fhSeam.seam_policy(workSeam, tau, "pile-finish-seam");
        fp_self_check(fhSeam, text, n, tau);
        auto fhSlimOwner = std::make_unique<SlimFingerprint<CorpusText>>(text, tau, false, pfckFd, 40, ckptCount);
        auto& fhSlim = *fhSlimOwner;
        SlimSeamWork workSlim(std::max<uint64_t>(1000000000ULL, 128 * n), UINT64_MAX);
        fhSlim.seam_policy(workSlim, tau, "pile-finish-corpus");
        fp_self_check(fhSlim, text, n, tau);
        // ---- PROB-CHI: install the tier. TRUST=1 spools a position-bucketed
        // claims list (checked later / standalone), TRUST=2 checks fused in the
        // proposing worker. Both take exactness away from the constructor and
        // hand it to the checker; nothing about the ANSWERS changes (the sink
        // path returns the same `guess` the verifier would have confirmed), so
        // a clean corpus must produce a byte-identical container either way.
        const int trustMode = int(pf_env_u64("XSA_FINISH_LCE_TRUST", 0, 0, 3));
        const std::string claimsDir = [] {
            const char* e = std::getenv("XSA_FINISH_CLAIMS_DIR");
            return std::string(e ? e : "");
        }();
        std::unique_ptr<PfClaimSpool> claimSpool;
        PfClaimFused fusedSink;
        PfClaimNull nullSink;
        fusedSink.text = &text;
        if (trustMode == 1) {
            // The claims tier exists to certify the LCE WALKS. In the Kasai arm
            // there are no band walks left to claim about, so the tier would
            // certify nothing while looking like a gate: refuse.
            pf_check(!kasaiOn, "XSA_FINISH_LCE_TRUST=1 is incompatible with the Kasai arm "
                               "(no LCE queries to claim); use TRUST=0/2/3 or XSA_FINISH_KASAI=0");
            pf_check(!claimsDir.empty(), "XSA_FINISH_LCE_TRUST=1 requires XSA_FINISH_CLAIMS_DIR");
            claimSpool = std::make_unique<PfClaimSpool>(claimsDir, n);
            fhSeam.sink = claimSpool.get();
            fhSlim.sink = claimSpool.get();
        } else if (trustMode == 2) {
            fhSeam.sink = &fusedSink;
            fhSlim.sink = &fusedSink;
        } else if (trustMode == 3) {
            fhSeam.sink = &nullSink;   // FLOOR ARM: measure, certify nothing
            fhSlim.sink = &nullSink;
        }
        const uint64_t lceFault = pf_env_u64("XSA_FINISH_LCE_FAULT", 0, 0, 1ull << 40);
        const bool trackDegrees = pf_env_u64("XSA_FINISH_PROBE_DEGREES", 0, 0, 1) != 0;
        fhSeam.faultEvery = fhSlim.faultEvery = lceFault;
        fhSeam.trackDegrees = fhSlim.trackDegrees = trackDegrees;
        fprintf(stderr, "XSA_FINISH_TIER lce_trust=%d claims_dir=%s lce_fault=%llu probe_degrees=%d "
                        "tier=%s constructor=%s\n",
                trustMode, claimsDir.empty() ? "-" : claimsDir.c_str(),
                (unsigned long long)lceFault, int(trackDegrees),
                trustMode == 0 ? "T0-full" :
                (trustMode == 3 ? "T2-floor-only(measurement,not-a-tier)" : "T3-hash-trust+checker"),
                trustMode == 0 ? "byte-verifies-every-answer" : "emits-claims-only");
        phase("fingerprints", last);

        LceWrap seamLce{&fhSeam, {}, n};
        SeamClasses seam;
        auto seamSymbol = [&](uint64_t pos) -> uint8_t {
            if (pos >= n) pf_fail("seam symbol outside text");
            return text[pos];
        };
        if (src.streaming) {
            seam.fromSpool = true; seam.depth = src.depth;
        } else if (useSpool) {
            seam.build_spool(wmFd, n, R,
                [&](uint64_t a, uint64_t b) { return seamLce(a, b); }, seamSymbol);
            int doneFd = ::open((wmFile + ".done").c_str(), O_RDONLY);
            pf_check(doneFd >= 0, "open seam spool done");
            uint64_t expected; pf_pread_full(doneFd, &expected, 8, 0, "seam spool done");
            ::close(doneFd); pf_check(expected == seam.total, "seam spool done row count");
        } else {
            seam.build(text, wm, n, R, 10,
                       [&](uint64_t a, uint64_t b) { return seamLce(a, b); }, seamSymbol);
        }
        if (wmFd >= 0) ::close(wmFd);
        wmFd = -1;
        // Last-consumer deletion: merge.active keeps the directory alive
        // during sparse replay. Both sides best-effort-remove the empty dir,
        // so either exit ordering removes it after the final unlink.
        if (!wmFile.empty()) {
            int removed = ::unlink(wmFile.c_str());
            if (useSpool) pf_check(removed == 0, "unlink consumed seam spool");
            removed = ::unlink((wmFile + ".done").c_str());
            if (useSpool) pf_check(removed == 0, "unlink consumed seam spool done");
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(wmFile).parent_path(), ec);
            if (ec)
                std::fprintf(stderr, "WM_DIR_REMOVE_FAILED path=%s err=%s\n",
                             wmFile.c_str(), ec.message().c_str());
            else if (std::filesystem::exists(std::filesystem::path(wmFile).parent_path()))
                std::fprintf(stderr, "WM_DIR_REMOVE_FAILED_STILL_THERE path=%s\n",
                             wmFile.c_str());
        }
        phase("seam-classes", last);
        if (pf_cache_journal_on()) pf_cache_journal(text, "seam-classes", 1024);

        // ---- scratch writers
        WitBuckets buckets(n, R, prefix, ramBudget);
        std::string sparsePath = prefix + ".sparse2";
        pf_check(::access(sparsePath.c_str(), F_OK) != 0, "sparse scratch exists");
        int sparseFd = ::open(sparsePath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        pf_check(sparseFd >= 0, "create sparse scratch");
        bool bridge = !bridgeDir.empty();
        int bCharsFd = -1, bLensFd = -1, bTailsFd = -1, bSAFd = -1, bTopFd = -1, bIntFd = -1;
        unsigned w4 = 1;
        while (w4 < 64 && (uint64_t(1) << w4) <= n - 1) ++w4;
        __uint128_t bTailRes = 0;
        unsigned bTailAvail = 0;
        if (bridge) {
            bCharsFd = ::open((bridgeDir + "/frag.b-chars").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            bLensFd = ::open((bridgeDir + "/frag.b-lens").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            bTailsFd = ::open((bridgeDir + "/frag.b-tails").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            bSAFd = ::open((bridgeDir + "/frag.sA").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            bTopFd = ::open((bridgeDir + "/frag.b-top").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            bIntFd = ::open((bridgeDir + "/frag.b-int").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            pf_check(bCharsFd >= 0 && bLensFd >= 0 && bTailsFd >= 0 && bSAFd >= 0 && bTopFd >= 0 && bIntFd >= 0, "create bridge temps");
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
        // finish-decomposed: one band's buffered fold output (the runs the
        // repair transducer emitted for that band, their global run indices,
        // and the LCE columns the verification stage fills in). Two of these
        // exist so band k's VERIFICATION can run on the pool while band k+1's
        // STATE MAINTENANCE (feed_raw) runs on this thread. A slot is a pure
        // per-band buffer: nothing in it is a summary of a boundary, and the
        // exact transducer state (rs) still transits every band on one thread.
        struct BandSlot {
            std::vector<NRun> nbuf;
            std::vector<uint64_t> gidx;   // global normalized run index per nbuf entry
            std::vector<uint64_t> rk;     // KASAI: the run's BWT rank base (NRun::rk)
            std::vector<uint64_t> topLcp, interior;
            std::vector<RawRun> raws;
            uint64_t prevTail = 0;
            size_t cnt = 0;
            explicit BandSlot(size_t reserve) { nbuf.reserve(reserve); gidx.reserve(reserve); rk.reserve(reserve); }
        };
        BandSlot slots[2]{BandSlot(1 << 17), BandSlot(1 << 17)};
        BandSlot* feedTarget = &slots[0];
        uint64_t prevTail = 0;
        RepairStream rs;
        rs.N = N; rs.n = n; rs.terminal = 0; rs.seam = &seam;
        // The band loop's sink. Passed DIRECTLY as the transducer's template sink
        // (hot path, inlined) and also installed as rs.emit for the final flush,
        // so both paths emit through identical code.
        auto bandSink = [&](const NRun& v) {
            // Sub-phase tag: the sampler separates the transducer's own work
            // from the sink callback below (this store is ~1 ns/run and is the
            // only cost the instrument adds to the fold).
            g_pfPhase.store(PF_EMIT, std::memory_order_relaxed);
            // emitOne() has already incremented rNorm: this run's global
            // index is rNorm-1 (0-based; sparse m11 tails live at indices
            // 0, 1024, 2048, ...).
            uint64_t idx = rs.rNorm - 1;
            if (idx % 1024 == 0) pf_write_full(sparseFd, &v.t, 8, "sparse tail");
            if (bridge) {
                pf_write_full(bCharsFd, &v.c, 1, "bridge chars");
                uint32_t len32 = uint32_t(v.len);
                pf_write_full(bLensFd, &len32, 4, "bridge lens");
                bridgeTailWrite(v.t);
            }
            feedTarget->nbuf.push_back(v);
            feedTarget->gidx.push_back(idx);
            feedTarget->rk.push_back(v.rk);
            g_pfPhase.store(PF_REPAIR, std::memory_order_relaxed);
        };
        rs.emit = bandSink;
        sweep.emitWitness = [&](uint64_t value) {
            buckets.add(value);
            if (bridge) pf_write_full(bSAFd, &value, 8, "bridge sA");
        };

        // ---- main band loop: pieces -> repair -> LCE -> sweep -> witnesses
        double lceWall = 0, repairWall = 0, advanceWall = 0;
        // v3 attribution: the repair wall is a pacing diagnostic that absorbs
        // piece-wait; these split it into WAIT (polling the producer's wire) and
        // WORK (band decode + the repair transducer), and count the units each
        // one moves so every wall has a per-unit rate.
        double waitWall = 0, readWall = 0, feedWall = 0, rlebwtWaitWall = 0;
        PfAcc waitAcc{"piece-wait"}, readAcc{"readband"}, feedAcc{"repair-feed"};
        PfAcc lceAcc{"lce"}, advAcc{"advance-witness"};
        uint64_t rawRunsFed = 0, bandReads = 0, advRuns = 0;
        pf_access_reset();
        std::array<double, 3> repairChunkWall{};
        uint64_t nbands = (R + src.B - 1) / src.B;
        uint64_t expectedRlebwtBytes = 0;
        // The verification pool. Default is --threads (the gate's knob); the
        // env override exists only to measure the fold/verification contention
        // curve, and at 100m it says 96 beats 64 and 48 (the verification is the
        // binding term, so giving it fewer threads is strictly worse).
        int poolThreads = std::max(1, std::min(threads, 256));
        if (const char* pt = getenv("XSA_FINISH_POOL")) {
            long v = std::strtol(pt, nullptr, 10);
            pf_check(v >= 1 && v <= 256, "XSA_FINISH_POOL out of range");
            poolThreads = int(v);
        }
        FinishPool pool(poolThreads);
        // ---- PART 2/B: THE BAND PREFETCHER (read ahead of the verified band)
        // OFF by default: the product's lazy demand path is unchanged unless the
        // operator asks for an arm (XSA_FINISH_PREFETCH=1 sweep | 2 scattered).
        // Depth (`XSA_FINISH_PREFETCH_JOBS`) and the per-job staging budget
        // (`XSA_FINISH_PREFETCH_BUDGET`) are the balance curve's two knobs: how
        // many bands of targets are in flight, and how much of the merged front
        // one job may stage.
        std::unique_ptr<PfPrefetcher> pfq;
        {
            const int pmode = pf_prefetch_mode();
            if (pmode) {
                pfq = std::make_unique<PfPrefetcher>(&text);
                pfq->mode = pmode;
                const char* a = std::getenv("XSA_FINISH_PREFETCH_ASSIST");
                if (a && *a) {
                    const std::string s(a);
                    pfq->assist = s == "willneed" ? 1 : (s == "both" ? 2 : (s == "populate" ? 0 : -1));
                    pf_check(pfq->assist >= 0, "XSA_FINISH_PREFETCH_ASSIST must be populate|willneed|both");
                }
                pfq->windowBytes = pf_env_u64("XSA_FINISH_PREFETCH_WINDOW", 256, 1, 1ull << 26);
                pfq->budgetBytes = pf_env_u64("XSA_FINISH_PREFETCH_BUDGET", 32ull << 20, 4096, 1ull << 34);
                pfq->gap = pf_env_u64("PILE_PREFETCH_GAP", 1ull << 16, 0, 1ull << 30);
                pfq->chunkBytes = pf_env_u64("XSA_FINISH_PREFETCH_CHUNK", 4ull << 20, 4096, 1ull << 32);
                pfq->JOBS = unsigned(pf_env_u64("XSA_FINISH_PREFETCH_JOBS", 2, 1, 64));
                // The scattered arm is DEFINED as "issue every target window as it
                // arrives", so it does not consult residency (and must not: at 256 B
                // per window that would be one mincore syscall per window, i.e. 78M
                // of them on the 1 GB fixture - measured at 132 s of syscall time).
                pfq->skipResident = pmode == 1 &&
                    pf_env_u64("XSA_FINISH_PREFETCH_SKIP_RESIDENT", 1, 0, 1) != 0;
                pfq->report();   // the POLICY line, before the first band
            }
        }
        // ---- PART C: THE COLD-CORPUS TRICK ----------------------------------
        // `XSA_FINISH_COLD_CORPUS=1` drops the WHOLE corpus from the page cache
        // immediately before the band sweep starts, and `XSA_FINISH_COLD_EVERY=n`
        // drops it again every n bands (the pile regime: the text cannot stay
        // resident through a pass). The drops are counted and timed, never silent.
        struct ColdCorpus {
            const CorpusText* text = nullptr;
            uint64_t every = 0, drops = 0, bytes = 0;
            double wall = 0;
            void drop() {
                if (!text) return;
                ++drops; bytes += text->size();
                const double t0 = tnow(); text->cold_all(); wall += tnow() - t0;
            }
            void report() const {
                fprintf(stderr, "XSA_FINISH_COLD_CORPUS enabled=%d every_bands=%llu drops=%llu "
                        "bytes=%llu wall=%.3f corpus_gb=%.3f\n",
                        int(text != nullptr), (unsigned long long)every,
                        (unsigned long long)drops, (unsigned long long)bytes, wall,
                        text ? double(text->size()) / 1e9 : 0.0);
            }
        } cold;
        if (pf_env_u64("XSA_FINISH_COLD_CORPUS", 0, 0, 1)) {
            cold.text = &text;
            cold.every = pf_env_u64("XSA_FINISH_COLD_EVERY", 0, 0, 1ull << 34);
        }
        if (repairChunkRuns) fprintf(stderr, "XSA_FINISH_REPAIR_CHUNKS runs=%llu reorder=one_ready_band planner=exact_serial_replay experimental=1\n", (unsigned long long)repairChunkRuns);
        // sol6b audit: the top/interior query set for a band is a pure function
        // of that band's own buffered runs (its head/tail SA coordinates, its
        // gidx, and the previous band's last tail). It does not read the
        // transducer, the seam cursor, or any producer state, which is exactly
        // why it can be moved off this thread without changing one query.
        //
        // ---- the fold rides the verification (finish-decomposed) ----------
        // STEP 0 measured the defect: read -> feed_raw (SERIAL) -> LCE (92x
        // parallel) -> advance (SERIAL) were strictly serialised per band, so
        // the sweep wall was the SUM of a parallel stage and two serial ones.
        // The verification of band k is a pure function of that band's own
        // buffered runs (its head/tail SA coordinates, its gidx, the previous
        // band's last tail - the sol6b audit) plus the immutable corpus and
        // PFCK sidecar, so it can run on the pool while the ordered fold
        // (feed_raw) of band k+1 proceeds on this thread. The query multiset,
        // its order and its arguments are unchanged; only the schedule moves.
        auto lceJob = [&](BandSlot& s) {
            size_t cnt = s.cnt;
            std::vector<NRun>& nb = s.nbuf;
            std::vector<uint64_t>& gx = s.gidx;
            uint64_t prevTailS = s.prevTail;
            // The claim counter MUST be shared by every worker of this job:
            // a per-thread local would make each worker walk the whole band
            // (96x the queries, which the seam-work journal refuses loudly).
            auto next = std::make_shared<std::atomic<size_t>>(0);
            if (!kasaiOn) {
                // THE OLD ARM (`XSA_FINISH_KASAI=0`), unchanged: every answer is
                // a full SlimFingerprint LCE walk, re-verified symbol by symbol.
                return std::function<void(int)>([&, cnt, &nb, &gx, prevTailS, next](int t) {
                    slim_set_thread_shard(unsigned(t));
                    for (;;) {
                        size_t i = next->fetch_add(1);
                        if (i >= cnt) break;
                        const NRun& v = nb[i];
                        uint64_t first = v.h, tail = v.t;
                        pf_check(first < n, "position resolution failed");
                        if (v.len == 1) pf_check(first == tail, "head-SA value/range or singleton mismatch");
                        pf_check(tail < n, "position resolution failed");
                        uint64_t prev = gx[i] == 0 ? 0 : (i ? nb[i - 1].t : prevTailS);
                        pf_check(prev < n, "previous position resolution failed");
                        s.topLcp[i] = gx[i] ? slimLce(prev, first) : 0;
                        s.interior[i] = v.len == 1 ? INF : slimLce(first, tail);
                    }
                    slimLce.flush_thread();
                });
            }
            // ---- THE KASAI ARM --------------------------------------------------
            // rank = ISA[head] from the LOCATE TABLE (exact), the value from ONE
            // staged column slice, and - only where the run stream skips a rank
            // (the seam-class holes) - the old walk for that single query, because
            // an adjacent-rank LCP is not defined across a hole.
            //
            // Two phases inside ONE pool job: the lookups are random reads and
            // must be parallel, but the slice bounds are only known once every
            // run's rank is. The last worker to arrive stages the slice, the rest
            // wait on a flag; the claim counter is reset behind that barrier.
            auto headRank = std::make_shared<std::vector<uint64_t>>(cnt);
            auto tailRank = std::make_shared<std::vector<uint64_t>>(cnt);
            auto band = std::make_shared<LcpColumn::Band>();
            auto arrived = std::make_shared<std::atomic<int>>(0);
            auto staged = std::make_shared<std::atomic<int>>(0);
            const int P = std::max(1, poolThreads);
            const uint64_t prevEndS = kasaiState.lastEndRank;
            const uint64_t prevHeadS = kasaiState.lastHeadRank;
            const bool firstBand = kasaiState.bands == 0;
            ++kasaiState.bands;
            return std::function<void(int)>(
                [&, cnt, &nb, &gx, prevTailS, next, headRank, band, arrived, staged,
                 prevEndS, prevHeadS, firstBand, P, tailRank](int t) {
                slim_set_thread_shard(unsigned(t));
                // PHASE 1: every run's exact rank WITHOUT an inverse.
                //
                // THE DESIGN: ranks ARE cumulative run lengths over the run list the
                // finish itself emits. So the cursor below is not an estimate that
                // needs correcting - it is the definition. An earlier version shipped
                // a producer-side exception table keyed by run index, then by head
                // position; both drifted, because the repair changes run LENGTHS and
                // every length change shifts every later rank. Nothing producer-side
                // can survive that; the finish's own stream is the only authority.
                //
                // The full ISA (--sa-locate, 10.5 TB at pile) is therefore a
                // REFERENCE, used only by the audit arm to check every derived rank.
                {
                    uint64_t cursor = rowCursor;
                    for (size_t i = 0; i < cnt; ++i) {
                        // TRAILING PADDING RUNS live ABOVE n (the bin-walk seeds
                        // n..n+9 plus the seam rows; CLOSURE's max_rank_end is n-2,
                        // so the real content ends there). A run that STRADDLES the
                        // end - the seam run covering the last text bytes AND the
                        // padding rows - is treated the same way: it never indexes
                        // the column, and clamping its tail would fake a
                        // non-contiguity (measured: it turned noncontiguous_runs from
                        // 0 into 1 and then failed 'an interior run collapsed').
                        if (cursor >= n || cursor + nb[i].len > n) {
                            // sentinel: outside the column's rank range, so downstream
                            // skips this run entirely (it is padding / the seam tail)
                            (*headRank)[i] = n;
                            (*tailRank)[i] = n;
                            cursor += nb[i].len;
                            continue;
                        }
                        const uint64_t h = cursor;
                        const uint64_t t = nb[i].len >= 2 ? cursor + nb[i].len - 1 : h;
                        if (kasaiAudit >= 1) {
                            pf_check(locateIsa.at(nb[i].h) == h,
                                     "KASAI AUDIT: derived head rank != ISA");
                            if (nb[i].len >= 2)
                                pf_check(locateIsa.at(nb[i].t) == t,
                                         "KASAI AUDIT: derived tail rank != ISA");
                        }
                        if (isaDebug) {
                            const uint64_t truth = trueIsa.at(nb[i].h);
                            const uint64_t dev = truth > h ? truth - h : h - truth;
                            if (dev) { ++devRuns; devSum += dev; }
                            if (dev > devMax) devMax = dev;
                        }
                        (*headRank)[i] = h;
                        (*tailRank)[i] = t;
                        cursor += nb[i].len;
                    }
                    rowCursor = cursor;
                }
                (void)next;
                // BARRIER: last arrival stages the slice.
                if (arrived->fetch_add(1) + 1 == P) {
                    uint64_t lo = ~uint64_t(0), hi = 0;
                    for (size_t i = 0; i < cnt; ++i) {
                        // Both ENDPOINT ranks are what the column answers need:
                        // a run's own rows can skip a rank (a hole inside a run),
                        // so hi is not headRank+len-1 in general.
                        // The sentinel rank (n) marks padding runs: it must not
                        // extend the band's column slice, which lives in [0,n-1].
                        if ((*headRank)[i] < n) lo = std::min(lo, (*headRank)[i]);
                        if ((*tailRank)[i] < n) {
                            lo = std::min(lo, (*tailRank)[i]);
                            hi = std::max(hi, (*tailRank)[i]);
                        }
                        if ((*headRank)[i] < n) hi = std::max(hi, (*headRank)[i]);
                        if (nb[i].len >= 2 && (*tailRank)[i] != (*headRank)[i] + nb[i].len - 1)
                            kasaiState.noncontig += 1;
                        // The stream is ascending EXCEPT for the terminal row,
                        // which the transducer emits late (measured: rank 1186920
                        // arrives after rank 1194188 at 100 MB). Order is not
                        // needed for correctness - only ADJACENCY is (the walk
                        // fallback covers the rest) - so this is counted, not
                        // refused.
                        if (i && (*headRank)[i] <= (*headRank)[i - 1])
                            kasaiState.disorder += 1;
                        const uint64_t prevEnd = i ? (*headRank)[i - 1] + nb[i - 1].len : prevEndS;
                        if (prevEnd != (*headRank)[i]) kasaiState.gaps += 1;
                    }
                    if (!firstBand && (*headRank)[0] <= prevHeadS) kasaiState.disorder += 1;
                    const uint64_t base = lo ? lo - 1 : 0;
                    lcpCol.load(*band, base, hi - base + 1);
                    kasaiState.lastHeadRank = (*headRank)[0];
                    kasaiState.lastEndRank = (*headRank)[cnt - 1] + nb[cnt - 1].len;
                    next->store(0);
                    staged->store(1, std::memory_order_release);
                }
                while (!staged->load(std::memory_order_acquire)) std::this_thread::yield();
                // PHASE 2: the column reads.
                for (;;) {
                    const size_t i = next->fetch_add(1);
                    if (i >= cnt) break;
                    const NRun& v = nb[i];
                    const uint64_t r = (*headRank)[i];
                    const uint64_t prevEnd = i ? (*headRank)[i - 1] + nb[i - 1].len : prevEndS;
                    uint64_t top = 0;
                    if (gx[i]) {
                        if (prevEnd == r) top = band->at(r - 1);
                        else {
                            // THE HOLE: the run stream skipped a rank, so this
                            // run's predecessor is not rank-adjacent and col[r-1]
                            // is not the answer. One walk, counted (the holes are
                            // the seam classes: 4 at 100 MB, 30 at 1 GB).
                            const uint64_t prevPos = i ? nb[i - 1].t : prevTailS;
                            pf_check(prevPos < n, "previous position resolution failed");
                            top = slimLce(prevPos, v.h);
                            kasaiState.fallback.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    uint64_t inter = INF;
                    if (v.len >= 2 && r < n && (*tailRank)[i] < n) {
                        // min-LCP over the rank interval between the run's two
                        // ENDPOINT RANKS (exact even when the run's own rows skip
                        // a rank: LCE(SA[a],SA[c]) = min col[a..c-1] for any a<c).
                        const uint64_t rt = (*tailRank)[i];
                        const uint64_t a = std::min(r, rt), c = std::max(r, rt);
                        pf_check(c > a, "KASAI: an interior run collapsed to one rank");
                        inter = ~uint64_t(0);
                        for (uint64_t t2 = a; t2 < c; ++t2) inter = std::min(inter, band->at(t2));
                        pf_check(inter != ~uint64_t(0), "empty interior interval");
                    }
                    if (kasaiAudit) {
                        // THE AUDIT ARM: the column answer must equal the LCE walk
                        // it replaces, entry for entry - the exactness gate at
                        // fixture scale (KASAI_FINISH_DESIGN.md gate 6).
                        const uint64_t first = v.h, tail = v.t;
                        pf_check(first < n && tail < n, "position resolution failed");
                        if (v.len == 1) pf_check(first == tail, "head-SA value/range or singleton mismatch");
                        const uint64_t prev = gx[i] == 0 ? 0 : (i ? nb[i - 1].t : prevTailS);
                        const uint64_t topOld = gx[i] ? slimLce(prev, first) : 0;
                        const uint64_t intOld = v.len == 1 ? INF : slimLce(first, tail);
                        if (topOld != top) {
                            if (kasaiAudit >= 2 && g_kasaiDbg.fetch_add(1) == 0) {
                                const size_t lo = i > 80 ? i - 80 : 0;
                                for (size_t j = lo; j <= i + 4 && j < cnt; ++j)
                                    fprintf(stderr, "KASAI_RUN j=%zu rank=%llu len=%llu head=%llu tail=%llu\n",
                                            j, (unsigned long long)(*headRank)[j],
                                            (unsigned long long)nb[j].len,
                                            (unsigned long long)nb[j].h,
                                            (unsigned long long)nb[j].t);
                            }
                            if (kasaiAudit < 2)
                                pf_fail("KASAI AUDIT topLcp mismatch at run " + std::to_string(r) +
                                        ": column=" + std::to_string(top) + " walk=" + std::to_string(topOld));
                            pf_check(g_kasaiDbg.load() <= 64,
                                     "KASAI AUDIT: column and walk disagree (see KASAI_DBG lines)");
                        }
                        if (intOld != inter)
                            pf_fail("KASAI AUDIT interior mismatch at run " + std::to_string(r) +
                                    ": column=" + std::to_string(inter) + " walk=" + std::to_string(intOld));
                    }
                    s.topLcp[i] = top;
                    s.interior[i] = inter;
                }
                if (kasaiAudit) slimLce.flush_thread();
            });
        };
        // Verification dispatched, ordered fold continues on this thread.
        BandSlot* lcePending = nullptr;
        double lceStart = 0, lceWaitWall = 0, overlapWorkWall = 0;
        auto dispatchLce = [&](BandSlot& s) {
            pf_check(!lcePending, "lce already dispatched");
            s.topLcp.assign(s.cnt, 0);
            s.interior.assign(s.cnt, INF);
            lcePending = &s;
            lceStart = tnow();
            pool.start(lceJob(s));
        };
        auto awaitLce = [&]() {
            if (!lcePending) return;
            double t0 = tnow();
            pool.wait();
            lceWaitWall += tnow() - t0;
            lceWall += tnow() - lceStart;
            lcePending = nullptr;
        };
        // Stage 3 (serial, ordered): the state machine must see every run in
        // stream order and the witnesses must reach the buckets in the same
        // order as before; only this stage may not move.
        auto finishAdvance = [&](BandSlot& s) {
            if (!s.cnt) return;
            if (bridge)
                for (size_t i = 0; i < s.cnt; ++i) {
                    pf_write_full(bTopFd, &s.topLcp[i], 8, "bridge top");
                    uint64_t iv = s.interior[i] == INF ? ~uint64_t(0) : s.interior[i];
                    pf_write_full(bIntFd, &iv, 8, "bridge int");
                }
            {
                PfRegion r("advance-witness", PF_ADVANCE);
                double ta = tnow();
                for (size_t i = 0; i < s.cnt; ++i) sweep.advance(s.nbuf[i], s.topLcp[i], s.interior[i]);
                advanceWall += tnow() - ta;
                r.close(); advAcc.add(r);
            }
            advRuns += s.cnt;
            // prevTail belongs to the FEED stage (it is the last tail emitted by
            // the most recently fed band, i.e. the neighbour for the next
            // band's first query). It must NOT be written here: with the
            // pipeline the next band is fed BEFORE this advance runs, so
            // restamping it from an older slot silently changes one top-LCP
            // query per band boundary (measured: +571 witnesses at 100m).
            s.nbuf.clear();
            s.gidx.clear();
            s.rk.clear();
            s.cnt = 0;
        };
        // Stage 1 (serial, ordered): the exact transducer transits every band
        // boundary on this thread - no summaries, no cold reconstruction.
        auto feed = [&](uint64_t band, BandSlot& s) -> bool {
            double t0 = tnow();
            double tw = 0;
            if (src.streaming) {
                {
                    PfRegion r("ready-band", PF_PIECEWAIT);
                    if (!src.ready_band(band)) { r.close(); return false; }
                    r.close(); waitAcc.add(r); tw += r.wall;
                }
                SeamClasses batch; batch.fromSpool = true; batch.depth = src.depth;
                while (src.delivered < src.completed.size()) {
                    const auto& values = src.completed[src.delivered++];
                    SeamClass cl{values[3],values[4],values[5],values[6],values[7],{}, {}};
                    cl.occ.assign(values.begin()+8,values.end()); batch.total += cl.occ.size();
                    batch.classes.push_back(std::move(cl));
                }
                if (!batch.classes.empty()) {
                    batch.rebuild(n,src.R,1u<<26,[&](uint64_t a,uint64_t b){return seamLce(a,b);},seamSymbol);
                    seam.total += batch.total; seam.maxSeamLce = std::max(seam.maxSeamLce,batch.maxSeamLce);
                    for (auto& cl : batch.classes) seam.classes.push_back(std::move(cl));
                }
                if (src.terminal) { R=src.R; nbands=src.finalBand+1; }
            }
            {
                PfRegion r("readband", PF_READBAND);
                src.read_band(band, s.raws);
                r.close(); readAcc.add(r); readWall += r.wall;
            }
            // PART 2/B: hand the NEXT band's verification targets to the
            // prefetcher NOW - the ordered fold below (and the previous band's
            // verification) is the window the read-ahead has to work in.
            if (pfq && !s.raws.empty()) {
                PfRegion r("prefetch-submit", PF_IDLE);
                pfq->submit(s.raws, n, 0);
                r.close();
            }
            waitWall += tw;
            ++bandReads;
            if (band == 0) {
                pf_check(!s.raws.empty() && s.raws[0].len == 1, "run zero is not the terminal padding row");
                rs.terminal = s.raws[0].c;
                pf_check(rs.terminal >= 6 && rs.terminal < 256, "unsupported terminal byte");
                fprintf(stderr, "PFP_TERMINAL byte=0x%02llx source=padding-row raw_n=%llu raw_r=%llu\n",
                        (unsigned long long)rs.terminal, (unsigned long long)N, (unsigned long long)R);
            }
            s.prevTail = prevTail;
            pf_check(s.nbuf.empty() && s.gidx.empty() && s.rk.empty(), "band slot not empty at feed");
            feedTarget = &s;
            {
                PfRegion r("repair-feed", PF_REPAIR);
                for (const auto& raw : s.raws) {
                    expectedRlebwtBytes += 4 * ((raw.len + 0x7fffff - 1) / 0x7fffff);
                    if (!repairChunkRuns) rs.feed_raw(raw, bandSink);
                }
                r.close(); feedAcc.add(r); feedWall += r.wall;
            }
            rawRunsFed += s.raws.size();
            if (repairChunkRuns) {
                auto wall = repair_chunks(rs, s.raws, pool, repairChunkRuns);
                for (size_t i=0; i<wall.size(); ++i) repairChunkWall[i] += wall[i];
            }
            repairWall += tnow() - t0;
            s.cnt = s.nbuf.size();
            if (s.cnt) prevTail = s.nbuf.back().t;
            // Flow credit. read_stream_band already unlinked this band's piece
            // and copied its raw runs into RAM; the transducer has consumed
            // them; everything left for this band (LCE + advance) touches only
            // this slot. Declaring consumption here - one stage earlier than
            // the pre-decomposition loop - is what lets the producer publish
            // band k+1 while this band's VERIFICATION is still running, and it
            // keeps consumed(k) BEFORE ready(k+1) so a W=1 window cannot
            // deadlock. The wire shape is untouched (one integer, one file).
            if (src.streaming) src.consumed_band(band);
            return true;
        };
        // Pipeline depth 1: the ordered stages alternate with the verification
        // of the previous band. `pipelined` is off only for the experimental
        // exact-checkpoint planner, which reuses the same pool internally.
        const bool pipelined = !repairChunkRuns;
        uint64_t band = 0;
        int cur = 0;
        // PART C: the cold-corpus arm starts here - the text is dropped from the
        // page cache AFTER every setup read (pfck, fingerprints, seam classes)
        // and immediately before the band sweep, so the sweep is the thing that
        // pays the device reads.
        if (cold.text) {
            cold.drop();
            // The cold claim, measured: residency sampled right after the drop.
            if (pf_cache_journal_on()) pf_cache_journal(text, "cold-start", 64);
        }
        bool active = feed(band, slots[cur]);
        if (active && slots[cur].cnt) dispatchLce(slots[cur]);
        while (active) {
            int nxt = cur ^ 1;
            bool more = false;
            // In streaming mode the band count is the producer's to grow, so
            // the next feed is always attempted and ready_band answers; in
            // file mode the band count is fixed here.
            bool wantNext = src.streaming || band + 1 < nbands;
            if (wantNext) {
                if (pipelined) {
                    double t0 = tnow();
                    // State maintenance of band k+1 runs while band k's
                    // verification finishes.
                    more = feed(band + 1, slots[nxt]);
                    overlapWorkWall += tnow() - t0;
                    awaitLce();
                    // Start the NEXT verification before the ordered advance:
                    // advance(k) needs only slot k's finished columns and
                    // slot k+1 is already fed and waiting, so the serial
                    // advance no longer costs a band's worth of verification
                    // slot (measured: the fold alone is 29 ns/raw run, the
                    // verification is 13 ms/band, so leaving the pool idle
                    // through the advance was the largest remaining hole).
                    if (more && slots[nxt].cnt) dispatchLce(slots[nxt]);
                } else {
                    awaitLce();
                    more = feed(band + 1, slots[nxt]);
                }
            } else {
                awaitLce();
            }
            finishAdvance(slots[cur]);
            if ((band & 63) == 0)
                fprintf(stderr, "XSA_FINISH_PROGRESS band=%llu/%llu runs=%llu chi_so_far=%llu repair_wall=%.1f lce_wall=%.1f lce_wait=%.1f advance_wall=%.1f wit_spills=%llu\n",
                        (unsigned long long)(band + 1), (unsigned long long)nbands,
                        (unsigned long long)rs.rNorm, (unsigned long long)sweep.chi,
                        repairWall, lceWall, lceWaitWall, advanceWall, (unsigned long long)buckets.spilled);
            if (cold.every && (band + 1) % cold.every == 0) cold.drop();
            if (pf_cache_journal_on() && (band & 63) == 0) {
                char label[64];
                std::snprintf(label, sizeof label, "band-%llu", (unsigned long long)(band + 1));
                pf_cache_journal(text, label, 1024);
            }
            if (!more) break;
            cur = nxt;
            ++band;
        }
        awaitLce();
        if (pfq) pfq->drain();
        // CACHE RESIDENCY (operator): sampled, never assumed. This is the number
        // that says whether the corpus stayed in the page cache through OUR OWN
        // scratch writes - the second-tier pool either survived or it did not.
        pf_cache_journal(text, "finish", 64);
        if (pfq) pfq->report();
        cold.report();
        // The repair stream's final flush (the coalesced tail runs held
        // back for char-merge parity) must advance the sweep exactly like
        // the band runs: the state machine needs ALL rNorm runs (v1 feeds
        // every run; the production sweep's last events live here).
        rs.finishStream();
        if (kasaiOn) {
            // The rank space is not covered exactly: the seam classes leave
            // HOLES (a row the class's runs do not describe). What must hold is
            // that the stream stays inside [0,n) in ascending order (asserted per
            // band), that the walks ran only at holes, and that the highest rank
            // any band reached is inside the space. The exactness of every value
            // is the AUDIT arm's job, not this counter's.
            pf_check(lcpCol.maxEnd <= n, "KASAI CLOSURE: a band's rank slice runs past n");
            if (isaDebug)
            fprintf(stderr, "XSA_FINISH_RANK_DEV deviating_runs=%llu max_dev=%llu mean_dev=%.2f\n",
                    (unsigned long long)devRuns, (unsigned long long)devMax,
                    devRuns ? double(devSum) / double(devRuns) : 0.0);
        fprintf(stderr,
                    "XSA_FINISH_KASAI_CLOSURE bands=%llu rank_holes=%llu disorder=%llu "
                    "noncontiguous_runs=%llu walk_fallbacks=%llu max_rank_end=%llu n=%llu\n",
                    (unsigned long long)kasaiState.bands, (unsigned long long)kasaiState.gaps,
                    (unsigned long long)kasaiState.disorder,
                    (unsigned long long)kasaiState.noncontig,
                    (unsigned long long)kasaiState.fallback.load(),
                    (unsigned long long)lcpCol.maxEnd, (unsigned long long)n);
        }
        // The final flush's coalesced tail runs continue the LAST band's slot:
        // it is empty and its prevTail is that band's last emitted tail, so the
        // same query coordinates apply. Verification + advance run here, after
        // every band has been through both stages.
        {
            BandSlot& tail = *feedTarget;
            // finishStream() has ALREADY appended the held-back coalesced tail
            // runs to this slot's nbuf; what must hold is that the slot was
            // emptied by the last advance (cnt == 0), so those runs are the
            // only content and their neighbour is prevTail.
            pf_check(tail.cnt == 0, "tail slot carry");
            tail.cnt = tail.nbuf.size();
            if (tail.cnt) {
                // The tail runs continue after the LAST band's last emitted run,
                // so the neighbour for the first tail query is that run's tail
                // (prevTail), NOT this slot's stale pre-band capture. Getting
                // this wrong changes exactly one top-LCP query per run and the
                // verified-symbol journal catches it even when the container
                // does not (measured: -4 symbols, identical chi and bytes).
                tail.prevTail = prevTail;
                tail.topLcp.assign(tail.cnt, 0);
                tail.interior.assign(tail.cnt, INF);
                lcePending = &tail;
                lceStart = tnow();
                pool.run(lceJob(tail));
                lceWall += tnow() - lceStart;
                lcePending = nullptr;
                finishAdvance(tail);
            }
        }
        {
            PfRegion r("advance-finish", PF_ADVANCE);
            double ta = tnow();
            sweep.finish();
            advanceWall += tnow() - ta;
            r.close(); advAcc.add(r);
        }
        // THE KASAI ARM's own accounting: what the column cost the finish (I/O
        // and bytes) against what the walks used to cost (the fingerprint's
        // verified symbols and hash probes, journaled by fhSlim.report below).
        if (kasaiOn)
            fprintf(stderr,
                    "XSA_FINISH_LCP_COLUMN_READS preads=%llu staged_bytes=%llu escapes=%llu "
                    "max_lcp=%llu audit=%d arm=kasai\n",
                    (unsigned long long)lcpCol.preads, (unsigned long long)lcpCol.stagedBytes,
                    (unsigned long long)lcpCol.escapeCount, (unsigned long long)lcpCol.maxLcp,
                    int(kasaiAudit));
        fprintf(stderr, "SWEEP_PHASE_WALLS repair=%.1f lce=%.1f advance_witness=%.1f lce_threads=%d\n",
                repairWall, lceWall, advanceWall, std::max(1, std::min(threads, 256)));
        // v3 anatomy: repairWall = pieceWait + bandRead + feed + residual.
        waitAcc.report("bands", bandReads);
        readAcc.report("raw_runs", rawRunsFed);
        feedAcc.report("raw_runs", rawRunsFed);
        lceAcc.report("runs", advRuns);
        advAcc.report("runs", advRuns);
        fprintf(stderr, "XSA_FINISH_REGION lce-span wall=%.3f waited=%.3f overlapped=%.3f units=%llu runs_ns=%.2f\n",
                lceWall, lceWaitWall, std::max(0.0, lceWall - lceWaitWall),
                (unsigned long long)advRuns, advRuns ? 1e9 * lceWall / double(advRuns) : 0.0);
        fprintf(stderr, "XSA_FINISH_OVERLAP feed_spans=%.3f lce_spans=%.3f exposed_lce=%.3f serial_fold=%.3f\n",
                overlapWorkWall, lceWall, lceWaitWall, feedWall + advanceWall + readWall + lceWaitWall);
        {
            uint64_t calls = 0, hops = 0;
            pf_access_totals(calls, hops);
            fprintf(stderr, "XSA_FINISH_SWEEP_ANATOMY piecewait=%.3f readband=%.3f feed=%.3f repair_total=%.3f raw_runs=%llu bands=%llu rlebwt_wait=%.3f ns_per_raw_run=%.2f\n",
                    waitWall, readWall, feedWall, repairWall,
                    (unsigned long long)rawRunsFed, (unsigned long long)bandReads, rlebwtWaitWall,
                    rawRunsFed ? 1e9 * feedWall / double(rawRunsFed) : 0.0);
            fprintf(stderr, "XSA_FINISH_CORPUS_ACCESS phase=sweep calls=%llu page_hops=%llu hop_rate=%.4f ns_per_call=%.2f\n",
                    (unsigned long long)calls, (unsigned long long)hops,
                    calls ? double(hops) / double(calls) : 0.0,
                    calls ? 1e9 * (lceWall + feedWall) / double(calls) : 0.0);
        }
        if (repairChunkRuns) fprintf(stderr, "REPAIR_CHUNK_PHASE_WALLS planner=%.3f workers=%.3f ordered_drain=%.3f outer_includes_readiness_and_piece_IO=1\n", repairChunkWall[0], repairChunkWall[1], repairChunkWall[2]);
        // Success-path budget reports (the refusal path always printed these;
        // the pile-week convergence needs the measured verified-symbol and
        // ladder-probe counts to attribute the LCE wall honestly).
        fhSlim.report("pile-finish-corpus");
        fhSeam.report("pile-finish-seam");
        // ---- PROB-CHI: close the ledger and (optionally) run the checker.
        // COMPLETENESS is a counter equality, not a promise: every LCE answer
        // that was HASH-DERIVED must have become a claim. `calls` counts every
        // segment query; the answers that were already byte-exact (the
        // <=16-symbol quick path) are not hash-derived and carry no claim.
        if (trustMode) {
            const uint64_t hashAnswers = (fhSlim.calls.read() - fhSlim.exactFast.read()) +
                                         (fhSeam.calls.read() - fhSeam.exactFast.read());
            if (trustMode == 1) {
                claimSpool->close();
                pf_check(claimSpool->claimCount.load() == hashAnswers,
                         "claims list is INCOMPLETE: " + std::to_string(claimSpool->claimCount.load()) +
                         " claims for " + std::to_string(hashAnswers) + " hash-derived answers");
                fprintf(stderr, "XSA_FINISH_CLAIMS_LEDGER hash_answers=%llu claims=%llu "
                                "exact_fast=%llu span_symbols=%llu complete=1\n",
                        (unsigned long long)hashAnswers, (unsigned long long)claimSpool->claimCount.load(),
                        (unsigned long long)(fhSlim.exactFast.read() + fhSeam.exactFast.read()),
                        (unsigned long long)claimSpool->spanSymbols.load());
                if (pf_env_u64("XSA_FINISH_CHECK_CLAIMS", 0, 0, 1)) pf_check_claims(text, claimsDir, threads);
            } else if (trustMode == 3) {
                pf_check(nullSink.claimCount.load() == hashAnswers,
                         "floor ledger mismatch: " + std::to_string(nullSink.claimCount.load()) +
                         " claims for " + std::to_string(hashAnswers) + " hash-derived answers");
                fprintf(stderr, "XSA_FINISH_CLAIMS_LEDGER hash_answers=%llu claims=%llu checked=0 "
                                "floor_arm=1 NOT_CERTIFIED=1 complete=1\n",
                        (unsigned long long)hashAnswers, (unsigned long long)nullSink.claimCount.load());
            } else {
                pf_check(fusedSink.claimCount.load() == hashAnswers,
                         "fused claim ledger mismatch: " + std::to_string(fusedSink.claimCount.load()) +
                         " claims for " + std::to_string(hashAnswers) + " hash-derived answers");
                fprintf(stderr, "XSA_FINISH_CLAIMS_LEDGER hash_answers=%llu claims=%llu checked_fused=1 "
                                "symbols_compared=%llu complete=1 violations=0\n",
                        (unsigned long long)hashAnswers, (unsigned long long)fusedSink.claimCount.load(),
                        (unsigned long long)fusedSink.comparedSymbols.load());
            }
        }
        pf_check(::close(sparseFd) == 0, "close sparse scratch");
        if (bridge) {
            if (bTailAvail) {
                uint64_t word = uint64_t(bTailRes);
                pf_write_full(bTailsFd, &word, 8, "bridge tails final");
                bTailRes = 0; bTailAvail = 0;
            }
            pf_check(::close(bCharsFd) == 0 && ::close(bLensFd) == 0 &&
                     ::close(bTailsFd) == 0 && ::close(bSAFd) == 0 && ::close(bTopFd) == 0 &&
                     ::close(bIntFd) == 0, "close bridge temps");
        }
        phase("sweep", last);
        if (pf_cache_journal_on()) pf_cache_journal(text, "sweep", 1024);
        const uint64_t chi = sweep.chi, rNorm = rs.rNorm;
        fprintf(stderr, "xsa chi-rspace: chi = %llu (N=%llu, R=%llu) stream-agg=1 fused=1 corpus_lce=1\n",
                (unsigned long long)chi, (unsigned long long)n, (unsigned long long)rNorm);
        if (chiExpect) pf_check(chi == chiExpect, "chi mismatch: got " + std::to_string(chi) + " want " + std::to_string(chiExpect));
        fprintf(stderr, "SLIM_SEEDS queries=%llu lce_wall=%.1f repair_wall=%.1f wit_ram_values=%llu wit_spills=%llu wit_disc_bytes=%llu\n",
                (unsigned long long)slimLce.seedQueries.read(), lceWall, repairWall,
                (unsigned long long)buckets.totalValues, (unsigned long long)buckets.spilled,
                (unsigned long long)buckets.discBytes());

        // ---- the PFCK's work is done: delete it before the container pass
        fhSlimOwner.reset();
        fhSeamOwner.reset();
        pf_check(::close(pfckFd) == 0, "close pfck fd");
        if (!keepPfck) {
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
            PfRegion r("m5-ef", PF_M5);
            unsigned l = chi ? unsigned(std::max(0, int(std::log2(double(n + 1) / double(chi))))) : 0;
            if (l > 63) l = 63;
            uint64_t lowbits = chi * l, highbits = chi ? (n >> l) + chi + 1 : 0;
            BitsOut low(prefix + ".bits-low"), high(prefix + ".bits-high");
            uint64_t i = 0, hpos = 0, prev = ~0ull;
            size_t sidx = 0;
            auto emitWitness = [&](uint64_t v) {
                pf_check(prev == ~0ull || v > prev, "witnesses not strictly increasing");
                prev = v;
                low.lsb(v, l);
                uint64_t mark = (v >> l) + i;
                if (mark > hpos) { high.putzeros(mark - hpos); hpos = mark; }
                high.bit(1); ++hpos;
                ++i;
                while (sidx < wantSamples.size() && wantSamples[sidx] < v)
                    pf_fail("sampled banked witness missing from the v2 stream: " + std::to_string(wantSamples[sidx]));
                if (sidx < wantSamples.size() && wantSamples[sidx] == v) ++sidx;
            };
            // finish-decomposed (m5): the witness set is already VALUE-RANGE
            // bucketed in RAM under the budget. When nothing spilled, sorting
            // each bucket in parallel and walking the buckets in range order is
            // the IDENTICAL sorted stream, and the old path's spill-all +
            // pread/varint merge (1.4 GB written and re-read at 1 GB, ~17 s)
            // buys nothing. Strictly-increasing is asserted row by row either
            // way, so a bucketing or order mistake is refused, not written.
            const bool resident = buckets.spilled == 0;
            {
                PfRegion rp(resident ? "m5-sort" : "m5-presort", PF_M5);
                if (resident) {
                    std::atomic<uint64_t> nextBucket{0};
                    pool.run([&](int) {
                        for (;;) {
                            uint64_t b = nextBucket.fetch_add(1);
                            if (b >= buckets.P) break;
                            std::sort(buckets.buf[b].begin(), buckets.buf[b].end());
                        }
                    });
                } else {
                    buckets.presort_parallel(threads);
                }
                rp.close(); rp.report(buckets.totalValues, "witnesses");
            }
            {
                PfRegion re("m5-encode", PF_M5);
                if (resident) {
                    for (uint64_t b = 0; b < buckets.P; ++b) {
                        for (uint64_t v : buckets.buf[b]) emitWitness(v);
                        std::vector<uint64_t>().swap(buckets.buf[b]);   // release as we go
                    }
                } else {
                    buckets.for_each_sorted(emitWitness);
                }
                re.close(); re.report(i, "witnesses");
            }
            fprintf(stderr, "XSA_FINISH_WITNESS_PATH resident=%d buckets=%llu spilled=%llu values=%llu "
                    "cache_release_calls=%llu cache_released_bytes=%llu\n",
                    int(resident), (unsigned long long)buckets.P,
                    (unsigned long long)buckets.spilled, (unsigned long long)buckets.totalValues,
                    (unsigned long long)buckets.cacheReleaseCalls,
                    (unsigned long long)buckets.cacheReleasedBytes);
            if (highbits > hpos) { high.putzeros(highbits - hpos); hpos = highbits; }
            pf_check(i == chi, "EF count");
            pf_check(low.bitPos == lowbits && high.bitPos == highbits, "EF dimensions");
            pf_check(sidx == wantSamples.size(), "witness sample check incomplete");
            low.pad();
            high.pad();
            container.begin(5, 105, chi);
            container.num(l); container.num(lowbits); container.num(highbits);
            low.copyTo(container);
            high.copyTo(container);
            r.close(); r.report(i, "witnesses");
        }
        if (!wantSamples.empty())
            fprintf(stderr, "WITNESS_SAMPLE_CHECK samples=%zu passed=1\n", wantSamples.size());
        phase("m5-ef", last);
        if (pf_cache_journal_on()) pf_cache_journal(text, "m5-ef", 1024);

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
            std::array<uint64_t, 8192> vals;
            for (uint64_t pos = 0; pos < cnt;) {
                size_t take = size_t(std::min<uint64_t>(vals.size(), cnt - pos));
                pf_read_full(fd, vals.data(), 8 * take, "sparse values");
                for (size_t j = 0; j < take; ++j) container.num(vals[j]);
                pos += take;
            }
            ::close(fd);
        }

        // ---- m1 runs: Huffman over the normalized run chars (frequencies
        // accumulated during the sweep), encoded from a final rlebwt re-scan
        // (the repair replayed sample-free: the normalized chars/lens are a
        // deterministic function of the raw runs and the class table).
        {
            PfRegion rM1("m1-runs", PF_M1);
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
            BitsOut headBits(prefix + ".bits-head"), lenBits(prefix + ".bits-len");
            uint64_t replayRuns = 0;
            {
                PfRegion rw("m1-rlebwt-wait", PF_M1);
                // Barrier: the merge publishes the rlebwt body only after
                // every piece band is emitted (the ordered segment
                // concatenation is the emission's last step). The sweep's
                // final consumed band implies the walk is done, but the
                // concat write may still be in flight. The piece lengths
                // determine its exact word count; size is the completion sentinel.
                double waited = 0;
                for (;;) {
                    struct stat st {};
                    bool complete = ::stat((prefix + ".rlebwt").c_str(), &st) == 0 &&
                                    uint64_t(st.st_size) == expectedRlebwtBytes;
                    if (complete) break;
                    if (waited > pollTimeout) pf_fail("timeout waiting for the complete rlebwt");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    waited += 0.1;
                }
                rw.close(); rw.report(expectedRlebwtBytes, "bytes");
            }
            {
                PfRegion rr("m1-replay", PF_M1);
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
                rr.close(); rr.report(replayRuns, "runs");
            }
            headBits.pad();
            lenBits.pad();
            container.begin(1, 101, rNorm);
            // C table = prefix sums of the normalized byte totals (the ri4 C)
            uint64_t acc = 0;
            for (unsigned c = 0; c < 256; ++c) { uint64_t v = acc; acc += rs.totals[c]; container.num(v); }
            container.write(lens.data(), 256);
            container.num(headBits.bitPos);
            container.num(lenBits.bitPos);
            headBits.copyTo(container);
            lenBits.copyTo(container);
            rM1.close(); rM1.report(rNorm, "runs");
        }
        phase("m1-runs", last);
        if (pf_cache_journal_on()) pf_cache_journal(text, "m1-runs", 1024);

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
            if (src.streaming) {
                ::unlink((src.dir + "/start").c_str()); ::unlink((src.dir + "/terminal").c_str());
            }
            if (::rmdir(src.dir.c_str()) != 0)
                fprintf(stderr, "XSA_FINISH_PIECES_DIR_NONEMPTY %s (left for inspection)\n", src.dir.c_str());
        }
        if (bridge) {
            ::unlink((bridgeDir + "/frag.b-chars").c_str());
            ::unlink((bridgeDir + "/frag.b-lens").c_str());
            ::unlink((bridgeDir + "/frag.b-tails").c_str());
            ::unlink((bridgeDir + "/frag.b-top").c_str());
            ::unlink((bridgeDir + "/frag.b-int").c_str());
            // frag.sA is a GATE artifact (the emission-order witness bridge):
            // keep it in the bridge dir.
        }
        if (!keepRlebwt) {
            // CACHE HYGIENE: the rlebwt has been fully replayed by m1 at this
            // point, so its pages are dead - release them before the unlink so
            // the corpus keeps the page cache it needs.
            if (pf_hygiene_on()) {
                int rfd = ::open((prefix + ".rlebwt").c_str(), O_RDONLY);
                if (rfd >= 0) { pf_release_cache(rfd); ::close(rfd); }
            }
            pf_check(::unlink((prefix + ".rlebwt").c_str()) == 0, "unlink rlebwt");
            pf_check(::unlink((prefix + ".rlebwt.meta").c_str()) == 0, "unlink rlebwt.meta");
            fprintf(stderr, "XSA_FINISH_RLEBWT_DELETED\n");
        }
        fprintf(stderr, "XSA_FINISH_DONE chi=%llu n=%llu r=%llu container=%s\n",
                (unsigned long long)chi, (unsigned long long)n, (unsigned long long)rNorm, out.c_str());
        pf_prof_dump("finish");
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "FATAL xsa_finish: %s\n", e.what());
        return 1;
    }
}
