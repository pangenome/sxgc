#ifndef SXI_SA_WORKSPACE_HPP
#define SXI_SA_WORKSPACE_HPP
#include <cstdint>
#include <climits>
#include <stdexcept>
// The pinned gSACA-K has 32/64-bit ABIs, not a five-byte ABI. Both are
// linked with distinct symbols; signed internal sentinels bound the 32-bit path.
extern "C" {
int sxi32_gsacak(unsigned char*, uint32_t*, int32_t*, uint32_t*, uint32_t);
int sxi32_gsacak_int(uint32_t*, uint32_t*, int32_t*, uint32_t*, uint32_t, uint32_t);
}
inline int sxi_sa32(uint8_t* s, uint32_t* sa, uint32_t n, uint64_t) {
    return sxi32_gsacak(s, sa, nullptr, nullptr, n);
}
inline int sxi_sa32(char* s, uint32_t* sa, uint32_t n, uint64_t k) {
    return sxi_sa32(reinterpret_cast<uint8_t*>(s), sa, n, k);
}
inline int sxi_sa32(uint32_t* s, uint32_t* sa, uint32_t n, uint64_t k) {
    if (k > INT32_MAX) throw std::runtime_error("32-bit gSACA-K alphabet overflow");
    return sxi32_gsacak_int(s, sa, nullptr, nullptr, n, static_cast<uint32_t>(k));
}
#endif
