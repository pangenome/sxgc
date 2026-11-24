/* Input includes up to eight bytes of lookbehind. Only new bytes contribute
 * rows and triggers. n is capped by the caller before *any* corpus I/O. */
#include <stddef.h>
#include <stdint.h>

void scanplan_chunk(const unsigned char *p, size_t history, size_t end,
                    uint64_t base, uint64_t *cells, uint64_t *state) {
    uint64_t start = state[0], m = state[1], high = state[2];
    uint64_t longest = state[3], stride = state[4];
    for (size_t j = history; j < end; ++j) {
        if (j) ++cells[p[j-1]*257 + 1 + p[j]];  /* second symbol 0 is END */
        if (j < 7 || base+j-history < 8) continue; /* skip window i=0 */
        uint64_t h = UINT64_C(1469598103934665603);
        for (size_t a = j-7; a <= j; ++a)
            h = (h ^ p[a]) * UINT64_C(1099511628211);
        if (h % 31) continue;
        uint64_t i = base+j-history-7, owned = i-start;
        if (owned+8 > longest) longest = owned+8;
        if (owned > stride) stride = owned;
        start = i; ++m; high += i > UINT32_MAX;
    }
    state[0]=start; state[1]=m; state[2]=high; state[3]=longest; state[4]=stride;
}
