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
// IN PROCESS (no agg file ever exists). Witness positions set bits in an
// mmap-backed scratch file (one bit per corpus byte). Ascending set bits
// stream directly into the existing m5 Elias-Fano sparse bitvector. m1 is Huffman-encoded
// from a final rlebwt re-scan (the repair replayed sample-free - the
// normalized chars/lens are a deterministic function of the raw runs and
// the class table). rlebwt is deleted after the container publishes
// (unless --keep-rlebwt).
#include "sxi_format.hpp"
#include "finish_spool.h"
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
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <string>
#include <stdexcept>
#include <functional>
#include <filesystem>

static constexpr uint64_t INF = UINT64_MAX;
// THE COOPERATIVE STOP FLAG: the driver writes <scratch>/stop.requested; every wait
// loop below polls it and exits 75 at its own boundary. NO SIGNALS are involved.
static std::string G_STOP_FLAG;
static const std::string& stopFlagPath() { return G_STOP_FLAG; }
static void setStopFlagFromPieceDir(const std::string& pieceDir) {
    G_STOP_FLAG = pieceDir + "/../../stop.requested";   // <scratch>/merged/frag.pieces
}
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

// ------------------------------------------------ WORK AVOIDED, ONE DEFINITION
// THE LINE, DEFINED ONCE, FOR BOTH SIDES OF THE PIPELINE (this file and
// xsa/src/recovery.rs). A phase's clean build performs `total` units of its own
// work; THIS attempt performed `performed` of them; so `avoided = total -
// performed` units were not redone, and `avoided_frac = avoided / total` is
// 0.000000 when the attempt did all of the phase's work and 1.000000 when it
// did none. `basis` names how `total` was derived.
//
// There is deliberately NO bare `ratio` field here or in the driver. The two
// lanes once printed `WORK_AVOIDED ... ratio=<r>` on this very field name with
// OPPOSITE meanings - performed/total in the native finish, avoided/total in the
// driver - so a gate reading `ratio<1.0` passed a full replay on one side and
// failed a no-work resume on the other. That is the ambiguity that once let
// replay-from-zero look like success; one name, one meaning, one emitter.
static void workAvoided(const char* phase, unsigned long long total,
                        unsigned long long performed, const char* basis) {
    if (performed > total) performed = total;
    const unsigned long long avoided = total - performed;
    fprintf(stderr,
            "WORK_AVOIDED phase=%s performed=%llu total=%llu avoided=%llu avoided_frac=%.6f basis=%s\n",
            phase, performed, total, avoided,
            total ? double(avoided) / double(total) : 0.0, basis);
}
// WHO OWNS THE ACCOUNTING OF A SHARED PHASE. The driver sets XSA_WORK_ROOT=driver
// in this stage's environment: it then prices phase=finish from the raw counters
// below, and this stage must not print a SECOND line for the same phase with a
// different number. With the variable unset (or `phase`) the finish is run as
// the accounting root - the standalone gate does exactly that - and emits the
// line itself. Anything else is a refusal, never a silent default.
static bool workRootDriver() {
    static const bool driver = [] {
        const char* p = getenv("XSA_WORK_ROOT");
        if (!p || !*p) return false;
        const std::string v(p);
        if (v == "driver") return true;
        if (v == "phase") return false;
        pf_fail("XSA_WORK_ROOT must be driver|phase");
        return false;
    }();
    return driver;
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

// Linux process residency: file-backed pages must not be counted as heap.
struct FinishRss {
    uint64_t rss = 0, anon = 0;
    static FinishRss read() {
        FinishRss r;
        FILE* f = std::fopen("/proc/self/status", "r");
        if (!f) return r;
        char line[256]; unsigned long long value;
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "VmRSS: %llu kB", &value) == 1) r.rss = value;
            if (std::sscanf(line, "RssAnon: %llu kB", &value) == 1) r.anon = value;
        }
        std::fclose(f); return r;
    }
    void journal(const char* stage) const {
        std::fprintf(stderr, "XSA_FINISH_RSS_AT stage=%s rss_kb=%llu anon_kb=%llu\n",
                     stage, (unsigned long long)rss, (unsigned long long)anon);
    }
};
static void finish_rss_at(const char* stage) { FinishRss::read().journal(stage); }
struct FinishRssSampler {
    std::atomic<bool> stop{false};
    FinishRss peak;
    std::thread worker;
    FinishRssSampler() : worker([this] {
        while (!stop.load()) {
            const auto now = FinishRss::read();
            if (now.rss > peak.rss) peak = now;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }) {}
    ~FinishRssSampler() {
        stop.store(true); worker.join();
        const auto now = FinishRss::read();
        if (now.rss > peak.rss) peak = now;
        peak.journal("peak_sampled_100ms");
    }
};

// ------------------------------------------------------- THE VIRTUAL END
// END IS VIRTUAL (SPEC_CHAIN_TO_PILE Phase A): no byte is appended to the
// corpus and no byte VALUE is sacrificed to carry it. The text is the corpus,
// n = file bytes, and the END symbol lives at the virtual index n, is strictly
// below every byte value and occurs exactly once. Its product symbol is 0.
//
// NOTE ON 0x1E: the legacy chunk front end kept the document separator at its
// own value (remap[0x1E] == 0x1E, enforced by xsa/src/sxi.rs on member 7). That
// is a CHUNK-FRONTEND contract, not a property of the tape route, and it is
// incompatible with an order-preserving relabel (only 6 present bytes sort
// below 0x1E in the 150 MB pile fixture, so 0x1E lands on symbol 7, not 30).
// This remap therefore does NOT special-case 0x1E; see the lane report.
static constexpr uint8_t SENTINEL_SYMBOL = 0;     // END's product symbol: minimal, unique

// ---------------------------------------------------------------- remap
// THE TABLE IS CORPUS-DERIVED, NOT UNIVERSAL. It is a 256-byte permutation (the
// container's member 7 and every consumer read it that way) that is ORDER
// PRESERVING on the text:
//
//     present byte b of rank j (0-based, byte order)  ->  1 + j
//     absent byte (never occurs in the text)          ->  the remaining symbols,
//                                                        k+1..255 first, then 0
//
// END HAS NO CODE: code 0 is END's cell in the 8-bit transport, so it may only
// be named by an ABSENT byte (the table stays a 256-byte bijection while no
// present byte can be confused with END). A universal table cannot satisfy
// "present -> 1..k" on an arbitrary corpus, which is why this is derived per
// corpus (xsa/src/chain.rs; reference generator tools/binbwt/emit_remap.py) and
// passed to every stage.
//
// k == 256 leaves no free code for END. The derivation still produces the valid
// identity bijection (planarity of the restriction is provable, not asserted),
// and this function REFUSES that shape here because a present byte would own
// code 0 and the END row would be unidentifiable.
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
    fprintf(stderr, "END_REMAP_TABLE path=%s sha_checked_by_caller=1 ends_in_sentinel=1 end_code=none\n",
            path.c_str());
}

// The table above is a claim about the CORPUS; prove it against the corpus in
// one sequential pass and refuse on any drift. This is the fail-loud gate that
// keeps "order preserving" from being a comment: the present bytes must map to
// exactly 1..k in increasing byte order, and END's cell (code 0) must be owned
// by an absent byte, since END has no code of its own.
static void verify_end_remap(const std::string& corpusPath, const uint8_t map[256]) {
    int fd = ::open(corpusPath.c_str(), O_RDONLY);
    pf_check(fd >= 0, "open corpus for the remap proof");
    std::array<uint64_t, 256> hist{};
    std::vector<uint8_t> block(1u << 22);   // heap: 4 MiB must not live on the stack
    for (uint64_t off = 0;;) {
        const ssize_t z = ::pread(fd, block.data(), block.size(), off_t(off));
        pf_check(z >= 0, "read corpus for the remap proof");
        if (z == 0) break;
        for (ssize_t i = 0; i < z; ++i) ++hist[block[size_t(i)]];
        off += uint64_t(z);
    }
    ::close(fd);
    uint64_t present = 0, zeroOwner = ~0ull;
    for (unsigned b = 0; b < 256; ++b) {
        if (map[b] == SENTINEL_SYMBOL) zeroOwner = b;
        if (!hist[b]) continue;
        pf_check(map[b] == uint8_t(1 + present),
                 "remap is not order preserving on the corpus at byte " + std::to_string(b));
        ++present;
    }
    pf_check(present >= 1 && present <= 255,
             "corpus uses all 256 byte values: present bytes need codes 1..k and END "
             "has no code of its own, so an 8-bit transport cannot carry this corpus "
             "(no code is left free for the END cell)");
    pf_check(zeroOwner < 256 && !hist[zeroOwner],
             "remap: the END cell (code 0) must be named by an ABSENT byte, not by a "
             "present one (END has no code of its own)");
    fprintf(stderr, "END_REMAP present=%llu order_preserving=1 end_symbol=%u end_code=none "
                    "alphabet=1..%llu unused_symbols=%llu end_cell_byte=0x%02x\n",
            (unsigned long long)present, unsigned(SENTINEL_SYMBOL),
            (unsigned long long)present, (unsigned long long)(255 - present),
            unsigned(zeroOwner));
}

// The order-preserving END-format table for one corpus (the generator behind
// the --remap file every stage takes). THE RULES ARE emit_remap.py's: present
// byte of rank j -> 1+j; absent bytes take k+1..255 in increasing byte order
// and then 0; k == 256 degenerates to the identity and is refused (no free
// code for END). Nothing is appended to the corpus.
static void build_end_remap(const std::string& corpusPath, const std::string& outPath) {
    int fd = ::open(corpusPath.c_str(), O_RDONLY);
    pf_check(fd >= 0, "open corpus for --sentinel-remap");
    std::array<uint64_t, 256> hist{};
    std::vector<uint8_t> block(1u << 22);   // heap: 4 MiB must not live on the stack
    for (uint64_t off = 0;;) {
        const ssize_t z = ::pread(fd, block.data(), block.size(), off_t(off));
        pf_check(z >= 0, "read corpus for --sentinel-remap");
        if (z == 0) break;
        for (ssize_t i = 0; i < z; ++i) ++hist[block[size_t(i)]];
        off += uint64_t(z);
    }
    ::close(fd);
    uint8_t map[256];
    unsigned present = 0;
    for (unsigned b = 0; b < 256; ++b) present += hist[b] != 0;
    if (present <= 255) {
        unsigned rank = 0;
        for (unsigned b = 0; b < 256; ++b)
            if (hist[b]) map[b] = uint8_t(1 + rank++);
        unsigned fill = present + 1;   // absent bytes: k+1..255 in byte order, then 0
        for (unsigned b = 0; b < 256; ++b)
            if (!hist[b]) map[b] = (fill <= 255) ? uint8_t(fill++) : SENTINEL_SYMBOL;
    } else {
        for (unsigned b = 0; b < 256; ++b) map[b] = uint8_t(b);   // identity
        fprintf(stderr, "END_REMAP_WARNING k=256 present=256 spare_symbol=none: every code "
                        "names a present byte, so the END cell has no symbol of its own\n");
    }
    uint8_t sorted[256];
    memcpy(sorted, map, 256);
    std::sort(sorted, sorted + 256);
    for (unsigned i = 0; i < 256; ++i) pf_check(sorted[i] == uint8_t(i), "end remap is not a bijection");
    int out = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    pf_check(out >= 0, "create " + outPath);
    pf_write_full(out, map, 256, "end remap");
    pf_check(::close(out) == 0, "close " + outPath);
    verify_end_remap(corpusPath, map);
    fprintf(stderr, "END_REMAP_WRITTEN path=%s alphabet=%u ends_in_sentinel=1 end_code=none "
                    "end_symbol=%u\n",
            outPath.c_str(), present, unsigned(SENTINEL_SYMBOL));
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
        // Four page-aligned windows must cover the entire corpus. Rounding
        // size_/4 DOWN loses up to four pages and otherwise selects the locked
        // byte path even with unlimited address space (100 GB needed 5 windows
        // for only 15448 uncovered bytes). Round the desired budget up first;
        // RLIMIT_AS/3 below remains the hard cap, never rounded upward.
        const uint64_t mapQuantum = windows.size() * 4096;
        pf_check(size_ <= UINT64_MAX - (mapQuantum - 1), "corpus mapping size overflow");
        const uint64_t alignedSize = (size_ + mapQuantum - 1) / mapQuantum * mapQuantum;
        uint64_t mapBudget = std::max<uint64_t>(32ull << 30, alignedSize);
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
    // THE TEXT IS T' = corpus . 0xF5 and n = size() includes the sentinel.
    // `size_` stays the FILE size: the file is the only text on disc and the
    // sentinel is a virtual index served by operator[] below (nothing is ever
    // appended to the corpus).
    uint64_t size() const { return size_ + 1; }
    uint64_t fileSize() const { return size_; }
    // PFCK and histogram scans already own bounded worker buffers. Reading
    // through the random-access mappings faults the entire corpus into process
    // RSS (97.9 million KiB at 100 GB), even though no heap allocation grows.
    // pread uses the same page cache without installing corpus-wide PTEs.
    void read_block(uint64_t off, uint8_t* dst, uint64_t len) const {
        pf_check(off <= size() && len <= size() - off, "corpus block bounds");
        const uint64_t bytes = off < size_ ? std::min(len, size_ - off) : 0;
        if (bytes) {
            pf_pread_full(fd, dst, bytes, off, "corpus bulk block");
            for (uint64_t i = 0; i < bytes; ++i) dst[i] = remap[dst[i]];
        }
        if (bytes < len) dst[bytes] = SENTINEL_SYMBOL;
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
        if (i > size_) pf_fail("corpus adapter bounds");
        // END IS VIRTUAL: one occurrence, at index size_, symbol 0 (a table cell is
        // NOT consulted - code 0 belongs to an absent byte, never to the text).
        if (i == size_) return SENTINEL_SYMBOL;
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
    std::string tmp=finish_spool::enabled()?out+".part":out;
    int fd = ::open(tmp.c_str(), O_RDWR | O_CREAT | (finish_spool::enabled()?O_TRUNC:O_EXCL), 0666);
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
    if(finish_spool::enabled())pf_check(::fdatasync(fd)==0,"sync pfck");
    pf_check(::close(fd) == 0, "close pfck");
    if(finish_spool::enabled()){
        pf_check(::rename(tmp.c_str(),out.c_str())==0,"publish pfck");
        finish_spool::syncdir(std::filesystem::path(out).parent_path().string());
    }
    fprintf(stderr, "XSA_FINISH_PFCK path=%s n=%llu tau=%llu count=%llu bytes=%llu wall=%.3f workers=%d segments=%llu worker_bytes=%llu source=corpus_adapter fixed_grid=1\n",
            out.c_str(), (unsigned long long)n, (unsigned long long)tau, (unsigned long long)count,
            (unsigned long long)(40 + 8 * count), tnow() - t0, workers,
            (unsigned long long)segments, (unsigned long long)workerBytes);
}

// ------------------------------------------------------------ piece source
struct RawRun { uint8_t c; uint64_t len, head, tail; };

// A piece whose bytes are already in the container prefix but whose durability
// has not been witnessed yet. `frontier` is the emitted-run count at which every
// run of the piece has been appended (UINT64_MAX until the band has been fed).
struct DeferredPiece { std::string path; uint64_t band; uint64_t frontier; };

struct PieceSource {
    std::string prefix, dir;
    bool streaming = false, terminal = false;
    // CONTAINER STREAMING: a piece may only be unlinked after the container
    // bytes covering it are fsynced (RESUME_RETENTION / PIECE_ABSORBED), so the
    // consumer defers the unlink to the finish's append frontier.
    bool deferConsume = false;
    std::vector<DeferredPiece> pendingPieces;
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
        pf_check(memcmp(magic, "BPT1", 4) == 0 && values[1] > 0 && values[1] <= n &&
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
            if (::access(stopFlagPath().c_str(), F_OK) == 0) {
                std::fprintf(stderr, "STOP_OBSERVED phase=finish-v2 waiting_for=band_progress waited_s=%.3f\n", waited);
                ::_exit(75);
            }
            if (!finish_spool::enabled() && pollTimeout != 0 && waited > pollTimeout) pf_fail("timeout waiting for band progress " + path);
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
                     cl[3] < cl[4] && cl[4] <= n && cl[5] < n && cl[6] < n && cl[7] < n,
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
        if (!finish_spool::enabled()) pf_check(::unlink(path.c_str()) == 0, "unlink consumed progress");
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
                if (::access(stopFlagPath().c_str(), F_OK) == 0) {
                    std::fprintf(stderr, "STOP_OBSERVED phase=finish-v2 waiting_for=terminal waited_s=%.3f\n", waited);
                    ::_exit(75);
                }
                if (!finish_spool::enabled() && pollTimeout != 0 && waited > pollTimeout) pf_fail("timeout waiting for terminal assertion");
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
            pf_check(lens[i]>0 && head<n && tail<n,"band record bounds");
            out.push_back({chars[i],lens[i],head,tail});
        }
        if (deferConsume) { std::lock_guard<std::mutex> lk(mu); pendingPieces.push_back({path, band, ~0ull}); }
        else if (!finish_spool::enabled()) pf_check(::unlink(path.c_str())==0,"unlink consumed band piece");
        // All raw runs are private now. Return the piece credit before any
        // repair, LCP, prefetch or verification work can wait on another stage.
        consumed_band(band);
    }
    void load_idx() {
        std::string path = prefix + ".runcols.idx";
        dir = prefix + ".pieces";
        setStopFlagFromPieceDir(dir);
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
                n=fields[0]; w=fields[1]; depth=fields[2]; window=fields[3]; R=n; streaming=true;
                return;
            }
            if (::access(stopFlagPath().c_str(), F_OK) == 0) {
                std::fprintf(stderr, "STOP_OBSERVED phase=finish-v2 waiting_for=runcols_idx waited_s=%.3f\n", waited);
                ::_exit(75);
            }
            if (!finish_spool::enabled() && pollTimeout != 0 && waited > pollTimeout) pf_fail("timeout waiting for the merge's runcols idx");
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
        setStopFlagFromPieceDir(dir);
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
                // COOPERATIVE STOP, at the FINISH's natural boundary: the piece it
                // is waiting for. The driver writes <scratch>/stop.requested; the
                // phase journals STOP_OBSERVED and exits 75. NO SIGNALS.
                if (::access((dir + "/../../stop.requested").c_str(), F_OK) == 0) {
                    std::fprintf(stderr, "STOP_OBSERVED phase=finish-v2 at_band=%llu waited_s=%.3f\n",
                                 (unsigned long long)band, waited);
                    ::_exit(75);
                }
                if (::access(stopFlagPath().c_str(), F_OK) == 0) {
                    std::fprintf(stderr, "STOP_OBSERVED phase=finish-v2 waiting_for=piece waited_s=%.3f\n", waited);
                    ::_exit(75);
                }
                if (!finish_spool::enabled() && pollTimeout != 0 && waited > pollTimeout) pf_fail("timeout waiting for piece " + name);
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
            if (!finish_spool::enabled()) pf_check(::unlink(name.c_str()) == 0, "unlink piece " + name);
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
                    if (!finish_spool::enabled() && pollTimeout != 0 && waited > pollTimeout) pf_fail("timeout waiting for piece " + name);
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
                   std::vector<RawRun> runs;   // lo/hi in NORMALIZED rows
                   // TRUE head/tail rank per entry of `runs` (see rebuild): the
                   // class rows are emitted in rebuild order, but the true order
                   // puts the class seed (the wrap suffix ending at the text
                   // end) first and the rest in the same content order.
                   std::vector<std::pair<uint64_t, uint64_t>> runTrue; };

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
    // --isa-sample, installed by the fold before build/rebuild: the class
    // true-rank model is verified against it at every anchored occurrence.
    // Called as (position, modelRank); a disagreement must pf_fail inside.
    std::function<void(uint64_t, uint64_t)> anchorVerify;
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
                     cl.before < n && cl.after < n, "seam spool class bounds");
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
        rebuild(n, rawR, limit, collection_lce, symbol, anchorVerify);
    }
    void build(const CorpusText& text, const PfWm& wm, uint64_t n, uint64_t rawR,
               const std::function<uint64_t(uint64_t, uint64_t)>& collection_lce,
               const std::function<uint8_t(uint64_t)>& symbol) {
        const uint64_t N = n, r = rawR;
        const uint64_t limit = std::max<uint64_t>(1000, r / 1000);
        // THE TERMINAL SYMBOL IS THE APPENDED SENTINEL: it occurs exactly once,
        // at the last position of the text, so the occurrence chain is empty
        // after step 0 and no class is ever pushed. The chain code below is kept
        // (it is the general contract of this routine); with the sentinel in the
        // alphabet it is inert BY DATA, not by a switch.
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
            pf_check(want != SENTINEL_SYMBOL, "unexpected sentinel inside the seam chain");
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
        // before = SA[lo-1]; with the sentinel the row order is cyclic-exact, so
        // lo == 0 wraps to the last row (the sentinel suffix at position n-1).
        // after = SA[hi], and hi == n wraps to row 0 the same way.
        for (auto& cl : classes) {
            cl.before = cl.lo ? wm.at(cl.lo - 1) : N - 1;
            cl.after = cl.hi < n ? wm.at(cl.hi) : N - 1;
            pf_check(cl.before < N && cl.after < N, "seam flank outside the text");
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
        rebuild(n, r, limit, collection_lce, symbol, anchorVerify);
    }
    void rebuild(uint64_t n, uint64_t r, uint64_t limit,
                 const std::function<uint64_t(uint64_t, uint64_t)>& collection_lce,
                 const std::function<uint8_t(uint64_t)>& symbol,
                 unsigned threads = 1) {
        rebuild(n, r, limit, collection_lce, symbol, nullptr, threads);
    }
    // `anchorVerify` (optional, --isa-sample): every anchored class occurrence
    // is checked against the model below and any disagreement refuses - the
    // product-scale proof that the stamped true ranks are the SA's ranks.
    uint64_t anchorChecks = 0;   // verification passes across all classes
    void rebuild(uint64_t n, uint64_t r, uint64_t limit,
                 const std::function<uint64_t(uint64_t, uint64_t)>& collection_lce,
                 const std::function<uint8_t(uint64_t)>& symbol,
                 const std::function<void(uint64_t, uint64_t)>& anchorVerify,
                 unsigned threads = 1) {
        // Product convention: finite suffix order. The terminal chain seed
        // sorts before every proper extension. Move the row itself before
        // coalescing, so stamps certify stream order rather than replace it.
        uint64_t maxSeam = 0;
        // ---- GATHER, THEN SORT --------------------------------------------------
        // The old comparator called collection_lce(), a random-access TEXT WALK, for
        // every one of its O(m log m) comparisons: O(m log m) scattered text reads to
        // order m members. Instead read each member's first K bytes ONCE - O(m)
        // scattered reads, issued IN PARALLEL because they are independent - then sort
        // an index array in RAM on that key, and fall back to a text walk only where
        // K-byte prefixes are EQUAL. Same order; a fraction of the text contact.
        for (auto& cl : classes) {
            std::vector<uint64_t> sa = cl.occ;
            const size_t msz = sa.size();
            size_t K = 32;
            while (K > 8 && (uint64_t(K) * uint64_t(msz)) > (uint64_t(256) << 20)) K >>= 1;
            K = size_t(std::min<uint64_t>(K, n));
            std::vector<uint8_t> key(msz * K);
            {
                const unsigned nt = std::max(1u, std::min<unsigned>(unsigned(threads),
                                       unsigned(msz ? (msz + 8191) / 8192 : 1)));
                std::vector<std::thread> gp;
                gp.reserve(nt);
                for (unsigned t = 0; t < nt; ++t)
                    gp.emplace_back([&, t] {
                        const size_t i0 = msz * t / nt, i1 = msz * (t + 1) / nt;
                        for (size_t i = i0; i < i1; ++i) {
                            const uint64_t p = sa[i];
                            uint8_t* dst = key.data() + i * K;
                            for (size_t j = 0; j < K; ++j) {
                                const uint64_t q = p + j;
                                dst[j] = symbol(q < n ? q : p);   // finite: nothing past the end
                            }
                        }
                    });
                for (auto& th : gp) th.join();
            }
            std::vector<uint64_t> idx(msz);
            for (size_t i = 0; i < msz; ++i) idx[i] = uint64_t(i);
            std::sort(idx.begin(), idx.end(), [&](uint64_t ia, uint64_t ib) {
                const int kc = std::memcmp(key.data() + size_t(ia) * K, key.data() + size_t(ib) * K, K);
                if (kc != 0) return kc < 0;
                // TIE at depth K: only now is a text walk needed.
                const uint64_t a = sa[ia], b = sa[ib];
                const uint64_t len = collection_lce(a, b);
                maxSeam = std::max(maxSeam, len);
                const uint64_t cap = std::min(n - a, n - b);
                return len >= cap ? a > b : symbol(a + len) < symbol(b + len);
            });
            {
                std::vector<uint64_t> ord(msz);
                for (size_t i = 0; i < msz; ++i) ord[i] = sa[idx[i]];
                sa.swap(ord);
            }
            auto seed = std::find(sa.begin(), sa.end(), cl.first);
            pf_check(seed != sa.end(), "seam class seed missing from its own occurrence set");
            std::rotate(sa.begin(), seed, seed + 1);
            auto trueIdx = [](uint64_t i) { return i; };
            // ANCHOR VERIFICATION: the model must reproduce the sampled ISA at
            // every anchored occurrence of the class, or the supply refuses.
            if (anchorVerify) {
                for (uint64_t i = 0; i < sa.size(); ++i) {
                    anchorVerify(sa[i], cl.lo + trueIdx(i));
                    ++anchorChecks;
                }
            }
            cl.runTrue.clear();
            cl.runs.clear();
            uint64_t runStartIdx = 0;
            for (uint64_t i = 0; i < sa.size(); ++i) {
                const uint64_t p = sa[i];
                uint8_t c = symbol((p + n - 1) % n);
                if (!cl.runs.empty() && cl.runs.back().c == c) {
                    ++cl.runs.back().len;
                    cl.runs.back().tail = p;
                } else {
                    if (!cl.runs.empty())
                        cl.runTrue.push_back({cl.lo + trueIdx(runStartIdx),
                                              cl.lo + trueIdx(i - 1)});
                    cl.runs.push_back({c, 1, p, p});
                    runStartIdx = i;
                }
            }
            if (!cl.runs.empty())
                cl.runTrue.push_back({cl.lo + trueIdx(runStartIdx),
                                      cl.lo + trueIdx(sa.size() - 1)});
            pf_check(cl.runTrue.size() == cl.runs.size(), "seam class run/rank count");
            uint64_t stamped = 0;
            for (auto& v : cl.runs) stamped += v.len;
            pf_check(stamped == cl.hi - cl.lo, "seam class rows != its span");
        }
        maxSeamLce = maxSeam;
        if (anchorVerify)
            fprintf(stderr, "XSA_FINISH_SEAM_ANCHORS checks=%llu mismatches=0 source=--isa-sample\n",
                    (unsigned long long)anchorChecks);
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
        std::vector<uint64_t> last{1,1,1,1,0,0,65535,65536,0,3,9,65539,4,9,3,0};
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
    write("g-1","BPG1",{1,1,1,1,0,0,65535,65536,0,3,9,65539,4,9,3,0});
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
        if (mode) expected.push_back({0, 3, 9, n-1, 4, {9, 3, 0}, {}});
        if (mode == 2) expected.push_back({6, 9, 8, 6, n-1, {8, 1, 7}, {}});
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
                uint64_t d = lce(x,y), cap = std::min(n-x,n-y);
                return d >= cap ? x > y : symbol(x+d) < symbol(y+d);
            });
            auto seed = std::find(order.begin(), order.end(), e.first);
            pf_check(seed != order.end(), "reader oracle seed");
            std::rotate(order.begin(), seed, seed + 1);
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
// rk is the transducer's source-row bookkeeping. Derived stream ranks are
// prefix sums of lengths; seam stamps must agree with those ranks or refuse.
struct NRun { uint8_t c; uint64_t len, h, t; uint64_t rk = 0;
             uint64_t ht = ~0ull, tt = ~0ull; };
