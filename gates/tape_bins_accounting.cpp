// Per-class demand accounting for a bins_sort run log (TAPE_BINS.md).
// Streams stage-0.log lines and produces:
//   - per-unit rollup (rows, sort_s, expand bytes, parse classes/rounds)
//   - the CASCADE LEDGER: for every census-routed class, the expansion's
//     paid estimate vs the parse's actual capture cost (rows x the
//     widened spans it walked), plus the counterfactual expansion cost
//     (rows x 2 x cmp_len at routing time)
//   - the top consumers by each measure
// Usage: tape_bins_accounting <stage-0.log>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

struct UnitRow {
    uint64_t unit = 0, rows = 0, sortS_x100 = 0, expandBytes = 0;
    uint64_t parseClasses = 0, parseRounds = 0, parseDictBytes = 0;
    uint64_t parsePhrases = 0, expandRounds = 0;
};
struct Cascade {
    uint64_t unit = 0, rows = 0, cmpLen = 0, clause = 0;   // clause: 0 width 1 depth 2 both
    uint64_t expandPaid = 0, parseCaptureCost = 0, rounds = 0, budgetB = 0;
};

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <stage-0.log>\n", argv[0]); return 2; }
    FILE* f = std::fopen(argv[1], "r");
    if (!f) { std::perror("open"); return 2; }
    static char line[1 << 16];
    std::unordered_map<uint64_t, UnitRow> units;
    std::vector<Cascade> cascades;
    uint64_t expandRoundsTot = 0, censusFires = 0, expandWantedTot = 0;
    uint64_t parseClassTot = 0, parseRoundsTot = 0, parseDictBytesTot = 0;
    // the widened span per round: B_r = 64 x 8^r (capped by the class's
    // final budget); spanBytes(B) = B*(k+w)*2 + 4096 with k=w=8
    auto spanOf = [](uint64_t B) { return B * 32 + 4096; };
    while (std::fgets(line, sizeof line, f)) {
        uint64_t unit = 0, rows = 0, v = 0;
        double sd = 0;
        if (std::sscanf(line, "SS_UNIT unit=%" SCNu64 " rows=%" SCNu64
                        " sort_s=%lf", &unit, &rows, &sd) == 3) {
            UnitRow& U = units[unit];
            U.unit = unit; U.rows = rows; U.sortS_x100 = uint64_t(sd * 100);
            const char* q = std::strstr(line, "expand_bytes=");
            if (q) U.expandBytes = std::strtoull(q + 13, nullptr, 10);
            const char* q2 = std::strstr(line, "expand_rounds=");
            if (q2) U.expandRounds = std::strtoull(q2 + 14, nullptr, 10);
            continue;
        }
        if (std::strncmp(line, "CENSUS unit=", 12) == 0) {
            ++censusFires;
            uint64_t cmpLen = 0, paid = 0, cap = 0;
            const char* q1 = std::strstr(line, "cmp_len="); if (q1) cmpLen = std::strtoull(q1 + 8, nullptr, 10);
            const char* q2 = std::strstr(line, "rows="); if (q2) rows = std::strtoull(q2 + 5, nullptr, 10);
            const char* q3 = std::strstr(line, "expand_paid_estimate="); if (q3) paid = std::strtoull(q3 + 21, nullptr, 10);
            const char* q4 = std::strstr(line, "parse_capture_cost="); if (q4) cap = std::strtoull(q4 + 19, nullptr, 10);
            const char* q5 = std::strstr(line, "clause=");
            uint64_t clause = 0;
            if (q5) {
                if (std::strncmp(q5 + 7, "width+depth", 11) == 0) clause = 2;
                else if (std::strncmp(q5 + 7, "depth", 5) == 0) clause = 1;
            }
            uint64_t unit2 = std::strtoull(line + 12, nullptr, 10);
            Cascade C;
            C.unit = unit2; C.rows = rows; C.cmpLen = cmpLen;
            C.clause = clause; C.expandPaid = paid;
            C.parseCaptureCost = cap;   // the one-shot model (rows x span_64)
            cascades.push_back(C);
            continue;
        }
        if (std::strncmp(line, "PARSE_CLASS unit=", 17) == 0) {
            ++parseClassTot;
            uint64_t unit2 = std::strtoull(line + 17, nullptr, 10);
            uint64_t rounds = 0, budgetB = 0, dictB = 0, phrases = 0, rows2 = 0;
            const char* q = std::strstr(line, "rounds="); if (q) rounds = std::strtoull(q + 7, nullptr, 10);
            const char* q2 = std::strstr(line, "budget="); if (q2) budgetB = std::strtoull(q2 + 7, nullptr, 10);
            const char* q3 = std::strstr(line, "dict_bytes="); if (q3) dictB = std::strtoull(q3 + 11, nullptr, 10);
            const char* q4 = std::strstr(line, "phrases="); if (q4) phrases = std::strtoull(q4 + 8, nullptr, 10);
            const char* q5 = std::strstr(line, "rows="); if (q5) rows2 = std::strtoull(q5 + 5, nullptr, 10);
            UnitRow& U = units[unit2];
            U.parseClasses += 1; U.parseRounds += rounds;
            U.parseDictBytes += dictB; U.parsePhrases += phrases;
            parseRoundsTot += rounds; parseDictBytesTot += dictB;
            // attribute to the latest cascade line of this unit, if any
            for (size_t i = cascades.size(); i-- > 0; ) {
                if (cascades[i].unit == unit2) {
                    Cascade& C = cascades[i];
                    C.rounds = rounds; C.budgetB = budgetB;
                    // the actual capture cost: rows x the sum of the
                    // widened spans it walked (B=64 x 8^r, r < rounds)
                    uint64_t cost = 0, B = 64;
                    for (uint64_t r = 0; r < rounds; ++r) {
                        cost += rows2 * spanOf(B);
                        if (B < budgetB) B *= 8;
                    }
                    C.parseCaptureCost = cost;
                    break;
                }
            }
            continue;
        }
        if (std::strncmp(line, "EXPAND unit=", 12) == 0) {
            ++expandRoundsTot;
            const char* q = std::strstr(line, "wanted_bytes=");
            if (q) expandWantedTot += std::strtoull(q + 13, nullptr, 10);
            uint64_t unit2 = std::strtoull(line + 12, nullptr, 10);
            units[unit2].expandRounds += 1;
            continue;
        }
    }
    std::fclose(f);

    std::vector<UnitRow> us;
    for (auto& kv : units) us.push_back(kv.second);
    std::sort(us.begin(), us.end(), [](const UnitRow& a, const UnitRow& b) {
        return a.unit < b.unit; });

    // per-unit rollup (top 40 by sort wall)
    std::vector<UnitRow> byWall = us;
    std::sort(byWall.begin(), byWall.end(), [](const UnitRow& a, const UnitRow& b) {
        return a.sortS_x100 > b.sortS_x100; });
    std::fprintf(stdout,
        "ACCOUNTING units=%zu census_fires=%llu expand_rounds=%llu"
        " expand_wanted_bytes=%llu parse_classes=%llu parse_rounds=%llu"
        " parse_dict_bytes=%llu\n",
        us.size(),
        (unsigned long long)censusFires,
        (unsigned long long)expandRoundsTot,
        (unsigned long long)expandWantedTot,
        (unsigned long long)parseClassTot,
        (unsigned long long)parseRoundsTot,
        (unsigned long long)parseDictBytesTot);
    std::fprintf(stdout, "TOP_UNITS_BY_SORT_WALL (unit rows sort_s expand_bytes parse_classes parse_rounds expand_rounds)\n");
    for (size_t i = 0; i < byWall.size() && i < 40; ++i) {
        const UnitRow& U = byWall[i];
        std::fprintf(stdout, "  unit=%llu rows=%llu sort_s=%.2f expand_bytes=%llu"
            " parse_classes=%llu parse_rounds=%llu expand_rounds=%llu\n",
            (unsigned long long)U.unit, (unsigned long long)U.rows,
            double(U.sortS_x100) / 100.0,
            (unsigned long long)U.expandBytes,
            (unsigned long long)U.parseClasses,
            (unsigned long long)U.parseRounds,
            (unsigned long long)U.expandRounds);
    }
    // the cascade ledger: the side-by-side the FAIL protocol requires
    uint64_t totPaid = 0, totCascade = 0, totCounter = 0;
    uint64_t depthFires = 0, widthFires = 0;
    for (const Cascade& C : cascades) {
        totPaid += C.expandPaid;
        totCascade += C.parseCaptureCost;
        totCounter += C.rows * 2 * C.cmpLen;
        if (C.clause == 1) ++depthFires;
        else if (C.clause == 2) ++widthFires;
    }
    std::fprintf(stdout,
        "CASCADE_LEDGER classes=%zu depth_only=%llu width_or_both=%llu"
        " expand_paid_estimate_sum=%llu parse_cascade_capture_sum=%llu"
        " counterfactual_expand_sum=%llu\n",
        cascades.size(), (unsigned long long)depthFires,
        (unsigned long long)widthFires,
        (unsigned long long)totPaid,
        (unsigned long long)totCascade,
        (unsigned long long)totCounter);
    std::vector<Cascade> byCost = cascades;
    std::sort(byCost.begin(), byCost.end(), [](const Cascade& a, const Cascade& b) {
        return a.parseCaptureCost > b.parseCaptureCost; });
    std::fprintf(stdout,
        "TOP_CASCADE_COSTS (unit rows cmp_len clause expand_paid parse_cascade counterfactual rounds final_B)\n");
    for (size_t i = 0; i < byCost.size() && i < 40; ++i) {
        const Cascade& C = byCost[i];
        std::fprintf(stdout,
            "  unit=%llu rows=%llu cmp_len=%llu clause=%s paid=%llu"
            " cascade=%llu counterfactual=%llu rounds=%llu B=%llu\n",
            (unsigned long long)C.unit, (unsigned long long)C.rows,
            (unsigned long long)C.cmpLen,
            C.clause == 1 ? "depth" : (C.clause == 2 ? "width+depth" : "width"),
            (unsigned long long)C.expandPaid,
            (unsigned long long)C.parseCaptureCost,
            (unsigned long long)(C.rows * 2 * C.cmpLen),
            (unsigned long long)C.rounds, (unsigned long long)C.budgetB);
    }
    return 0;
}
