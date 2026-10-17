// Isolate the inherited sink's per-row cost from bin-bwt construction: run the
// exact bin_order with (a) a no-op emitter and (b) the real SequentialV2 sink.
#define main retired_merge_main
#include "../../bit6/cross_lcp_merge.cpp"
#undef main
int main(int argc, char** argv) {
    require(argc == 5, "rowcost CORPUS REMAP MODE(noop|sink) PREFIX");
    const std::string path = argv[1], mapPath = argv[2], mode = argv[3], prefix = argv[4];
    TextSeg segment; segment.path = path; segment.len = fs::file_size(path);
    std::ifstream map(mapPath, std::ios::binary);
    map.read((char*)segment.remap.data(), 256);
    require(bool(map), "remap read");
    CorpusText text; text.open({segment}, true);
    BinWork total;
    const double t0 = now_sec();
    if (mode == "sink") {
        SequentialV2 out(prefix, text, text.n, text.n);
        out.beginShard(0, 0, text.n);
        bin_order(text, true, 96, 24, 16, U(1) << 40, 1, false, 0, total,
                  [&](U row, U p) { out.row(row, p); });
        out.endShard();
    } else {
        U checksum = 0;
        bin_order(text, true, 96, 24, 16, U(1) << 40, 1, false, 0, total,
                  [&](U row, U p) { checksum += p ^ row; });
        require(checksum != 0, "noop emitter checksum");
    }
    const double wall = now_sec() - t0;
    std::printf("ROWCOST mode=%s n=%llu wall_s=%.3f emit_s=%.3f build_s=%.3f "
                "sweep_s=%.3f ns_per_row_emit=%.3f\n", mode.c_str(),
                (unsigned long long)text.n, wall, total.tEmit, total.tBuild,
                total.tSweep, 1e9 * wall / double(text.n));
    return 0;
}