// Shared by every arm; debug mode cannot waive either certificate.
static void require_stream_run(const NRun& v, uint64_t row, uint64_t n) {
    pf_check(v.len && row <= n && v.len <= n - row,
             "STREAM CLOSURE: invalid row extent (refusing publication)");
    pf_check(v.ht == ~0ull || v.ht == row,
             "STREAM STAMP: head disagrees with derived rank (refusing publication)");
    pf_check(v.tt == ~0ull || v.tt == row + v.len - 1,
             "STREAM STAMP: tail disagrees with derived rank (refusing publication)");
}
static void require_zero_closure(uint64_t holes, uint64_t disorder,
                                 uint64_t noncontiguous, uint64_t fallbacks) {
    pf_check(holes == 0 && disorder == 0 && noncontiguous == 0 && fallbacks == 0,
             "KASAI CLOSURE: rank holes, disorder, noncontiguous runs, or fallback (refusing publication)");
}
static int selftest_stream_guard() {
    require_stream_run({5, 3, 0, 0, 0, 7, 9}, 7, 10);
    require_zero_closure(0, 0, 0, 0);
    for (unsigned mode = 0; mode < 8; ++mode) {
        pid_t child = ::fork(); pf_check(child >= 0, "fork stream guard test");
        if (!child) {
            int quiet = ::open("/dev/null", O_WRONLY);
            ::dup2(quiet, STDERR_FILENO); ::close(quiet);
            NRun v{5, 3, 0, 0, 0, 7, 9};
            if (mode == 0) v.ht = 8;
            if (mode == 1) v.tt = 8;
            if (mode == 2) v.len = 0;
            if (mode == 3) v.len = 4;
            if (mode < 4) require_stream_run(v, 7, 10);
            else require_zero_closure(mode == 4, mode == 5, mode == 6, mode == 7);
            _exit(0);
        }
        int status;
        pf_check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 2,
                 "stream guard failed to refuse");
    }
    printf("STREAM_GUARD_SELFTEST_PASS head,tail,zero,overflow,holes,disorder,noncontiguous,fallback refusals=8\n");
    return 0;
}
// Serial repair attribution. Sample hashed input ordinals (not every 1024th
// ordinal, which would alias the sparse-tail writes). Nested scopes charge
// exclusive TSC ticks; read/decode is timed once per band, without sampling.
// Probe overhead is included, so these are attribution counters, not a CPU
// frequency estimate. No files, ISA, or per-run scratch are introduced.
struct RepairCycles {
    enum Term { RAW, COALESCE, SEAM, STAMP, GUARD, OUTPUT, TERMS };
    uint64_t ticks[TERMS]{}, inputs = 0, samples = 0, readTicks = 0;
    bool enabled = true, active = false;
    int phase = RAW;
    uint64_t last = 0;
    void change(int to) {
        const uint64_t now = __rdtsc();
        ticks[phase] += now - last; last = now; phase = to;
    }
    struct Scope {
        RepairCycles* c;
        int previous = RAW;
        Scope(RepairCycles* counter, int term) : c(counter && counter->active ? counter : nullptr) {
            if (c) { previous = c->phase; c->change(term); }
        }
        ~Scope() { if (c) c->change(previous); }
    };
    struct Input {
        RepairCycles* c;
        explicit Input(RepairCycles* counter) : c(counter) {
            if (!c) return;
            const uint64_t ordinal = c->inputs++;
            c->active = c->enabled && ((ordinal * 11400714819323198485ull) >> 54) == 0;
            if (c->active) { ++c->samples; c->phase = RAW; c->last = __rdtsc(); }
        }
        ~Input() { if (c && c->active) { c->change(RAW); c->active = false; } }
    };
    void report() const {
        fprintf(stderr, "SWEEP_CYCLES phase=repair enabled=%d inputs=%llu samples=%llu "
                "sample_stride_mean=1024 exclusive=1 probe_overhead_included=1 "
                "read_decode=%.2f raw=%.2f coalescing=%.2f seamSplice=%.2f "
                "stamping=%.2f require_stream_run=%.2f output_staging=%.2f units=tsc_ticks_per_input\n",
                int(enabled), (unsigned long long)inputs, (unsigned long long)samples,
                inputs ? double(readTicks)/inputs : 0.0,
                mean(RAW), mean(COALESCE), mean(SEAM), mean(STAMP), mean(GUARD), mean(OUTPUT));
    }
    double mean(int term) const { return samples ? double(ticks[term])/samples : 0.0; }
};

