#define main cross_lcp_merge_main
#include "../../bit6/cross_lcp_merge.cpp"
#undef main
int main() {
    const std::string prefix = "/tmp/sol5i-flow-" + std::to_string(getpid());
    const std::string marker = prefix + ".runcols.consumed";
    auto consumed = [&](U band) {
        const std::string tmp = marker + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w"); require(f != nullptr, "flow oracle marker");
        fprintf(f, "%llu\n", (unsigned long long)band);
        require(fclose(f) == 0 && rename(tmp.c_str(), marker.c_str()) == 0, "flow oracle publish");
    };
    require(v2_consumed_band(prefix) == -1, "flow oracle initial C=-1");
    v2_flow_wait(prefix, 0, 1);
    consumed(1);
    v2_flow_wait(prefix, 2, U(INT64_MAX)); // signed C+W overflowed before the fix
    consumed(U(INT64_MAX));
    v2_flow_wait(prefix, 10, U(INT64_MAX));
    require(unlink(marker.c_str()) == 0, "flow oracle reset");
    double start = now_sec();
    std::thread advance([&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); consumed(0); });
    v2_flow_wait(prefix, 2, 2); // blocked until C=0 gives band 2 credit
    advance.join();
    require(now_sec() - start >= .025 && now_sec() - start < 2, "flow oracle bounded wait");
    require(unlink(marker.c_str()) == 0, "flow oracle cleanup");
    std::puts("FLOW_ORACLE_PASS initial_minus_one=1 oversized_positive_window=1 unsigned_credit=1 blocked_until_consumed=1");
}
