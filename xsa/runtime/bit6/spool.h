// THE SPOOL LAYER: the filesystem is the journal.
//
// Every intermediate this pipeline produces is a SELF-DESCRIBING SPOOL:
//
//   * its NAME encodes its place in the deterministic plan (band / piece index),
//   * its 160-byte HEADER carries the plan identity, the index, the row/run
//     counts, the key-range endpoints and the SHA-256 of its payload,
//   * it is published ATOMICALLY (`<name>.part` -> fdatasync -> hash -> rename
//     -> fsync(dir)), so a final name never covers a partial payload,
//   * it is DELETED only once the state that consumed it is itself durable
//     (fsynced + renamed). Its absence therefore MEANS "already incorporated";
//     where absence is ambiguous (a spool that may never have been produced),
//     a zero-byte positional tombstone carries the same meaning as the absence
//     it accompanies - see RESUME.md.
//
// There is no separate progress journal. Restart derives the plan from the
// corpus + knobs (already deterministic and SHA-checked), enumerates the spools
// that are present, and recomputes exactly the ones that are missing.
//
// A missing spool is recomputed from the corpus. It is never silently skipped:
// every consumer refuses a spool whose header does not match the plan.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>

#include "durable_piece.h"

namespace spool {

// Fixed 176-byte header. Little endian, no native padding surprises: every
// field is an explicit 8-byte little-endian word (x86-64 host, same as every
// other on-disk structure in this tree).
constexpr size_t HDR = 176;
// Magic words. The KIND lives in the magic so a spool identifies itself even if
// it is found under an unexpected name.
constexpr const char* MAGIC_BAND_SA = "SXSPB001";    // scatter_sort band rows
constexpr const char* MAGIC_BAND_LCP = "SXSPL001";   // scatter_sort band LCP slice
constexpr const char* MAGIC_PIECE = "SXSPP001";      // merge band piece

struct Hdr {
    char magic[8] = {0};
    uint64_t version = 1;
    uint64_t index = 0;        // plan position: band index / piece band index
    uint64_t rows = 0;         // rows (BWT ranks) the spool spans
    uint64_t runs = 0;         // runs, when the producer knows them (else 0)
    uint64_t first_pos = 0;    // text position of the spool's first row
    uint64_t last_pos = 0;     // text position of the spool's last row
    uint64_t rank_base = 0;    // global rank of the first row
    uint64_t payload_off = HDR;
    uint64_t payload_bytes = 0;
    uint64_t aux = 0;          // kind-specific (LCP: escape count; piece: shard)
    uint64_t aux2 = 0;         // kind-specific (LCP: max_lcp)
    uint8_t plan[32] = {0};    // plan identity (corpus, knobs, binaries)
    uint8_t sha[32] = {0};     // SHA-256 of payload[0:payload_bytes]
    uint8_t pad[16] = {0};
};
static_assert(sizeof(Hdr) == HDR, "spool header must be exactly 176 bytes");

inline std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}

// Parse a 64-hex-character plan identity. Anything else is a configuration
// error, not a spool mismatch: refuse loudly.
inline bool parse_plan(const std::string& text, uint8_t out[32]) {
    if (text.size() != 64) return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < 32; ++i) {
        const int hi = val(text[2 * i]), lo = val(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t((hi << 4) | lo);
    }
    return true;
}

// The plan identity of the running build, set once from the command line. Every
// spool written by this process carries it, and every spool read is checked
// against it: a spool from a different corpus/knobs/binaries can never be
// adopted, whatever its name says.
inline uint8_t g_plan[32] = {0};
inline bool g_resume = false;      // adopt the spools that are already present

// ---- the fault-injection hook (V4 of the resume gates) ----------------------
//
// `XSA_TEST_KILL_AT=<stage>:<n>` SIGKILLs this process the n-th time the named
// stage is reached, so the fsync/unlink ORDER is proven by a real kill instead of
// being asserted in a comment. Stages:
//   post-fsync   - the payload is fsynced, the final name does not exist yet
//   post-publish - the rename + directory fsync completed
//   pre-unlink   - the consuming state is durable; the consumed spool is not gone
inline void test_kill(const char* stage) {
    static std::atomic<uint64_t> seen{0};
    static const std::string spec = [] {
        const char* e = ::getenv("XSA_TEST_KILL_AT");
        return std::string(e ? e : "");
    }();
    if (spec.empty()) return;
    const size_t colon = spec.rfind(':');
    if (colon == std::string::npos) return;
    if (spec.compare(0, colon, stage) != 0) return;
    const uint64_t want = std::strtoull(spec.c_str() + colon + 1, nullptr, 10);
    if (!want) return;
    if (seen.fetch_add(1) + 1 != want) return;
    std::fprintf(stderr, "XSA_TEST_KILL stage=%s n=%llu pid=%d\n", stage,
                 (unsigned long long)want, int(::getpid()));
    std::fflush(nullptr);
    ::raise(SIGKILL);
}

inline void write_hdr_full(int fd, const Hdr& h) {
    const char* p = reinterpret_cast<const char*>(&h);
    size_t off = 0;
    while (off < HDR) {
        ssize_t n = ::write(fd, p + off, HDR - off);
        if (n <= 0) { std::fprintf(stderr, "spool: header write failed\n"); std::exit(2); }
        off += size_t(n);
    }
}

