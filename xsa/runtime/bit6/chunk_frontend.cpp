// Document-aligned cyclic BWT chunks. This is merge material, not a merger.
// Build: cc -O3 -I bit6/third_party/libsais -c bit6/third_party/libsais/token-libsais.c -o /tmp/libsais.o
//        c++ -O3 -std=c++17 bit6/chunk_frontend.cpp /tmp/libsais.o -o /tmp/chunk_frontend
#include "third_party/libsais/token-libsais.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static void require(bool b, const char* msg) { if (!b) throw std::runtime_error(msg); }
static void write_all(int fd, const void* data, size_t n) {
    auto* p = static_cast<const uint8_t*>(data);
    while (n) {
        ssize_t k = ::write(fd, p, n);
        require(k > 0, "chunk output write failed");
        p += k; n -= static_cast<size_t>(k);
    }
}
struct Writer {
    int fd;
    std::array<uint8_t, 1<<20> data{};
    size_t used = 0;
    explicit Writer(int descriptor):fd(descriptor) {}
    void flush() { if (used) { write_all(fd, data.data(), used); used = 0; } }
    void word(uint64_t value, unsigned bytes) {
        if (used + bytes > data.size()) flush();
        for (unsigned i = 0; i < bytes; ++i)
            data[used++] = static_cast<uint8_t>(value >> (8 * i));
    }
};
struct Run { uint8_t c; uint64_t len; uint64_t h, t; };
// Corpus-reference sidecar (chunk-i.ref): the chunk's text is NOT copied;
// consumers read remap[corpus[offset..offset+n)] instead. Layout:
//   "SXRF" | u32 ver=1 | u8 flags=0 | remap[256] | u32 pathLen | path | u64 offset | u64 n
// offset/n are the chunk's corpus range (already the .crle header values);
// the remap is embedded so every chunk is self-contained.
static void emit_ref(const fs::path& refPath, const std::array<uint8_t,256>& remap,
                     const fs::path& corpus, uint64_t offset, uint64_t n) {
    int fd = ::open(refPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    require(fd >= 0, "chunk ref output exists or cannot be created");
    try {
        Writer w(fd);
        for (uint8_t c : {'S','X','R','F'}) w.word(c, 1);
        w.word(1, 4);
        w.word(0, 1);
        for (unsigned i = 0; i < 256; ++i) w.word(remap[i], 1);
        std::string p = fs::absolute(corpus).string();
        require(p.size() <= 0xffffffffu, "corpus path too long");
        w.word(p.size(), 4);
        for (char c : p) w.word(static_cast<uint8_t>(c), 1);
        w.word(offset, 8);
        w.word(n, 8);
        w.flush();
        require(::close(fd) == 0, "chunk ref close failed");
    } catch (...) { ::close(fd); throw; }
}
// Order-column sidecar (chunk-i.pos, SXP4): the merge load phase is
// DERIVATION (BWT/LF walks from the runs) - the pile-scale bottleneck
// (~0.7 MB/s serial). The chunker ALREADY computes the cyclic order when
// it builds the runs, so persisting it is one write with zero extra
// compute: the merge's raw-chunk load collapses to pure reads (1GB: load
// 1845s -> 3.0s). Layout:
//   "SXP4" | u32 ver=1 | u64 offset | u64 n | u64 period | u64 rsvd |
//   u64 groups | per group: startRow, byteOff, basePos | zigzag varints
// CHUNK_NO_POS=1 restores the legacy artifact set (walk-derived loads).
static void emit_pos(const fs::path& posPath, const std::vector<int32_t>& order,
                     const std::vector<uint8_t>& text, uint64_t offset, uint64_t n) {
    int fd = ::open(posPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    require(fd >= 0, "chunk pos output exists or cannot be created");
    try {
        Writer w(fd);
        for (uint8_t c : {'S','X','P','4'}) w.word(c, 1);
        w.word(1, 4);
        w.word(offset, 8); w.word(n, 8);
        // Minimal cyclic period (same rule the merge applies): the merge's
        // periodic anchor form needs it; computed here once, on the
        // resident text, instead of a merge-time divisor sweep.
        uint64_t period = n;
        {
            std::vector<uint64_t> divs;
            for (uint64_t d = 1; d * d <= n; ++d)
                if (n % d == 0) { divs.push_back(d); if (d != n / d) divs.push_back(n / d); }
            std::sort(divs.begin(), divs.end());
            for (uint64_t d : divs) {
                if (d >= n) continue;
                bool ok = true;
                for (uint64_t i = 0; i + d < n && ok; ++i) ok = text[i] == text[i + d];
                if (ok) { period = d; break; }
            }
        }
        w.word(period, 8); w.word(0, 8);
        // SXP4 body: groups of 4096 rows; per group (startRow, byteOff,
        // basePos) in the table, zigzag varint position deltas in the
        // stream. Absolute positions stay 64-bit (bases/n/offsets); only
        // bounded within-group deltas are narrow. ~3.13 B/position measured
        // on pile-like 100MB text (8n -> ~3.1n: the chunk-disc lever).
        {
            const uint64_t GRP = 4096;
            uint64_t groups = (n + GRP - 1) / GRP;
            std::vector<uint64_t> starts(groups), offs(groups), bases(groups);
            std::vector<uint8_t> stream;
            stream.reserve(n * 4 + 64);
            auto putv = [&](uint64_t v) {
                while (v >= 0x80) { stream.push_back(uint8_t(v) | 0x80); v >>= 7; }
                stream.push_back(uint8_t(v));
            };
            for (uint64_t g = 0; g < groups; ++g) {
                uint64_t b0 = g * GRP, len = std::min<uint64_t>(GRP, n - b0);
                starts[g] = b0;
                bases[g] = len ? static_cast<uint64_t>(order[b0]) : 0;
                offs[g] = stream.size();
                uint64_t prev = bases[g];
                for (uint64_t i = 1; i < len; ++i) {
                    uint64_t p = static_cast<uint64_t>(order[b0 + i]);
                    putv(p >= prev ? 2 * (p - prev) : 2 * (prev - p) - 1);   // standard zigzag
                    prev = p;
                }
            }
            w.word(groups, 8);
            w.word(0, 8);   // pad: the group table starts at byte 56
            uint64_t streamBase = 56 + 24 * groups;
            for (uint64_t g = 0; g < groups; ++g) {
                w.word(starts[g], 8);
                w.word(streamBase + offs[g], 8);
                w.word(bases[g], 8);
            }
            w.flush();
            write_all(fd, stream.data(), stream.size());
        }
        require(::close(fd) == 0, "chunk pos close failed");
    } catch (...) { ::close(fd); throw; }
}
static void emit(const fs::path& output, const std::vector<uint8_t>& text,
                 uint64_t offset, unsigned index,
                 const std::array<uint8_t,256>& remap, const fs::path& corpus) {
    require(!text.empty() && text.back() == 0x1e, "unaligned chunk");
    uint64_t n = text.size();
    require(n <= INT32_MAX / 2, "libsais doubled chunk exceeds int32 length");
    auto start = Clock::now();
    std::vector<uint8_t> doubled(2*n);
    std::copy(text.begin(), text.end(), doubled.begin());
    std::copy(text.begin(), text.end(), doubled.begin() + n);
    std::vector<int32_t> sa(2*n), plcp(2*n);
    require(libsais(doubled.data(), sa.data(), static_cast<int32_t>(2*n), 0, nullptr) == 0,
            "libsais suffix sort failed");
    require(libsais_plcp(doubled.data(), sa.data(), plcp.data(), static_cast<int32_t>(2*n)) == 0,
            "libsais PLCP failed");
    // Adjacent suffixes with >=n common bytes are the same cyclic rotation.
    // Regular suffix order breaks periodic ties by remaining length; the
    // repository breaks them by source position, so reorder each equal group.
    std::vector<int32_t> order;
    order.reserve(n);
    int32_t minimum = INT32_MAX;
    size_t group = 0;
    for (size_t i = 0; i < sa.size(); ++i) {
        if (i) minimum = std::min(minimum, plcp[sa[i]]);
        if (static_cast<uint64_t>(sa[i]) >= n) continue;
        if (!order.empty() && minimum < static_cast<int32_t>(n)) {
            std::sort(order.begin() + group, order.end());
            group = order.size();
        }
        order.push_back(sa[i]);
        minimum = INT32_MAX;
    }
    std::sort(order.begin() + group, order.end());
    require(order.size() == n, "cyclic SA projection failed");
    std::vector<Run> runs;
    for (int32_t pos : order) {
        uint8_t c = text[(static_cast<uint64_t>(pos) + n - 1) % n];
        if (runs.empty() || runs.back().c != c || runs.back().len == UINT64_MAX)
            runs.push_back({c, 0, static_cast<uint64_t>(pos), static_cast<uint64_t>(pos)});
        Run& r = runs.back();
        ++r.len; r.t = static_cast<uint64_t>(pos);
    }
    double wall = std::chrono::duration<double>(Clock::now() - start).count();
    fs::path path = output / ("chunk-" + std::to_string(index) + ".crle");
    // SXCR v3 (default): COMPACT run records - char + varint length; the
    // head/tail samples are NOT embedded (they are derivable from the
    // persisted order column chunk-N.pos, which v3 requires). 25 B/run ->
    // ~2 B/run: the disc lever that fits the pile (~9.7n -> ~0.8n of runs).
    // CHUNK_NO_POS=1 restores the v2 sample-bearing 25-byte records.
    bool v3 = !getenv("CHUNK_NO_POS");
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    require(fd >= 0, "chunk output exists or cannot be created");
    try {
        Writer writer(fd);
        for (uint8_t c : {'S','X','C','R'}) writer.word(c, 1);
        writer.word(v3 ? 3 : 2, 4); writer.word(offset, 8); writer.word(n, 8);
        writer.word(runs.size(), 8);
        if (v3) {
            for (auto r : runs) {
                writer.word(r.c, 1);
                uint64_t len = r.len;
                while (len >= 0x80) { writer.word(uint64_t(uint8_t(len) | 0x80), 1); len >>= 7; }
                writer.word(len, 1);
            }
        } else {
            for (auto r : runs) {
                writer.word(r.c, 1); writer.word(r.len, 8);
                writer.word(r.h, 8); writer.word(r.t, 8);
            }
        }
        writer.flush();
        require(::close(fd) == 0, "chunk output close failed");
    } catch (...) { ::close(fd); throw; }
    // Corpus-referenced text: default ON (the chunk's text lives in the
    // corpus; merge sides read remap[corpus[offset..offset+n)]). CHUNK_NO_REF
    // restores the legacy no-sidecar artifact set (text re-derived by walk).
    if (!getenv("CHUNK_NO_REF"))
        emit_ref(output / ("chunk-" + std::to_string(index) + ".ref"),
                 remap, corpus, offset, n);
    if (!getenv("CHUNK_NO_POS"))
        emit_pos(output / ("chunk-" + std::to_string(index) + ".pos"),
                 order, text, offset, n);
    std::printf("CHUNK index=%u offset=%llu n=%llu runs=%zu sort_wall_seconds=%.6f path=%s\n",
                index, (unsigned long long)offset, (unsigned long long)n,
                runs.size(), wall, path.c_str());
    std::fflush(stdout);
}
int main(int argc, char** argv) {
    try {
        require(argc == 4 || argc == 5,
                "usage: chunk_frontend INPUT CHUNKS OUTPUT_DIR [REMAP_256]");
        const fs::path input = argv[1], output = argv[3];
        size_t wanted = std::stoul(argv[2]);
        require(wanted > 0 && wanted < 1000000, "invalid chunk count");
        require(fs::is_regular_file(input), "input is not a regular file");
        require(fs::is_directory(output), "output directory missing");
        std::array<uint8_t,256> remap{};
        for (unsigned i = 0; i < 256; ++i) remap[i] = i;
        if (argc == 5) {
            std::ifstream map(argv[4], std::ios::binary);
            require(bool(map.read(reinterpret_cast<char*>(remap.data()), 256)) && map.peek() == EOF,
                    "remap must have exactly 256 bytes");
        }
        auto checkmap = remap;
        std::sort(checkmap.begin(), checkmap.end());
        for (unsigned i = 0; i < 256; ++i) require(checkmap[i] == i, "remap not bijective");
        require(remap[0x1e] == 0x1e, "remap changes document separator");
        uint64_t total = fs::file_size(input), target = (total + wanted - 1) / wanted;
        std::ifstream source(input, std::ios::binary);
        require(bool(source) && total, "empty/unreadable input");
        std::vector<uint8_t> chunk;
        chunk.reserve(target + 65536);
        std::array<char,1<<20> block{};
        uint64_t offset = 0, consumed = 0;
        unsigned index = 0;
        while (source) {
            source.read(block.data(), block.size());
            size_t got = static_cast<size_t>(source.gcount());
            for (size_t i = 0; i < got; ++i) {
                uint8_t original = static_cast<uint8_t>(block[i]);
                chunk.push_back(remap[original]);
                ++consumed;
                if (original == 0x1e && chunk.size() >= target && index + 1 < wanted) {
                    emit(output, chunk, offset, index++, remap, input);
                    offset = consumed; chunk.clear();
                }
            }
        }
        require(consumed == total, "short source read");
        require(!chunk.empty() && chunk.back() == 0x1e, "input must end at document boundary");
        emit(output, chunk, offset, index++, remap, input);
        require(index == wanted, "document boundaries did not permit requested chunk count");
        std::printf("CHUNKS_PASS count=%u source_bytes=%llu\n", index,
                    (unsigned long long)consumed);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "CHUNK_FRONTEND_FATAL %s\n", e.what());
        return 1;
    }
}