struct RepairStream {
    RepairCycles* cycles = nullptr; // only the ordered product transducer
    // N == n: the transport frame is gone. The merge emits exactly the n rows of
    // the sentinel-terminated text, row 0 being the sentinel suffix itself.
    uint64_t N = 0, n = 0;
    SeamClasses* seam = nullptr;
    std::function<void(const NRun&)> emit;
    bool sampleChecks = true;   // false for the m1 replay (samples absent)
    uint64_t rows = 0;
    bool srcHave = false;
    RawRun srcPending{};
    // THE SENTINEL ROW CERTIFICATE: the minimal symbol 0 occurs exactly ONCE in
    // the BWT, on the row of the suffix that starts at text position 0 (its
    // predecessor is the appended sentinel at n-1). One row, no frame, no
    // rewrite: this counter replaces zeroes/dollarSeen/beforeTerminal.
    uint64_t sentinelRows = 0;
    uint64_t row = 0;
    size_t next = 0;
    bool have = false;
    NRun pending{};
    uint64_t rNorm = 0, nNorm = 0;
    std::array<uint64_t, 256> totals{}, runFreq{};
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
        RepairCycles::Scope cycleScope(cycles, RepairCycles::COALESCE);
        if (!v.len) return;
        if (have && pending.c == v.c) {
            pending.len += v.len; pending.t = v.t;
            // The merged run's TRUE tail is the newest piece's tail (the head
            // stamp, when present, belongs to the first piece and survives).
            pending.tt = v.tt;
        } else {
            if (have) emitOne(sink);
            RepairCycles::Scope stampScope(cycles, RepairCycles::STAMP);
            pending = v; pending.rk = base; have = true;
        }
    }
    template<class Sink> void append(const NRun& v, Sink& sink) { appendAt(v, row, sink); }
    template<class Sink> void emitOne(Sink& sink) {
        RepairCycles::Scope cycleScope(cycles, RepairCycles::STAMP);
        ++rNorm; nNorm += pending.len;
        totals[pending.c] += pending.len;
        ++runFreq[pending.c];
        sink(pending);
    }
    void append(const NRun& v) { append(v, emit); }
    void emitOne() { emitOne(emit); }
    void feed_raw(const RawRun& raw) { feed_raw(raw, emit); }
    template<class Sink> void feed_raw(const RawRun& raw, Sink& sink) {
        feed_raw(raw, sink, srcPending, srcHave);
    }
    // Keep the source coalescer local to the ordered band loop. The sink reads
    // emitted-run accounting, not this delayed source record; separating them
    // lets the compiler retain the source fields across the callback.
    template<class Sink> void feed_raw(const RawRun& raw, Sink& sink,
                                       RawRun& source, bool& sourceHave) {
        RepairCycles::Input cycleInput(cycles);
        // NO FRAME: the first raw row is product row 0 like any other. Every row
        // is checked against the transported extent, and the row extent must sum
        // to exactly n.
        pf_check(raw.len && raw.len <= N - rows, "RLE length");
        RawRun v = raw;
        rows += raw.len;
        if (sampleChecks) pf_check(v.head < n && v.tail < n, "endpoint outside normalized text");
        // THE SENTINEL ROW: symbol 0 is the appended end-of-text byte and occurs
        // exactly once, on the row whose suffix is position 0 (head == tail == 0,
        // a length-1 run). Anything else is a corrupt frame.
        if (v.c == SENTINEL_SYMBOL) {
            ++sentinelRows;
            if (sampleChecks)
                pf_check(v.len == 1 && v.head == 0 && v.tail == 0,
                         "the sentinel symbol appears off the SA-zero singleton row");
        }
        // Alphabet invariant: product symbols 1..k are the present corpus bytes
        // (an order-preserving relabel) and 0 is the sentinel; no symbol outside
        // that set can occur, because absent bytes map to unused symbols.
        RepairCycles::Scope coalesceScope(cycles, RepairCycles::COALESCE);
        if (sourceHave && source.c == v.c) { source.len += v.len; source.tail = v.tail; }
        else {
            if (sourceHave) {
                const uint64_t end = row + source.len;
                if (!seam || next == seam->classes.size() || end <= seam->classes[next].lo) {
                    // Both coalescers still run in order. Outside a seam the
                    // splice is exactly one append, with the same row stamp;
                    // avoid constructing a second temporary and entering the
                    // class traversal for every ordinary character change.
                    append({source.c, source.len, source.head, source.tail}, sink);
                    row = end;
                } else {
                    seamSplice({source.c, source.len, source.head, source.tail}, sink);
                }
            }
            source = v; sourceHave = true;
        }
    }
    void seamSplice(NRun v) { seamSplice(v, emit); }
    template<class Sink> void seamSplice(NRun v, Sink& sink) {
        RepairCycles::Scope cycleScope(cycles, RepairCycles::SEAM);
        uint64_t end = row + v.len;
        while (row < end) {
            if (seam && next < seam->classes.size() && row >= seam->classes[next].lo) {
                SeamClass& cl = seam->classes[next];
                if (row == cl.lo) {
                    // The class's reconstructed runs are contiguous from the
                    // class's first row: stamp each piece's own rank base, and
                    // its independently verified head/tail rank. Both the
                    // emitted class and the rank model put the seed first.
                    pf_check(cl.runTrue.size() == cl.runs.size(),
                             "seam splice run/rank count");
                    uint64_t base = row;
                    for (size_t k = 0; k < cl.runs.size(); ++k) {
                        RepairCycles::Scope stampScope(cycles, RepairCycles::STAMP);
                        const auto& piece = cl.runs[k];
                        NRun nr{piece.c, piece.len, piece.head, piece.tail, base};
                        nr.ht = cl.runTrue[k].first;
                        nr.tt = cl.runTrue[k].second;
                        appendAt(nr, base, sink);
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
        pf_check(sentinelRows == 1, "the sentinel row is missing or duplicated");
        fprintf(stderr, "ENDPOINT_STREAM_PASS n=%llu k=1 r=%llu sentinel_rows=%llu seam_classes=%zu\n",
                (unsigned long long)n, (unsigned long long)rNorm,
                (unsigned long long)sentinelRows, seam ? seam->classes.size() : 0);
    }
};

// ------------------------------------------------------------- sweep state
// Verbatim port of the production scan-rs state machine (xsa chi-rspace,
// bit6/teralcp_chi.cpp): no walks, no SA, no text access in the sweep
// itself. The v2 corpus has one unique virtual END; content bytes do not
// determine terminal or collection semantics.
template<class Sink = std::function<void(uint64_t)>>
struct SweepImpl {
    bool cyclic = true;
    uint64_t n = 0, bigN = 0, chi = 0;
    Sink emitWitness;
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
    // ---- ACTIVE-SET INDEX ---------------------------------------------------
    // threshold_reset used to scan all 256 rr_len entries per character change
    // (2 KB of AVX2 work, 57.3M times at 150 MB) to find the set with
    // rr_len[cc] > m3, which MEASURES at 1.47 entries on average. A binary
    // max-heap over rr_len finds exactly that set in O(k log 256) instead: the
    // same set, emitted in ascending cc order (the scalar/AVX2 loop's order), so
    // the witness stream is byte-identical.
    int32_t heap[256];
    int32_t heapPos[256];
    int32_t heapN = 0;
    void heapSwap(int32_t a, int32_t b) {
        std::swap(heap[a], heap[b]); heapPos[heap[a]] = a; heapPos[heap[b]] = b;
    }
    void heapUp(int32_t i) {
        while (i > 0) {
            const int32_t p = (i - 1) / 2;
            if (rr_len[heap[i]] > rr_len[heap[p]]) { heapSwap(i, p); i = p; } else break;
        }
    }
    void heapDown(int32_t i) {
        for (;;) {
            const int32_t l = 2 * i + 1, r = l + 1;
            int32_t b = i;
            if (l < heapN && rr_len[heap[l]] > rr_len[heap[b]]) b = l;
            if (r < heapN && rr_len[heap[r]] > rr_len[heap[b]]) b = r;
            if (b == i) break;
            heapSwap(i, b); i = b;
        }
    }
    void heapInit() {
        heapN = 0;
        for (int cc = 0; cc < 256; ++cc) heapPos[cc] = -1;
        for (int cc = 0; cc < 256; ++cc) {
            heap[heapN] = cc; heapPos[cc] = heapN; ++heapN;
        }
        for (int32_t i = heapN / 2; i >= 0; --i) heapDown(i);
    }
    void heapRaise(int cc, int64_t v) {   // rr_len[cc] increases
        rr_len[cc] = v;
        if (heapPos[cc] >= 0) heapUp(heapPos[cc]);
    }
    // INSTRUMENTATION (product-cheap: plain increments, printed once at finish).
    uint64_t statAdvance = 0, statCharChange = 0, statScanWords = 0, statFired = 0;
    uint64_t statActSeen = 0, statEmit = 0, statMaxAct = 0;
    uint64_t previous_interior = INF;
    int64_t m = IINF;
    int32_t p = -1;
    uint64_t p_sa_last = 0;
    bool first = true;
    explicit SweepImpl(uint64_t nNorm, Sink sink = {})
        : n(nNorm), bigN(nNorm), emitWitness(std::move(sink)) {
        for (int c = 0; c < 256; ++c) { rr_len[c] = -1; rr_pos[c] = 0; rr_act[c] = false; }
        heapInit();
    }
    uint64_t witness(uint64_t pos) const {
        pf_check(pos < n, "chi-rspace: endpoint outside text");
        if (cyclic) return pos == 0 ? 0 : n - pos;
        return bigN - pos;
    }
    void emit(uint64_t value) {
        ++chi; ++statEmit;
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
        // HEAP PATH: pop everything with rr_len > m3 (measured 1.47 entries per
        // call), then emit in ASCENDING cc order - the order the 256-bit scan
        // produced - so the witness stream does not change by one byte.
        {
            int32_t fired[256];
            int32_t nf = 0;
            for (int w = 0; w < 4; ++w) scratch[w] = 0;
            while (heapN && rr_len[heap[0]] > m3 && nf < 256) {
                const int32_t cc = heap[0];
                fired[nf++] = cc;
                if (cc) scratch[cc >> 6] |= uint64_t(1) << (cc & 63);
                rr_len[cc] = m3;
                heapDown(0);
            }
            // ascending cc order == what the old bit-scan produced
            for (int32_t a = 1; a < nf; ++a) {
                const int32_t v = fired[a];
                int32_t b = a - 1;
                while (b >= 0 && fired[b] > v) { fired[b + 1] = fired[b]; --b; }
                fired[b + 1] = v;
            }
            scratch[0] &= ~1ull;   // cc 0 is never in the machine
            // the 256-wide AVX2 scan this replaced is kept for the audit arm
            for (int32_t j = 0; j < nf; ++j) {
                const int cc = fired[j];
                if (!cc) continue;
                if (rr_act[cc]) emit(rr_pos[cc]);
                rr_pos[cc] = 0; rr_act[cc] = false;
            }
            return;
        }
        if (avx2_ok()) gt_masks(rr_len, m3, scratch);
        else gt_masks_scalar(rr_len, m3, scratch);
        scratch[0] &= ~1ull;   // cc 0 is never in the machine
        // what the 256-wide scan actually finds
        {
            uint64_t any = 0;
            for (int w4 = 0; w4 < 4; ++w4) any |= scratch[w4];
            uint64_t bits2 = any, n2 = 0;
            while (bits2) { bits2 &= bits2 - 1; ++n2; }
            statFired += n2; ++statScanWords;
            if (n2 > statMaxAct) statMaxAct = n2;
            statActSeen += n2;
        }
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
        ++statAdvance;
        if (c != p) {
            ++statCharChange;
            int64_t m3 = std::min(mm, lcp_b);
            alignas(32) uint64_t scratch[4];
            threshold_reset(m3, scratch);
            if (lcp_b > rr_len[p]) { heapRaise(p, lcp_b); rr_pos[p] = witness(p_sa_last); rr_act[p] = true; }
            if (lcp_b > rr_len[c]) { heapRaise(c, lcp_b); rr_pos[c] = witness(v.h); rr_act[c] = true; }
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
        fprintf(stderr, "SWEEP_STATS advances=%llu char_changes=%llu scans=%llu fired=%llu "
                        "mean_fired=%.2f max_fired=%llu emits=%llu chi=%llu\n",
                (unsigned long long)statAdvance, (unsigned long long)statCharChange,
                (unsigned long long)statScanWords, (unsigned long long)statFired,
                statScanWords ? double(statFired) / double(statScanWords) : 0.0,
                (unsigned long long)statMaxAct, (unsigned long long)statEmit,
                (unsigned long long)chi);
    }
};

using Sweep = SweepImpl<>;

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

// ------------------------------------------------------ witness position bitmap
// The sweep retains its interval-lifetime selection rule. Membership is keyed
// by text position, not suffix rank. Ascending set bits feed the existing m5
// Elias-Fano codec without an O(chi) vector or external comparison sort.
struct WitnessBitmap {
    uint64_t n, bytes, totalValues = 0, duplicates = 0;
    std::string path;
    int fd = -1;
    uint8_t* bits = nullptr;
    std::vector<uint64_t> delta;
    WitnessBitmap(uint64_t size, const std::string& prefix, bool enabled = true)
        : n(size), bytes(enabled ? size / 8 + (size % 8 != 0) : 0),
          path(prefix + ".chi-bitmap") {
        if (!bytes) return;
        pf_check(bytes <= uint64_t(INT64_MAX) && bytes <= SIZE_MAX, "chi bitmap size overflow");
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | (finish_spool::enabled() ? O_TRUNC : O_EXCL), 0600);
        pf_check(fd >= 0, "create chi bitmap");
        pf_check(::ftruncate(fd, off_t(bytes)) == 0, "size chi bitmap");
        void* map = ::mmap(nullptr, size_t(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        pf_check(map != MAP_FAILED, "mmap chi bitmap");
        bits = static_cast<uint8_t*>(map);
        pf_check(::madvise(bits, size_t(bytes), MADV_RANDOM) == 0, "advise chi bitmap random");
        journal(); // Record the mapping immediately, even if the sweep later fails.
    }
    WitnessBitmap(const WitnessBitmap&) = delete;
    WitnessBitmap& operator=(const WitnessBitmap&) = delete;
    ~WitnessBitmap() { cleanup(); }
    bool add(uint64_t v) {
        pf_check(bits && v < n, "witness out of bitmap range");
        const uint8_t mask = uint8_t(1u << (v & 7));
        if (bits[v >> 3] & mask) { ++duplicates; return false; }
        bits[v >> 3] |= mask;
        ++totalValues;
        if (finish_spool::enabled()) delta.push_back(v);
        return true;
    }
    template<class Fn> void for_each_sorted(Fn fn) {
        if (!bytes) return;
        pf_check(::madvise(bits, size_t(bytes), MADV_SEQUENTIAL) == 0, "advise chi bitmap sequential");
        uint64_t count = 0, prev = 0;
        for (uint64_t byte = 0; byte < bytes; ++byte) {
            unsigned word = bits[byte];
            while (word) {
                const uint64_t v = byte * 8 + unsigned(__builtin_ctz(word));
                pf_check(v < n && (!count || v > prev), "chi positions not strictly increasing");
                fn(v); prev = v; ++count;
                word &= word - 1;
            }
        }
        pf_check(count == totalValues, "chi bitmap popcount mismatch");
    }
    void journal() const {
        struct rusage ru {};
        pf_check(::getrusage(RUSAGE_SELF, &ru) == 0, "chi getrusage");
        fprintf(stderr, "XSA_FINISH_CHI_BITMAP bytes=%llu mmap=1 peak_rss_kb=%llu entries=%llu duplicates=%llu\n",
                (unsigned long long)bytes, (unsigned long long)ru.ru_maxrss,
                (unsigned long long)totalValues, (unsigned long long)duplicates);
    }
    void cleanup() {
        if (bits) { ::munmap(bits, size_t(bytes)); bits = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; ::unlink(path.c_str()); }
    }
};

struct WitnessBatch {
    WitnessBitmap* bitmap;
    int bridgeFd;
    std::array<uint64_t, 4096> values{};
    size_t count = 0;
    void flush() {
        if (bridgeFd >= 0 && count) pf_write_full(bridgeFd, values.data(), count * 8, "bridge sA");
        count = 0;
    }
    void operator()(uint64_t value) {
        if (!bitmap->add(value)) return; // first occurrence owns this position
        if (bridgeFd < 0) return;
        values[count++] = value;
        if (count == values.size()) flush();
    }
};

static int selftest_witness_batch() {
    char dir[] = "/tmp/xsa-chi-bitmap-XXXXXX";
    pf_check(::mkdtemp(dir) != nullptr, "bitmap selftest directory");
    const std::string prefix = std::string(dir) + "/test";
    for (uint64_t n : {1ull, 2ull, 7ull, 8ull, 9ull, 17ull, 65535ull, 100003ull}) {
        {
            WitnessBitmap bitmap(n, prefix);
            WitnessBatch sink{&bitmap, -1};
            for (uint64_t i = n; i > 0; --i) { sink(i - 1); sink(i - 1); }
            sink.flush();
            uint64_t expected = 0;
            bitmap.for_each_sorted([&](uint64_t v) { pf_check(v == expected++, "bitmap selftest order"); });
            pf_check(expected == n && bitmap.totalValues == n && bitmap.duplicates == n,
                     "bitmap selftest uniqueness");
            pf_check(bitmap.bytes == (n + 7) / 8, "bitmap selftest size");
        }
        pf_check(::access((prefix + ".chi-bitmap").c_str(), F_OK) != 0, "bitmap selftest cleanup");
    }
    {
        WitnessBitmap bitmap(100003, prefix);
        const std::vector<uint64_t> expected{0, 7, 8, 31, 99999, 100002};
        const std::string bridge = std::string(dir) + "/bridge";
        int fd = ::open(bridge.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        pf_check(fd >= 0, "selftest bridge open");
        WitnessBatch sink{&bitmap, fd};
        for (auto it = expected.rbegin(); it != expected.rend(); ++it) { sink(*it); sink(*it); }
        sink.flush();
        size_t at = 0;
        bitmap.for_each_sorted([&](uint64_t v) { pf_check(v == expected.at(at++), "sparse bitmap order"); });
        pf_check(at == expected.size(), "sparse bitmap count");
        std::vector<uint64_t> actual(at);
        pf_pread_full(fd, actual.data(), at * 8, 0, "selftest bridge read");
        pf_check(std::equal(actual.begin(), actual.end(), expected.rbegin()), "bridge first occurrence order");
        struct stat st {};
        pf_check(::fstat(fd, &st) == 0 && uint64_t(st.st_size) == at * 8, "bridge duplicate suppression");
        ::close(fd); ::unlink(bridge.c_str());
    }
    {
        WitnessBitmap empty(9, prefix);
        empty.for_each_sorted([](uint64_t) { pf_fail("empty bitmap emitted a witness"); });
    }
    { WitnessBitmap disabled(100, prefix, false); pf_check(disabled.fd < 0, "disabled chi allocated"); }
    pf_check(::rmdir(dir) == 0, "bitmap selftest directory cleanup");
    printf("WITNESS_BATCH_SELFTEST_PASS bitmap=1 duplicates=1 order=1 boundaries=1 cleanup=1 disabled=1\n");
    return 0;
}

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
    BitsOut() = default; // bounded, memory-only encoded worker fragment
    explicit BitsOut(const std::string& path_) : path(path_) {
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | (finish_spool::enabled() ? O_TRUNC : O_EXCL), 0600);
        pf_check(fd >= 0, "create bit spool");
        mem.reserve(1 << 20);
    }
    ~BitsOut() { if (fd >= 0) ::close(fd); if (!path.empty()) ::unlink(path.c_str()); }
    void drain() {
        pf_write_full(fd, mem.data(), mem.size(), "bit spool");
        byteCount += mem.size(); mem.clear();
    }
    void byte(uint8_t v) {
        mem.push_back(v);
        if (fd >= 0 && mem.size() == (1 << 20)) drain();
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
    // Concatenate an unpadded bit sequence at the current bit offset. The
    // source is padded only for storage; its padding bits never enter this
    // stream. In particular, bucket/slot boundaries are NOT byte boundaries.
    void appendPacked(const BitsOut& part) {
        pf_check(part.fd < 0 && part.nbits == 0 &&
                 part.mem.size() == (part.bitPos + 7) / 8, "invalid packed fragment");
        const uint64_t whole = part.bitPos / 8;
        uint64_t at = 0;
        if (!nbits && fd < 0) {
            mem.insert(mem.end(), part.mem.begin(), part.mem.begin() + whole);
            at = whole; bitPos += 8 * whole;
        } else if (!nbits) {
            while (at < whole) {
                size_t take = size_t(std::min<uint64_t>(whole - at, (1 << 20) - mem.size()));
                mem.insert(mem.end(), part.mem.begin() + at, part.mem.begin() + at + take);
                at += take; bitPos += 8 * take;
                if (fd >= 0 && mem.size() == (1 << 20)) drain();
            }
        } else {
            for (; at < whole; ++at) lsb(part.mem[at], 8);
        }
        if (part.bitPos & 7) lsb(part.mem[whole], unsigned(part.bitPos & 7));
    }
    size_t bytes() const { return byteCount + mem.size(); }
    // ---- random-access slice of the spool (block-compressed m1 only)
    // The m1 block writer copies one _contiguous_ byte range per block and the
    // blocks are visited in stream order, so a single forward window is enough:
    // a refill reads SPILL_WINDOW bytes from just before the requested offset.
    // Every range is checked to be inside [0, byteCount); it is NOT checked to
    // be monotone, so a caller bug shows up as a slow read, never as bad bytes.
    std::vector<uint8_t> spill;
    uint64_t spillBase = 0, spillHave = 0;
    static constexpr size_t SPILL_WINDOW = 1u << 20;
    const uint8_t* window(uint64_t off, size_t z) {
        drain();
        pf_check(fd >= 0, "spool window on a memory-only fragment");
        pf_check(off <= byteCount && z <= byteCount - off, "spool window out of range");
        pf_check(z <= SPILL_WINDOW, "spool window larger than the read window");
        if (off >= spillBase && off + z <= spillBase + spillHave)
            return spill.data() + (off - spillBase);
        if (spill.size() != SPILL_WINDOW) spill.resize(SPILL_WINDOW);
        uint64_t take = std::min<uint64_t>(SPILL_WINDOW, byteCount - off);
        pf_pread_full(fd, spill.data(), take, off, "bit spool window");
        spillBase = off; spillHave = take;
        return spill.data();
    }
};

// ---------------------------------------------------- ROUTE B: one-pass m1
//
// ---- m1 RANDOM ACCESS (the access structure, not the product) -----------
// m1 v1 (codec 101) is TWO sequential entropy streams over the whole run
// sequence (Huffman heads, gamma lengths) with no per-run alignment and no
// restart directory: run k is unreachable without decoding runs 0..k, so a
// cold query decodes 4.3 GB and pays 25.8 s / 51 GB RSS before its first
// answer (tools/binbwt/RESULTS_QUERY_QLITE.md).
//
// m1 v3 (codec 114, this arm) keeps the SAME two codes and the SAME runs and
// splits them into blocks that are independently decodable:
//
//   [C table 2048][lens 256][nblocks][m1_block_runs][total_runs][payload_off]
//   [block directory: nblocks x 16 bytes = (payload byte off u64, head bits u32,
//                                     length bits u32)]
//   [block payloads: head bitstream, then length bitstream, both byte-aligned,
//                    each block 8-byte aligned in the payload area]
//
// A block is M1_BLOCK_RUNS runs wide, 8-byte aligned and self-describing, so
// decoding block b is one windowed read of ~200 B plus M1_BLOCK_RUNS Huffman
// symbols and gammas - no predecessor. Nothing about the run SEQUENCE changes:
// same symbols, same lengths, same order, only the boundaries are declared.
//
// The stride is 64 and not 256 because it is also the sampling stride of the
// two new directories below: run k's block, run k's run-start sample and run
// k's symbol-scan window are then the same 64-run window, so ONE block decode
// answers a rank step instead of up to four. Cost per block decode is
// ~64 x (6.0 ns Huffman + 2.0 ns gamma) = 0.5 us (decode rates measured in
// RESULTS_QUERY_QLITE.md); its block directory costs 8 B / 64 runs = 0.043n.
static constexpr uint64_t M1_BLOCK_RUNS = 1024;
// m12: run-start samples. Entry j is the row of run j*M1_BLOCK_RUNS, i.e. the
// first row of that run. 8 B per 64 runs = 0.125 B/run = 434 MB at 10 GB
// (R=3.47e9, 2.88 rows/run), i.e. 0.043n.
static constexpr uint64_t PF_RUNSTART_STRIDE = M1_BLOCK_RUNS;
// m13: per-symbol sampled run directory. Entry (run k, count of c in runs
// before k). Sampled at every PF_RUNDIR_OCC-th occurrence of c AND at any
// occurrence that is PF_RUNDIR_GAP runs past the previous sample, so the scan
// from a sample to a query run is bounded by PF_RUNDIR_GAP runs = one block.
static constexpr uint64_t PF_RUNDIR_OCC = 256;
static constexpr uint64_t PF_RUNDIR_GAP = 1024;
// Level-2 strides of the two run directories (named, not knobs): the level-2
// arrays are read whole at open and bisected in cache, and the level-1 window
// one level below them is what a lookup actually touches.
static constexpr uint64_t PF_RUNSTART_L2 = 65536;   // runs per m12 level-2 cell
static constexpr uint64_t PF_RUNDIR_L2 = 512;       // m13 entries per level-2 cell
// Bytes a block's bit range occupies. A block ALWAYS starts on a byte boundary:
// its predecessor's close called pad(), which flushes the pending partial byte
// and leaves the next msb()/gamma() at the start of the next byte. pad() never
// moves bitPos, so the ABSOLUTE bitPos values lag the physical stream by the pad
// bits already spent: the difference of two ceil(bitPos/8) terms under-counts a
// block (measured: it truncated block spans by up to 7 bits, which the query
// reader reported as "head stream consumed 534 of 540 bits"). The count is the
// ceiling of the block's own bit length, and nothing about lo.
static inline uint64_t blkBytes(uint64_t lo, uint64_t hi) {
    pf_check(hi >= lo, "m1 block bit range is not monotone");
    return (hi - lo + 7) / 8;
}
// The full-width m1 codec id (legacy stream=101, packed blocks=102).
static constexpr unsigned PF_M1_CODEC_BLOCKED = 114;
// THE INCREMENTAL (STREAMING) CONTAINER, version 7, m1 codec 116.
//
// WHY THIS EXISTS. In the v6 layout the 64-byte header and the 40-byte member
// directory are a PREFIX whose size is known but whose CONTENT is not, and m1's
// own per-block directory sits in front of m1's payload. m1's size grows with
// every codec block the sweep produces, so the payload can only be written after
// the sweep has ended: at Pile scale that means the entire encoded run stream
// (~0.277 n) must be staged in scratch before a single container byte exists.
//
// Version 7 writes members in STREAM ORDER and moves both directories to the
// TAIL:
//   [0,64)                      fixed header (length/flags/CRC patched once at close)
//   [64, ...)                   member bytes, appended in stream order
//   [dirOff, dirOff+40*count)   the member directory, dirOff = length-40*count
// and m1 codec 116 is
//   [0,2048) C table  [2048,2304) code lengths  [2304,2336) block_runs/payloadOff
//   [2336, ...) payload, one self-describing block pair at a time, 8-byte aligned
//   [tail 16*nblocks) the per-block directory
// The payload BYTES are identical to codec 114's (same block encoding, same
// padding, same relative offsets); only the directory position changes and the
// two close-time fields (nblocks, rNorm) are left zero, because the reader takes
// nblocks from count/block_runs and rNorm from the member count.
static constexpr unsigned PF_M1_CODEC_STREAM = 116;
static constexpr uint64_t PF_CONTAINER_V7 = 7;
// Container header flag bit: "m1 is block-compressed and the run directories
// are present". The flagged set is 1|8|16|32 = 57.
static constexpr uint64_t PF_FLAG_M1_BLOCKED = 32;

// ===================== THE LCP OVER RUNS (member m14, codec 117) ==============
// WHY. The suffix tree IS the LCP-interval tree: a node is a distinct context,
// its children are contiguous in SA order, and a node's children are delimited by
// the ranks where LCP drops below the node's depth. So the column that makes the
// tree navigable is LCP itself - and until now the finish CONSUMED it (the
// producer's band slices -> `topLcp`/`interior`) and then threw it away, which is
// why nothing in the container can be navigated as a tree (m7 is a 256-byte
// permutation, m11 holds 30.6 M sampled run-tail SA values; see
// CHI_TRANSITIONS.md and VIRTUAL_TREE.md).
//
// WHAT IS PERSISTED, EXACTLY. One value per BWT RUN, in run (stream) order:
//
//     lcpRuns[i] = LCP[r_i] = lce(SA[r_i - 1], SA[r_i]),   r_i = sum(len[0..i-1])
//
// i.e. the LEFT-neighbour LCP at run i's HEAD row, which is exactly what the fold
// already computes per run as `topLcp[i]` (the producer's slice at rank r_i, or
// the LCE walk on the XSA_LCE_VERIFY arm). lcpRuns[0] = 0 is the array's
// conventional sentinel (row 0 has no left neighbour). r-space is the point: R is
// ~3x smaller than n at the Pile (31,381,597,694 vs 100,093,934,467) and it is
// the space m1/m12/m13 are already organised in.
//
// NOT PERSISTED, AND SAYING SO: the LCP values of the rows INSIDE a run. A run's
// interior rows carry their own drops (an all-equal text is one run whose interior
// is the whole tree), so this member alone delimits children at RUN granularity.
// The finish computes that interior minimum (`interior[i]`) for chi; it is not
// part of m14.
//
// ENCODING: blocks of PF_LCP_BLOCK_RUNS runs, each block independently decodable,
// one 16-byte directory entry per block in a TRAILING section (the v7 rule: a
// directory the writer cannot know before the bytes exists cannot be a prefix).
// A block is encoded with the cheaper of two exact schemes - see encodeLcpBlock -
// and the values are u64 in BOTH schemes, so nothing is ever truncated: an LCP of
// n-1 survives a fixture as readily as a Pile LCP of 30. Random access =
// dir[i/block] (one pread) + one bit-slice (or one byte) at a computed offset +
// a binary search in that block's small escape list. Never a linear scan.
static constexpr uint64_t PF_CONTAINER_V8 = 8;
static constexpr unsigned PF_LCP_CODEC = 117;
static constexpr uint64_t PF_LCP_BLOCK_RUNS = 1024;
static constexpr uint64_t PF_LCP_HEAD = 40;      // fixed head, never patched
static constexpr unsigned PF_LCP_ENC_VERSION = 1;
// Container header flag bit 7: "member m14 (LCP over runs) is present".
// Version 8 sets it; version 7 (--no-lcp-runs) must not.
static constexpr uint64_t PF_FLAG_LCP_RUNS = 128;

// THE PROBLEM the two-pass m1 solved: its head stream is a Huffman code over
// the NORMALIZED RUN SYMBOLS, and the codebook was built from run frequencies
// that are only complete at the end of the sweep. So the sweep could not
// encode; the merge's rlebwt wire (4+ bytes per run) staged every run, and m1
// replayed it from the beginning (the "m1-rle bwt wait" + "m1 replay" phases).
// That wire is 2.09n of allocated scratch at 10 GB - more than half the
// measured 3.976n whole-run peak.
//
// WHY ONE PASS SUFFICES: the BWT is a PERMUTATION of the text, so the histogram
// of the symbols it emits is the histogram of the text's bytes. The m1 symbol
// histogram is therefore knowable BEFORE the first run exists - it is the
// remapped corpus byte histogram, and the finish already owns the corpus. What
// the corpus histogram is NOT is the histogram of run STARTS (a frequent byte
// forms long runs, so it starts fewer runs than its byte share), which is why
// the corpus code is a slightly longer code than the exact one; m1 PUBLISHES
// its code lengths, so the decoder is unaffected and the streams stay exactly
// decodable. Measured on the accepted 10 GB container (tools/binbwt/m1_stream
// --hist): corpus-histogram lengths cost 5.411692 bits/run against 5.311431 for
// the exact-run lengths, i.e. +1.89% of the head stream = +0.0044n of container
// bytes, in exchange for -2.09n of scratch and the whole replay phase.
//
// THE CANONICAL DERIVATION (reproducible, and the decoder's inverse):
//   1. weights w[c] = max(corpus byte count of c, 1). The +1 pseudo-count is
//      what makes the code TOTAL: every one of the 256 symbols gets a code, so
//      no emitted symbol can lack one and no length is ever zero.
//   2. Huffman tree by (weight, creation index) - the priority queue is keyed
//      on (w, idx) exactly as the frozen legacy derivation is, so ties break by
//      ascending symbol index first, then by internal-node creation order.
//   3. lengths by an iterative depth-first walk; every leaf depth is its length.
//   4. canonical codes: sort symbols by (length, symbol) ascending, then
//      code = 0; for each in that order: code <<= (len - previous_len);
//      codes[symbol] = code++; previous_len = len. That is the same
//      length-then-symbol canonical form the m1 reader reconstructs from its
//      256 published lengths alone (tools/binbwt/stream_vs_bwt.cpp M1, and
//      xsa/src/sxi2.rs), so this encoder needs to publish nothing new.
// The head stream is written MSB-first inside each code (BitsOut::msb) and the
// lengths keep the existing gamma code, byte for byte.
struct M1Code {
    std::array<uint64_t, 256> lens{};
    std::array<uint64_t, 256> codes{};
};

static M1Code canonical_huffman_m1(const std::array<uint64_t, 256>& weight) {
    M1Code out;
    struct Node { uint64_t w; int left, right, sym; };
    std::vector<Node> tree;
    tree.reserve(512);
    using Q = std::pair<uint64_t, int>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
    for (int c = 0; c < 256; ++c) {
        const uint64_t w = std::max<uint64_t>(weight[size_t(c)], 1);
        q.push({w, int(tree.size())});
        tree.push_back({w, -1, -1, c});
    }
    while (q.size() > 1) {
        const auto a = q.top(); q.pop();
        const auto b = q.top(); q.pop();
        const uint64_t w = a.first + b.first;
        pf_check(w >= a.first && w >= b.first, "m1 codebook weight overflow");
        tree.push_back({w, a.second, b.second, -1});
        q.push({w, int(tree.size() - 1)});
    }
    std::vector<std::pair<int, unsigned>> stack{{int(q.top().second), 0}};
    while (!stack.empty()) {
        const auto [idx, depth] = stack.back(); stack.pop_back();
        if (tree[size_t(idx)].sym >= 0) {
            pf_check(depth >= 1 && depth <= 63, "m1 codebook depth exceeds 63 bits");
            out.lens[size_t(tree[size_t(idx)].sym)] = depth;
            continue;
        }
        pf_check(tree[size_t(idx)].right >= 0, "m1 codebook internal node without two children");
        stack.push_back({tree[size_t(idx)].left, depth + 1});
        stack.push_back({tree[size_t(idx)].right, depth + 1});
    }
    std::vector<std::pair<unsigned, unsigned>> order;
    order.reserve(256);
    for (unsigned c = 0; c < 256; ++c) {
        pf_check(out.lens[c] >= 1 && out.lens[c] <= 63, "m1 codebook is not total");
        order.push_back({unsigned(out.lens[c]), c});
    }
    std::sort(order.begin(), order.end());
    uint64_t code = 0;
    unsigned prevLen = 0;
    for (const auto& [len, sym] : order) {
        code <<= (len - prevLen);
        pf_check(code < (uint64_t(1) << len), "m1 canonical code is oversubscribed");
        out.codes[sym] = code++;
        prevLen = len;
    }
    return out;
}

// The remapped corpus byte histogram: m1's codebook weights. `read_block` is
// the adapter's own byte path (T[i] = REMAP[corpus[i]]), so this counts the
// NORMALIZED symbols, i.e. exactly the alphabet m1 emits. Blocks are strided
// across threads; every count is exact and the total is required to be n.
static std::array<uint64_t, 256> corpus_byte_histogram(const CorpusText& text, int threads) {
    const uint64_t n = text.size();
    const int T = std::max(1, std::min<int>(threads, 32));
    constexpr uint64_t block = 1ull << 20;
    const uint64_t blocks = (n + block - 1) / block;
    std::vector<std::array<uint64_t, 256>> partial(static_cast<size_t>(T));
    std::vector<std::thread> pool;
    pool.reserve(size_t(T));
    for (int t = 0; t < T; ++t) {
        pool.emplace_back([&, t] {
            std::vector<uint8_t> buf(static_cast<size_t>(block));
            auto& h = partial[size_t(t)];
            for (uint64_t b = uint64_t(t); b < blocks; b += uint64_t(T)) {
                const uint64_t off = b * block;
                const uint64_t len = std::min<uint64_t>(block, n - off);
                if (!len) continue;
                text.read_block(off, buf.data(), len);
                for (uint64_t i = 0; i < len; ++i) ++h[buf[size_t(i)]];
            }
        });
    }
    for (auto& th : pool) th.join();
    std::array<uint64_t, 256> total{};
    for (const auto& h : partial)
        for (unsigned c = 0; c < 256; ++c) {
            pf_check(total[c] <= UINT64_MAX - h[c], "corpus histogram overflow");
            total[c] += h[c];
        }
    uint64_t sum = 0;
    for (unsigned c = 0; c < 256; ++c) { pf_check(sum <= UINT64_MAX - total[c], "corpus histogram total overflow"); sum += total[c]; }
    pf_check(sum == n, "corpus histogram total != n");
    return total;
}

// A helper thread that must never outlive the main scope: pf_fail() exits, but
// an exception must not reach std::thread's terminate().
struct ThreadGuard {
    std::thread t;
    ThreadGuard() = default;
    ThreadGuard(const ThreadGuard&) = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;
    ~ThreadGuard() { if (t.joinable()) t.join(); }
    void join() { if (t.joinable()) t.join(); }
};

static int selftest_packed_bits() {
    uint64_t cases = 0;
    for (unsigned offset = 0; offset < 64; ++offset) {
        BitsOut expected, actual;
        for (unsigned j = 0; j < offset; ++j) { expected.bit(j & 1); actual.bit(j & 1); }
        for (unsigned size = 0; size <= 257; ++size) {
            BitsOut part;
            for (unsigned j = 0; j < size; ++j) {
                const unsigned bit = unsigned((j * 13 + size * 7) ^ (j >> 2)) & 1;
                expected.bit(bit); part.bit(bit);
            }
            part.pad(); actual.appendPacked(part); ++cases;
        }
        expected.pad(); actual.pad();
        pf_check(expected.bitPos == actual.bitPos && expected.mem == actual.mem,
                 "packed concatenation disagrees with scalar bits");
    }
    BitsOut large, joined;
    large.putzeros((1 << 23) + 17); large.bit(1); large.pad();
    joined.appendPacked(large); joined.pad();
    pf_check(joined.bitPos == large.bitPos && joined.mem == large.mem,
             "large packed fragment disagrees");
    printf("PACKED_BITS_SELFTEST_PASS fragments=%llu offsets=64 empty=1 padding=1 large=1\n",
           (unsigned long long)cases);
    return 0;
}

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
// ===================== m14: the block codec (writer side) ====================
// One block = PF_LCP_BLOCK_RUNS consecutive runs' LCP values. Two exact schemes,
// the cheaper one wins PER BLOCK (mode is inside the block, so the reader never
// needs the writer's reasons):
//
//   mode 0  every value is zero                     -> 1 byte
//   mode 1  u8 array, value < 255 inline, 255 = escape
//   mode 2  w-bit packed array (1 <= w <= 32), 0 inline means escape
//   then    esc_count u32, then esc_count * {u16 index, u64 value} (10 B each,
//           sorted by index, so a lookup BINARY SEARCHES them)
//   then    zero padding to an 8-byte multiple
//
// Both schemes carry escapes as full u64, so the codec is lossless for every LCP
// in [0, n-1] - a fixture whose whole text is one run (LCP ~ n) and a Pile run
// with LCP 3 are the same format. Mode 2 exists because the LCP array is skewed:
// a block of 1024 runs whose values are almost all <= 3 but contains one 300 must
// not pay 9 bits for all 1024 (mode 2 with w=4 and one escape is 4x cheaper).
static inline unsigned lcpBits(uint64_t v) { return v ? 64u - unsigned(__builtin_clzll(v)) : 0u; }


// Encode one block into `o`; returns the block's byte length INCLUDING the
// 8-byte padding, and appends the bytes to `o` and the escape count to `escOut`.
static size_t encodeLcpBlock(const uint64_t* v, size_t n, std::vector<uint8_t>& o, uint64_t& escOut,
                             uint8_t* modeOut, unsigned* widthOut) {
    pf_check(n > 0 && n <= PF_LCP_BLOCK_RUNS, "LCP block size");
    const size_t start = o.size();
    uint32_t hist[65] = {0};
    unsigned maxb = 0;
    size_t n255 = 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned b = lcpBits(v[i]);
        ++hist[b];
        if (b > maxb) maxb = b;
        if (v[i] == 255) ++n255;
    }
    if (maxb == 0) {
        o.push_back(0);
        while ((o.size() - start) % 8) o.push_back(0);
        escOut = 0; *modeOut = 0; *widthOut = 0;
        return o.size() - start;
    }
    // CHOOSE THE CHEAPER SCHEME, from the width histogram alone (one pass over the
    // block, not one per candidate): escapes cost a fixed 10 bytes each.
    static const unsigned widths[] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 20, 24, 32};
    size_t escA = n255;
    for (unsigned b = 9; b <= 64; ++b) escA += hist[b];
    size_t bestBytes = 1 + n + 4 + 10 * escA;   // mode 1
    unsigned bestMode = 1, bestW = 0;
    for (unsigned w : widths) {
        size_t above = 0;
        for (unsigned b = w + 1; b <= 64; ++b) above += hist[b];
        const size_t bytes = 1 + 1 + (n * size_t(w) + 7) / 8 + 4 + 10 * above;
        if (bytes < bestBytes) { bestBytes = bytes; bestMode = 2; bestW = w; }
    }
    // THE MARKER IS THE ALL-ONES FIELD, and a GENUINE all-ones value escapes too:
    // an inline 0 cannot mark an escape because 0 is the most common LCP in the
    // array (the mode-2 selftest caught exactly that).
    const uint64_t limit = bestMode == 2 ? ((uint64_t(1) << bestW) - 1) : 255;
    uint64_t esc = 0;
    for (size_t i = 0; i < n; ++i) if (v[i] >= limit) ++esc;
    o.push_back(uint8_t(bestMode));
    if (bestMode == 1) {
        for (size_t i = 0; i < n; ++i) o.push_back(v[i] >= limit ? uint8_t(255) : uint8_t(v[i]));
    } else {
        o.push_back(uint8_t(bestW));
        const size_t at = o.size(), packed = (n * size_t(bestW) + 7) / 8;
        o.resize(at + packed, 0);
        uint8_t* p = o.data() + at;
        uint64_t acc = 0; unsigned have = 0; size_t oi = 0;
        for (size_t i = 0; i < n; ++i) {
            acc |= (v[i] >= limit ? limit : v[i]) << have;
            have += bestW;
            while (have >= 8) { p[oi++] = uint8_t(acc); acc >>= 8; have -= 8; }
        }
        if (have) p[oi++] = uint8_t(acc);
        pf_check(oi == packed, "LCP packed block length");
    }
    {
        uint8_t e[4];
        put(e, esc, 4);
        o.insert(o.end(), e, e + 4);
    }
    for (size_t i = 0; i < n; ++i) {
        if (v[i] < limit) continue;
        uint8_t e[10];
        put(e, uint16_t(i), 2);
        put(e + 2, v[i], 8);
        o.insert(o.end(), e, e + 10);
    }
    while ((o.size() - start) % 8) o.push_back(0);
    escOut = esc; *modeOut = uint8_t(bestMode); *widthOut = bestW;
    return o.size() - start;
}

// Decode value #j of ONE block - the reader's random-access rule, verbatim, so the
// writer's selftest exercises exactly the rule a container reader uses: one bit
// slice (mode 2) or one byte (mode 1), then a binary search in the block's escape
// list. Never a scan over the block, never a walk over preceding blocks.
static uint64_t decodeLcpBlockValue(const uint8_t* b, size_t n, size_t j) {
    pf_check(j < n, "LCP block index");
    const uint8_t mode = b[0];
    if (mode == 0) return 0;
    uint64_t inlineVal = 0;
    bool escaped = false;
    size_t escAt = 0;
    if (mode == 1) {
        inlineVal = b[1 + j];
        escaped = inlineVal == 255;
        escAt = 1 + n;
    } else if (mode == 2) {
        const unsigned w = b[1];
        pf_check(w >= 1 && w <= 32, "LCP block width");
        const uint64_t bit = uint64_t(j) * w;
        const uint8_t* p = b + 2 + size_t(bit / 8);
        const unsigned sh = unsigned(bit % 8);
        uint64_t acc = 0;
        for (unsigned i = 0; i < 5; ++i) acc |= uint64_t(p[i]) << (8 * i);
        inlineVal = (acc >> sh) & ((uint64_t(1) << w) - 1);
        escaped = inlineVal == ((uint64_t(1) << w) - 1);   // all-ones field = escape
        escAt = 2 + (n * size_t(w) + 7) / 8;
    } else {
        pf_fail("LCP block mode");
    }
    if (!escaped) return inlineVal;
    const uint8_t* e = b + escAt;
    const uint64_t count = sxi::get(e, 4);
    pf_check(count <= n, "LCP block escape count");
    const uint8_t* list = e + 4;
    size_t lo = 0, hi = size_t(count);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const uint64_t idx = sxi::get(list + 10 * mid, 2);
        if (idx == j) return sxi::get(list + 10 * mid + 2, 8);
        if (idx < j) lo = mid + 1; else hi = mid;
    }
    pf_fail("LCP block escape list has no entry for an escaped value");
}

// The run-LCP writer: values arrive in RUN ORDER from the serial fold, blocks are
// encoded and appended to a payload spool, and the 16-byte-per-block directory is
// accumulated in RAM (16 B per 1024 runs - the same shape m1's own block
// directory already carries).
struct LcpRunsWriter {
    int fd = -1;
    std::string spoolPath;
    std::vector<uint64_t> blk;
    std::vector<uint8_t> dir;    // per CLOSED block: off u64, span u32, esc u32
    std::vector<uint8_t> out;
    uint64_t payloadBytes = 0;   // bytes of CLOSED blocks (the next block's offset)
    uint64_t count = 0;          // values accepted
    uint64_t blocks = 0;
    uint64_t escapes = 0;
    uint64_t maxValue = 0;
    uint64_t u8Blocks = 0, packBlocks = 0, zeroBlocks = 0;
    uint64_t packedBits = 0;     // bits spent on inline fields
    // DEPTH ACCOUNTING. The member stores the LCP at FULL depth (u64, exact), but
    // the question the tree operations actually ask is shallow: the median context
    // that repeats anywhere in 100 GB of Pile is 18 bytes, entropy is 0 by depth
    // 32 (CONTEXT_DEPTH.md), and a node deeper than that delimits unique suffixes.
    // So the writer counts where the depth goes, which is what makes "how much
    // would a depth-limited member save" a MEASUREMENT instead of a guess: a value
    // above 64 that had to leave its block as a full u64 escape costs 10 bytes,
    // and `deepEscapeBytes` is exactly those bytes.
    uint64_t depthBuckets[8] = {0};   // 0 | 1-8 | 9-16 | 17-32 | 33-64 | 65-255 | 256-64K | 64K+
    uint64_t deepEscapeRuns = 0;      // escapes whose value exceeds 64
    uint64_t deepEscapeBytes = 0;     // 10 B each
    durable_piece::Sha256 sha;   // running digest of the payload bytes
    bool on = false;

    void open(const std::string& path, bool resume) {
        spoolPath = path;
        // A FRESH build must not find a stale spool (unless the spool mode owns it);
        // a RESUMING build keeps every byte the checkpoint's frontier covers - the
        // prefix digest is verified before a byte is appended (resumeFrontier).
        const int extra = resume ? 0 : (finish_spool::enabled() ? O_TRUNC : O_EXCL);
        if (!resume && !finish_spool::enabled())
            pf_check(::access(spoolPath.c_str(), F_OK) != 0, "LCP runs scratch exists");
        fd = ::open(spoolPath.c_str(), O_RDWR | O_CREAT | extra, 0666);
        pf_check(fd >= 0, "create LCP runs scratch");
        blk.reserve(PF_LCP_BLOCK_RUNS);
        dir.reserve(1 << 12);
        out.reserve(1 << 22);
        on = true;
    }
    inline void push(uint64_t v) {
        if (v > maxValue) maxValue = v;
        ++depthBuckets[depthBucketOf(v)];
        blk.push_back(v);
        ++count;
        if (blk.size() == PF_LCP_BLOCK_RUNS) flushBlock();
    }
    static unsigned depthBucketOf(uint64_t v) {
        if (v == 0) return 0;
        if (v <= 8) return 1;
        if (v <= 16) return 2;
        if (v <= 32) return 3;
        if (v <= 64) return 4;
        if (v <= 255) return 5;
        if (v <= 65535) return 6;
        return 7;
    }
    void flushBlock() {
        pf_check(on, "LCP runs write before open");
        const size_t start = out.size();
        uint64_t esc = 0;
        uint8_t mode = 0;
        unsigned w = 0;
        const size_t span = encodeLcpBlock(blk.data(), blk.size(), out, esc, &mode, &w);
        pf_check(span % 8 == 0, "LCP block not 8-byte aligned");
        // The escape list is written for every value >= the block's limit; a value
        // above 64 is by definition in it: charge its 10 bytes to DEPTH.
        for (uint64_t v : blk)
            if (v > 64 && v >= (mode == 2 ? ((uint64_t(1) << w) - 1) : 255)) {
                ++deepEscapeRuns;
                deepEscapeBytes += 10;
            }
        if (mode == 0) ++zeroBlocks; else if (mode == 1) ++u8Blocks; else { ++packBlocks; packedBits += uint64_t(w) * blk.size(); }
        escapes += esc;
        uint8_t d[16];
        put(d, payloadBytes, 8);
        put(d + 8, uint32_t(span), 4);
        put(d + 12, uint32_t(esc > 0xffffffffu ? 0xffffffffu : esc), 4);
        dir.insert(dir.end(), d, d + 16);
        payloadBytes += span;
        ++blocks;
        blk.clear();
        if (out.size() >= (4u << 20)) flushOut();
        (void)start;
    }
    void flushOut() {
        if (out.empty()) return;
        pf_write_full(fd, out.data(), out.size(), "LCP runs payload");
        sha.update(out.data(), out.size());
        out.clear();
    }
    // close the OPEN block (the last, short one) and make the spool durable.
    void finish() {
        if (blk.size()) flushBlock();
        flushOut();
        pf_check(::fsync(fd) == 0, "fsync LCP runs scratch");
    }
    void syncSpool() { pf_check(::fsync(fd) == 0, "fsync LCP runs scratch (checkpoint)"); }
    // the fixed 40-byte member head (nothing here is only known at close).
    void head(uint8_t* h, uint64_t n) const {
        memset(h, 0, PF_LCP_HEAD);
        put(h, PF_LCP_BLOCK_RUNS, 8);
        put(h + 8, n, 8);
        put(h + 16, PF_LCP_HEAD, 8);      // payload offset (constant)
        put(h + 24, 0, 8);                // count: derived (= the member count)
        put(h + 32, PF_LCP_ENC_VERSION, 4);
        put(h + 36, PF_LCP_CODEC, 4);
    }
    uint64_t memberBytes() const { return PF_LCP_HEAD + payloadBytes + 16 * blocks; }
    std::string shaHex() const { durable_piece::Sha256 t = sha; return t.finish(); }
    // Empty the spool (a fresh build; or a checkpoint that no longer exists).
    void truncateSpool() {
        pf_check(::ftruncate(fd, 0) == 0, "truncate LCP runs scratch");
        pf_check(::lseek(fd, 0, SEEK_SET) == 0, "seek LCP runs scratch");
        blk.clear(); dir.clear(); out.clear();
        payloadBytes = 0; count = 0; blocks = 0; escapes = 0; maxValue = 0;
        u8Blocks = packBlocks = zeroBlocks = packedBits = 0;
        for (auto& b : depthBuckets) b = 0;
        deepEscapeRuns = deepEscapeBytes = 0;
        sha = durable_piece::Sha256();
    }
    // RESUME: the spool is append-only in units of CLOSED blocks, so the durable
    // prefix IS the checkpoint's frontier. Verify it by digest (never trust a
    // length), then discard whatever a killed run appended past it.
    void resumeFrontier(uint64_t bytes, const std::string& expectSha, uint64_t expectBlocks) {
        struct stat st {};
        pf_check(::fstat(fd, &st) == 0, "stat LCP runs scratch");
        const uint64_t before = uint64_t(st.st_size);
        pf_check(before >= bytes, "retained LCP runs scratch is shorter than its checkpoint frontier");
        durable_piece::Sha256 s;
        std::vector<uint8_t> buf(1 << 20);
        for (uint64_t pos = 0; pos < bytes;) {
            size_t z = size_t(std::min<uint64_t>(buf.size(), bytes - pos));
            pf_pread_full(fd, buf.data(), z, pos, "LCP runs prefix verify");
            s.update(buf.data(), z);
            pos += z;
        }
        const std::string got = s.finish();
        pf_check(got == expectSha,
                 "retained LCP runs scratch digest does not match its checkpoint (" + got + " vs " + expectSha + ")");
        sha = s;
        pf_check(::ftruncate(fd, off_t(bytes)) == 0, "truncate uncommitted LCP runs tail");
        pf_check(::lseek(fd, off_t(bytes), SEEK_SET) == 0, "seek LCP runs frontier");
        pf_check(payloadBytes == bytes && blocks == expectBlocks,
                 "restored LCP runs state disagrees with the durable spool frontier");
        fprintf(stderr, "RESUME_MODE lcp_runs_bytes=%llu blocks=%llu file_bytes_before=%llu truncated_bytes=%llu\n",
                (unsigned long long)bytes, (unsigned long long)blocks, (unsigned long long)before,
                (unsigned long long)(before - bytes));
    }
};

// Round-trip oracle for the m14 block codec: every value of every block must come
// back EXACTLY through the random-access rule. Adversarial by construction - all
// zeros, one huge outlier, the 254/255 mode boundary, u32/u64 boundaries, a whole
// text that is one run, and pseudo-random mixes - because a wrong LCP is worse
// than no LCP.
static int selftest_lcp_runs() {
    uint64_t cases = 0, values = 0;
    // THE CROSS-IMPLEMENTATION DUMP. The Rust accessor (xsa/src/lcpruns.rs) is a
    // SECOND decoder of these bytes, and a decoder that shares a writer's
    // misunderstanding is no oracle at all. With LCP_BLOCK_DUMP=<path> this
    // selftest writes the C++ encoder's exact bytes for every adversarial case
    // together with the values they mean, so the Rust side can be proved against
    // the WRITER, not against itself:
    //   "LCPBLK01" | u64 records | per record: u64 n, u64 span, span bytes, n*u64
    FILE* dump = nullptr;
    if (const char* dp = std::getenv("LCP_BLOCK_DUMP")) {
        pf_check(*dp, "LCP_BLOCK_DUMP must not be empty");
        dump = std::fopen(dp, "wb");
        pf_check(dump != nullptr, "create LCP_BLOCK_DUMP");
        pf_check(std::fwrite("LCPBLK01", 1, 8, dump) == 8, "write LCP_BLOCK_DUMP magic");
        const uint64_t zero = 0;
        pf_check(std::fwrite(&zero, 8, 1, dump) == 1, "write LCP_BLOCK_DUMP count");
    }
    auto emit = [&](const uint8_t* bytes, size_t span, const uint64_t* want, size_t n) {
        if (!dump) return;
        const uint64_t n64 = n, sp64 = span;
        pf_check(std::fwrite(&n64, 8, 1, dump) == 1, "write LCP_BLOCK_DUMP n");
        pf_check(std::fwrite(&sp64, 8, 1, dump) == 1, "write LCP_BLOCK_DUMP span");
        pf_check(std::fwrite(bytes, 1, span, dump) == span, "write LCP_BLOCK_DUMP bytes");
        pf_check(std::fwrite(want, 8, n, dump) == n, "write LCP_BLOCK_DUMP values");
    };
    std::vector<std::vector<uint64_t>> suite;
    for (size_t n : {size_t(1), size_t(2), size_t(3), size_t(17), size_t(255), size_t(256),
                     size_t(1023), size_t(PF_LCP_BLOCK_RUNS)}) {
        std::vector<uint64_t> z(n, 0);
        suite.push_back(z);
        std::vector<uint64_t> ones(n, 1);
        suite.push_back(ones);
        std::vector<uint64_t> bite(n, 0);
        bite[n / 2] = 1000000;                     // one outlier: escapes must fire
        suite.push_back(bite);
        std::vector<uint64_t> edge(n);
        for (size_t i = 0; i < n; ++i) {
            static const uint64_t v[] = {0, 1, 254, 255, 256, 65535, 65536, 0xffffffffull,
                                         0x100000000ull, 18446744073709551615ull, 3, 4095, 4096};
            edge[i] = v[i % (sizeof(v) / sizeof(v[0]))];
        }
        suite.push_back(edge);
        std::vector<uint64_t> ramp(n);
        for (size_t i = 0; i < n; ++i) ramp[i] = i;
        suite.push_back(ramp);
        std::vector<uint64_t> rnd(n);
        uint64_t st = 0x9e3779b97f4a7c15ull ^ (n * 131);
        for (size_t i = 0; i < n; ++i) {
            st ^= st << 13; st ^= st >> 7; st ^= st << 17;
            rnd[i] = (st >> (i % 61)) & ((uint64_t(1) << (i % 41 + 1)) - 1);
        }
        suite.push_back(rnd);
        std::vector<uint64_t> skew(n);             // the real shape: tiny values, rare deep ones
        for (size_t i = 0; i < n; ++i) skew[i] = (i % 97 == 0) ? (uint64_t(300) + i) : (i % 4);
        suite.push_back(skew);
    }
    for (const auto& v : suite) {
        std::vector<uint8_t> o;
        uint64_t esc = 0;
        uint8_t mode = 0;
        unsigned w = 0;
        const size_t span = encodeLcpBlock(v.data(), v.size(), o, esc, &mode, &w);
        pf_check(span % 8 == 0 && o.size() == span, "LCP block span is not the encoded bytes");
        emit(o.data(), span, v.data(), v.size());
        for (size_t j = 0; j < v.size(); ++j) {
            const uint64_t back = decodeLcpBlockValue(o.data(), v.size(), j);
            if (back != v[j]) {
                fprintf(stderr, "LCP_RUNS_SELFTEST_FAIL n=%zu j=%zu mode=%u w=%u want=%llu got=%llu\n",
                        v.size(), j, mode, w, (unsigned long long)v[j], (unsigned long long)back);
                return 1;
            }
            ++values;
        }
        ++cases;
    }
    // multi-block: a block boundary must not leak state either way
    {
        LcpRunsWriter wtr;
        std::vector<uint64_t> all;
        for (size_t i = 0; i < PF_LCP_BLOCK_RUNS * 2 + 7; ++i) all.push_back((i * 2654435761ull) % 9);
        std::vector<uint8_t> blk;
        uint64_t esc = 0;
        uint8_t mode = 0;
        unsigned wd = 0;
        size_t at = 0;
        std::vector<std::pair<size_t, size_t>> spans;
        while (at < all.size()) {
            const size_t take = std::min<size_t>(PF_LCP_BLOCK_RUNS, all.size() - at);
            const size_t sp = encodeLcpBlock(all.data() + at, take, blk, esc, &mode, &wd);
            const size_t start = blk.size() - sp;
            emit(blk.data() + start, sp, all.data() + at, take);
            for (size_t j = 0; j < take; ++j)
                pf_check(decodeLcpBlockValue(blk.data() + start, take, j) == all[at + j],
                         "LCP multi-block round trip");
            at += take;
            ++cases;
        }
        values += all.size();
        (void)wtr;
    }
    
    if (dump) {
        // Patch the record count that the magic reserved: the reader refuses a
        // truncated dump by count, so a half-written file can never be read as a
        // shorter suite.
        const uint64_t records = cases;
        pf_check(std::fseek(dump, 8, SEEK_SET) == 0, "seek LCP_BLOCK_DUMP count");
        pf_check(std::fwrite(&records, 8, 1, dump) == 1, "patch LCP_BLOCK_DUMP count");
        pf_check(std::fclose(dump) == 0, "close LCP_BLOCK_DUMP");
        fprintf(stderr, "LCP_RUNS_SELFTEST_DUMP path=%s records=%llu (the C++ encoder's exact "
                        "bytes + the values they mean, for the Rust decoder)\n",
                std::getenv("LCP_BLOCK_DUMP"), (unsigned long long)records);
    }
    printf("LCP_RUNS_SELFTEST_PASS blocks=%llu values=%llu exact=1 modes=zero|u8+escape|packed+escape\n",
           (unsigned long long)cases, (unsigned long long)values);
    return 0;
}

struct ContainerOut {
    std::string path, temp;
    int fd = -1;
    unsigned count = 0;
    std::vector<sxi::Member> ms;
    bool done = false;
    // ---- STREAMING LAYOUT (container version 7, see PF_M1_CODEC_STREAM) ----
    // `streaming` selects the tail-directory layout: the header reserve is a
    // FIXED 64 bytes and one member (m1) is appended in place as its blocks are
    // produced. `extent` is the first free byte after the last member and
    // `streamIdx` is the member being appended to.
    bool streaming = false;
    uint64_t writeVersion = 0;   // 0 = the layout's default (v7 streaming, v6 prefix)
    uint64_t extent = 0;
    int64_t streamIdx = -1;
    uint64_t streamPayload = 0;   // payload bytes appended (m1: from 2336)
    uint64_t syncEvery = 1ull << 30;
    uint64_t unsynced = 0;
    uint64_t lastSyncBytes = 0;
    // Running SHA-256 of the streaming member's bytes IN FILE ORDER, so a
    // checkpoint can name the durable prefix by digest and a restart can verify
    // it instead of trusting a length.
    durable_piece::Sha256 memberSha;
    bool resume = false;
    std::string m1ShaHex() const { durable_piece::Sha256 t = memberSha; return t.finish(); }
    uint64_t headerBytes() const { return streaming ? 64 : 64 + 40 * uint64_t(count); }
    // RESUME: reopen the retained .partial for append instead of creating it.
    // Nothing is written here; the caller verifies the durable prefix and then
    // calls resumeFrontier(), which truncates the uncommitted tail.
    ContainerOut(const std::string& p, unsigned count_, bool streaming_, bool resume_)
        : path(p), temp(p + ".partial"), count(count_), streaming(streaming_), resume(resume_) {
        pf_check(streaming_ && resume_, "resume needs the streaming layout");
        fd = ::open(temp.c_str(), O_RDWR);
        pf_check(fd >= 0, "reopen retained container partial for append");
        struct stat st {};
        pf_check(::fstat(fd, &st) == 0 && uint64_t(st.st_size) >= 64, "stat retained container partial");
        ms.push_back({1, PF_M1_CODEC_STREAM, 64, uint64_t(st.st_size) - 64, 0, ~0u});
        streamIdx = 0;
        extent = uint64_t(st.st_size);
    }
    // Restore the append frontier: hash and CRC the durable member prefix in ONE
    // sequential pass, refuse a digest that does not match the checkpoint, then
    // discard whatever a killed run appended after that frontier.
    void resumeFrontier(uint64_t payloadBytes, const std::string& expectSha) {
        pf_check(resume, "resumeFrontier on a fresh container");
        const uint64_t end = 2336 + payloadBytes;
        struct stat st {};
        pf_check(::fstat(fd, &st) == 0, "stat retained container partial");
        const uint64_t before = uint64_t(st.st_size);
        pf_check(before >= 64 + end, "retained container prefix is shorter than its checkpoint frontier");
        durable_piece::Sha256 sha;
        std::vector<uint8_t> buf(1 << 20);
        uint32_t crcState = ~0u;
        for (uint64_t pos = 0; pos < end;) {
            size_t z = size_t(std::min<uint64_t>(buf.size(), end - pos));
            pf_pread_full(fd, buf.data(), z, 64 + pos, "resume prefix verify");
            sha.update(buf.data(), z);
            crcState = crc(crcState, buf.data(), z);
            pos += z;
        }
        const std::string got = sha.finish();
        pf_check(got == expectSha,
                 "retained container prefix digest does not match its checkpoint (" + got + " vs " + expectSha + ")");
        memberSha = sha;
        ms[0].bytes = end; ms[0].checksum = crcState; extent = 64 + end;
        if (before > 64 + end) pf_check(::ftruncate(fd, off_t(64 + end)) == 0, "truncate uncommitted container tail");
        fprintf(stderr, "RESUME_MODE container_off=%llu container_bytes=%llu prefix_sha256=%s "
                "file_bytes_before=%llu truncated_bytes=%llu\n",
                (unsigned long long)(64 + end), (unsigned long long)before, got.c_str(),
                (unsigned long long)before, (unsigned long long)(before - (64 + end)));
    }
    ContainerOut(const std::string& p, unsigned count_, bool streaming_ = false)
        : path(p), temp(p + ".partial"), count(count_), streaming(streaming_) {
        pf_check(finish_spool::enabled() || ::access(path.c_str(), F_OK) != 0, "output exists; refusing to clobber");
        // O_RDWR when streaming: CONTAINER_FINAL hashes the durable bytes back.
        fd = ::open(temp.c_str(), (streaming ? O_RDWR : O_WRONLY) | O_CREAT |
                    (finish_spool::enabled() ? O_TRUNC : O_EXCL), 0666);
        pf_check(fd >= 0, "create container partial");
        std::vector<char> z(headerBytes(), 0);
        pf_write_full(fd, z.data(), z.size(), "container header reserve");
        extent = headerBytes();
    }
    ~ContainerOut() { if (!done && fd >= 0) { ::close(fd); ::unlink(temp.c_str()); } }
    // Begin the streaming member: it owns [headerBytes(), ...) and every later
    // member lands after whatever it produced.
    uint64_t beginStream(unsigned id, unsigned codec, uint64_t cnt) {
        pf_check(streaming && ms.empty(), "beginStream needs the streaming layout and an empty member set");
        ms.push_back({id, codec, extent, 0, cnt, ~0u});
        streamIdx = 0;
        extent = ms.back().offset;
        fprintf(stderr, "CONTAINER_STREAM mode=incremental member=m%u id=%u codec=%u off=%llu count=%llu\n",
                id, id, codec, (unsigned long long)ms.back().offset, (unsigned long long)cnt);
        return ms.back().offset;
    }
    // Append to the streaming member at its own tail. Sequential by
    // construction: off = member.offset + member.bytes.
    void append(const void* p, size_t z) {
        pf_check(streamIdx >= 0, "container append without a streaming member");
        auto& m = ms.at(size_t(streamIdx));
        pf_pwrite_full_or_seek(fd, (const unsigned char*)p, z, m.offset + m.bytes);
        m.bytes += z; m.checksum = crc(m.checksum, (const unsigned char*)p, z);
        if (streaming && int64_t(ms.size()) > streamIdx) memberSha.update(p, z);
        extent = std::max(extent, m.offset + m.bytes);
        unsynced += z;
    }
    uint64_t streamBytes() const { return streamIdx < 0 ? 0 : ms.at(size_t(streamIdx)).bytes; }
    // Durability of the appended bytes. m1 is appended block by block; calling
    // fsync per block would be 1e7 syncs at Pile scale, so the caller syncs on a
    // byte threshold and every piece release is gated on it.
    void syncNow(const char* why) {
        pf_check(::fsync(fd) == 0, "fsync streaming container");
        lastSyncBytes = streamBytes();
        fprintf(stderr, "CONTAINER_SYNC why=%s member=m%u member_bytes=%llu unsynced_bytes=%llu\n",
                why, ms.at(size_t(streamIdx)).id, (unsigned long long)lastSyncBytes,
                (unsigned long long)unsynced);
        unsynced = 0;
    }
    bool wantsSync() const { return unsynced >= syncEvery; }
    // begin a member at the next 8-aligned offset after the header reserve
    void begin(unsigned id, unsigned codec, uint64_t cnt) {
        uint64_t here = headerBytes();
        for (auto& m : ms) here = std::max(here, m.offset + m.bytes);
        here = std::max(here, extent);
        here = (here + 7) / 8 * 8;
        ms.push_back({id, codec, here, 0, cnt, ~0u});
        extent = here;
        fprintf(stderr, "PILE_CONTAINER_BEGIN id=%u codec=%u count=%llu offset=%llu\n",
                id, codec, (unsigned long long)cnt, (unsigned long long)here);
        if (streaming)
            fprintf(stderr, "CONTAINER_STREAM mode=incremental member=m%u id=%u codec=%u off=%llu count=%llu\n",
                    id, id, codec, (unsigned long long)here, (unsigned long long)cnt);
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
        uint64_t membersEnd = 0;
        for (auto& m : ms) membersEnd = std::max(membersEnd, m.offset + m.bytes);
        // v7: the member directory is a TRAILING section, so `length` is the
        // file size and the directory starts at length-40*count - no prefix
        // slot is ever reserved and no member offset is ever guessed.
        uint64_t dirOff = 0, length = membersEnd;
        if (streaming) {
            dirOff = (membersEnd + 7) / 8 * 8;
            length = dirOff + 40 * uint64_t(ms.size());
        }
        std::vector<unsigned char> h(64 + 40 * ms.size());
        memcpy(h.data(), "SXI2", 4);
        put(h.data() + 4, writeVersion ? writeVersion : (streaming ? PF_CONTAINER_V7 : 6), 4); put(h.data() + 8, n); put(h.data() + 16, k);
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
        if (streaming) {
            // membersEnd .. dirOff is zero padding so the trailing directory
            // starts 8-aligned; its bytes are the SAME 40-byte entries the CRC
            // above was computed over, so a reader validates them in place.
            if (dirOff > membersEnd) {
                std::vector<char> pad(size_t(dirOff - membersEnd), 0);
                pf_pwrite_full_or_seek(fd, (const unsigned char*)pad.data(), pad.size(), membersEnd);
            }
            pf_pwrite_full_or_seek(fd, h.data() + 64, 40 * ms.size(), dirOff);
            // ONLY the fixed 64-byte header goes to offset 0: the directory is a
            // trailing section, so the bytes after the header belong to member 1.
            pf_pwrite_full_or_seek(fd, h.data(), 64, 0);
        } else {
            pf_pwrite_full_or_seek(fd, h.data(), h.size(), 0);
        }
        pf_check(fsync(fd) == 0, "fsync container");
        if (streaming) {
            // CONTAINER_FINAL: the published object's size and digest, measured
            // on the bytes that are now durable. Sequential read-back; this is
            // the only pass over the finished object.
            durable_piece::Sha256 sha;
            std::array<uint8_t, 1 << 20> buf{};
            for (uint64_t pos = 0; pos < length;) {
                size_t z = size_t(std::min<uint64_t>(buf.size(), length - pos));
                pf_pread_full(fd, buf.data(), z, pos, "container hash read-back");
                sha.update(buf.data(), z); pos += z;
            }
            fprintf(stderr, "CONTAINER_FINAL bytes=%llu sha256=%s members=%zu\n",
                    (unsigned long long)length, sha.finish().c_str(), ms.size());
        }
        pf_check(::close(fd) == 0, "close container");
        fd = -1;
        int linked=::link(temp.c_str(),path.c_str());
        if(linked!=0 && finish_spool::enabled() && errno==EEXIST){
            int old=::open(path.c_str(),O_RDONLY), fresh=::open(temp.c_str(),O_RDONLY);
            pf_check(old>=0&&fresh>=0,"open existing container for verification");
            struct stat st{};pf_check(::fstat(old,&st)==0&&uint64_t(st.st_size)==length,"existing container length mismatch");
            std::array<uint8_t,1<<20> x,y;
            for(uint64_t pos=0;pos<length;){size_t z=std::min<uint64_t>(x.size(),length-pos);
                pf_pread_full(old,x.data(),z,pos,"existing container");pf_pread_full(fresh,y.data(),z,pos,"rebuilt container");
                pf_check(memcmp(x.data(),y.data(),z)==0,"existing container differs from rebuilt spools");pos+=z;}
            ::close(old);::close(fresh);
        }else pf_check(linked==0,"publish container (exists?)");
        finish_spool::syncdir(std::filesystem::path(path).parent_path().empty()?".":std::filesystem::path(path).parent_path().string());
        pf_check(::unlink(temp.c_str()) == 0, "unlink container partial");
        done = true;
        fprintf(stderr, "SXI2_PASS bytes=%llu lite=1 members=%zu\n", (unsigned long long)length, ms.size());
    }
};

// Batched little-endian counters: the m1 block directory and the two run
// directories are ~1e8 values, and one pwrite per value would be ~1e8 syscalls.
struct NumBuf {
    std::vector<uint64_t> b;
    NumBuf() { b.reserve(4096); }
    void put(ContainerOut& c, uint64_t v) { b.push_back(v); if (b.size() == 4096) flush(c); }
    void flush(ContainerOut& c) {
        if (!b.empty()) { c.write(b.data(), b.size() * 8); b.clear(); }
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
         a.pending.len == b.pending.len && a.pending.h == b.pending.h && a.pending.t == b.pending.t &&
         a.pending.ht == b.pending.ht && a.pending.tt == b.pending.tt)) &&
        a.sentinelRows == b.sentinelRows && a.rNorm == b.rNorm &&
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
        // NO FRAME. The stream is n rows of the sentinel-terminated text: one
        // singleton sentinel row (symbol 0, head == tail == 0) plus ordinary
        // text rows over the present-byte symbols. Adjacent equal symbols are
        // included so both coalescers are exercised, and every split width is
        // tried, including both sides of the sentinel row.
        std::vector<RawRun> raw;
        uint64_t n = 0;
        for (unsigned i = 0; i < 24; ++i) {
            uint8_t alphabet[] = {5, 9, 10};
            uint64_t len = 1 + ((seed * 17 + i * 13) % 4);
            raw.push_back({alphabet[((i / (1 + seed % 3)) + seed) % 3], len,
                           1 + i, len == 1 ? 1 + i : 25 + i});
            if (i == seed % 24) { raw.push_back({SENTINEL_SYMBOL, 1, 0, 0}); n += 1; }
            n += len;
        }
        SeamClasses seam;
        if (seed & 2) {
            seam.classes.push_back({2,5,1,17,19,{},{{5,1,7,7},{0,2,8,9}}});
            seam.classes.push_back({8,12,1,21,23,{},{{9,4,11,13}}});
            seam.classes[0].runTrue = {{2,2},{3,4}};
            seam.classes[1].runTrue = {{8,11}};
        }
        std::vector<NRun> expected;
        RepairStream initial; initial.N=n; initial.n=n; initial.seam=&seam;
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
                         expected[i].h==actual[i].h && expected[i].t==actual[i].t &&
                         expected[i].rk==actual[i].rk && expected[i].ht==actual[i].ht &&
                         expected[i].tt==actual[i].tt,
                         "repair oracle serialized run bytes and rank stamps");
            ++cases;
        }
    }
    printf("CHUNKED_REPAIR_ORACLE_PASS streams=128 widths=%llu workers=4 both_coalescers=1 class_flanks=1 sentinel_row=1 no_frame=1\n", (unsigned long long)cases);
    return 0;
}

