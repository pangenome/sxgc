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
struct Run { uint8_t c; uint32_t len; uint64_t h, t; };
static void emit(const fs::path& output, const std::vector<uint8_t>& text,
                 uint64_t offset, unsigned index) {
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
        if (runs.empty() || runs.back().c != c || runs.back().len == UINT32_MAX)
            runs.push_back({c, 0, static_cast<uint64_t>(pos), static_cast<uint64_t>(pos)});
        Run& r = runs.back();
        ++r.len; r.t = static_cast<uint64_t>(pos);
    }
    double wall = std::chrono::duration<double>(Clock::now() - start).count();
    fs::path path = output / ("chunk-" + std::to_string(index) + ".crle");
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    require(fd >= 0, "chunk output exists or cannot be created");
    try {
        Writer writer(fd);
        for (uint8_t c : {'S','X','C','R'}) writer.word(c, 1);
        writer.word(1, 4); writer.word(offset, 8); writer.word(n, 8);
        writer.word(runs.size(), 8);
        for (auto r : runs) {
            writer.word(r.c, 1); writer.word(r.len, 4);
            writer.word(r.h, 8); writer.word(r.t, 8);
        }
        writer.flush();
        require(::close(fd) == 0, "chunk output close failed");
    } catch (...) { ::close(fd); throw; }
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
                    emit(output, chunk, offset, index++);
                    offset = consumed; chunk.clear();
                }
            }
        }
        require(consumed == total, "short source read");
        require(!chunk.empty() && chunk.back() == 0x1e, "input must end at document boundary");
        emit(output, chunk, offset, index++);
        require(index == wanted, "document boundaries did not permit requested chunk count");
        std::printf("CHUNKS_PASS count=%u source_bytes=%llu\n", index,
                    (unsigned long long)consumed);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "CHUNK_FRONTEND_FATAL %s\n", e.what());
        return 1;
    }
}
