// Document-aligned cyclic BWT chunks. This is merge material, not a merger.
// Build: cc -O3 -I bit6/third_party/libsais -c bit6/third_party/libsais/token-libsais.c -o /tmp/libsais.o
//        c++ -O3 -std=c++17 bit6/chunk_frontend.cpp /tmp/libsais.o -o /tmp/chunk_frontend
#include "third_party/libsais/token-libsais.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
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
// CHUNK_POS_MODE=dense writes the SXP3 8-byte column instead: compact
// cuts the pos disc ~2.4x but costs ~2.3x walk wall (windowed decode per
// access); the plan (XSA_POS_MODE) picks per scale - dense when the disc
// budget allows it, compact when it does not.
static uint64_t cyclic_period(const std::vector<uint8_t>& text) {
    const uint64_t n = text.size();
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
    return period;
}
static void emit_pos(const fs::path& posPath, const std::vector<int32_t>& order,
                     uint64_t period, uint64_t offset, uint64_t n) {
    int fd = ::open(posPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    require(fd >= 0, "chunk pos output exists or cannot be created");
    try {
        Writer w(fd);
        const char* pm = getenv("CHUNK_POS_MODE");
        const bool dense = pm && !std::strcmp(pm, "dense");
        for (uint8_t c : {'S','X','P'}) w.word(c, 1);
        w.word(dense ? '3' : '4', 1);
        w.word(dense ? 1 : 2, 4); w.word(offset, 8); w.word(n, 8);
        w.word(period, 8); w.word(0, 8);
        if (dense) {
            for (uint64_t i = 0; i < n; ++i) w.word(order[i], 8);
        } else {
            const uint64_t GRP = 4096, groups = (n + GRP - 1) / GRP;
            w.word(groups, 8); w.word(0, 8);
            // Static Huffman model over zigzag-varint bytes. Group-local
            // byte padding preserves bounded random-access decoding.
            auto delta = [&](uint64_t i) {
                int64_t d = int64_t(order[i]) - order[i - 1];
                return uint64_t(d >= 0 ? 2 * d : -2 * d - 1);
            };
            auto bytes = [&](uint64_t g, auto consume) {
                uint64_t begin = g * GRP, end = std::min(begin + GRP, n);
                for (uint64_t i = begin + 1; i < end; ++i) {
                    uint64_t v = delta(i);
                    while (v >= 128) { consume(uint8_t(v) | 128); v >>= 7; }
                    consume(uint8_t(v));
                }
            };
            std::array<uint64_t,256> freq{};
            for (uint64_t g = 0; g < groups; ++g) bytes(g, [&](uint8_t c){ ++freq[c]; });
            struct Node { uint64_t weight; int left, right; };
            std::vector<Node> nodes(256);
            using Item = std::pair<uint64_t,int>;
            std::priority_queue<Item,std::vector<Item>,std::greater<Item>> heap;
            for (int c = 0; c < 256; ++c) if (freq[c]) heap.push({freq[c],c});
            if (heap.empty()) heap.push({1,0});
            while (heap.size() > 1) {
                auto x=heap.top(); heap.pop(); auto y=heap.top(); heap.pop();
                int id=nodes.size(); nodes.push_back({x.first+y.first,x.second,y.second});
                heap.push({nodes.back().weight,id});
            }
            std::array<unsigned,256> lengths{};
            auto visit = [&](auto&& self, int id, unsigned depth) -> void {
                if (id < 256) { lengths[id]=std::max(1u,depth); return; }
                self(self,nodes[id].left,depth+1); self(self,nodes[id].right,depth+1);
            };
            visit(visit,heap.top().second,0);
            std::vector<std::pair<unsigned,unsigned>> alphabet;
            for (unsigned c=0;c<256;++c) if (lengths[c]) alphabet.push_back({lengths[c],c});
            std::sort(alphabet.begin(),alphabet.end());
            std::array<uint64_t,256> codes{};
            uint64_t code=0; unsigned previous=0;
            for (auto [len,c]:alphabet) {
                require(len <= 32,"pos Huffman code too long");
                code <<= len-previous; codes[c]=code++; previous=len;
            }
            // SXP4 v2: model follows the unchanged group table.
            // Model must follow table, so seek past its reserved region.
            w.flush();
            require(::lseek(fd,56+24*groups,SEEK_SET)>=0,"pos model seek");
            for (unsigned c=0;c<256;++c) w.word(lengths[c],1);
            w.flush();
            require(::lseek(fd,56,SEEK_SET)>=0,"pos table seek");
            uint64_t byteOff=56+24*groups+256, oldBytes=56+24*groups;
            for (uint64_t g=0;g<groups;++g) {
                w.word(g*GRP,8); w.word(byteOff,8); w.word(order[g*GRP],8);
                uint64_t bits=0;
                bytes(g,[&](uint8_t c){ bits+=lengths[c]; ++oldBytes; });
                byteOff+=(bits+7)/8;
            }
            w.flush();
            require(::lseek(fd,56+24*groups+256,SEEK_SET)>=0,"pos body seek");
            for (uint64_t g=0;g<groups;++g) {
                uint8_t pending=0; unsigned used=0;
                bytes(g,[&](uint8_t c){
                    for (unsigned k=lengths[c];k;--k) {
                        pending=uint8_t((pending<<1)|((codes[c]>>(k-1))&1));
                        if (++used==8) { w.word(pending,1); pending=0; used=0; }
                    }
                });
                if (used) w.word(uint8_t(pending<<(8-used)),1);
            }
            std::fprintf(stderr,"CHUNK_POS_CODEC n=%llu old_bytes=%llu new_bytes=%llu B_per_pos=%.6f\n",
                (unsigned long long)n,(unsigned long long)oldBytes,
                (unsigned long long)byteOff,double(byteOff)/n);

        }
        w.flush();
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
    const uint64_t period = cyclic_period(text);
    std::vector<int32_t> sa(2*n);
    require(libsais(doubled.data(), sa.data(), static_cast<int32_t>(2*n), 0, nullptr) == 0,
            "libsais suffix sort failed");
    // Filtering never overwrites an unread SA entry. Distinct rotations
    // already have cyclic order in the doubled suffix array.
    size_t retained = 0;
    for (size_t i = 0; i < sa.size(); ++i)
        if (uint64_t(sa[i]) < n) sa[retained++] = sa[i];
    require(retained == n, "cyclic SA projection failed");
    if (period < n) {
        // Equal rotations form contiguous residue classes modulo period.
        // Emit each class in source-position order, preserving legacy ties.
        const uint64_t count = n / period;
        for (uint64_t begin = 0; begin < n; begin += count) {
            const uint64_t residue = uint64_t(sa[begin]) % period;
            for (uint64_t j = 0; j < count; ++j)
                sa[begin + j] = int32_t(residue + j * period);
        }
    }
    std::vector<uint8_t>().swap(doubled);
    double wall = std::chrono::duration<double>(Clock::now() - start).count();
    uint64_t runCount = 0;
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
        writer.word(0, 8); // patched after the bounded streaming walk
        Run run{};
        auto flushRun = [&] {
            if (!run.len) return;
            ++runCount;
            writer.word(run.c, 1);
            if (v3) {
                uint64_t len = run.len;
                while (len >= 0x80) { writer.word(uint8_t(len) | 0x80, 1); len >>= 7; }
                writer.word(len, 1);
            } else {
                writer.word(run.len, 8); writer.word(run.h, 8); writer.word(run.t, 8);
            }
        };
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t pos = uint64_t(sa[i]);
            uint8_t c = text[pos ? pos - 1 : n - 1];
            if (!run.len || run.c != c || run.len == UINT64_MAX) {
                flushRun(); run = {c, 0, pos, pos};
            }
            ++run.len; run.t = pos;
        }
        flushRun(); writer.flush();
        require(::lseek(fd, 24, SEEK_SET) == 24, "chunk run count seek failed");
        writer.word(runCount, 8); writer.flush();
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
                 sa, period, offset, n);
    std::printf("CHUNK index=%u offset=%llu n=%llu runs=%llu sort_wall_seconds=%.6f path=%s\n",
                index, (unsigned long long)offset, (unsigned long long)n,
                (unsigned long long)runCount, wall, path.c_str());
    std::fflush(stdout);
}
// PILE-WEEK (pile-week-v1) CHUNK PARALLELISM: the chunker was serial per
// chunk (64 x ~5s at the 10GB pile shape; 204s at the 1GB gate = the largest
// single-thread term after the merge). The phase split below preserves the
// banked byte stream exactly: phase 1 is the SAME serial boundary scan
// (document-aligned at >=target, the last byte of every chunk is 0x1e);
// phase 2 runs the per-chunk sorts concurrently (each worker preads its
// range and applies the same remap; emit() is unchanged). RAM bound: each
// worker holds ~11x its chunk bytes (text + doubled text + SA), so the worker
// count is a knob (CHUNK_WORKERS, default 1 = the banked serial behavior)
// and the front door sizes it from --memory and journals it. The artifacts
// are byte-identical by construction (same boundaries, same emit); the 100MB
// and 1GB gates confirm.
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
        unsigned workers = 1;
        if (const char* w = getenv("CHUNK_WORKERS")) {
            workers = unsigned(std::stoul(w));
            require(workers >= 1 && workers < 1000000, "invalid CHUNK_WORKERS");
        }
        uint64_t total = fs::file_size(input), target = (total + wanted - 1) / wanted;
        require(total, "empty/unreadable input");
        // ---- phase 1: the banked serial boundary scan (chunk boundaries and
        // the consumed count are computed BEFORE any sort runs).
        struct Bound { uint64_t offset, len; };
        std::vector<Bound> bounds;
        {
            std::ifstream source(input, std::ios::binary);
            require(bool(source), "input unreadable");
            std::array<char,1<<20> block{};
            uint64_t offset = 0, consumed = 0, chunkLen = 0;
            unsigned index = 0;
            while (source) {
                source.read(block.data(), block.size());
                size_t got = static_cast<size_t>(source.gcount());
                for (size_t i = 0; i < got; ++i) {
                    uint8_t original = static_cast<uint8_t>(block[i]);
                    ++consumed;
                    ++chunkLen;
                    if (original == 0x1e && chunkLen >= target && index + 1 < wanted) {
                        bounds.push_back({offset, chunkLen});
                        offset = consumed; chunkLen = 0; ++index;
                    }
                }
            }
            require(consumed == total, "short source read");
            require(chunkLen && chunkLen <= total, "input must end at document boundary");
            bounds.push_back({offset, chunkLen});
            ++index;
            require(index == wanted, "document boundaries did not permit requested chunk count");
        }
        // ---- phase 2: concurrent per-chunk sorts (byte-identical emit)
        {
            int ifd = ::open(input.c_str(), O_RDONLY);
            require(ifd >= 0, "reopen input for chunk reads");
            std::atomic<size_t> next{0};
            auto runOne = [&](unsigned index) {
                const Bound& b = bounds[index];
                std::vector<uint8_t> chunk;
                chunk.reserve(b.len + 65536);
                uint64_t done = 0;
                std::array<char, 1<<20> block{};
                while (done < b.len) {
                    size_t want = static_cast<size_t>(std::min<uint64_t>(block.size(), b.len - done));
                    ssize_t got = ::pread(ifd, block.data(), want, static_cast<off_t>(b.offset + done));
                    require(got > 0, "chunk range pread failed");
                    for (size_t i = 0; i < static_cast<size_t>(got); ++i)
                        chunk.push_back(remap[static_cast<uint8_t>(block[i])]);
                    done += static_cast<uint64_t>(got);
                }
                require(!chunk.empty() && chunk.back() == 0x1e, "unaligned chunk");
                emit(output, chunk, b.offset, index, remap, input);
            };
            if (workers <= 1) {
                for (size_t i = 0; i < bounds.size(); ++i) runOne(unsigned(i));
            } else {
                std::vector<std::thread> ts;
                std::atomic<unsigned> failed{0};
                std::string failMsg;
                for (unsigned t = 0; t < std::min<unsigned>(workers, unsigned(bounds.size())); ++t)
                    ts.emplace_back([&] {
                        for (;;) {
                            size_t i = next.fetch_add(1);
                            if (i >= bounds.size()) break;
                            try {
                                runOne(unsigned(i));
                            } catch (const std::exception& e) {
                                unsigned was = 0;
                                if (!failed.compare_exchange_strong(was, 1)) return;
                                failMsg = e.what();
                                return;
                            } catch (...) {
                                unsigned was = 0;
                                if (!failed.compare_exchange_strong(was, 1)) return;
                                failMsg = "unknown chunk worker failure";
                                return;
                            }
                        }
                    });
                for (auto& t : ts) t.join();
                if (failed.load()) {
                    std::fprintf(stderr, "CHUNK_FRONTEND_FATAL %s\n", failMsg.c_str());
                    return 1;
                }
            }
            ::close(ifd);
        }
        std::printf("CHUNKS_PASS count=%u source_bytes=%llu workers=%u\n", unsigned(bounds.size()),
                    (unsigned long long)total, workers);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "CHUNK_FRONTEND_FATAL %s\n", e.what());
        return 1;
    }
}
