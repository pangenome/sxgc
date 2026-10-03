/* lz77z.c - greedy LZ77 factor count z for a large byte file (tight upper bound).
 *
 * v2 - PROGRESSIVE INSERTION. v1 pre-inserted every position into the hash
 * chains, so a chain walk at position i had to traverse all FUTURE occurrences
 * of the 8-gram before reaching valid candidates p < i (quadratic blowup on
 * repetitive text). v2 inserts positions lazily: before matching at i, all
 * positions in [0, i) have been inserted, so chains contain exactly the same
 * candidate set as v1 (all previous occurrences, newest first) and the walk
 * budget counts real candidates only. Same z, no wasted walks.
 *
 * Method:
 *   - Single-pass corpus read (THE LAW): the file is read exactly once,
 *     fully into RAM (~1.1 GB); all later access is in-memory.
 *   - Two chain tables: 4-gram (H4 = 2^26) and 8-gram (H8 = 2^28).
 *     Any longest match of length >= 8 shares its 8-gram with the source;
 *     matches of length 4..7 are reachable via the 4-gram chain; true longest
 *     matches of length 2..3 are rare in text and only add factors, so the
 *     reported z is a tight UPPER bound on exact greedy z.
 *   - Greedy parse: at each factor start walk the 8-gram chain (budget CHAIN
 *     candidates, newest first, lazy reject on T[p+best] != T[i+best]), then
 *     the 4-gram chain; longest match wins; overlapping copies allowed;
 *     factor = phrase if len >= MINMATCH else 1-byte literal.
 *   - Progress: stderr line every 64M consumed bytes.
 *
 * Validated: identical z to v1 on a 5 MB slice (618,254) and identical z
 * semantics; chain cap 128 costs only +0.67% vs uncapped on 5 MB.
 *
 * Usage: lz77z file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define HB4 26
#define HB8 28
#define H4  (1u << HB4)
#define H8  (1u << HB8)
#define CHAIN 128
#define MINMATCH 2

static uint32_t *h4h, *h8h, *p4, *p8;
static uint8_t *T;
static size_t n;

static inline uint32_t hk4(size_t i) {
    uint32_t x;
    memcpy(&x, T + i, 4);
    x *= 0x9E3779B1u;
    x ^= x >> 15;
    return x >> (32 - HB4);
}
static inline uint32_t hk8(size_t i) {
    uint64_t x;
    memcpy(&x, T + i, 8);
    x *= 0x9E3779B97F4A7C15ULL;
    return (uint32_t)(x >> (64 - HB8));
}
static double nowsec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

/* insert position j (must be called for j in increasing order) */
static inline void insert_pos(size_t j) {
    uint32_t a = hk4(j), b = hk8(j);
    p4[j] = h4h[a] ? h4h[a] - 1 : 0xFFFFFFFFu;
    p8[j] = h8h[b] ? h8h[b] - 1 : 0xFFFFFFFFu;
    h4h[a] = (uint32_t)(j + 1);
    h8h[b] = (uint32_t)(j + 1);
}

/* walk chain for the k-gram at i; all entries are < i by construction */
static size_t walk(uint32_t *hh, uint32_t *pp, uint32_t (*hk)(size_t),
                   size_t i, size_t *ext) {
    uint32_t p = hh[hk(i)] ? hh[hk(i)] - 1 : 0xFFFFFFFFu;
    size_t seen = 0, best = 0;
    while (p != 0xFFFFFFFFu && seen < CHAIN) {
        size_t pos = p;
        p = pp[pos];
        seen++;                     /* every entry is a valid candidate */
        if (i + best >= n || pos + best >= n) break;
        if (T[pos + best] != T[i + best]) continue; /* lazy reject */
        (*ext)++;
        size_t l = 0;
        while (i + l < n && T[pos + l] == T[i + l]) l++;
        if (l > best) best = l;
    }
    return best;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    long long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    n = (size_t)fsz;
    T = malloc(n);
    if (!T) { fprintf(stderr, "oom text\n"); return 1; }
    if (fread(T, 1, n, f) != n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f); /* THE LAW: single-pass corpus read, file closed */

    h4h = malloc((size_t)H4 * 4);
    h8h = malloc((size_t)H8 * 4);
    p4  = malloc(n * 4);
    p8  = malloc(n * 4);
    if (!h4h || !h8h || !p4 || !p8) { fprintf(stderr, "oom tables\n"); return 1; }
    memset(h4h, 0, (size_t)H4 * 4);
    memset(h8h, 0, (size_t)H8 * 4);

    double t0 = nowsec(), tlast = t0;
    size_t i = 0, ins = 0, z = 0, lits = 0, phr = 0, sumlen = 0, maxlen = 0, ext = 0;
    size_t nextlog = 64u << 20;
    while (i < n) {
        while (ins < i) { insert_pos(ins); ins++; }  /* chains gain exactly [0, i) */
        size_t best = 0;
        if (i + 8 <= n) {
            best = walk(h8h, p8, hk8, i, &ext);
            size_t b4 = walk(h4h, p4, hk4, i, &ext);
            if (b4 > best) best = b4;
        } else {
            break;                              /* tail handled below */
        }
        size_t len = (best >= MINMATCH) ? best : 1;
        if (best >= MINMATCH) { phr++; sumlen += len; if (len > maxlen) maxlen = len; }
        else lits++;
        z++;
        i += len;
        if (i >= nextlog) {
            double tn = nowsec();
            fprintf(stderr, "at %.0f MB: z=%zu, %.1f s since last report\n",
                    (double)i / 1048576.0, z, tn - tlast);
            tlast = tn;
            nextlog += 64u << 20;
        }
    }
    /* tail positions (i + 8 > n): literals */
    while (i < n) { lits++; z++; i++; }
    fprintf(stderr, "parse pass done in %.1f s (extensions=%zu)\n", nowsec() - t0, ext);

    printf("n            = %zu\n", n);
    printf("z (factors)  = %zu\n", z);
    printf("  literals   = %zu\n", lits);
    printf("  phrases    = %zu (avg len %.2f, max %zu)\n",
           phr, phr ? (double)sumlen / phr : 0.0, maxlen);
    printf("z/n          = %.6f\n", (double)z / (double)n);
    printf("n/z          = %.3f\n", (double)n / (double)z);
    return 0;
}
