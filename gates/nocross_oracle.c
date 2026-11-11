/* gates/nocross_oracle.c — THE LAW'S brute-force oracle for X3/X4, no index,
 * no Rust, no code shared with the query paths.
 *
 * ONE pass over the corpus classifies every occurrence of every sampled
 * context under the no-cross law:
 *
 *   the occurrence at q of context C (len L) is
 *     - a CROSSING match (counted nowhere) iff a document start lies inside
 *       (q, q+L-1]  (rank(q) != rank(q+L-1));
 *     - on the END branch iff q+L == text_len (the structural END) or q+L is
 *       a document start ("this context ends a document");
 *     - otherwise on the content branch corpus[q+L].
 *
 * Reads the document map as an XSEP v1 seps.bin (the pile-map sidecar), the
 * corpus, and a query file: one context per line, "off ctxlen" (the context
 * is corpus[off-ctxlen .. off)). Prints one line per query:
 *
 *   ORACLE off=<o> ctxlen=<l> width=<w> eof=<e> pairs=<b1:x,b2:y,...>
 *
 * width counts the NON-CROSSING occurrences (content + END), pairs are sorted
 * count-desc, byte-asc to match the consult row layout.
 *
 *   usage: nocross_oracle SEPS_BIN CORPUS TEXT_LEN QUERIES_FILE
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *text;
static uint64_t *starts;
static uint64_t nstarts;

static uint64_t base_of(uint64_t p) {
    /* largest start <= p (starts[0] == 0) */
    uint64_t lo = 0, hi = nstarts;
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (starts[mid] <= p) lo = mid; else hi = mid;
    }
    return starts[lo];
}

static int is_start(uint64_t p) { return base_of(p) == p; }