inline bool read_hdr(int fd, Hdr& h) {
    char* p = reinterpret_cast<char*>(&h);
    size_t off = 0;
    while (off < HDR) {
        ssize_t n = ::pread(fd, p + off, HDR - off, off_t(off));
        if (n <= 0) return false;
        off += size_t(n);
    }
    return true;
}

// SHA-256 of [off, off+len) of an open file. Streamed: a spool is verified
// without ever materialising its payload.
inline bool hash_range(int fd, uint64_t off, uint64_t len, uint8_t out[32]) {
    durable_piece::Sha256 ctx;
    static thread_local std::vector<char> buf;
    if (buf.empty()) buf.resize(1u << 20);
    uint64_t done = 0;
    while (done < len) {
        const size_t take = size_t(std::min<uint64_t>(len - done, buf.size()));
        ssize_t n = ::pread(fd, buf.data(), take, off_t(off + done));
        if (n <= 0) return false;
        ctx.update(buf.data(), size_t(n));
        done += uint64_t(n);
    }
    const std::string hexdig = ctx.finish();
    if (hexdig.size() != 64) return false;
    uint8_t tmp[32];
    if (!parse_plan(hexdig, tmp)) return false;
    std::memcpy(out, tmp, 32);
    return true;
}

inline void sync_dir(const std::string& dir) {
    int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (d < 0 || ::fsync(d) != 0) {
        std::fprintf(stderr, "spool: directory fsync failed: %s\n", dir.c_str());
        std::exit(2);
    }
    ::close(d);
}

// THE ATOMIC PUBLICATION. The caller has written header+payload into `fd`
// (opened on `<fin>.part`). This fsyncs the payload, hashes it, renames it into
// its final name, fsyncs the directory, and only then returns. Nothing is ever
// visible under a final name until its bytes are durable and hashed.
inline void publish_part(int fd, const std::string& tmp, const std::string& fin,
                         const std::string& dir, const uint8_t sha[32]) {
    durable_piece::check(::fdatasync(fd) == 0, "fdatasync spool");
    test_kill("post-fsync");
    durable_piece::check(::close(fd) == 0, "close spool");
    // The producer computed the hash while writing; verify the file it just
    // wrote really is the bytes it hashed (a short/lost write must not publish).
    int in = ::open(tmp.c_str(), O_RDONLY);
    durable_piece::check(in >= 0, "open spool for verify");
    uint8_t got[32];
    struct stat st {};
    durable_piece::check(::fstat(in, &st) == 0, "stat spool");
    durable_piece::check(hash_range(in, HDR, uint64_t(st.st_size) - uint64_t(HDR), got),
                         "hash published spool");
    durable_piece::check(std::memcmp(got, sha, 32) == 0, "spool payload hash mismatch");
    durable_piece::check(::close(in) == 0, "close spool verify");
    durable_piece::check(::rename(tmp.c_str(), fin.c_str()) == 0, "rename spool");
    sync_dir(dir);
    test_kill("post-publish");
}

// Verify a present spool against the plan and its own header. `expect_magic`
// identifies the kind, `expect_index` the plan position. The payload hash is
// checked: a spool's provenance is verifiable FROM ITSELF ALONE, so a truncated
// or foreign file can never be adopted.
inline bool verify(const std::string& path, const char* expect_magic,
                   uint64_t expect_index, const uint8_t plan[32], Hdr* out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    Hdr h;
    struct stat st {};
    bool ok = read_hdr(fd, h) && ::fstat(fd, &st) == 0;
    if (ok && out) *out = h;
    if (ok) {
        ok = std::memcmp(h.magic, expect_magic, 8) == 0 && h.version == 1 &&
             h.index == expect_index && h.payload_off == HDR &&
             uint64_t(st.st_size) == h.payload_off + h.payload_bytes;
    }
    if (ok && plan) ok = std::memcmp(h.plan, plan, 32) == 0;
    if (ok) {
        uint8_t got[32];
        ok = hash_range(fd, h.payload_off, h.payload_bytes, got) &&
             std::memcmp(got, h.sha, 32) == 0;
    }
    ::close(fd);
    return ok;
}

// Header-only check (no payload hash): used by a DIRECTORY SCAN that only needs
// the plan position and the row extent, e.g. to locate the spool containing a
// frontier row. The consumer that actually takes the payload verifies the hash
// against the bytes it read (spool::verify / the streaming check in tape_emit).
inline bool peek(const std::string& path, const char* expect_magic,
                 uint64_t expect_index, const uint8_t plan[32], Hdr* out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    Hdr h;
    struct stat st {};
    bool ok = read_hdr(fd, h) && ::fstat(fd, &st) == 0;
    ::close(fd);
    if (!ok) return false;
    if (std::memcmp(h.magic, expect_magic, 8) != 0 || h.version != 1 ||
        h.index != expect_index || h.payload_off != HDR ||
        uint64_t(st.st_size) != h.payload_off + h.payload_bytes)
        return false;
    if (plan && std::memcmp(h.plan, plan, 32) != 0) return false;
    if (out) *out = h;
    return true;
}

}  // namespace spool
