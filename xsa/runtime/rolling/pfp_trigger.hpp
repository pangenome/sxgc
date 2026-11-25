#pragma once
#include <cstdint>

// This predicate defines a set of complete words, not a choice among their
// occurrences. Never suppress a hit because of a previous hit or an I/O seam.
namespace xsa_pfp {
inline constexpr unsigned width = 8;
inline bool trigger(const std::uint8_t* p) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned j = 0; j < width; ++j) h = (h ^ p[j]) * 1099511628211ULL;
    return h % 31 == 0;
}
}
