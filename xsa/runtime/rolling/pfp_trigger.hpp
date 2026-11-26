#pragma once
#include <cstdint>

// This predicate defines a set of complete words, not a choice among their
// occurrences. Never suppress a hit because of a previous hit or an I/O seam.
namespace xsa_pfp {
inline constexpr unsigned width = 8;
// At an integer level the alphabet is whole unsigned IDs: advance by one
// symbol, xor one numeric ID (not a byte of its little-endian encoding).
template<class Symbol> inline bool trigger(const Symbol* p) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned j = 0; j < width; ++j) h = (h ^ p[j]) * 1099511628211ULL;
    return h % 31 == 0;
}
}
