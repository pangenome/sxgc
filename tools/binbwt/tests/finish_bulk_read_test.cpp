// Build with tools/build_slim_dump.sh OUTPUT tools/binbwt/tests/finish_bulk_read_test.cpp.
// Independent byte oracle, virtual-END edges, and a residency regression gate.
#define main finish_program_main
#include "../../../bit6/xsa_finish.cpp"
#undef main

int main() {
    char path[] = "/tmp/finish-bulk-read-XXXXXX";
    int fd = ::mkstemp(path);
    pf_check(fd >= 0, "create bulk-read fixture");
    constexpr uint64_t bytes = 128ull << 20;
    std::vector<uint8_t> block(1u << 20);
    auto raw = [](uint64_t i) { return uint8_t(i * 73 + i / 257); };
    for (uint64_t off = 0; off < bytes; off += block.size()) {
        for (size_t i = 0; i < block.size(); ++i) block[i] = raw(off + i);
        pf_write_full(fd, block.data(), block.size(), "bulk-read fixture");
    }
    ::close(fd);
    uint8_t map[256];
    for (unsigned i = 0; i < 256; ++i) map[i] = uint8_t(255 - i);
    CorpusText text(path, map);
    ::unlink(path);
    const auto before = FinishRss::read();
    for (uint64_t off = 0; off < bytes; off += block.size()) {
        text.read_block(off, block.data(), block.size());
        for (size_t i = 0; i < block.size(); ++i)
            pf_check(block[i] == uint8_t(255 - raw(off + i)), "bulk remap byte mismatch");
    }
    for (uint64_t off : {uint64_t(0), uint64_t(4095), bytes - 1, bytes, bytes + 1}) {
        const uint64_t len = std::min<uint64_t>(33, bytes + 1 - off);
        std::array<uint8_t, 35> edge{}; edge.fill(0xa5);
        text.read_block(off, edge.data() + 1, len);
        pf_check(edge[0] == 0xa5 && edge[len + 1] == 0xa5, "bulk write extent");
        for (uint64_t i = 0; i < len; ++i)
            pf_check(edge[i + 1] == (off + i == bytes ? SENTINEL_SYMBOL : uint8_t(255 - raw(off + i))),
                     "bulk edge or virtual END mismatch");
    }
    const auto after = FinishRss::read();
    pf_check(after.rss < before.rss + (16u << 10), "bulk scan installed corpus-sized resident mappings");
    std::fprintf(stderr, "FINISH_BULK_READ_PASS bytes=%llu rss_before_kb=%llu rss_after_kb=%llu\n",
                 (unsigned long long)bytes, (unsigned long long)before.rss, (unsigned long long)after.rss);
}