// CPU-only attribution: the legacy repair timer also charges producer waits,
// class rebuilds and piece I/O, so it cannot by itself measure this scheduler.
static int benchmark_repair_chunks() {
    constexpr uint64_t count = 1u << 20;
    std::vector<RawRun> raw{{SENTINEL_SYMBOL,1,0,0}};
    for (uint64_t i=1; i<count; ++i) raw.push_back({uint8_t((i/2)%2 ? 5 : 9),1,1,1});
    RepairStream initial; initial.N=count; initial.n=count;
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
    bool finite = false; // Seam discovery compares rotations; product LCP compares suffixes.
    void flush_thread() const { fh->flushThread(); seedQueries.flushThread(); }
    uint64_t operator()(uint64_t i, uint64_t j) const {
        uint64_t size = n, answer = 0;
        if (i >= size || j >= size) pf_fail("cyclic LCE bounds");
        if (finite) {
            const uint64_t cap = std::min(n - i, n - j);
            if (i == j) { seedQueries.add(1); return cap; }
            return std::min(fh->lce(i, j, cap), cap);
        }
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
// The prebuilt LCP column, read as a table. THE CONVENTION (measured against
// scatter_sort's --lcp-out, 300/300 at the 2 MB fixture): col[r] is the LEFT-
// neighbour LCP, LCP[r] = lce(SA[r-1], SA[r]) - the standard LCP array. The
// finish reads it in RANK ORDER along the ordered fold - each band stages its
// own CONTIGUOUS rank slice with one pread - so the sweep's two query families
// cost a column read and a range minimum instead of an LCE walk:
//
//     topLcp[i]   = col[r_i]              (lce(prev row, this row))
//     interior[i] = min(col[r_i + 1 .. r_i + len_i - 1])
//                                   (lce(SA[a], SA[c]) = min LCP[a+1..c], any a<c)
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
// run lengths do not. The fold VERIFIES its rank cursor at every anchored head and
// tail (ISA[p] from the producer, one probe per anchored endpoint) and refuses on
// any disagreement: the cursor is the definition of rank over the emitted stream,
// so an anchor mismatch means the stream itself left the true suffix order.
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
    std::atomic<uint64_t> fallback{0};   // invariant zero: no fallback implementation exists
};

struct LcpColumn {
    int fd = -1;
    uint64_t n = 0, escapeCount = 0, maxLcp = 0, flags = 0, fileBytes = 0;
    std::vector<std::pair<uint64_t, uint64_t>> esc;   // (rank, exact value), sorted
    uint64_t preads = 0, stagedBytes = 0, maxEnd = 0;
    // ===================== BAND-STREAM MODE (the product route) =====================
    // The column is NOT one n-byte file. The producer (scatter_sort --tape-bands)
    // writes ONE SLICE PER BAND beside that band's SA stream, in rank order:
    //
    //     band_<i>.lcp      48 B header {rows, escapes, max_lcp, rank_base, flags}
    //                       + rows bytes (u8 column, 255 = escaped)
    //                       + escapes*16 (rank, exact value), ranks ascending
    //     band_<i>.lcp.done the slice is complete (the producer publishes it BEFORE
    //                       band_<i>.done, so a band's SA never exists without its
    //                       LCP slice)
    //
    // The fold reads ranks in ASCENDING order, so this reader is a STREAM: it
    // opens slices as the rank cursor reaches them, copies their payloads, and
    // immediately returns disk credit (unlink + .lcp.consumed). Residency is
    //      staged span for one fold band + overlapping private slice copies
    // and never n (never 1.34n): the producer's own flow control is satisfied by
    // the .lcp.consumed markers this reader publishes.
    //
    // The private copy containing the cursor's LAST rank stays until the next load,
    // because a fold band's span starts at the previous band's end rank (base =
    // lo-1) and re-reads exactly that one byte.
    struct SliceMeta {
        uint64_t rows = 0, rankBase = 0, escapeCount = 0, maxLcp = 0;
        // ITS OWN BAND INDEX. Deriving the name as nextSlice-live.size() silently
        // renames the front every time an EMPTY band goes by (an empty slice is
        // unlinked and never enters `live`), which tried to unlink a slice that had
        // already been removed - measured at 4 MB: FATAL unlink retired LCP slice
        // band_59.lcp while the cursor was at 60.
        uint64_t index = 0;
        int fd = -1;
        std::vector<std::pair<uint64_t, uint64_t>> esc;
        std::vector<uint8_t> payload;
        uint64_t end() const { return rankBase + rows; }
    };
    std::string bandDir;                 // non-empty => band-stream mode
    std::deque<SliceMeta> live;          // private copies still needed by the rank cursor
    uint64_t nextSlice = 0;              // the next band index to open
    uint64_t covered = 0;                // end rank of the last opened slice
    uint64_t slicesOpened = 0, slicesRetired = 0, emptySlices = 0;
    uint64_t maxSliceBytes = 0, maxLiveBytes = 0, escapesRead = 0;
    uint64_t sliceWaitWallMs = 0;
    static constexpr uint32_t SLICE_MAGIC = 0x534c5853u;   // 'SXLS'
    static constexpr uint32_t SLICE_VERSION = 1;
    static constexpr uint32_t SLICE_HDR = 48;
    // A slice is published (renamed into place, then .lcp.done) before the band's
    // SA stream, so in an ordered pipeline this wait never triggers; it exists so
    // a broken producer fails LOUD here instead of publishing wrong coordinates.
    void waitSlice(const std::string& path, uint64_t timeoutMs) {
        const double t0 = tnow();
        while (::access(path.c_str(), F_OK) != 0) {
            if (!finish_spool::enabled() && timeoutMs != 0 && (tnow() - t0) * 1000.0 > double(timeoutMs))
                pf_fail("timeout waiting for the LCP band slice " + path +
                        " (the producer never published it)");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        sliceWaitWallMs += uint64_t((tnow() - t0) * 1000.0);
    }
    void openBands(const std::string& dir, uint64_t nExpect, uint64_t pollTimeout) {
        bandDir = dir;
        n = nExpect;
        pollMs = pollTimeout * 1000ull; // zero explicitly disables polling deadlines
        struct stat st {};
        pf_check(::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode),
                 "--lcp-bands is not a directory: " + dir);
        if (const char* t = ::getenv("XSA_FINISH_LCP_BAND_TIMEOUT_MS"))
            pollMs = std::strtoull(t, nullptr, 10);
        fprintf(stderr, "XSA_FINISH_LCP_BANDS dir=%s n=%llu mode=stream\n",
                dir.c_str(), (unsigned long long)n);
    }
    uint64_t pollMs = 3600ull * 1000ull;
    void loadSliceEscapes(SliceMeta& s) {
        if (!s.escapeCount) return;
        std::vector<uint8_t> buf(size_t(s.escapeCount * 16));
        pread_at(s.fd, buf.data(), buf.size(), uint64_t(SLICE_HDR) + s.rows, "lcp slice escapes");
        s.esc.resize(size_t(s.escapeCount));
        for (size_t i = 0; i < s.esc.size(); ++i) {
            std::memcpy(&s.esc[i].first, buf.data() + i * 16, 8);
            std::memcpy(&s.esc[i].second, buf.data() + i * 16 + 8, 8);
            pf_check(s.esc[i].first >= s.rankBase && s.esc[i].first < s.rankBase + s.rows,
                     "lcp slice escape rank outside its own slice");
            pf_check(s.esc[i].second >= 255, "lcp slice escape below the u8 cap");
            if (i) pf_check(s.esc[i].first > s.esc[i - 1].first,
                            "lcp slice escapes are not strictly ordered");
        }
        escapesRead += s.escapeCount;
        maxLcp = std::max(maxLcp, s.maxLcp);
        if (s.maxLcp) {
            uint64_t mx = 0;
            for (const auto& e : s.esc) mx = std::max(mx, e.second);
            pf_check(!s.esc.empty() || s.maxLcp < 255,
                     "lcp slice declares max_lcp >= 255 with no escape entry");
            (void)mx;
        }
    }
    void retireFront() {
        // Disk credit was returned at copy time. Keep only the private copy
        // needed for the one-rank overlap with the following fold.
        live.pop_front();
    }
    // Open the next slice in rank order. Empty bands carry rows == 0: they hold
    // no bytes, so they are consumed immediately (the producer is waiting on that
    // marker) and the budget does not advance.
    void openNextSlice() {
        const std::string base = bandDir + "/band_" + std::to_string(nextSlice);
        const std::string path = base + ".lcp";
        waitSlice(path + ".done", pollMs);
        SliceMeta s;
        s.index = nextSlice;
        s.fd = ::open(path.c_str(), O_RDONLY);
        pf_check(s.fd >= 0, "open LCP slice " + path + ": " + std::strerror(errno));
        uint8_t h[SLICE_HDR];
        pread_at(s.fd, h, SLICE_HDR, 0, "lcp slice header");
        uint32_t magic = 0, ver = 0;
        std::memcpy(&magic, h + 0, 4);
        std::memcpy(&ver, h + 4, 4);
        std::memcpy(&s.rows, h + 8, 8);
        std::memcpy(&s.escapeCount, h + 16, 8);
        std::memcpy(&s.maxLcp, h + 24, 8);
        std::memcpy(&s.rankBase, h + 32, 8);
        uint64_t flags = 0;
        std::memcpy(&flags, h + 40, 8);
        pf_check(magic == SLICE_MAGIC, "LCP slice magic: " + path);
        pf_check(ver == SLICE_VERSION, "LCP slice version: " + path);
        pf_check(flags == 1, "LCP slice flags: only u8+escape is defined: " + path);
        pf_check(s.rankBase == covered, "LCP slices do not tile the rank space: " + path +
                 " rank_base=" + std::to_string(s.rankBase) + " expected=" + std::to_string(covered));
        pf_check(s.rankBase + s.rows <= n, "LCP slice runs past n: " + path);
        struct stat st {};
        pf_check(::fstat(s.fd, &st) == 0, "stat LCP slice " + path);
        pf_check(uint64_t(st.st_size) == uint64_t(SLICE_HDR) + s.rows + s.escapeCount * 16,
                 "LCP slice size != 48 + rows + escapes*16: " + path);
        ++nextSlice;
        ++slicesOpened;
        if (!s.rows) {
            pf_check(s.escapeCount == 0, "empty LCP slice with escapes: " + path);
            ::close(s.fd); s.fd = -1;
            if (!finish_spool::enabled()) pf_check(::unlink(path.c_str()) == 0, "unlink empty LCP slice " + path);
            int cf = ::open((base + ".lcp.consumed").c_str(), O_WRONLY | O_CREAT | (finish_spool::enabled() ? O_TRUNC : O_EXCL), 0666);
            pf_check(cf >= 0, "publish consumed empty LCP slice " + base);
            ::close(cf);
            ++slicesRetired;
            ++emptySlices;
            return;
        }
        s.payload.resize(size_t(s.rows));
        pread_at(s.fd, s.payload.data(), s.rows, SLICE_HDR, "lcp slice payload");
        loadSliceEscapes(s);
        ::close(s.fd); s.fd = -1;
        if (!finish_spool::enabled()) pf_check(::unlink(path.c_str()) == 0, "unlink copied LCP slice " + path);
        int cf = ::open((base + ".lcp.consumed").c_str(), O_WRONLY | O_CREAT | (finish_spool::enabled() ? O_TRUNC : O_EXCL), 0666);
        pf_check(cf >= 0, "publish copied LCP slice " + base); ::close(cf);
        ++slicesRetired;
        covered += s.rows;
        maxSliceBytes = std::max(maxSliceBytes, s.rows + s.escapeCount * 16);
        live.push_back(std::move(s));
    }
    bool bandMode() const { return !bandDir.empty(); }
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
        b.u8.assign(size_t(len ? len : 1), 0);
        b.deep.clear();
        if (len && bandMode()) { loadBands(b, base, len); return; }
        if (len) pread_at(fd, b.u8.data(), len, uint64_t(48) + base, "lcp column slice");
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
    // THE STREAM: stage [base, base+len) from the slices that cover it, then
    // retire every slice whose last rank the range has passed.
    void loadBands(Band& b, uint64_t base, uint64_t len) {
        const uint64_t hiRank = base + len - 1;
        while (covered <= hiRank) openNextSlice();
        for (auto& s : live) {
            if (s.end() <= base || s.rankBase > hiRank) continue;
            const uint64_t lo = std::max(base, s.rankBase), hi2 = std::min(hiRank, s.end() - 1);
            std::memcpy(b.u8.data() + (lo - base), s.payload.data() + (lo - s.rankBase), hi2 - lo + 1);
            for (const auto& e : s.esc)
                if (e.first >= base && e.first <= hiRank) b.deep.push_back(e);
            ++preads;
        }
        uint64_t liveBytes = len;
        for (const auto& s : live) liveBytes += s.payload.size() + s.esc.size() * 16;
        maxLiveBytes = std::max(maxLiveBytes, liveBytes);
        // RETIRE what this range has passed. The slice holding hiRank stays: the
        // next fold band's range starts at exactly hiRank (base = lo-1).
        while (!live.empty() && live.front().end() <= hiRank) retireFront();
        ++stagedLoads;
        stagedBytes += len;
        if (base + len > maxEnd) maxEnd = base + len;
    }
    uint64_t stagedLoads = 0;
    // The stream is the whole column: prove it before publication.
    void finishBands() {
        pf_check(!bandMode() || covered == n,
                 "LCP band stream does not cover n: covered=" + std::to_string(covered) +
                 " n=" + std::to_string(n));
        fprintf(stderr,
                "XSA_FINISH_LCP_BANDS_DONE dir=%s slices=%llu retired=%llu empty=%llu loads=%llu "
                "preads=%llu staged_bytes=%llu escapes=%llu max_lcp=%llu "
                "max_slice_bytes=%llu max_live_lcp_slice_bytes=%llu wait_ms=%llu live=%zu "
                "bytes_per_rank=%.6f\n",
                bandDir.c_str(), (unsigned long long)slicesOpened,
                (unsigned long long)slicesRetired, (unsigned long long)emptySlices,
                (unsigned long long)stagedLoads, (unsigned long long)preads,
                (unsigned long long)stagedBytes, (unsigned long long)escapesRead,
                (unsigned long long)maxLcp, (unsigned long long)maxSliceBytes,
                (unsigned long long)maxLiveBytes, (unsigned long long)sliceWaitWallMs,
                live.size(), n ? double(stagedBytes) / double(n) : 0.0);
        // THE CONSUMER SIDE OF THE SAME CLAIM. The producer journals LCP_SLICE
        // (bytes of slice published and not yet retired, i.e. the DISC footprint);
        // this is what the fold ever held at once (one band's staged span plus the
        // private overlapping slice copies) - bytes and as a fraction of n.
        // Disk slices are deleted immediately after copying.
        fprintf(stderr,
                "LCP_SLICE_CONSUMER max_live_bytes=%llu slices=%llu retired=%llu "
                "peak_over_n=%.6f staged_bytes=%llu bytes_per_rank=%.6f escapes=%llu "
                "max_slice_bytes=%llu empty_slices=%llu wait_ms=%llu\n",
                (unsigned long long)maxLiveBytes, (unsigned long long)slicesOpened,
                (unsigned long long)slicesRetired,
                n ? double(maxLiveBytes) / double(n) : 0.0,
                (unsigned long long)stagedBytes, n ? double(stagedBytes) / double(n) : 0.0,
                (unsigned long long)escapesRead, (unsigned long long)maxSliceBytes,
                (unsigned long long)emptySlices, (unsigned long long)sliceWaitWallMs);
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
        if (argc == 2 && std::string(argv[1]) == "--selftest-packed-bits") return selftest_packed_bits();
        if (argc == 2 && std::string(argv[1]) == "--selftest-lcp-runs") return selftest_lcp_runs();
        if (argc == 2 && std::string(argv[1]) == "--selftest-witness-batch") return selftest_witness_batch();
        if (argc == 2 && std::string(argv[1]) == "--selftest-stream-guard") return selftest_stream_guard();
        if (argc == 2 && std::string(argv[1]) == "--selftest-repair-chunks") return selftest_repair_chunks();
        if (argc == 2 && std::string(argv[1]) == "--selftest-sweep-monoid") return selftest_sweep_monoid();
        if (argc == 3 && std::string(argv[1]) == "--selftest-seam-spool") return selftest_seam_spool(argv[2]);
        uint64_t chiExpect = 0, sampleCount = 1024, pollTimeout = 86400, ramBudget = 1ull << 30;
        int threads = int(std::thread::hardware_concurrency());
        std::string prefix, corpus, remap, out, bridgeDir, sampleCheck, wmPath, sentinelRemap;
        std::string checkClaimsDir;   // PROB-CHI: standalone checker mode
        bool debugArtifacts = false;
        bool buildChi = true; // chi enabled by default on all routes
        std::string lcpColumnPath;    // KASAI: the prebuilt LCP column (--lcp-column, fixtures)
        std::string lcpBandsDir;      // KASAI: the PRODUCER's per-band LCP slices (--lcp-bands)
        std::string locateIsaPath;    // KASAI: the locate table (--sa-locate)
        std::string runRanksPath;     // KASAI: prefix-sum run ranks (--run-ranks)
        std::string isaSamplePath;    // KASAI: sampled inverse anchors (--isa-sample)
        uint64_t repairChunkRuns = 0; // experimental exact-checkpoint planner
        bool keepRlebwt = false, keepPfck = false;
        // ROUTE B default: m1 is encoded DURING the sweep and the merge's
        // rlebwt wire is never written. `--m1-replay` restores the two-pass
        // (rlebwt + replay) arm for A/B decode only; it is the frozen legacy
        // path and must stay byte-identical to the banked container SHAs.
        bool m1Replay = pf_env_str("XSA_FINISH_M1_REPLAY") == "1";
        bool containerStreamFlag = false;   // --container-stream (SXI2 v7, tail directories)
        bool containerLegacy = false;       // --container-legacy (frozen v6 layout)
        // THE LCP-OVER-RUNS MEMBER (m14, codec 117). On by default on the streaming
        // product arm; --no-lcp-runs (or XSA_LCP_RUNS=0) omits it and writes version 7
        // byte-for-byte, which is how the member's ADDITIVITY is proved: every other
        // member of the two containers must be identical.
        bool lcpRunsWanted = pf_env_str("XSA_LCP_RUNS") != "0";
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
            else if (a == "--debug-artifacts") debugArtifacts = true;
            else if (a == "--lcp-column") lcpColumnPath = need("--lcp-column");
            else if (a == "--lcp-bands") lcpBandsDir = need("--lcp-bands");
            else if (a == "--run-ranks") runRanksPath = need("--run-ranks");
            else if (a == "--isa-sample") isaSamplePath = need("--isa-sample");
            else if (a == "--sa-locate") locateIsaPath = need("--sa-locate");
            else if (a == "--output") out = need("--output");
            else if (a == "--bridge-dir") bridgeDir = need("--bridge-dir");
            else if (a == "--witness-sample-check") sampleCheck = need("--witness-sample-check");
            else if (a == "--wm") wmPath = need("--wm");
            else if (a == "--repair-chunk-runs") repairChunkRuns = strtoull(need("--repair-chunk-runs").c_str(), nullptr, 10);
            else if (a == "--with-chi") buildChi = true;
            else if (a == "--no-chi") buildChi = false;
            else if (a == "--chi-expect") chiExpect = strtoull(need("--chi-expect").c_str(), nullptr, 10);
            else if (a == "--sample-count") sampleCount = strtoull(need("--sample-count").c_str(), nullptr, 10);
            else if (a == "--poll-timeout") pollTimeout = strtoull(need("--poll-timeout").c_str(), nullptr, 10);
            else if (a == "--threads") threads = atoi(need("--threads").c_str());
            else if (a == "--wit-ram-budget") ramBudget = strtoull(need("--wit-ram-budget").c_str(), nullptr, 10);
            else if (a == "--keep-rlebwt") keepRlebwt = true;
            else if (a == "--keep-pfck") keepPfck = true;
            else if (a == "--m1-replay") m1Replay = true;
            else if (a == "--no-lcp-runs") lcpRunsWanted = false;
            else if (a == "--lcp-runs") lcpRunsWanted = true;
            else if (a == "--container-stream") containerStreamFlag = true;
            else if (a == "--container-legacy") containerLegacy = true;
            else if (a == "--sentinel-remap") sentinelRemap = need("--sentinel-remap");
            else pf_fail("unknown argument " + a);
        }
        // THE REMAP TABLE GENERATOR: one corpus -> the order-preserving sentinel
        // map every stage takes as --remap. Nothing is written to the corpus.
        if (!sentinelRemap.empty()) {
            pf_check(!corpus.empty() && prefix.empty() && remap.empty() && out.empty() &&
                     checkClaimsDir.empty(), "--sentinel-remap runs alone: --corpus C --sentinel-remap OUT");
            build_end_remap(corpus, sentinelRemap);
            return 0;
        }
        pf_check(!checkClaimsDir.empty() ||
                 (!prefix.empty() && !corpus.empty() && !remap.empty() && !out.empty()),
                 "usage: xsa_finish --prefix MERGED/frag --corpus CORPUS --remap MAP.bin --output CONTAINER.sxi2 "
                 "[--debug-artifacts] [--bridge-dir DIR] [--witness-sample-check BANKED.sA] [--chi-expect N] [--threads N] "
                 "[--keep-rlebwt] [--keep-pfck] [--poll-timeout S] [--repair-chunk-runs N] [--m1-replay] "
                 "[--lcp-bands BANDS_DIR | --lcp-column COLUMN.sxlc --sa-locate ISA.sxis]");
        pf_check(!repairChunkRuns || repairChunkRuns >= 1024, "experimental repair chunks require at least 1024 runs");
        fprintf(stderr, "XSA_FINISH_M1_ARM %s\n",
                m1Replay ? "legacy-replay (rlebwt wire, exact-run codebook)"
                         : "route-b one-pass (corpus-histogram codebook, no rlebwt)");
        pf_check(m1Replay || !keepRlebwt,
                 "--keep-rlebwt asks for the rlebwt wire, which route B never writes; pass --m1-replay");
        FinishRssSampler rssSampler;
        finish_rss_at("start");
        double last = tnow();
        pf_prof_start();
        if (const char* acc = getenv("XSA_FINISH_ACCESS"))
            g_pfAccessOn.store(*acc && *acc != '0', std::memory_order_relaxed);
        uint8_t remapTable[256];
        load_remap_table(remap, remapTable);
        // The table's order-preservation claim is proved against THIS corpus
        // here, before any stage reads a byte through it.
        verify_end_remap(corpus, remapTable);
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
        const bool spoolMode = finish_spool::enabled();
        // ---- CONTAINER STREAMING (SXI2 version 7) -------------------------------
        // Build the container AS THE SWEEP PROGRESSES: member bytes are appended
        // in stream order into <output>.partial, the per-block and per-member
        // directories are TRAILING sections, and the 64-byte header is patched
        // once at close. v6 stays the default until the owner accepts the product
        // change; this arm is opt-in and its reader is `xsa query`.
        // DEFAULT ON (owner-authorised product change): the incremental layout is
        // the default from SXI2 v7; --container-legacy (or XSA_CONTAINER_STREAM=0)
        // selects the v6 layout for A/B comparison and for the frozen old SHAs.
        const bool containerLegacyFlag = containerLegacy || pf_env_str("XSA_CONTAINER_STREAM") == "0";
        const bool containerStream = !m1Replay && !containerLegacyFlag;
        if (containerStream)
            fprintf(stderr, "CONTAINER_STREAM mode=declare version=%llu member_order=stream "
                    "trailing_directory=1 m1_codec=%u header_bytes=64\n",
                    (unsigned long long)PF_CONTAINER_V7, PF_M1_CODEC_STREAM);
        pf_check(!spoolMode || (!m1Replay && !repairChunkRuns && !debugArtifacts && sampleCheck.empty() && isaSamplePath.empty() &&
                              pf_env_u64("XSA_FINISH_LCE_TRUST",0,0,3)==0),
                 "spool checkpoints currently require the standard route-B product path");
        const std::string spoolDir = prefix + ".finish-spool";
        // A streaming build is RESUMING when its native finish checkpoint exists:
        // the retained container prefix is then verified, never rebuilt.
        const bool containerResume = containerStream && spoolMode &&
            ::access((spoolDir + "/state").c_str(), F_OK) == 0;
        std::unique_ptr<finish_spool::Lock> spoolLock;
        std::string spoolIdentity;
        if(spoolMode)spoolLock=std::make_unique<finish_spool::Lock>(spoolDir);
        PieceSource src;
        src.prefix = prefix;
        src.pollTimeout = pollTimeout;
        CorpusText text(corpus, remapTable);
        const uint64_t tau = pfck_spacing();
        std::string pfckPath = prefix + ".pfck2";
        // Start before either publication poll: only corpus + remap are needed.
        std::thread readiness([&] {
            if (!finish_spool::enabled() || ::access(pfckPath.c_str(), F_OK) != 0)
                build_pfck(text, text.size(), tau, threads, ramBudget, pfckPath);
        });
        // ROUTE B: the m1 codebook weights, computed while the producer
        // publishes its wire. The corpus is the only input (a permutation
        // invariant of the BWT), the pass is read-only through the adapter's own
        // byte path, and it is JOINED before the first run is encoded below, so
        // it can never change what the sweep emits - only how the first bit is
        // written. Skipped entirely by the legacy replay arm, whose codebook
        // comes from the complete run histogram after the sweep.
        std::unique_ptr<std::array<uint64_t, 256>> m1Weights;
        ThreadGuard m1WeightsThread;
        if (!m1Replay) {
            m1Weights = std::make_unique<std::array<uint64_t, 256>>();
            m1WeightsThread.t = std::thread([&] { *m1Weights = corpus_byte_histogram(text, 8); });
        }
        if (std::getenv("XSA_FINISH_RSS_READINESS_ONLY")) {
            readiness.join(); m1WeightsThread.join();
            finish_rss_at("readiness-pfck-histogram-probe");
            if (!keepPfck) pf_check(::unlink(pfckPath.c_str()) == 0, "unlink probe pfck");
            return 0;
        }
        src.load_idx();
        // N == n: no transport frame. The producer emits exactly the n rows of
        // the sentinel-terminated text, the first of which is the sentinel suffix.
        const uint64_t n = src.n, N = n;
        uint64_t R = src.R;
        pf_check(text.size() == n, "corpus size != the merge's text length (adapter: T[i]=REMAP[corpus[i]])");

        // ---- KASAI: the prebuilt LCP column (kasai-finish), proved HERE, before
        // a byte of the sweep runs: n identity, the file's own arithmetic, and a
        // strictly ordered escape list. A supplied column always enables its
        // rank and closure guards; no environment switch can turn them off.
        const bool kasaiOff = pf_env_u64("XSA_FINISH_KASAI", 1, 0, 1) == 0;
        pf_check(!kasaiOff || (lcpColumnPath.empty() && lcpBandsDir.empty()),
                 "a supplied LCP column cannot be disabled by XSA_FINISH_KASAI");
        pf_check(lcpColumnPath.empty() || lcpBandsDir.empty(),
                 "--lcp-column (one file) and --lcp-bands (per-band stream) are exclusive");
        const int kasaiAudit = int(pf_env_u64("XSA_FINISH_KASAI_AUDIT", 0, 0, 2));
        LcpColumn lcpCol;
        LocateIsa locateIsa;
        RunRankExc runRankExc;
        SampIsa sampIsa;
        TrueIsa trueIsa;
        const bool isaDebug = !pf_env_str("XSA_FINISH_ISA_DEBUG").empty();
        pf_check(debugArtifacts || (!isaDebug && pf_env_str("XSA_FINISH_DUMP_STREAM").empty() &&
                                    pf_env_str("XSA_FINISH_DUMP_ANSWERS").empty()),
                 "offline ISA/dumps require explicit --debug-artifacts (outside product space contract)");
        pf_check(debugArtifacts || bridgeDir.empty(),
                 "bridge dumps require explicit --debug-artifacts");
        pf_check(debugArtifacts || runRanksPath.empty(),
                 "offline rank exceptions require explicit --debug-artifacts");
        pf_check(debugArtifacts || (locateIsaPath.empty() && !kasaiAudit),
                 "full ISA audit requires explicit --debug-artifacts");
        fprintf(stderr, "XSA_FINISH_DEBUG_ARTIFACTS enabled=%d product_dump_scratch_bytes=0\n",
                int(debugArtifacts));
        if (isaDebug) trueIsa.load(pf_env_str("XSA_FINISH_ISA_DEBUG"), n);
        // Dev counters vs ground truth. Atomics: the psi-walk phase 1 is
        // per-run parallel, so several workers may touch them.
        std::atomic<uint64_t> devMax{0}, devRuns{0}, devSum{0};
        int64_t devPrevOff = 0;   // signed cursor offset at the previous run (cursor path, dev-only)
        std::atomic<uint64_t> devTransDump{0};
        std::atomic<uint64_t> rkDevMax{0}, rkDevRuns{0}, rkDevSum{0};
        std::atomic<uint64_t> devDump{0};
        uint64_t streamFnv = 1469598103934665603ull, streamH = 0, streamT = 0, streamLen = 0, streamRuns = 0;
        uint64_t gapDump = 0;
        const bool runRankExcEmpty = runRanksPath.empty();   // false => use the prefix sum
        uint64_t rowCursor = 0;      // prefix-sum row cursor carried across bands
        uint64_t anchorChecks = 0;   // --isa-sample verifications performed (head+tail)
        KasaiState kasaiState;
        // THE PRODUCT ARM IS THE BAND STREAM: the producer writes each band's LCP
        // slice beside its SA stream and this fold reads them lazily in rank order.
        const bool kasaiOn = (!lcpColumnPath.empty() || !lcpBandsDir.empty()) && !kasaiOff;
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
            if (!lcpBandsDir.empty()) lcpCol.openBands(lcpBandsDir, n, pollTimeout);
            else lcpCol.open(lcpColumnPath, n);
            if (!locateIsaPath.empty()) locateIsa.open(locateIsaPath, n);
            if (!runRanksPath.empty()) runRankExc.load(runRanksPath);
            if (!isaSamplePath.empty()) sampIsa.open(isaSamplePath, n);
        }
        else if (!lcpColumnPath.empty() || !lcpBandsDir.empty())
            fprintf(stderr, "XSA_FINISH_LCP_COLUMN_IGNORED path=%s reason=XSA_FINISH_KASAI=0\n",
                    (lcpColumnPath.empty() ? lcpBandsDir : lcpColumnPath).c_str());
        else
            pf_check(!kasaiAudit, "XSA_FINISH_KASAI_AUDIT=1 requires --lcp-column");
        fprintf(stderr, "XSA_FINISH_KASAI arm=%s audit=%d lcp_source=%s column=%s\n",
                kasaiOn ? "on" : "off", int(kasaiAudit),
                lcpBandsDir.empty() ? (lcpColumnPath.empty() ? "none" : "column-file")
                                    : "sorter-bands",
                lcpColumnPath.empty() ? (lcpBandsDir.empty() ? "none" : lcpBandsDir.c_str())
                                      : lcpColumnPath.c_str());

        // The v2 text has a unique virtual END, independent of content bytes.
        // Newlines and record separators are ordinary bytes: the old v1
        // content heuristic must not classify raw text as a collection.
        pf_check(text.fileSize() + 1 == n,
                 "END row count is not file bytes + the virtual END");
        fprintf(stderr, "XSA_FINISH_CYCLIC=1 terminal=virtual-END content_classification=none k=1\n");

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
                if (!finish_spool::enabled() && pollTimeout != 0 && waited > pollTimeout) pf_fail("timeout waiting for the merge's seam spool + .done");
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
                if (!finish_spool::enabled() && pollTimeout != 0 && waited > pollTimeout) pf_fail("timeout waiting for the merge's WM scratch");
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
        finish_rss_at("readiness-pfck");
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
        // THE CLASS TRUE-RANK SUPPLY's verification: the rebuild checks every
        // anchored class occurrence against --isa-sample and refuses on any
        // disagreement (product-scale proof; fixture scale adds TrueIsa on top).
        seam.anchorVerify = sampIsa.map ? [&](uint64_t pos, uint64_t modelRank) {
            if (!sampIsa.has(pos)) return;
            const uint64_t truth = sampIsa.at(pos);
            if (truth != modelRank) {
                fprintf(stderr,
                        "XSA_FINISH_SEAM_ANCHOR_MISMATCH pos=%llu model_rank=%llu anchor_rank=%llu\n",
                        (unsigned long long)pos, (unsigned long long)modelRank,
                        (unsigned long long)truth);
                pf_fail("SEAM ANCHOR: the class true-rank model disagrees with "
                        "the sampled ISA (refusing)");
            }
        } : std::function<void(uint64_t, uint64_t)>{};
        auto seamSymbol = [&](uint64_t pos) -> uint8_t {
            if (pos >= n) pf_fail("seam symbol outside text");
            return text[pos];
        };
        const double seamBuildT0 = tnow();
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
            seam.build(text, wm, n, R,
                       [&](uint64_t a, uint64_t b) { return seamLce(a, b); }, seamSymbol);
        }
        fprintf(stderr, "SEAM_PHASE build=%.3f classes=%zu rows=%llu\n",
                tnow() - seamBuildT0, seam.classes.size(), (unsigned long long)seam.total);
        if (wmFd >= 0) ::close(wmFd);
        wmFd = -1;
        // Last-consumer deletion: merge.active keeps the directory alive
        // during sparse replay. Both sides best-effort-remove the empty dir,
        // so either exit ordering removes it after the final unlink.
        if (!wmFile.empty() && !finish_spool::enabled()) {
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
        finish_rss_at("repair-state");
        if (pf_cache_journal_on()) pf_cache_journal(text, "seam-classes", 1024);

        if (spoolMode) {
            durable_piece::Sha256 identity;
            const char schema[] = "finish-state-v1-1024";
            identity.update(schema, sizeof schema); identity.update(remapTable, 256);
            for (const std::string& path : {corpus, std::string("/proc/self/exe")}) {
                int f = ::open(path.c_str(), O_RDONLY); pf_check(f >= 0, "open spool identity input");
                std::array<char, 1<<20> b;
                for (;;) { ssize_t z=::read(f,b.data(),b.size()); if(z<0&&errno==EINTR)continue;
                    pf_check(z>=0,"read spool identity");if(!z)break;identity.update(b.data(),z); }
                ::close(f);
            }
            spoolIdentity = identity.finish();
        }
        // ---- scratch writers
        WitnessBitmap bitmap(n, prefix, buildChi);
        finish_rss_at("chi-bitmap-allocated");
        std::string sparsePath = prefix + ".sparse2";
        pf_check(finish_spool::enabled() || ::access(sparsePath.c_str(), F_OK) != 0, "sparse scratch exists");
        int sparseFd = ::open(sparsePath.c_str(), O_RDWR | O_CREAT | (finish_spool::enabled() ? O_TRUNC : O_EXCL), 0666);
        pf_check(sparseFd >= 0, "create sparse scratch");
        // ---- THE LCP-OVER-RUNS PAYLOAD SPOOL (member m14) --------------------
        // The encoded blocks are produced by the ordered fold (one value per run)
        // and are placed in the container after the sweep, when m1 has closed: v7
        // appends, and only ONE member can be appended at a time, so m14's bytes
        // are staged through this spool exactly as m5's two Elias-Fano streams and
        // m11's tails already are. Nothing about the corpus is re-read to build it:
        // the values ARE the fold's `topLcp`, which the producer's band slices
        // already delivered. On a spool resume the retained prefix is verified by
        // digest and truncated to the checkpoint's frontier (see state()).
        const bool lcpRunsOn = lcpRunsWanted && containerStream;
        LcpRunsWriter lcpRuns;
        if (lcpRunsOn) {
            lcpRuns.open(prefix + ".lcpruns", containerResume);
            if (!containerResume) lcpRuns.truncateSpool();
            fprintf(stderr, "LCP_RUNS_ARMS member=m14 codec=%u block_runs=%llu payload_off=%llu "
                    "encoding=block-width-or-u8+escape exact_u64=1 random_access=block-directory\n",
                    PF_LCP_CODEC, (unsigned long long)PF_LCP_BLOCK_RUNS, (unsigned long long)PF_LCP_HEAD);
        } else {
            fprintf(stderr, "LCP_RUNS member=absent reason=%s\n",
                    !lcpRunsWanted ? "disabled-by-flag"
                                   : "container-layout=prefix-v6-or-m1-replay-frozen-arm");
        }
        // Dev-only offline-analysis dumps (see bandSink / finishAdvance).
        int streamDumpFd = -1, answersDumpFd = -1;
        const std::string streamDumpPath = pf_env_str("XSA_FINISH_DUMP_STREAM");
        const std::string answersDumpPath = pf_env_str("XSA_FINISH_DUMP_ANSWERS");
        if (!streamDumpPath.empty()) {
            streamDumpFd = ::open(streamDumpPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            pf_check(streamDumpFd >= 0, "create XSA_FINISH_DUMP_STREAM");
            fprintf(stderr, "XSA_FINISH_DUMP_STREAM path=%s\n", streamDumpPath.c_str());
        }
        if (!answersDumpPath.empty()) {
            answersDumpFd = ::open(answersDumpPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            pf_check(answersDumpFd >= 0, "create XSA_FINISH_DUMP_ANSWERS");
            fprintf(stderr, "XSA_FINISH_DUMP_ANSWERS path=%s\n", answersDumpPath.c_str());
        }
        const bool streamDump = streamDumpFd >= 0, answersDump = answersDumpFd >= 0;
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

        pf_check(buildChi || (!chiExpect && sampleCheck.empty() && !bridge), "missing member m5 (chi): --chi-expect, witness check and bridge require --with-chi");
        fprintf(stderr, "CHI_POLICY present=%d member=m5 construction=%s\n", int(buildChi), buildChi ? "enabled" : "disabled");
        SweepImpl<WitnessBatch> sweep(n, WitnessBatch{&bitmap, bridge ? bSAFd : -1});
        LceWrap slimLce{&fhSlim, {}, n, true};
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
        finish_rss_at("band-arrays-allocated");
        BandSlot* feedTarget = &slots[0];
        uint64_t prevTail = 0;
        // ROUTE B: the m1 head/length streams and the codebook they encode with,
        // allocated BEFORE the sweep because the sweep encodes. The weights were
        // tallied from the corpus while the producer published its wire; joining
        // here means the codebook is fixed before the first run exists, so the
        // sweep is still a single deterministic pass with no lookahead.
        std::unique_ptr<BitsOut> m1Head, m1Len;
        M1Code m1Code{};
        std::array<unsigned char, 256> m1LensOut{};
        uint64_t m1SweepRuns = 0;
        // ---- CONTAINER STREAMING state (empty unless --container-stream) --------
        std::unique_ptr<ContainerOut> streamOwner;
        ContainerOut* streamOut = nullptr;
        uint64_t m1StreamHeadBits = 0, m1StreamLenBits = 0, m1StreamPayload = 0;
        // Runs whose bytes sit in CLOSED (appended) codec blocks. A piece may only
        // be unlinked while every one of its runs is below this count.
        uint64_t m1StreamClosedRuns = 0;
        std::string m1ResumeSha;   // checkpoint digest of the durable container prefix
        std::string lcpResumeSha;  // checkpoint digest of the durable LCP-runs spool prefix
        // ---- m1 v2 access structure, accumulated in stream order during the
        // sweep (the runs are already going past; a second pass would be the
        // 25 s decode this change exists to delete).
        //   m1BlkHead/m1BlkLen  bitPos of each block boundary, nblocks+1 entries
        //   m1RunStart          row of run j*PF_RUNSTART_STRIDE
        //   m1DirRun/m1DirCum   per-symbol sampled (run index, count before it)
        struct M1DirState {
            std::vector<uint64_t> blkHead, blkLen;
            std::vector<uint64_t> runStart;
            std::vector<uint64_t> dirRun[256], dirCum[256];
            std::vector<uint16_t> dirStride[256];
            uint64_t row = 0;                 // rows emitted so far (= run-start of the next run)
            uint64_t occ[256] = {0};          // runs of c emitted so far
            uint64_t cum[256] = {0};          // rows of c emitted so far
            uint64_t lastSample[256] = {0};   // run index of the last sample of c
            uint64_t lastSampleOcc[256] = {0};// occurrence index of the last sample of c
            void begin() { blkHead.clear(); blkLen.clear(); blkHead.push_back(0); blkLen.push_back(0); }
        };
        M1DirState m1dir;
        if (!m1Replay) m1dir.begin();
        if (!m1Replay) {
            m1WeightsThread.join();
            m1Code = canonical_huffman_m1(*m1Weights);
            for (unsigned c = 0; c < 256; ++c) m1LensOut[c] = (unsigned char)m1Code.lens[c];
            uint64_t maxLen = 0, minLen = 64, symbols = 0;
            for (unsigned c = 0; c < 256; ++c) {
                maxLen = std::max<uint64_t>(maxLen, m1Code.lens[c]);
                minLen = std::min<uint64_t>(minLen, m1Code.lens[c]);
                symbols += (*m1Weights)[c] ? 1 : 0;
            }
            fprintf(stderr, "XSA_FINISH_M1_CODEBOOK arm=route-b source=corpus_byte_histogram "
                    "symbols=256 observed_symbols=%llu min_len=%llu max_len=%llu pseudo_count=1 "
                    "weights_sum=%llu\n",
                    (unsigned long long)symbols, (unsigned long long)minLen,
                    (unsigned long long)maxLen, (unsigned long long)n);
            if (containerStream && containerResume) {
                // RESUME AT A CONTAINER OFFSET. The retained .partial is reopened
                // for append; its durable prefix is verified by digest and any
                // bytes a killed run appended past the last checkpoint frontier
                // are truncated after the sweep state has been restored.
                streamOwner = std::make_unique<ContainerOut>(
                    out, (m1Replay ? 6u : 8u) - (buildChi ? 0u : 1u) + (lcpRunsOn ? 1u : 0u), true, true);
                streamOut = streamOwner.get();
                m1Head = std::make_unique<BitsOut>();
                m1Len = std::make_unique<BitsOut>();
                fprintf(stderr, "RESUME_MODE pending container=%s retained_bytes=%llu "
                        "verify=sha256_over_durable_prefix truncate=uncommitted_tail\n",
                        (out + ".partial").c_str(), (unsigned long long)streamOut->streamBytes());
            } else if (containerStream) {
                // THE CONTAINER EXISTS BEFORE THE FIRST RUN IS SWEPT. Everything
                // in m1's fixed head is known here: the C table is the corpus byte
                // histogram (asserted equal to the emitted symbol totals before
                // publication), the code lengths are the canonical codebook, and
                // payload_off is the constant 2336. nblocks and rNorm are derived
                // by the reader (count/block_runs and the member count), so the
                // head is written ONCE and never patched - the member CRC it is
                // part of stays valid.
                streamOwner = std::make_unique<ContainerOut>(
                    out, (m1Replay ? 6u : 8u) - (buildChi ? 0u : 1u) + (lcpRunsOn ? 1u : 0u), true);
                streamOut = streamOwner.get();
                const uint64_t off = streamOut->beginStream(1, PF_M1_CODEC_STREAM, 0);
                pf_check(off == 64, "streaming m1 does not start at the header end");
                std::vector<uint8_t> head(2336, 0);
                uint64_t acc = 0;
                for (unsigned c = 0; c < 256; ++c) {
                    put(head.data() + 8 * c, acc, 8);
                    acc += (*m1Weights)[c];
                }
                pf_check(acc == n, "streaming m1 C table does not close on n");
                memcpy(head.data() + 2048, m1LensOut.data(), 256);
                put(head.data() + 2304, 0, 8);                 // nblocks: derived
                put(head.data() + 2312, M1_BLOCK_RUNS, 8);
                put(head.data() + 2320, 0, 8);                 // rNorm: = member count
                put(head.data() + 2328, 2336, 8);              // payload offset (constant)
                streamOut->append(head.data(), head.size());
                fprintf(stderr, "XSA_FINISH_M1_STREAM_HEAD bytes=2336 payload_off=2336 "
                        "block_runs=%llu nblocks=derived rnorm=member_count\n",
                        (unsigned long long)M1_BLOCK_RUNS);
                // Per-block, memory-only encoders: a block is appended to the
                // container the moment it closes, so no n-proportional bit spool
                // (v6's 0.277 n term) is ever created.
                m1Head = std::make_unique<BitsOut>();
                m1Len = std::make_unique<BitsOut>();
            } else {
                m1Head = std::make_unique<BitsOut>(prefix + ".bits-head");
                m1Len = std::make_unique<BitsOut>(prefix + ".bits-len");
            }
        }
        RepairCycles repairCycles;
        repairCycles.enabled = pf_env_u64("XSA_FINISH_REPAIR_CYCLES", 1, 0, 1) && !repairChunkRuns;
        RepairStream rs;
        // Experimental checkpoint replay copies rs into workers. Attribution
        // belongs only to the serial transducer, never a shared worker counter.
        rs.cycles = repairChunkRuns ? nullptr : &repairCycles;
        rs.N = N; rs.n = n; rs.seam = &seam;
        // The band loop's sink. Passed DIRECTLY as the transducer's template sink
        // (hot path, inlined) and also installed as rs.emit for the final flush,
        // so both paths emit through identical code.
        uint64_t certifiedRows = 0;
        uint64_t spoolPiece=0, spoolBlockLo=0, spoolHeadOff=0, spoolLenOff=0;
        std::vector<uint64_t> spoolRuns;
        std::map<uint64_t,std::string> entropyPaths;
        auto entropyPayload = [&](std::vector<uint64_t>& runs) {
            pf_check(runs.size()%2==0 && runs.size()<=2*M1_BLOCK_RUNS,"spool codec record count");
            BitsOut h, l;
            for(size_t i=0;i<runs.size();i+=2){
                pf_check(runs[i]<256 && runs[i+1]>0,"spool codec input");
                h.msb(m1Code.codes[runs[i]],m1Code.lens[runs[i]]);l.gamma(runs[i+1]);
            }
            h.pad();l.pad();finish_spool::Archive a;
            a.scalar(h.bitPos);a.scalar(l.bitPos);
            uint64_t nh=h.mem.size(),nl=l.mem.size();a.scalar(nh);a.bytes(h.mem.data(),nh);a.scalar(nl);a.bytes(l.mem.data(),nl);
            return a.data;
        };
        auto publishCodecBlock = [&]() {
            if(!spoolMode || spoolRuns.empty())return;
            finish_spool::Meta meta{spoolPiece,(m1SweepRuns-1)/M1_BLOCK_RUNS,spoolBlockLo,m1dir.row,
                                    m1dir.row-spoolBlockLo,spoolRuns.size()/2};
            std::string name=spoolDir+"/piece-"+std::to_string(meta.piece)+"-block-"+std::to_string(meta.block)+
                "-keys-"+std::to_string(meta.lo)+"-"+std::to_string(meta.hi);
            finish_spool::Archive a; a.bytes(spoolIdentity.data(),64);a.vector(spoolRuns);
            finish_spool::publish(name+".runs",a.data,meta);
            std::vector<uint8_t> existing;
            m1Head->pad();m1Len->pad();
            uint64_t hb=m1Head->bitPos-m1dir.blkHead.back(),lb=m1Len->bitPos-m1dir.blkLen.back();
            uint64_t nh=(hb+7)/8,nl=(lb+7)/8;
            if(!finish_spool::load(name+".entropy",existing)){
                finish_spool::Archive encoded;encoded.scalar(hb);encoded.scalar(lb);encoded.scalar(nh);
                const uint8_t* head=m1Head->window(spoolHeadOff,nh);encoded.bytes(const_cast<uint8_t*>(head),nh);
                encoded.scalar(nl);const uint8_t* len=m1Len->window(spoolLenOff,nl);encoded.bytes(const_cast<uint8_t*>(len),nl);
                finish_spool::publish(name+".entropy",encoded.data,meta);
            }
            spoolHeadOff+=nh;spoolLenOff+=nl;
            entropyPaths[meta.block]=name+".entropy";
            spoolRuns.clear();spoolBlockLo=m1dir.row;
        };
        // Runs whose container bytes are durable: every run below this count sits
        // in a CLOSED codec block that has been appended to the container.
        auto appendedRuns = [&]() -> uint64_t { return m1StreamClosedRuns; };
        // PIECE ABSORPTION. A tape piece may be unlinked only after the container
        // bytes covering it are fsynced (RESUME_RETENTION). Pieces are batched
        // (64 pieces or 256 MiB) so one fsync covers many, and every release is
        // journalled with the offset it became durable at.
        auto releasePieces = [&](bool force) {
            if (!containerStream || src.pendingPieces.empty()) return;
            const uint64_t frontier = appendedRuns();
            uint64_t covered = 0;
            for (auto& p : src.pendingPieces)
                if (p.frontier != ~0ull && p.frontier <= frontier) ++covered;
            if (!covered) return;
            if (!force && covered < 64) return;
            if (streamOut->unsynced) streamOut->syncNow("piece-release");
            std::vector<DeferredPiece> keep;
            keep.reserve(src.pendingPieces.size());
            for (auto& p : src.pendingPieces) {
                if (p.frontier == ~0ull || p.frontier > frontier) { keep.push_back(p); continue; }
                struct stat st {};
                const uint64_t pieceBytes = ::stat(p.path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
                pf_check(::unlink(p.path.c_str()) == 0, "unlink absorbed piece " + p.path);
                fprintf(stderr, "PIECE_ABSORBED idx=%llu container_off=%llu bytes=%llu\n",
                        (unsigned long long)p.band, (unsigned long long)streamOut->streamBytes(),
                        (unsigned long long)pieceBytes);
            }
            src.pendingPieces.swap(keep);
        };
        // Arm a band's pieces: its last run has been emitted, so the frontier they
        // become releasable at is the current emitted-run count.
        auto armPieces = [&](uint64_t band) {
            if (!containerStream) return;
            for (auto& p : src.pendingPieces) if (p.band == band && p.frontier == ~0ull) p.frontier = m1SweepRuns;
        };
        // Close the open codec block INTO THE CONTAINER. The bytes appended here are
        // exactly the bytes v6's assembly pass would have copied out of its bit
        // spool: the head bytes, the length bytes, then zero padding to 8.
        auto closeStreamBlock = [&]() {
            const uint64_t hb = m1Head->bitPos, lb = m1Len->bitPos;
            const size_t nh = m1Head->mem.size(), nl = m1Len->mem.size();
            pf_check(nh == (hb + 7) / 8 && nl == (lb + 7) / 8, "streaming block byte count");
            streamOut->append(m1Head->mem.data(), nh);
            if (nl) streamOut->append(m1Len->mem.data(), nl);
            size_t pad = size_t((8 - ((nh + nl) % 8)) % 8);
            if (pad) { static const uint8_t zero[8] = {0}; streamOut->append(zero, pad); }
            m1StreamPayload += nh + nl + pad;
            m1StreamClosedRuns = m1SweepRuns;
            m1StreamHeadBits += hb; m1StreamLenBits += lb;
            m1dir.blkHead.push_back(m1StreamHeadBits);
            m1dir.blkLen.push_back(m1StreamLenBits);
            m1Head->mem.clear(); m1Head->bitPos = 0; m1Head->acc = 0; m1Head->nbits = 0;
            m1Len->mem.clear(); m1Len->bitPos = 0; m1Len->acc = 0; m1Len->nbits = 0;
            const uint64_t blocks = m1StreamHeadBits ? m1dir.blkHead.size() - 1 : 0;
            if ((blocks & 255) == 0 || streamOut->wantsSync())
                fprintf(stderr, "CONTAINER_STREAM mode=incremental member=m1 off=%llu blocks=%llu "
                        "payload_bytes=%llu head_bits=%llu len_bits=%llu\n",
                        (unsigned long long)streamOut->streamBytes(), (unsigned long long)blocks,
                        (unsigned long long)m1StreamPayload, (unsigned long long)m1StreamHeadBits,
                        (unsigned long long)m1StreamLenBits);
            if (streamOut->wantsSync()) releasePieces(false);
        };
        auto bandSink = [&](const NRun& v) {
            RepairCycles::Scope outputScope(rs.cycles, RepairCycles::OUTPUT);
            {
                RepairCycles::Scope guardScope(rs.cycles, RepairCycles::GUARD);
                require_stream_run(v, certifiedRows, n);
            }
            certifiedRows += v.len;
            // Stream fingerprint (dev): identical piece sets must produce an
            // identical (c,len,h,t) sequence, and a differing one must be visible.
            streamFnv = (streamFnv ^ (v.len * 0x9e3779b97f4a7c15ull + v.h + (v.t << 1) + v.c)) *
                        0x100000001b3ull;
            streamH += v.h; streamT += v.t; streamLen += v.len; ++streamRuns;
            // ROUTE B: ENCODE HERE, one pass, in stream order. The head symbol
            // uses the corpus-histogram canonical code (published in m1); the
            // length keeps the existing gamma code, which is stateless and needs
            // no codebook at all. Nothing is staged: no rlebwt, no replay.
            if (m1Head) {
                const uint64_t k = m1SweepRuns;
                if (k && k % M1_BLOCK_RUNS == 0) {
                    // Close the block: pad both streams so block k/M1_BLOCK_RUNS
                    // starts on a byte boundary. pad() is a no-op on an aligned
                    // stream, so the byte length of a stream is ALWAYS
                    // ceil(bitPos/8) at a block boundary.
                    m1Head->pad();
                    m1Len->pad();
                    if (containerStream) {
                        closeStreamBlock();
                    } else {
                        publishCodecBlock();
                        m1dir.blkHead.push_back(m1Head->bitPos);
                        m1dir.blkLen.push_back(m1Len->bitPos);
                    }
                }
                if(spoolMode && !containerStream){spoolRuns.push_back(v.c);spoolRuns.push_back(v.len);}
                m1Head->msb(m1Code.codes[v.c], unsigned(m1Code.lens[v.c]));
                m1Len->gamma(v.len);
                if (k % PF_RUNSTART_STRIDE == 0) m1dir.runStart.push_back(m1dir.row);
                const unsigned ch = unsigned(v.c);
                if (m1dir.occ[ch] % PF_RUNDIR_OCC == 0 || k - m1dir.lastSample[ch] >= PF_RUNDIR_GAP) {
                    // close the previous sample's occurrence stride
                    if (!m1dir.dirStride[ch].empty())
                        m1dir.dirStride[ch].back() = uint16_t(m1dir.occ[ch] - m1dir.lastSampleOcc[ch]);
                    m1dir.dirRun[ch].push_back(k);
                    m1dir.dirCum[ch].push_back(m1dir.cum[ch]);
                    m1dir.dirStride[ch].push_back(0);
                    m1dir.lastSample[ch] = k;
                    m1dir.lastSampleOcc[ch] = m1dir.occ[ch];
                }
                m1dir.row += v.len;
                m1dir.cum[ch] += v.len;
                ++m1dir.occ[ch];
                ++m1SweepRuns;
            }
            // XSA_FINISH_DUMP_STREAM (dev only): the emitted run stream, one
            // 40-byte record per run, for offline ground-truth analysis.
            if (streamDump) {
                pf_check(v.len <= UINT32_MAX, "debug stream length exceeds record format");
                struct { uint64_t idx, rk, h, t; uint32_t len; uint8_t c, pad[3]; } rec;
                rec.idx = rs.rNorm - 1; rec.rk = v.rk; rec.h = v.h; rec.t = v.t;
                rec.len = uint32_t(v.len); rec.c = v.c; memset(rec.pad, 0, 3);
                pf_write_full(streamDumpFd, &rec, sizeof rec, "stream dump");
            }
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
            // ---- THE KASAI ARM --------------------------------------------------
            // Derive ranks from the ordered stream lengths. Worker zero
            // constructs the rank arrays; all workers arrive before the last
            // arrival stages the column slice and publishes it with release.
            // Every worker acquires that publication before reading the slice.
            // Nonadjacency is an error, never a request for a walk fallback.
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
                const int wid = t;   // worker index; `t` is shadowed by the run tail below
                // PHASE 1: every run's exact rank WITHOUT an inverse.
                //
                // The rank is the prefix sum of emitted row lengths. Seam
                // stamps must agree with this independently derived cursor at
                // both endpoints; disagreement refuses before column reads.
                // Sparse ISA anchors are an additional check, not authority to
                // override the cursor or waive closure.
                //
                // SINGLE WORKER (the race fix): this block used to run in EVERY
                // pool worker over the same shared arrays. `rowCursor` was read
                // at worker-start and written at phase-1 end, so a worker that
                // woke late read an already-advanced cursor, mixed foreign values
                // into headRank/tailRank, and double-advanced the stream for the
                // next band. Measured at 150 MB with the race present:
                // deviating_runs 21.6M of 57.3M, rank_holes 35.6M, and DIFFERENT
                // closure counters on every run of the same pieces - all four
                // prior "stream is not rank-ordered" negative results were
                // measured through this race.
                if (wid == 0) {
                    uint64_t cursor = rowCursor;
                    for (size_t i = 0; i < cnt; ++i) {
                        // Stamps are assertions, never an alternate rank supply.
                        pf_check(nb[i].ht == ~0ull || nb[i].ht == cursor,
                                 "KASAI STAMP: head stamp disagrees with derived cursor");
                        pf_check(nb[i].tt == ~0ull || nb[i].tt == cursor + nb[i].len - 1,
                                 "KASAI STAMP: tail stamp disagrees with derived cursor");
                        // Every run lies inside [0, n): with the sentinel there is
                        // no frame above n and no rank outside the column.
                        pf_check(cursor < n && cursor + nb[i].len <= n,
                                 "KASAI STAMP: run outside the text rows");
                        const uint64_t h = cursor;
                        const uint64_t t = cursor + nb[i].len - 1;
                        if (sampIsa.map) {
                            if (nb[i].h < n && sampIsa.has(nb[i].h)) {
                                const uint64_t truth = sampIsa.at(nb[i].h);
                                if (truth != h) {
                                    fprintf(stderr, "XSA_FINISH_ANCHOR_MISMATCH kind=head i=%zu pos=%llu derived=%llu anchor=%llu len=%llu\n",
                                            i, (unsigned long long)nb[i].h, (unsigned long long)h,
                                            (unsigned long long)truth, (unsigned long long)nb[i].len);
                                    pf_fail("KASAI ANCHOR: derived head rank != the sampled ISA anchor");
                                }
                                ++anchorChecks;
                            }
                            if (nb[i].len >= 2 && nb[i].t < n && sampIsa.has(nb[i].t)) {
                                const uint64_t truth = sampIsa.at(nb[i].t);
                                if (truth != t) {
                                    fprintf(stderr, "XSA_FINISH_ANCHOR_MISMATCH kind=tail i=%zu pos=%llu derived=%llu anchor=%llu len=%llu\n",
                                            i, (unsigned long long)nb[i].t, (unsigned long long)t,
                                            (unsigned long long)truth, (unsigned long long)nb[i].len);
                                    pf_fail("KASAI ANCHOR: derived tail rank != the sampled ISA anchor");
                                }
                                ++anchorChecks;
                            }
                        }
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
                            if (dev) { devRuns.fetch_add(1); devSum.fetch_add(dev); }
                            uint64_t prev = devMax.load(std::memory_order_relaxed);
                            while (dev > prev &&
                                   !devMax.compare_exchange_weak(prev, dev, std::memory_order_relaxed)) {}
                            // The OFFSET PROFILE: the signed cursor offset and its
                            // transitions localise the stream's tiling breaks (a
                            // hole flips the offset by its size; an anti-hole flips
                            // it back). One line per transition, dev-only.
                            {
                                const int64_t off = int64_t(truth) - int64_t(h);
                                if (off != devPrevOff) {
                                    devPrevOff = off;
                                    if (devTransDump.fetch_add(1) < 96) {
                                        fprintf(stderr, "XSA_FINISH_DEVTRANS run_idx=%llu i=%zu off=%lld head=%llu len=%llu cursor=%llu truth=%llu\n",
                                                (unsigned long long)(streamRuns + i), i, (long long)off,
                                                (unsigned long long)nb[i].h, (unsigned long long)nb[i].len,
                                                (unsigned long long)h, (unsigned long long)truth);
                                    }
                                }
                            }
                            // Is the transducer's OWN row base (the row the sweep
                            // state machine stamps on the run) already exact?
                            const uint64_t rkRank = nb[i].rk >= 10 ? nb[i].rk - 10 : 0;
                            const uint64_t rkDev = truth > rkRank ? truth - rkRank : rkRank - truth;
                            if (rkDev) { rkDevRuns.fetch_add(1); rkDevSum.fetch_add(rkDev); }
                            prev = rkDevMax.load(std::memory_order_relaxed);
                            while (rkDev > prev &&
                                   !rkDevMax.compare_exchange_weak(prev, rkDev, std::memory_order_relaxed)) {}
                            if (dev && devDump.fetch_add(1) < 24) {
                                fprintf(stderr, "XSA_FINISH_DEVDUMP i=%zu head=%llu len=%llu cursor=%llu rk=%llu truth=%llu dev=%lld rkdev=%lld\n",
                                        i, (unsigned long long)nb[i].h, (unsigned long long)nb[i].len,
                                        (unsigned long long)h, (unsigned long long)rkRank,
                                        (unsigned long long)truth, (long long)dev, (long long)rkDev);
                            }
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
                        // head and tail both lie in the contiguous stream span
                        // [0, n) - there is no frame above n any more.
                        pf_check((*headRank)[i] < n && (*tailRank)[i] < n,
                                 "band rank outside the text rows");
                        lo = std::min(lo, (*headRank)[i]);
                        lo = std::min(lo, (*tailRank)[i]);
                        hi = std::max(hi, (*tailRank)[i]);
                        hi = std::max(hi, (*headRank)[i]);
                        if (nb[i].len >= 2 && (*tailRank)[i] != (*headRank)[i] + nb[i].len - 1)
                            kasaiState.noncontig += 1;
                        // Seed-first stream ranks must be contiguous and increasing.
                        if (i && (*headRank)[i] <= (*headRank)[i - 1])
                            kasaiState.disorder += 1;
                        const uint64_t prevEnd = i ? (*tailRank)[i - 1] + 1 : prevEndS;
                        if (prevEnd != (*headRank)[i]) {
                            kasaiState.gaps += 1;
                            if (wid == 0 && gapDump < 8) {
                                ++gapDump;
                                fprintf(stderr, "XSA_FINISH_GAPDUMP i=%zu prevEnd=%llu head=%llu len=%llu rk=%llu head_pos=%llu tail_pos=%llu\n",
                                        i, (unsigned long long)prevEnd, (unsigned long long)(*headRank)[i],
                                        (unsigned long long)nb[i].len, (unsigned long long)nb[i].rk,
                                        (unsigned long long)nb[i].h, (unsigned long long)nb[i].t);
                            }
                        }
                    }
                    if (!firstBand && (*headRank)[0] <= prevHeadS) kasaiState.disorder += 1;
                    require_zero_closure(kasaiState.gaps, kasaiState.disorder,
                                         kasaiState.noncontig, kasaiState.fallback.load());
                    const uint64_t base = lo ? lo - 1 : 0;
                    if (kasaiOn) lcpCol.load(*band, base, hi - base + 1);
                    kasaiState.lastHeadRank = (*headRank)[0];
                    kasaiState.lastEndRank = (*tailRank)[cnt - 1] + 1;
                    next->store(0);
                    staged->store(1, std::memory_order_release);
                }
                while (!staged->load(std::memory_order_acquire)) std::this_thread::yield();
                if (!buildChi) {
                    // Rank tiling and anchor checks above remain mandatory. The
                    // chi-only `interior` answer is not computed when chi is
                    // absent, but `topLcp` IS: it is now a PRODUCT value (member
                    // m14, the LCP over runs), not only an input to the chi sweep.
                    for (size_t i = size_t(wid); i < cnt; i += size_t(P)) {
                        pf_check(nb[i].h < n && nb[i].t < n, "position resolution failed");
                        pf_check(nb[i].len != 1 || nb[i].h == nb[i].t,
                                 "head-SA value/range or singleton mismatch");
                    }
                    if (!kasaiOn) {
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
                        }
                        slimLce.flush_thread();
                        return;
                    }
                    for (;;) {
                        const size_t i = next->fetch_add(1);
                        if (i >= cnt) break;
                        const uint64_t r = (*headRank)[i];
                        uint64_t top = 0;
                        if (gx[i]) {
                            pf_check(i ? (*tailRank)[i - 1] + 1 == r : prevEndS == r,
                                     "KASAI CLOSURE: nonadjacent top query (refusing publication)");
                            top = band->at(r);
                        }
                        s.topLcp[i] = top;
                    }
                    return;
                }
                if (!kasaiOn) {
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
                    return;
                }
                // PHASE 2: column answers for EVERY run, including sparse m11
                // rows. gx is the global run index, not a sparse-row flag.
                // Closure above certifies the previous row is the previous
                // run's tail; only the explicit kasaiAudit arm may walk text.
                for (;;) {
                    const size_t i = next->fetch_add(1);
                    if (i >= cnt) break;
                    const NRun& v = nb[i];
                    const uint64_t r = (*headRank)[i];
                    const uint64_t prevEnd = i ? (*tailRank)[i - 1] + 1 : prevEndS;
                    uint64_t top = 0;
                    if (gx[i]) {
                        pf_check(prevEnd == r,
                                 "KASAI CLOSURE: nonadjacent top query (refusing publication)");
                        top = band->at(r);
                    }
                    uint64_t inter = INF;
                    if (v.len >= 2 && r < n && (*tailRank)[i] < n) {
                        // min-LCP over the rank interval between the run's two
                        // ENDPOINT RANKS: LCE(SA[a],SA[c]) = min col[a+1..c].
                        const uint64_t rt = (*tailRank)[i];
                        pf_check(rt == r + v.len - 1,
                                 "KASAI CLOSURE: noncontiguous interior query (refusing publication)");
                        inter = ~uint64_t(0);
                        for (uint64_t t2 = r + 1; t2 <= rt; ++t2) inter = std::min(inter, band->at(t2));
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
                for (size_t i = 0; i < s.cnt; ++i) {
                    // XSA_FINISH_DUMP_ANSWERS (dev only): the verified per-run
                    // answers, for offline A/B between the arms.
                    if (answersDump) {
                        const uint64_t answers[2]{s.topLcp[i], s.interior[i]};
                        pf_write_full(answersDumpFd, answers, sizeof answers, "answers dump top+inter");
                    }
                    if (buildChi) sweep.advance(s.nbuf[i], s.topLcp[i], s.interior[i]);
                    // THE LCP OVER RUNS (member m14): the value this fold already
                    // holds for run `advRuns + i`, appended in RUN ORDER. This is
                    // the whole construction - no corpus pass, no second structure:
                    // the producer's band slice (or the A/B arm's LCE walk) has
                    // already produced it, and until now it was discarded here.
                    if (lcpRunsOn) lcpRuns.push(s.topLcp[i]);
                }
                advanceWall += tnow() - ta;
                r.close(); advAcc.add(r);
            }
            advRuns += s.cnt;
            if (lcpRunsOn) pf_check(lcpRuns.count == advRuns, "LCP runs count != folded runs");
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
                    batch.anchorVerify = seam.anchorVerify;
                    const double seamRebuildT0 = tnow();
                    batch.rebuild(n,src.R,1u<<26,[&](uint64_t a,uint64_t b){return seamLce(a,b);},seamSymbol,
                                  batch.anchorVerify, unsigned(std::max<int>(1, threads)));
                    fprintf(stderr, "SEAM_PHASE rebuild=%.3f\n", tnow() - seamRebuildT0);
                    seam.total += batch.total; seam.maxSeamLce = std::max(seam.maxSeamLce,batch.maxSeamLce);
                    for (auto& cl : batch.classes) seam.classes.push_back(std::move(cl));
                }
                if (src.terminal) { R=src.R; nbands=src.finalBand+1; }
            }
            {
                PfRegion r("readband", PF_READBAND);
                const uint64_t readStart = __rdtsc();
                src.read_band(band, s.raws);
                if (band == 0) finish_rss_at("run-stream-first-band-decoded");
                repairCycles.readTicks += __rdtsc() - readStart;
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
                // NO FRAME TO INSPECT: row zero is the sentinel suffix (position
                // n-1, predecessor symbol text[n-2]), an ordinary product row.
                pf_check(!s.raws.empty() && s.raws[0].len >= 1 && s.raws[0].head < n,
                         "run zero is not the sentinel row");
                fprintf(stderr, "PFP_TERMINAL row=0 sentinel_symbol=%u predecessor=0x%02llx raw_n=%llu raw_r=%llu\n",
                        unsigned(SENTINEL_SYMBOL), (unsigned long long)s.raws[0].c,
                        (unsigned long long)N, (unsigned long long)R);
            }
            s.prevTail = prevTail;
            pf_check(s.nbuf.empty() && s.gidx.empty() && s.rk.empty(), "band slot not empty at feed");
            feedTarget = &s;
            {
                PfRegion r("repair-feed", PF_REPAIR);
                RawRun source = rs.srcPending;
                bool sourceHave = rs.srcHave;
                for (const auto& raw : s.raws) {
                    expectedRlebwtBytes += 4 * ((raw.len + 0x7fffff - 1) / 0x7fffff);
                    if (!repairChunkRuns) rs.feed_raw(raw, bandSink, source, sourceHave);
                }
                if (!repairChunkRuns) { rs.srcPending = source; rs.srcHave = sourceHave; }
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
            return true;
        };
        // Pipeline depth 1: the ordered stages alternate with the verification
        // of the previous band. `pipelined` is off only for the experimental
        // exact-checkpoint planner, which reuses the same pool internally.
        const bool pipelined = !repairChunkRuns && !spoolMode;
        uint64_t band = 0;
        uint64_t restoredAdvances=0, restoredRawRuns=0, restoredBands=0, spoolGeneration=0;
        bool spoolEof=false;
        auto state = [&](finish_spool::Archive& a) {
            std::string identity=spoolIdentity;a.bytes(identity.data(),64);
            pf_check(identity==spoolIdentity,"finish checkpoint identity mismatch");
            uint64_t policy=(buildChi?1:0)|(kasaiOn?2:0),savedPolicy=policy,savedTau=tau;
            a.scalar(savedPolicy);a.scalar(savedTau);
            pf_check(savedPolicy==policy&&savedTau==tau,"finish checkpoint policy mismatch");
            a.scalar(spoolGeneration);a.scalar(band);a.scalar(spoolEof);
            a.scalar(R);a.scalar(nbands);a.scalar(prevTail);a.scalar(certifiedRows);
            a.scalar(rawRunsFed);a.scalar(bandReads);a.scalar(advRuns);a.scalar(expectedRlebwtBytes);
            a.scalar(streamFnv);a.scalar(streamH);a.scalar(streamT);a.scalar(streamLen);a.scalar(streamRuns);
            a.scalar(lcpCol.maxEnd);a.scalar(lcpCol.nextSlice);a.scalar(lcpCol.covered);
            a.scalar(lcpCol.slicesOpened);a.scalar(lcpCol.slicesRetired);a.scalar(lcpCol.emptySlices);
            a.scalar(lcpCol.escapesRead);a.scalar(lcpCol.maxLcp);
            uint64_t liveCount=lcpCol.live.size();a.scalar(liveCount);
            if(a.reading){pf_check(liveCount<=(a.data.size()-a.at)/8,"LCP live count");lcpCol.live.clear();lcpCol.live.resize(liveCount);}
            for(auto& c:lcpCol.live){
                a.scalar(c.rankBase);a.scalar(c.rows);a.scalar(c.escapeCount);a.scalar(c.maxLcp);a.scalar(c.index);
                uint64_t len=c.payload.size();a.scalar(len);
                if(a.reading){pf_check(len<=a.data.size()-a.at,"LCP live payload size");c.payload.resize(len);}
                a.bytes(c.payload.data(),len);
                len=c.esc.size();a.scalar(len);
                if(a.reading){pf_check(len<=(a.data.size()-a.at)/16,"LCP live escapes size");c.esc.resize(len);}
                for(auto& e:c.esc){a.scalar(e.first);a.scalar(e.second);}
            }
            a.scalar(rowCursor);a.scalar(anchorChecks);a.scalar(kasaiState.bands);
            a.scalar(kasaiState.lastHeadRank);a.scalar(kasaiState.lastEndRank);
            a.scalar(kasaiState.gaps);a.scalar(kasaiState.disorder);a.scalar(kasaiState.noncontig);
            // Failed closure checkpoints are never published.
            pf_check(kasaiState.fallback.load()==0,"checkpoint walk fallback");
            a.scalar(src.terminal);a.scalar(src.progress);a.scalar(src.consumed);
            a.scalar(src.finalBand);a.scalar(src.capturedRows);a.scalar(src.delivered);a.scalar(src.R);
            a.map(src.bandRefs);a.map(src.bandCounts);a.map(src.classExtents);
            uint64_t count=src.completed.size();a.scalar(count);
            if(a.reading){pf_check(count<=(a.data.size()-a.at)/8,"completed state size");src.completed.resize(count);}
            for(auto& c:src.completed)a.vector(c);
            a.scalar(seam.depth);a.scalar(seam.total);a.scalar(seam.maxSeamLce);
            count=seam.classes.size();a.scalar(count);
            if(a.reading){pf_check(count<=(a.data.size()-a.at)/8,"seam state size");seam.classes.resize(count);}
            auto raw = [&](RawRun& v){a.scalar(v.c);a.scalar(v.len);a.scalar(v.head);a.scalar(v.tail);};
            auto normal = [&](NRun& v){a.scalar(v.c);a.scalar(v.len);a.scalar(v.h);a.scalar(v.t);a.scalar(v.rk);a.scalar(v.ht);a.scalar(v.tt);};
            for(auto& c:seam.classes){
                a.scalar(c.lo);a.scalar(c.hi);a.scalar(c.first);a.scalar(c.before);a.scalar(c.after);a.vector(c.occ);
                count=c.runs.size();a.scalar(count);if(a.reading){pf_check(count<=(a.data.size()-a.at)/32,"seam runs size");c.runs.resize(count);}
                for(auto& r:c.runs)raw(r);
                count=c.runTrue.size();a.scalar(count);if(a.reading){pf_check(count<=(a.data.size()-a.at)/16,"seam ranks size");c.runTrue.resize(count);}
                for(auto& r:c.runTrue){a.scalar(r.first);a.scalar(r.second);}
            }
            a.scalar(rs.rows);a.scalar(rs.srcHave);raw(rs.srcPending);a.scalar(rs.sentinelRows);
            a.scalar(rs.row);a.scalar(rs.next);a.scalar(rs.have);normal(rs.pending);
            a.scalar(rs.rNorm);a.scalar(rs.nNorm);a.array(rs.totals);a.array(rs.runFreq);
            a.scalar(sweep.chi);a.array(sweep.rr_len);a.array(sweep.rr_pos);a.array(sweep.rr_act);
            a.scalar(sweep.previous_interior);a.scalar(sweep.m);a.scalar(sweep.p);a.scalar(sweep.p_sa_last);a.scalar(sweep.first);
            a.scalar(sweep.statAdvance);a.scalar(sweep.statCharChange);a.scalar(sweep.statScanWords);a.scalar(sweep.statFired);
            a.scalar(sweep.statActSeen);a.scalar(sweep.statEmit);a.scalar(sweep.statMaxAct);
            if(a.reading)sweep.heapInit();
            a.scalar(bitmap.totalValues);a.scalar(bitmap.duplicates);
            a.scalar(m1SweepRuns);a.scalar(spoolPiece);a.scalar(spoolBlockLo);a.scalar(spoolHeadOff);a.scalar(spoolLenOff);a.vector(spoolRuns);
            a.vector(m1dir.blkHead);a.vector(m1dir.blkLen);a.vector(m1dir.runStart);
            a.scalar(m1dir.row);a.array(m1dir.occ);a.array(m1dir.cum);a.array(m1dir.lastSample);a.array(m1dir.lastSampleOcc);
            for(unsigned c=0;c<256;++c){a.vector(m1dir.dirRun[c]);a.vector(m1dir.dirCum[c]);a.vector(m1dir.dirStride[c]);}
            if(containerStream){
                // The m1 bytes are IN THE CONTAINER PREFIX, verified by digest on
                // reopen (see RESUME_MODE container_off=). Only the frontier is
                // state, and it is derived from the block arithmetic, not trusted.
                a.scalar(m1StreamHeadBits);a.scalar(m1StreamLenBits);a.scalar(m1StreamPayload);
                a.scalar(m1StreamClosedRuns);
                // The DIGEST of the durable member prefix [0, 2336+payload).
                uint8_t hex[64] = {0};
                if(!a.reading){const std::string h=streamOut->m1ShaHex();std::memcpy(hex,h.data(),64);}
                a.bytes(hex,64);
                if(a.reading)m1ResumeSha.assign((const char*)hex,64);
                // The OPEN codec block's bits. A checkpoint may fall inside a
                // 1024-run block and that block's encoded bits exist nowhere else:
                // they are bounded by one block, so they ride the checkpoint
                // instead of a spool.
                for(BitsOut* b:{m1Head.get(),m1Len.get()}){
                    uint64_t bitPos=b->bitPos,nmem=b->mem.size(),nbits=b->nbits;
                    uint64_t accl=uint64_t(b->acc),acch=uint64_t(b->acc>>64);
                    a.scalar(bitPos);a.scalar(nmem);a.scalar(nbits);a.scalar(accl);a.scalar(acch);
                    if(a.reading)b->mem.resize(nmem);
                    a.bytes(b->mem.data(),nmem);
                    if(a.reading){b->bitPos=bitPos;b->nbits=unsigned(nbits);b->acc=(__uint128_t(acch)<<64)|accl;}
                }
            } else for(BitsOut* b:{m1Head.get(),m1Len.get()}){
                if(!a.reading)b->drain();
                a.scalar(b->bitPos);a.scalar(b->nbits);
                uint64_t acc=uint64_t(b->acc);a.scalar(acc);if(a.reading)b->acc=acc;
                a.scalar(b->byteCount);finish_spool::file(a,b->fd,b->byteCount);
                if(a.reading){b->mem.clear();b->spill.clear();b->spillHave=0;b->spillBase=0;}
            }
            uint64_t sparseBytes=((rs.rNorm+1023)/1024)*8;
            finish_spool::file(a,sparseFd,sparseBytes);
            if(lcpRunsOn){
                // THE LCP OVER RUNS. The spool is append-only in CLOSED blocks, so
                // its durable prefix is exactly the checkpoint frontier: the length
                // and the digest of that prefix ride the checkpoint, the block
                // directory (16 B per 1024 runs) rides it too, and the OPEN block's
                // values (<= 1024 u64) exist nowhere else, exactly like m1's open
                // codec block.
                a.scalar(lcpRuns.payloadBytes);a.scalar(lcpRuns.count);a.scalar(lcpRuns.blocks);
                a.scalar(lcpRuns.escapes);a.scalar(lcpRuns.maxValue);
                a.scalar(lcpRuns.u8Blocks);a.scalar(lcpRuns.packBlocks);a.scalar(lcpRuns.zeroBlocks);
                a.scalar(lcpRuns.packedBits);
                a.vector(lcpRuns.blk);
                uint64_t dlen=lcpRuns.dir.size();a.scalar(dlen);
                if(a.reading){pf_check(dlen<=(a.data.size()-a.at),"lcp runs directory size");lcpRuns.dir.resize(dlen);}
                a.bytes(lcpRuns.dir.data(),dlen);
                uint8_t lhex[64]={0};
                if(!a.reading){const std::string h=lcpRuns.shaHex();std::memcpy(lhex,h.data(),64);}
                a.bytes(lhex,64);
                if(a.reading)lcpResumeSha.assign((const char*)lhex,64);
            }
            a.end();
        };
        auto checkpoint = [&](bool eof) {
            if(!spoolMode)return;
            pf_check(!lcePending,"checkpoint with verification in flight");
            sweep.emitWitness.flush();
            pf_check(bitmap.totalValues==sweep.chi && bitmap.duplicates==0,"checkpoint chi population");
            // The LCP-runs spool is a REFERENCED durable input of the checkpoint: its
            // closed blocks must be on the device before the state that names their
            // frontier and digest is published, exactly as the chi base and the
            // entropy spools already must be. The OPEN block rides the state.
            if(lcpRunsOn){lcpRuns.flushOut();lcpRuns.syncSpool();
                pf_check(lcpRuns.count==advRuns,"LCP runs checkpoint frontier != folded runs");}
            uint64_t oldGeneration=spoolGeneration; ++spoolGeneration;spoolEof=eof;
            finish_spool::Archive d;d.vector(bitmap.delta);
            finish_spool::publish(spoolDir+"/delta-"+std::to_string(spoolGeneration),d.data);
            // Compact into a NEW immutable base. The mmap is only a volatile overlay.
            std::vector<uint8_t> base;
            if(bitmap.bytes)base.assign(bitmap.bits,bitmap.bits+bitmap.bytes);
            finish_spool::publish(spoolDir+"/base-"+std::to_string(spoolGeneration),base);
            finish_spool::Archive a;state(a);
            finish_spool::publish(spoolDir+"/state",a.data);
            finish_spool::journal("FINISH_CHECKPOINT next_band="+std::to_string(band)+
                " runs="+std::to_string(advRuns)+" generation="+std::to_string(spoolGeneration)+
                " eof="+std::to_string(eof)+" state="+spoolDir+"/state");
            // Deletion is commit: base AND referring state are durable first.
            finish_spool::before_unlink();
            pf_check(::unlink((spoolDir+"/delta-"+std::to_string(spoolGeneration)).c_str())==0,"unlink compacted chi delta");
            if(oldGeneration)pf_check(::unlink((spoolDir+"/base-"+std::to_string(oldGeneration)).c_str())==0,"unlink old chi base");
            finish_spool::syncdir(spoolDir);bitmap.delta.clear();
            fprintf(stderr,"FINISH_CHECKPOINT band=%llu runs=%llu eof=%d generation=%llu\n",
                (unsigned long long)band,(unsigned long long)advRuns,int(eof),(unsigned long long)spoolGeneration);
        };
        if(spoolMode){
            uint64_t present=0,missing=0;
            for(const auto& entry:std::filesystem::directory_iterator(spoolDir)){
                if(entry.path().extension()!=".runs")continue;
                std::vector<uint8_t> payload;finish_spool::Meta meta;
                finish_spool::load(entry.path().string(),payload,&meta);
                finish_spool::Archive a(std::move(payload));std::string id(64,' ');a.bytes(id.data(),64);
                pf_check(id==spoolIdentity,"foreign entropy input spool");
                std::vector<uint64_t> runs;a.vector(runs);a.end();
                std::string path=std::filesystem::path(entry.path()).replace_extension(".entropy").string();
                std::vector<uint8_t> encoded;
                pf_check(meta.runs==runs.size()/2 && !runs.empty() && runs.size()%2==0 && meta.hi>meta.lo,
                         "codec source spool dimensions");
                uint64_t rows=0;for(size_t i=1;i<runs.size();i+=2){pf_check(runs[i]<=n-rows,"codec source row sum");rows+=runs[i];}
                pf_check(rows==meta.rows && rows==meta.hi-meta.lo,"codec source row extent");
                pf_check(entropyPaths.emplace(meta.block,path).second,"duplicate codec block spool");
                if(finish_spool::load(path,encoded)){++present;}
                else {++missing;finish_spool::publish(path,entropyPayload(runs),meta);
                    fprintf(stderr,"SPOOL_RECOMPUTE block=%llu piece=%llu runs=%llu\n",
                        (unsigned long long)meta.block,(unsigned long long)meta.piece,(unsigned long long)meta.runs);}
            }
            std::vector<uint8_t> saved;
            if(finish_spool::load(spoolDir+"/state",saved)){
                finish_spool::Archive a(std::move(saved));state(a);
                std::vector<uint8_t> base;pf_check(finish_spool::load(spoolDir+"/base-"+std::to_string(spoolGeneration),base),"missing committed chi base");
                pf_check(base.size()==bitmap.bytes,"chi base dimensions");
                if(bitmap.bytes)memcpy(bitmap.bits,base.data(),bitmap.bytes);
                if(containerStream){
                    // VERIFY THE RETAINED PREFIX, THEN TRUNCATE THE UNCOMMITTED TAIL.
                    pf_check(containerResume, "a streaming checkpoint exists but no retained container partial does");
                    pf_check(m1ResumeSha.size()==64, "checkpoint carries no container prefix digest");
                    streamOut->resumeFrontier(m1StreamPayload,m1ResumeSha);
                    m1StreamClosedRuns=(m1dir.blkHead.size()?m1dir.blkHead.size()-1:0)*M1_BLOCK_RUNS;
                    fprintf(stderr,"RESUME_MODE container_off=%llu payload_bytes=%llu closed_blocks=%llu "
                            "closed_runs=%llu pending_block_runs=%llu work_avoided_basis=finish_advance_counters\n",
                            (unsigned long long)streamOut->streamBytes(),(unsigned long long)m1StreamPayload,
                            (unsigned long long)(m1dir.blkHead.size()?m1dir.blkHead.size()-1:0),
                            (unsigned long long)m1StreamClosedRuns,
                            (unsigned long long)(m1SweepRuns-m1StreamClosedRuns));
                }
                bitmap.delta.clear();restoredAdvances=advRuns;restoredRawRuns=rawRunsFed;restoredBands=bandReads;
                if(lcpRunsOn){
                    pf_check(lcpResumeSha.size()==64,"checkpoint carries no LCP runs digest");
                    lcpRuns.resumeFrontier(lcpRuns.payloadBytes,lcpResumeSha,lcpRuns.blocks);
                }
                fprintf(stderr,"SPOOL_STATE blocks_present=%llu blocks_missing=%llu resumed_from=%llu\n",
                    (unsigned long long)present,(unsigned long long)missing,(unsigned long long)(m1SweepRuns/M1_BLOCK_RUNS));
            }else{
                if(lcpRunsOn) lcpRuns.truncateSpool();
                fprintf(stderr,"SPOOL_STATE blocks_present=%llu blocks_missing=%llu resumed_from=none\n",
                (unsigned long long)present,(unsigned long long)missing);
            }
        }
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
        spoolPiece=band;
        // Deferred (fsync-gated) piece deletion is the PRODUCT rule. Under the
        // resume protocol the raw pieces are a RESTART DEPENDENCY (RESUME_RETENTION),
        // so they are retained and the finish's own checkpoint is what bounds the
        // window; deleting them would make an unrecoverable gap for a replay.
        src.deferConsume = containerStream && !spoolMode;
        bool active = !spoolEof && feed(band, slots[cur]);
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
                } else if (!spoolMode) {
                    awaitLce();
                    more = feed(band + 1, slots[nxt]);
                } else {
                    awaitLce();
                }
            } else {
                awaitLce();
            }
            finishAdvance(slots[cur]);
            if(containerStream){armPieces(band);releasePieces(false);}
            if(spoolMode){
                // Save the NEXT band cursor at a quiescent barrier.
                if(rawRunsFed>=n/4 && ((band+1)%64==0)) {++band;checkpoint(false);--band;}
                spoolPiece=band+1;
                if(wantNext)more=feed(band+1,slots[nxt]);
                if(more&&slots[nxt].cnt)dispatchLce(slots[nxt]);
            }
            if (band == 0) finish_rss_at("first-band-repaired");
            if ((band & 4095) == 0) finish_rss_at("sweep-progress");
            static bool rssMarker = false;
            if (!rssMarker && sweep.chi > 3000000000ull) {
                rssMarker = true; finish_rss_at("chi_so_far_gt_3e9");
            }
            if ((band & 63) == 0)
                fprintf(stderr, "XSA_FINISH_PROGRESS band=%llu/%llu runs=%llu chi_so_far=%llu repair_wall=%.1f lce_wall=%.1f lce_wait=%.1f advance_wall=%.1f wit_spills=%llu\n",
                        (unsigned long long)(band + 1), (unsigned long long)nbands,
                        (unsigned long long)rs.rNorm, (unsigned long long)sweep.chi,
                        repairWall, lceWall, lceWaitWall, advanceWall, (unsigned long long)0);
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
        if(!spoolEof) rs.finishStream();
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
            // Bounds alone do not establish the rank-closure contract.
            pf_check(lcpCol.maxEnd <= n, "KASAI CLOSURE: a band's rank slice runs past n");
            if (isaDebug) {
                fprintf(stderr, "XSA_FINISH_STREAM_FNV runs=%llu fnv=%llu hsum=%llu tsum=%llu lensum=%llu\n",
                        (unsigned long long)streamRuns, (unsigned long long)streamFnv,
                        (unsigned long long)streamH, (unsigned long long)streamT,
                        (unsigned long long)streamLen);
                fprintf(stderr, "XSA_FINISH_RANK_DEV deviating_runs=%llu max_dev=%llu mean_dev=%.2f\n",
                        (unsigned long long)devRuns.load(), (unsigned long long)devMax.load(),
                        devRuns.load() ? double(devSum.load()) / double(devRuns.load()) : 0.0);
                fprintf(stderr, "XSA_FINISH_RANK_DEV_RK deviating_runs=%llu max_dev=%llu mean_dev=%.2f\n",
                        (unsigned long long)rkDevRuns.load(), (unsigned long long)rkDevMax.load(),
                        rkDevRuns.load() ? double(rkDevSum.load()) / double(rkDevRuns.load()) : 0.0);
            }
        fprintf(stderr,
                    "XSA_FINISH_KASAI_CLOSURE bands=%llu rank_holes=%llu disorder=%llu "
                    "noncontiguous_runs=%llu walk_fallbacks=%llu max_rank_end=%llu n=%llu\n",
                    (unsigned long long)kasaiState.bands, (unsigned long long)kasaiState.gaps,
                    (unsigned long long)kasaiState.disorder,
                    (unsigned long long)kasaiState.noncontig,
                    (unsigned long long)kasaiState.fallback.load(),
                    (unsigned long long)(kasaiOn ? lcpCol.maxEnd : rowCursor), (unsigned long long)n);
            if (sampIsa.map)
                fprintf(stderr, "XSA_FINISH_KASAI_ANCHORS checks=%llu mismatches=0 source=--isa-sample\n",
                        (unsigned long long)anchorChecks);
        }
        pf_check(certifiedRows == n,
                 "STREAM CLOSURE: incomplete stream (refusing publication)");
        // Check AFTER the final tail band, before finalizing or publishing.
        // Bank parity and sparse anchor checks cannot waive closure failures.
        require_zero_closure(kasaiState.gaps, kasaiState.disorder,
                             kasaiState.noncontig, kasaiState.fallback.load());
        pf_check(rowCursor == n, "KASAI CLOSURE: derived ranks do not cover n");
        {
            PfRegion r("advance-finish", PF_ADVANCE);
            double ta = tnow();
            if (buildChi && !spoolEof) { sweep.finish(); sweep.emitWitness.flush(); }
            if(spoolMode && !spoolEof){publishCodecBlock();checkpoint(true);}
            if(spoolMode){
                // THE PHASE'S RAW COUNTERS. What this attempt advanced, what it
                // restored from its own durable checkpoint, and the run total both
                // are measured against; the driver prices phase=finish from these.
                fprintf(stderr,"FINISH_ADVANCE_COUNTERS adv_total=%llu adv_performed=%llu adv_restored=%llu "
                    "adv_restored_raw_runs=%llu adv_restored_bands=%llu basis=finish_spool_state\n",
                    (unsigned long long)advRuns,(unsigned long long)(advRuns-restoredAdvances),
                    (unsigned long long)restoredAdvances,(unsigned long long)restoredRawRuns,
                    (unsigned long long)restoredBands);
                // Standalone run (accounting root): this stage owns phase=finish
                // and emits the one WORK_AVOIDED line for it. Under the driver the
                // driver reports the phase and this branch stays silent.
                if(!workRootDriver())
                    workAvoided("finish",advRuns,advRuns-restoredAdvances,"finish_advance_counters");
                if(containerResume)
                    fprintf(stderr,"WORK_AVOIDED_SUMMARY phase=finish container_off=%llu performed=%llu "
                            "total=%llu avoided=%llu avoided_frac=%.6f basis=finish_advance_counters "
                            "restored_runs=%llu restored_bands=%llu\n",
                            (unsigned long long)streamOut->streamBytes(),
                            (unsigned long long)(advRuns-restoredAdvances),(unsigned long long)advRuns,
                            (unsigned long long)restoredAdvances,
                            advRuns?double(restoredAdvances)/double(advRuns):0.0,
                            (unsigned long long)restoredAdvances,(unsigned long long)restoredBands);
            }
            advanceWall += tnow() - ta;
            r.close(); advAcc.add(r);
        }
        // THE KASAI ARM's own accounting: what the column cost the finish (I/O
        // and bytes) against what the walks used to cost (the fingerprint's
        // verified symbols and hash probes, journaled by fhSlim.report below).
        if (kasaiOn) {
            fprintf(stderr,
                    "XSA_FINISH_LCP_COLUMN_READS preads=%llu staged_bytes=%llu escapes=%llu "
                    "max_lcp=%llu audit=%d arm=kasai\n",
                    (unsigned long long)lcpCol.preads, (unsigned long long)lcpCol.stagedBytes,
                    (unsigned long long)(lcpCol.bandMode() ? lcpCol.escapesRead : lcpCol.escapeCount),
                    (unsigned long long)lcpCol.maxLcp, int(kasaiAudit));
            // THE BAND STREAM'S OWN RECEIPT: coverage, residency and the retirement
            // count are what bound the disc and RAM at pile scale.
            if (lcpCol.bandMode()) lcpCol.finishBands();
        }
        repairCycles.report();
        fprintf(stderr, "SWEEP_PHASE_WALLS repair=%.6f lce=%.6f advance_witness=%.6f lce_threads=%d\n",
                repairWall, lceWall, advanceWall, std::max(1, std::min(threads, 256)));
        // v3 anatomy: repairWall = pieceWait + bandRead + feed + residual.
        waitAcc.report("bands", bandReads-restoredBands);
        readAcc.report("raw_runs", rawRunsFed-restoredRawRuns);
        feedAcc.report("raw_runs", rawRunsFed-restoredRawRuns);
        lceAcc.report("runs", advRuns-restoredAdvances);
        advAcc.report("runs", advRuns-restoredAdvances);
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
        if (streamDumpFd >= 0) pf_check(::close(streamDumpFd) == 0, "close stream dump");
        if (answersDumpFd >= 0) pf_check(::close(answersDumpFd) == 0, "close answers dump");
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
        finish_rss_at("chi-bitmap-populated");
        if (pf_cache_journal_on()) pf_cache_journal(text, "sweep", 1024);
        const uint64_t chi = sweep.chi, rNorm = rs.rNorm;
        fprintf(stderr, "xsa chi-rspace: chi = %llu (N=%llu, R=%llu) stream-agg=1 fused=1 corpus_lce=1\n",
                (unsigned long long)chi, (unsigned long long)n, (unsigned long long)rNorm);
        if (chiExpect) pf_check(chi == chiExpect, "chi mismatch: got " + std::to_string(chi) + " want " + std::to_string(chiExpect));
        fprintf(stderr, "SLIM_SEEDS queries=%llu lce_wall=%.1f repair_wall=%.1f wit_values=%llu wit_spills=%llu wit_bitmap_bytes=%llu\n",
                (unsigned long long)slimLce.seedQueries.read(), lceWall, repairWall,
                (unsigned long long)bitmap.totalValues, (unsigned long long)0,
                (unsigned long long)bitmap.bytes);

        // ---- the PFCK's work is done: delete it before the container pass
        fhSlimOwner.reset();
        fhSeamOwner.reset();
        pf_check(::close(pfckFd) == 0, "close pfck fd");
        if (!keepPfck && !finish_spool::enabled()) {
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

        // Record the observed process high-water RSS before publishing m5.
        // This is process RSS, not an estimate of chi-owned allocations.
        if (buildChi) {
            bitmap.journal();
            struct rusage ru {};
            pf_check(::getrusage(RUSAGE_SELF, &ru) == 0, "chi pre-publish getrusage");
            fprintf(stderr, "XSA_FINISH_CHI_PEAK peak_bytes=%llu scope=process_rss stage=before_publish\n",
                    (unsigned long long)ru.ru_maxrss * 1024);
            fflush(stderr);
        }

        // ---- m5 EF over ascending bitmap positions
        // 8 members in the blocked arm (m1 v2 + m12 + m13), 6 in the frozen
        // --m1-replay arm (m1 codec 101, no run directories).
        // In streaming mode the container already exists (opened before the sweep,
        // m1 payload appended in stream order). Its remaining members are appended
        // after it; nothing is rewritten.
        std::unique_ptr<ContainerOut> ownedContainer;
        if (containerStream) {
            pf_check(streamOut != nullptr, "streaming container was never opened");
            if (streamOut->unsynced) streamOut->syncNow("pre-close");
            // The sweep is over: every run has been emitted, so every piece still
            // pending is covered up to the last CLOSED block. The final short
            // block is appended with m1's directory and releases the rest.
            for (auto& p : src.pendingPieces) if (p.frontier == ~0ull) p.frontier = m1SweepRuns;
            releasePieces(true);
        } else {
            ownedContainer = std::make_unique<ContainerOut>(out, (m1Replay ? 6 : 8) - (buildChi ? 0 : 1));
        }
        ContainerOut& container = containerStream ? *streamOut : *ownedContainer;
        // ---- M1 FINALIZATION, BEFORE ANY OTHER MEMBER IS PLACED ---------------
        // The trailing block directory belongs to m1, so it must be appended while
        // m1 is still the last member: the next member's offset is m1's END, and
        // v7 places members by appending, not by reserving a prefix.
        auto finalizeStreamM1 = [&]() {
            if (!containerStream) return;
                // The final SHORT block (rNorm % M1_BLOCK_RUNS) is still open: it
                // is appended like every other block, so the payload is complete
                // before the directory that indexes it.
                if (m1SweepRuns % M1_BLOCK_RUNS) { m1Head->pad(); m1Len->pad(); closeStreamBlock(); }
                // THE PAYLOAD IS ALREADY IN THE CONTAINER. What is left is the
                // per-block directory, which goes at the END of the member (this
                // is the relocation that makes the payload appendable: v6 puts it
                // in front, so its 16*nblocks bytes must be known before the first
                // payload byte can be placed).
                pf_check(streamOut != nullptr && m1dir.blkHead.size() >= 1, "m1 streaming state");
                const uint64_t nblocks = (rNorm + M1_BLOCK_RUNS - 1) / M1_BLOCK_RUNS;
                pf_check(m1dir.blkHead.size() == nblocks + 1 && m1dir.blkLen.size() == nblocks + 1,
                         "m1 streaming block boundary count");
                // tiling witness: the block spans must add up to exactly the bytes
                // appended, block by block, in the same arithmetic the reader uses.
                std::vector<uint8_t> dir;
                dir.reserve(size_t(16 * nblocks));
                uint64_t payloadBytes = 0;
                for (uint64_t b = 0; b < nblocks; ++b) {
                    const uint64_t hbits = m1dir.blkHead[b + 1] - m1dir.blkHead[b];
                    const uint64_t lbits = m1dir.blkLen[b + 1] - m1dir.blkLen[b];
                    pf_check(hbits <= 0xffff && lbits <= 0xffff, "m1 block bit count overflows u16");
                    pf_check(payloadBytes % 8 == 0, "m1 streaming block not 8-byte aligned");
                    uint8_t e[16];
                    put(e, payloadBytes, 8);
                    put(e + 8, hbits | (lbits << 32), 8);
                    dir.insert(dir.end(), e, e + 16);
                    payloadBytes += blkBytes(m1dir.blkHead[b], m1dir.blkHead[b + 1]) +
                                    blkBytes(m1dir.blkLen[b], m1dir.blkLen[b + 1]);
                    while (payloadBytes % 8) ++payloadBytes;
                }
                pf_check(payloadBytes == m1StreamPayload,
                         "m1 streaming payload size disagrees with its block arithmetic (" +
                         std::to_string(payloadBytes) + " vs " + std::to_string(m1StreamPayload) + ")");
                container.append(dir.data(), dir.size());
                container.ms[0].count = rNorm;
                pf_check(container.ms[0].bytes == 2336 + payloadBytes + 16 * nblocks, "m1 streaming member size");
                fprintf(stderr, "XSA_FINISH_M1_STREAM codec=%u runs=%llu block_runs=%llu blocks=%llu "
                        "dir_bytes=%llu payload_bytes=%llu m1_bytes=%llu trailing_directory=1\n",
                        PF_M1_CODEC_STREAM, (unsigned long long)rNorm,
                        (unsigned long long)M1_BLOCK_RUNS, (unsigned long long)nblocks,
                        (unsigned long long)(16 * nblocks), (unsigned long long)payloadBytes,
                        (unsigned long long)container.ms[0].bytes);
                m1dir.blkHead.clear(); m1dir.blkLen.clear();
                // m1's payload AND its directory are now in the container: every
                // piece still pending is fully covered, so the last of them goes.

            releasePieces(true);
            pf_check(src.pendingPieces.empty(), "pieces still pending absorption at m1 close");
        };
        finalizeStreamM1();
        if (buildChi) {
            PfRegion r("m5-ef", PF_M5);
            unsigned l = chi ? unsigned(std::max(0, int(std::log2(double(n + 1) / double(chi))))) : 0;
            if (l > 63) l = 63;
            uint64_t lowbits = chi * l, highbits = chi ? (n >> l) + chi + 1 : 0;
            BitsOut low(prefix + ".bits-low"), high(prefix + ".bits-high");
            uint64_t i = 0, hpos = 0, prev = ~0ull;
            size_t sidx = 0;
            // A sequential bitmap scan is the sorted, duplicate-free position
            // stream required by codec 105. Only the two bounded BitsOut buffers
            // are resident; EF payloads are streamed to scratch files.
            pf_check(bitmap.totalValues == chi && bitmap.duplicates == 0,
                     "sweep emitted duplicate positions or chi count differs");
            bitmap.for_each_sorted([&](uint64_t v) {
                pf_check(prev == ~0ull || v > prev, "witnesses not strictly increasing");
                prev = v;
                low.lsb(v, l);
                const uint64_t mark = (v >> l) + i;
                pf_check(mark >= hpos && mark < highbits, "EF high extent");
                high.putzeros(mark - hpos); high.bit(1);
                hpos = mark + 1; ++i;
                if (sidx < wantSamples.size() && wantSamples[sidx] < v)
                    pf_fail("sampled banked witness missing: " + std::to_string(wantSamples[sidx]));
                if (sidx < wantSamples.size() && wantSamples[sidx] == v) ++sidx;
            });
            bitmap.journal();
            bitmap.cleanup();
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
            const uint64_t publishedBytes = 24 + low.bytes() + high.bytes();
            fprintf(stderr, "XSA_FINISH_CHI_PUBLISHED entries=%llu bytes=%llu bits_per_entry=%.6f\n",
                    (unsigned long long)chi, (unsigned long long)publishedBytes,
                    chi ? 8.0 * double(publishedBytes) / double(chi) : 0.0);
            r.close(); r.report(i, "witnesses");
        }
        if (!wantSamples.empty())
            fprintf(stderr, "WITNESS_SAMPLE_CHECK samples=%zu passed=1\n", wantSamples.size());
        phase("m5-ef", last);
        finish_rss_at("published-chi");
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

        // ---- m14: THE LCP OVER RUNS (the suffix tree's child delimiters) ------
        // One u64 value per run, in run order, encoded in blocks of
        // PF_LCP_BLOCK_RUNS with a 16-byte directory entry per block in a TRAILING
        // section (the v7 rule). Nothing is re-derived from the corpus here: the
        // spool already holds the fold's own `topLcp` values, block-encoded.
        if (lcpRunsOn) {
            PfRegion r14("m14-lcp-runs", PF_M1);
            lcpRuns.finish();   // close the short final block and fsync the spool
            pf_check(lcpRuns.count == rNorm,
                     "LCP over runs: " + std::to_string(lcpRuns.count) + " values for " +
                         std::to_string(rNorm) + " runs (refusing publication)");
            const uint64_t nblocks = lcpRuns.blocks;
            pf_check(nblocks == (rNorm + PF_LCP_BLOCK_RUNS - 1) / PF_LCP_BLOCK_RUNS,
                     "LCP over runs: block count disagrees with the run count");
            pf_check(lcpRuns.dir.size() == 16 * nblocks, "LCP over runs: directory size");
            // tiling witness: the directory's spans must add up to EXACTLY the payload
            // bytes the spool holds - a dropped or double-counted block fails here, at
            // build time, instead of appearing to a reader as a wrong LCP.
            {
                uint64_t at = 0;
                for (uint64_t b = 0; b < nblocks; ++b) {
                    const uint8_t* d = lcpRuns.dir.data() + 16 * b;
                    const uint64_t off = sxi::get(d, 8);
                    const uint64_t span = sxi::get(d + 8, 4);
                    pf_check(off == at && span % 8 == 0 && span >= 1, "LCP over runs: block span tiling");
                    at += span;
                }
                pf_check(at == lcpRuns.payloadBytes, "LCP over runs: spans do not tile the payload");
            }
            container.begin(14, PF_LCP_CODEC, rNorm);
            {
                uint8_t head[PF_LCP_HEAD];
                lcpRuns.head(head, n);
                container.write(head, PF_LCP_HEAD);
            }
            {
                std::vector<uint8_t> buf(4u << 20);
                for (uint64_t pos = 0; pos < lcpRuns.payloadBytes;) {
                    const size_t z = size_t(std::min<uint64_t>(buf.size(), lcpRuns.payloadBytes - pos));
                    pf_pread_full(lcpRuns.fd, buf.data(), z, pos, "LCP runs payload");
                    container.write(buf.data(), z);
                    pos += z;
                }
            }
            container.write(lcpRuns.dir.data(), lcpRuns.dir.size());
            pf_check(container.ms.back().bytes == lcpRuns.memberBytes(), "LCP over runs: member size");
            fprintf(stderr,
                    "LCP_RUNS runs=%llu bytes=%llu ratio_vs_u32=%.6f encoding=block-width-or-u8+escape "
                    "block=%llu\n",
                    (unsigned long long)rNorm, (unsigned long long)lcpRuns.memberBytes(),
                    rNorm ? double(lcpRuns.memberBytes()) / double(4 * rNorm) : 0.0,
                    (unsigned long long)PF_LCP_BLOCK_RUNS);
            fprintf(stderr,
                    "LCP_RUNS_ENCODING member=m14 codec=%u blocks=%llu payload_bytes=%llu dir_bytes=%llu "
                    "escapes=%llu max_lcp=%llu zero_blocks=%llu u8_blocks=%llu packed_blocks=%llu "
                    "packed_bits_per_run=%.4f bits_per_run=%.4f n=%llu\n",
                    PF_LCP_CODEC, (unsigned long long)nblocks, (unsigned long long)lcpRuns.payloadBytes,
                    (unsigned long long)lcpRuns.dir.size(), (unsigned long long)lcpRuns.escapes,
                    (unsigned long long)lcpRuns.maxValue, (unsigned long long)lcpRuns.zeroBlocks,
                    (unsigned long long)lcpRuns.u8Blocks, (unsigned long long)lcpRuns.packBlocks,
                    rNorm ? double(lcpRuns.packedBits) / double(rNorm) : 0.0,
                    rNorm ? 8.0 * double(lcpRuns.memberBytes()) / double(rNorm) : 0.0,
                    (unsigned long long)n);
            // WHERE THE DEPTH GOES. `bytes_if_clamped_at_64` is an ESTIMATE (an
            // upper bound on the saving): it removes the 10-byte escapes that carry
            // values above 64 and charges nothing for the clamping in their place.
            // The shallow buckets are the ones the tree operations consume; the
            // deep ones delimit unique suffixes (CONTEXT_DEPTH.md).
            fprintf(stderr,
                    "LCP_RUNS_DEPTH runs=%llu b0=%llu b1_8=%llu b9_16=%llu b17_32=%llu b33_64=%llu "
                    "b65_255=%llu b256_64k=%llu b64k_up=%llu deep_runs_gt64=%llu deep_escape_bytes=%llu "
                    "escape_bytes_total=%llu bytes_if_clamped_at_64=%llu frac_le64=%.6f\n",
                    (unsigned long long)rNorm,
                    (unsigned long long)lcpRuns.depthBuckets[0], (unsigned long long)lcpRuns.depthBuckets[1],
                    (unsigned long long)lcpRuns.depthBuckets[2], (unsigned long long)lcpRuns.depthBuckets[3],
                    (unsigned long long)lcpRuns.depthBuckets[4], (unsigned long long)lcpRuns.depthBuckets[5],
                    (unsigned long long)lcpRuns.depthBuckets[6], (unsigned long long)lcpRuns.depthBuckets[7],
                    (unsigned long long)lcpRuns.deepEscapeRuns,
                    (unsigned long long)lcpRuns.deepEscapeBytes,
                    (unsigned long long)(10 * lcpRuns.escapes),
                    (unsigned long long)(lcpRuns.memberBytes() - lcpRuns.deepEscapeBytes),
                    rNorm ? double(rNorm - lcpRuns.deepEscapeRuns) / double(rNorm) : 0.0);
            r14.close(); r14.report(rNorm, "runs");
        }

        // ---- m1 runs.
        // The head stream is a Huffman code over the NORMALIZED RUN SYMBOLS; the
        // length stream is gamma-coded (stateless, no codebook at all). Only the
        // head needs a codebook, and route B derives that codebook from a
        // permutation invariant of the text (see canonical_huffman_m1 and
        // corpus_byte_histogram above), so m1 is ENCODED DURING THE SWEEP in
        // stream order (bandSink): the merge's rlebwt wire is never written and
        // never read, and there is no replay and no second pass.
        // LEGACY (--m1-replay, A/B decode only): the EXACT codebook needs the
        // complete run histogram, so the runs are replayed from the rlebwt wire
        // instead. That block is frozen - it is the path the banked container
        // SHAs were produced by, and it must stay byte-identical.
        {
            PfRegion rM1("m1-runs", PF_M1);
            // The legacy arm stages its streams only now (its codebook needs the
            // complete run histogram); the one-pass arm's streams already hold
            // every run, encoded during the sweep.
            if (m1Replay) {
                m1Head = std::make_unique<BitsOut>(prefix + ".bits-head");
                m1Len = std::make_unique<BitsOut>(prefix + ".bits-len");
            }
            BitsOut& headBits = *m1Head;
            BitsOut& lenBits = *m1Len;
            uint64_t encodedRuns = m1SweepRuns;
            if (m1Replay) {
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
                std::array<unsigned char, 256>& lens = m1LensOut;
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
                        if (!finish_spool::enabled() && pollTimeout != 0 && waited > pollTimeout) pf_fail("timeout waiting for the complete rlebwt");
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        waited += 0.1;
                    }
                    rw.close(); rw.report(expectedRlebwtBytes, "bytes");
                }
                {
                    PfRegion rr("m1-replay", PF_M1);
                    RepairStream rs2;
                    rs2.N = N; rs2.n = n; rs2.seam = &seam;
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
                encodedRuns = replayRuns;
            } else {
            // ROUTE B guards, before a byte of the container is written.
            // (1) The codebook was weighted by a histogram over the SAME
            // alphabet the sweep emitted, and the C table m1 publishes is that
            // histogram's prefix sum. 256 exact equalities therefore prove that
            // the corpus histogram, the codebook weights and the emitted symbol
            // totals agree - the "the BWT is a permutation of the text" premise
            // this arm rests on, checked per cell instead of assumed.
            // (2) The wire this arm exists to delete must really be absent: a
            // stale rlebwt means the producer was not told CROSS_RLEBWT=0 and
            // the scratch saving is not what the run would claim.
            for (unsigned c = 0; c < 256; ++c)
                pf_check((*m1Weights)[c] == rs.totals[c],
                         "corpus byte histogram diverges from the emitted symbol totals on char " +
                         std::to_string(c));
            struct stat wire {};
            pf_check(::stat((prefix + ".rlebwt").c_str(), &wire) != 0,
                     "route B found an rlebwt wire: the producer must run with CROSS_RLEBWT=0");
            fprintf(stderr, "XSA_FINISH_M1_HISTOGRAM cells=256 agreement=exact weights=corpus_byte_histogram "
                    "rlebwt_absent=1 runs=%llu\n", (unsigned long long)rNorm);
          }
            pf_check(encodedRuns == rNorm, "m1 encoded run count");
            headBits.pad();
            lenBits.pad();
            if (containerStream) {
                // m1 (codec 116) is COMPLETE IN THE CONTAINER: the payload was
                // appended block by block during the sweep and the trailing block
                // directory was appended above. No byte of m1 is serialized here,
                // which is the whole point of the v7 layout.
            } else if (m1Replay) {
                // LEGACY (frozen): one sequential stream pair, codec 101. This is
                // the arm the banked container SHAs were produced by; it must stay
                // byte-identical, so it gains nothing and loses nothing here.
                container.begin(1, 101, rNorm);
                // C table = prefix sums of the normalized byte totals (the ri4 C)
                uint64_t acc = 0;
                for (unsigned c = 0; c < 256; ++c) { uint64_t v = acc; acc += rs.totals[c]; container.num(v); }
                container.write(m1LensOut.data(), 256);
                container.num(headBits.bitPos);
                container.num(lenBits.bitPos);
                headBits.copyTo(container);
                lenBits.copyTo(container);
            } else {
                // ---- m1 v3 (codec 114): the same runs, the same two codes, cut
                // into M1_BLOCK_RUNS-run blocks that are each independently
                // decodable, plus the two run directories that let the query
                // kernel reach run k without decoding a single predecessor.
                const uint64_t nblocks = (rNorm + M1_BLOCK_RUNS - 1) / M1_BLOCK_RUNS;
                pf_check(m1dir.blkHead.size() == nblocks && m1dir.blkLen.size() == nblocks,
                         "m1 block boundary count");
                m1dir.blkHead.push_back(headBits.bitPos);
                m1dir.blkLen.push_back(lenBits.bitPos);
                pf_check(m1dir.runStart.size() == (rNorm + PF_RUNSTART_STRIDE - 1) / PF_RUNSTART_STRIDE,
                         "run-start sample count");
                pf_check(m1dir.row == n, "run-start samples do not close on n");
                const uint64_t dirBytes = 16 * nblocks;
                const uint64_t payloadOff = 2336 + dirBytes;
                pf_check(payloadOff % 8 == 0, "m1 payload must start 8-byte aligned");
                // pass 0: the block directory, derived from the bit boundaries
                // alone (no spool read): (payload byte offset u64, head bits u32,
                // length bits u32).
                container.begin(1, PF_M1_CODEC_BLOCKED, rNorm);
                uint64_t acc = 0;
                for (unsigned c = 0; c < 256; ++c) { uint64_t v = acc; acc += rs.totals[c]; container.num(v); }
                container.write(m1LensOut.data(), 256);
                container.num(nblocks);
                container.num(M1_BLOCK_RUNS);
                container.num(rNorm);
                container.num(payloadOff);
                uint64_t payloadBytes = 0;
                {
                    NumBuf dirb;
                    for (uint64_t b = 0; b < nblocks; ++b) {
                        const uint64_t hbits = m1dir.blkHead[b + 1] - m1dir.blkHead[b];
                        const uint64_t lbits = m1dir.blkLen[b + 1] - m1dir.blkLen[b];
                        pf_check(m1dir.blkHead[b + 1] >= m1dir.blkHead[b] &&
                                 m1dir.blkLen[b + 1] >= m1dir.blkLen[b], "m1 block bit range");
                        pf_check(hbits <= 0xffff && lbits <= 0xffff, "m1 block bit count overflows u16");
                        pf_check(payloadBytes % 8 == 0, "m1 block not 8-byte aligned");
                        dirb.put(container, payloadBytes); // full-width byte offset
                        dirb.put(container, hbits | (lbits << 32));
                        // a block's bytes are ceil(bits/8) of BOTH streams (the block
                        // starts byte-aligned and its end is padded to a byte).
                        payloadBytes += blkBytes(m1dir.blkHead[b], m1dir.blkHead[b + 1]) +
                                        blkBytes(m1dir.blkLen[b], m1dir.blkLen[b + 1]);
                        while (payloadBytes % 8) ++payloadBytes;
                    }
                    dirb.flush(container);
                }
                // pass 1: the payload. Both spools are walked forward in block
                // order (one windowed read each), so this is a single sequential
                // pass over the two entropy streams.
                {
                    std::vector<uint8_t> out2;
                    out2.reserve(4u << 20);
                    uint64_t at = 0;
                    // Independent tiling witness: the per-block spans must add up to
                    // EXACTLY the byte length of each spool. Each block starts on a byte
                    // boundary (the previous close called pad()), so the spool holds
                    // sum(ceil(bits_b/8)) bytes - more than ceil(total_bits/8) by the pad
                    // bits spent between blocks - and a span that drops or double-counts
                    // bits fails here, at build time, instead of showing up in the reader
                    // as a bit-count mismatch.
                    uint64_t spanH = 0, spanL = 0;
                    // PHYSICAL byte offsets, kept as running sums. They are NOT
                    // ceil(blkHead[b]/8): pad() flushes the pending partial byte but
                    // never advances bitPos, so bitPos lags the physical stream by the
                    // pad bits already spent and every offset derived from it drifts
                    // (measured: 6 bits, which the reader reported as "block 874: code
                    // lengths sum to 534 of 534 vs 540").
                    uint64_t hOff = 0, lOff = 0;
                    for (uint64_t b = 0; b < nblocks; ++b) {
                        const uint64_t hbits = m1dir.blkHead[b + 1] - m1dir.blkHead[b];
                        const uint64_t lbits = m1dir.blkLen[b + 1] - m1dir.blkLen[b];
                        const size_t hbytes = size_t(blkBytes(m1dir.blkHead[b], m1dir.blkHead[b + 1]));
                        const size_t lbytes = size_t(blkBytes(m1dir.blkLen[b], m1dir.blkLen[b + 1]));
                        spanH += hbytes; spanL += lbytes;
                        if(spoolMode){
                            auto found=entropyPaths.find(b);pf_check(found!=entropyPaths.end(),"missing entropy block index");
                            std::vector<uint8_t> payload;finish_spool::Meta meta;
                            pf_check(finish_spool::load(found->second,payload,&meta),"missing assembly entropy block");
                            pf_check(meta.block==b&&meta.runs==std::min<uint64_t>(M1_BLOCK_RUNS,rNorm-b*M1_BLOCK_RUNS),"entropy block assembly index");
                            pf_check(meta.lo==m1dir.runStart[b]&&meta.hi==(b+1<nblocks?m1dir.runStart[b+1]:n)&&meta.rows==meta.hi-meta.lo,
                                     "entropy block assembly rank range");
                            finish_spool::Archive a(std::move(payload));uint64_t hb=0,lb=0,z=0;
                            a.scalar(hb);a.scalar(lb);pf_check(hb==hbits&&lb==lbits,"entropy block bit lengths");
                            a.scalar(z);pf_check(z==hbytes&&z<=a.data.size()-a.at,"entropy head bytes");
                            out2.insert(out2.end(),a.data.begin()+a.at,a.data.begin()+a.at+z);a.at+=z;
                            a.scalar(z);pf_check(z==lbytes&&z<=a.data.size()-a.at,"entropy length bytes");
                            out2.insert(out2.end(),a.data.begin()+a.at,a.data.begin()+a.at+z);a.at+=z;a.end();
                            hOff+=hbytes;lOff+=lbytes;
                        }else {
                        if (hbytes) {
                            const uint8_t* q = headBits.window(hOff, hbytes);
                            out2.insert(out2.end(), q, q + hbytes);
                            hOff += hbytes;
                        }
                        if (lbytes) {
                            const uint8_t* q = lenBits.window(lOff, lbytes);
                            out2.insert(out2.end(), q, q + lbytes);
                            lOff += lbytes;
                        }
                        }
                        at += hbytes + lbytes;
                        while (at % 8) { out2.push_back(0); ++at; }
                        pf_check(at <= payloadBytes, "m1 payload overflow");
                        if (out2.size() >= (3u << 20)) { container.write(out2.data(), out2.size()); out2.clear(); }
                    }
                    if (!out2.empty()) { container.write(out2.data(), out2.size()); out2.clear(); }
                    pf_check(spanH == headBits.bytes(),
                             "m1 head block spans do not tile the head stream (" +
                             std::to_string(spanH) + " vs " + std::to_string(headBits.bytes()) + ")");
                    pf_check(spanL == lenBits.bytes(),
                             "m1 length block spans do not tile the length stream (" +
                             std::to_string(spanL) + " vs " + std::to_string(lenBits.bytes()) + ")");
                    pf_check(at == payloadBytes, "m1 payload size disagrees with its directory");
                }
                pf_check(container.ms.back().bytes == payloadOff + payloadBytes, "m1 member size");
                fprintf(stderr, "XSA_FINISH_M1_BLOCKS codec=%u runs=%llu block_runs=%llu blocks=%llu "
                        "dir_bytes=%llu payload_bytes=%llu m1_bytes=%llu\n",
                        PF_M1_CODEC_BLOCKED, (unsigned long long)rNorm,
                        (unsigned long long)M1_BLOCK_RUNS, (unsigned long long)nblocks,
                        (unsigned long long)dirBytes, (unsigned long long)payloadBytes,
                        (unsigned long long)(payloadOff + payloadBytes));
            }
            // m12/m13 are stream-order directories either way: they are written
            // from the in-RAM sample state after m1 closes, never appended early.
            if (!m1Replay) {
                // ---- m12: run-start samples. Entry j is the row of run
                // j*PF_RUNSTART_STRIDE (the run's first row), plus a level-2 index
                // every PF_RUNSTART_L2 runs so a lookup is a 16-entry bisect in a
                // cache-resident array followed by a ~16-entry bisect in one window.
                {
                    const uint64_t cnt = m1dir.runStart.size();
                    const uint64_t l2stride = PF_RUNSTART_L2;
                    const uint64_t l2cnt = (rNorm + l2stride - 1) / l2stride;
                    const uint64_t l2off = 64, l1off = l2off + 8 * l2cnt;
                    container.begin(12, 112, cnt);
                    container.num(PF_RUNSTART_STRIDE);
                    container.num(cnt);
                    container.num(rNorm);
                    container.num(n);
                    container.num(l2stride);
                    container.num(l2cnt);
                    container.num(l2off);
                    container.num(l1off);
                    NumBuf b12;
                    for (uint64_t j = 0; j < l2cnt; ++j) {
                        const uint64_t k = j * l2stride;
                        pf_check(k % PF_RUNSTART_STRIDE == 0 && k / PF_RUNSTART_STRIDE < cnt,
                                 "run-start level-2 index is not on a level-1 sample");
                        b12.put(container, m1dir.runStart[k / PF_RUNSTART_STRIDE]);
                    }
                    for (uint64_t v : m1dir.runStart) b12.put(container, v);
                    b12.flush(container);
                    fprintf(stderr, "XSA_FINISH_M12_RUNSTARTS stride=%llu samples=%llu l2_stride=%llu "
                            "l2_entries=%llu bytes=%llu\n",
                            (unsigned long long)PF_RUNSTART_STRIDE, (unsigned long long)cnt,
                            (unsigned long long)l2stride, (unsigned long long)l2cnt,
                            (unsigned long long)(l1off + 8 * cnt));
                }

                // ---- m13: per-symbol sampled run directory. For each symbol c,
                // entries (run k, rows of c before k) at every PF_RUNDIR_OCC-th
                // occurrence of c and at any occurrence PF_RUNDIR_GAP runs past the
                // previous sample, so the run-space scan from a sample to the query
                // run is bounded by PF_RUNDIR_GAP runs. `stride` is the occurrence
                // distance to the next sample: stride 1 means the next sample IS the
                // next run of c, which lets a rank inside that span skip the scan
                // entirely - the fast path that keeps a rare symbol O(log m).
                {
                    uint64_t total = 0, l2total = 0;
                    for (unsigned c = 0; c < 256; ++c) {
                        pf_check(m1dir.dirRun[c].size() == m1dir.dirCum[c].size() &&
                                 m1dir.dirRun[c].size() == m1dir.dirStride[c].size(),
                                 "run directory shape");
                        if (!m1dir.dirRun[c].empty()) m1dir.dirStride[c].back() =
                            uint16_t(m1dir.occ[c] - m1dir.lastSampleOcc[c]);
                        total += m1dir.dirRun[c].size();
                        l2total += (m1dir.dirRun[c].size() + PF_RUNDIR_L2 - 1) / PF_RUNDIR_L2;
                    }
                    const uint64_t countsOff = 72, offsOff = countsOff + 8 * 256;
                    const uint64_t l2off = offsOff + 8 * 257, entoff = l2off + 16 * l2total;
                    container.begin(13, 115, total);
                    container.num(PF_RUNDIR_OCC);
                    container.num(PF_RUNDIR_GAP);
                    container.num(256);
                    container.num(PF_RUNDIR_L2);
                    container.num(total);
                    container.num(n);
                    container.num(rNorm);
                    container.num(entoff);
                    container.num(l2off);
                    NumBuf b13;
                    for (unsigned c = 0; c < 256; ++c) b13.put(container, m1dir.dirRun[c].size());
                    uint64_t off = 0;
                    for (unsigned c = 0; c < 256; ++c) { b13.put(container, off); off += m1dir.dirRun[c].size(); }
                    pf_check(off == total, "run directory offsets");
                    b13.put(container, total);
                    b13.flush(container);
                    // level-2 index: every PF_RUNDIR_L2-th entry of each symbol
                    for (unsigned c = 0; c < 256; ++c) {
                        const size_t m = m1dir.dirRun[c].size();
                        for (size_t j = 0; j < m; j += PF_RUNDIR_L2) {
                            b13.put(container, m1dir.dirCum[c][j]);
                            b13.put(container, m1dir.dirRun[c][j]);       // full-width run index
                        }
                    }
                    b13.flush(container);
                    // codec 115 entries: {cum u64, run u64, stride u64}
                    for (unsigned c = 0; c < 256; ++c) {
                        const size_t m = m1dir.dirRun[c].size();
                        // a symbol absent from the text has no entries (and needs
                        // none: its C block is empty, so every rank of it is 0).
                        pf_check(m > 0 || rs.totals[c] == 0,
                                 "a symbol present in the BWT has no run-directory sample");
                        for (size_t j = 0; j < m; ++j) {
                            b13.put(container, m1dir.dirCum[c][j]);
                            b13.put(container, m1dir.dirRun[c][j]);
                            b13.put(container, m1dir.dirStride[c][j]);
                        }
                    }
                    b13.flush(container);
                    pf_check(container.ms.back().bytes == entoff + 24 * total, "m13 member size");
                    fprintf(stderr, "XSA_FINISH_M13_RUNDIR occ_stride=%llu gap=%llu entries=%llu "
                            "l2_stride=%llu l2_entries=%llu bytes=%llu\n",
                            (unsigned long long)PF_RUNDIR_OCC, (unsigned long long)PF_RUNDIR_GAP,
                            (unsigned long long)total, (unsigned long long)PF_RUNDIR_L2,
                            (unsigned long long)l2total, (unsigned long long)(entoff + 24 * total));
                }
            }
            rM1.close(); rM1.report(rNorm, "runs");
        }
        phase("m1-runs", last);
        if (pf_cache_journal_on()) pf_cache_journal(text, "m1-runs", 1024);

        // ---- publish the lite container (flags: complete | text | remapped)
        // v8 = v7's incremental layout + member m14 (the LCP over runs); flag bit 7
        // declares m14 so a reader never has to guess whether the tree delimiters
        // are there.
        if (lcpRunsOn) container.writeVersion = PF_CONTAINER_V8;
        container.finish(n, 1, rNorm, (buildChi ? 1 : 64) | 8 | 16 | (m1Replay ? 0 : PF_FLAG_M1_BLOCKED) |
                                         (lcpRunsOn ? PF_FLAG_LCP_RUNS : 0));
        phase("publish", last);

        // ---- cleanup: scratch only. rlebwt (+meta) go after the container.
        if (buildChi) bitmap.journal();
        bitmap.cleanup();
        pf_check(::unlink(sparsePath.c_str()) == 0, "unlink sparse scratch");
        if (lcpRunsOn) {
            pf_check(::close(lcpRuns.fd) == 0, "close LCP runs scratch");
            pf_check(::unlink((prefix + ".lcpruns").c_str()) == 0, "unlink LCP runs scratch");
        }
        if (!spoolMode) {
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
        if (!m1Replay) {
            // ROUTE B: there is no rlebwt to release or unlink - the arm exists
            // to never write it. The absence was already asserted before m1 was
            // published; state it again here so the log cannot be read as a
            // silent skip.
            fprintf(stderr, "XSA_FINISH_RLEBWT_NEVER_WRITTEN arm=route-b\n");
        } else if (!keepRlebwt) {
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
        struct rusage finishUsage {};
        pf_check(::getrusage(RUSAGE_SELF, &finishUsage) == 0, "finish getrusage");
        fprintf(stderr, "XSA_FINISH_PEAK_RSS kib=%llu\n", (unsigned long long)finishUsage.ru_maxrss);
        pf_prof_dump("finish");
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "FATAL xsa_finish: %s\n", e.what());
        return 1;
    }
}