typedef struct { uint64_t off; uint32_t len; const uint8_t *ctx; uint64_t count; uint64_t eof; uint64_t cross; uint64_t byte_count[256]; } Query;

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: nocross_oracle SEPS_BIN CORPUS TEXT_LEN QUERIES_FILE\n");
        return 2;
    }
    /* ---- the document map (XSEP v1): 48 B header, u32 deltas, anchors */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    uint64_t map_len = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *map = malloc(map_len);
    if (!map || fread(map, 1, map_len, f) != map_len) { fprintf(stderr, "short read %s\n", argv[1]); return 2; }
    fclose(f);
    if (map_len < 48 || memcmp(map, "XSEP", 4)) { fprintf(stderr, "%s is not XSEP\n", argv[1]); return 2; }
    uint64_t docs = 0;
    memcpy(&docs, map + 16, 8);
    starts = malloc((docs + 1) * sizeof(uint64_t));
    if (!starts) { fprintf(stderr, "oom\n"); return 2; }
    uint64_t at = 0;
    nstarts = 0;
    starts[nstarts++] = 0;
    for (uint64_t i = 1; i < docs; i++) {
        uint32_t d;
        memcpy(&d, map + 48 + 4 * i, 4);
        at += d;
        starts[nstarts++] = at;
    }

    /* ---- the corpus, capped at TEXT_LEN (the container's text) */
    uint64_t text_len = strtoull(argv[3], 0, 10);
    f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 2; }
    fseek(f, 0, SEEK_END);
    uint64_t flen = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < text_len) { fprintf(stderr, "corpus shorter than TEXT_LEN\n"); return 2; }
    text = malloc(text_len);
    if (!text || fread(text, 1, text_len, f) != text_len) { fprintf(stderr, "short read %s\n", argv[2]); return 2; }
    fclose(f);

    /* ---- the queries */
    f = fopen(argv[4], "r");
    if (!f) { perror(argv[4]); return 2; }
    uint64_t cap = 1024;
    Query *qs = malloc(cap * sizeof(Query));
    uint64_t nq = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned long long off; unsigned long long cl;
        if (sscanf(line, "%llu %llu", &off, &cl) != 2) { fprintf(stderr, "bad query line: %s", line); return 2; }
        if (cl == 0 || off < cl || off > text_len) { fprintf(stderr, "bad query off=%llu ctxlen=%llu\n", off, cl); return 2; }
        if (nq == cap) { cap *= 2; qs = realloc(qs, cap * sizeof(Query)); }
        qs[nq].off = off;
        qs[nq].len = (uint32_t)cl;
        qs[nq].ctx = text + (off - cl);
        qs[nq].count = 0; qs[nq].eof = 0; qs[nq].cross = 0;
        memset(qs[nq].byte_count, 0, sizeof qs[nq].byte_count);
        nq++;
    }
    fclose(f);
    if (nq == 0) { fprintf(stderr, "no queries\n"); return 2; }

    /* ---- ONE pass over the corpus: candidate gate on the first 4 bytes */
    /* build a tiny open-addressing table over the first-4-byte keys */
    uint64_t tsize = 1;
    while (tsize < nq * 4) tsize <<= 1;
    uint64_t *table = malloc(tsize * sizeof(uint64_t));
    memset(table, 0xFF, tsize * sizeof(uint64_t));
    uint64_t *chain = malloc((nq + 1) * sizeof(uint64_t));
    for (uint64_t i = 0; i < nq; i++) {
        uint32_t key;
        uint32_t klen = qs[i].len < 4 ? qs[i].len : 4;
        memcpy(&key, qs[i].ctx, klen);
        if (klen < 4) { /* short contexts: fall back to full scan below */ }
        uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ULL & (tsize - 1);
        while (table[h] != 0xFFFFFFFFFFFFFFFFULL) h = (h + 1) & (tsize - 1);
        table[h] = i;
        (void)chain;
    }
    for (uint64_t p = 0; p + 4 <= text_len; p++) {
        uint32_t key;
        memcpy(&key, text + p, 4);
        uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ULL & (tsize - 1);
        while (table[h] != 0xFFFFFFFFFFFFFFFFULL) {
            uint64_t qi = table[h];
            Query *q = &qs[qi];
            if (q->len >= 4 && p + q->len <= text_len && !memcmp(text + p, q->ctx, q->len)) {
                uint64_t at2 = p + q->len;
                if (at2 == text_len) { q->eof++; continue; }
                if (base_of(at2 - 1) > p) { q->cross++; continue; }
                if (is_start(at2)) { q->eof++; continue; }
                q->byte_count[text[at2]]++;
            }
            h = (h + 1) & (tsize - 1);
        }
    }
    /* short contexts (< 4 bytes): full scans */
    for (uint64_t qi = 0; qi < nq; qi++) {
        Query *q = &qs[qi];
        if (q->len >= 4) continue;
        for (uint64_t p = 0; p + q->len <= text_len; p++) {
            if (memcmp(text + p, q->ctx, q->len)) continue;
            uint64_t at2 = p + q->len;
            if (at2 == text_len) { q->eof++; continue; }
            if (base_of(at2 - 1) > p) { q->cross++; continue; }
            if (is_start(at2)) { q->eof++; continue; }
            q->byte_count[text[at2]]++;
        }
    }

    for (uint64_t qi = 0; qi < nq; qi++) {
        Query *q = &qs[qi];
        uint64_t sum = 0;
        int order[256];
        for (int b = 0; b < 256; b++) order[b] = b;
        for (int b = 0; b < 256; b++) sum += q->byte_count[b];
        /* count-desc, byte-asc: simple selection over 256 entries */
        printf("ORACLE off=%llu ctxlen=%u width=%llu eof=%llu crossing=%llu pairs=",
               (unsigned long long)q->off, q->len,
               (unsigned long long)(sum + q->eof), (unsigned long long)q->eof,
               (unsigned long long)q->cross);
        int printed = 0;
        for (;;) {
            int best = -1;
            for (int b = 0; b < 256; b++) {
                if (q->byte_count[b] == 0) continue;
                if (best < 0 || q->byte_count[b] > q->byte_count[best] ||
                    (q->byte_count[b] == q->byte_count[best] && b < best)) {
                    /* tie byte-asc handled by strict >: recheck */
                    if (best < 0 || q->byte_count[b] > q->byte_count[best]) best = b;
                    else if (q->byte_count[b] == q->byte_count[best] && b < best) best = b;
                }
            }
            if (best < 0) break;
            printf("%s%d:%llu", printed ? "," : "", best, (unsigned long long)q->byte_count[best]);
            q->byte_count[best] = 0;
            printed = 1;
        }
        printf("\n");
    }
    return 0;
}
