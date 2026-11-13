/* gates/forward_only_fixture.c - a tiny corpus + its XSEP v1 document map.
 *
 * Writes:
 *   out.bin     the corpus, documents separated by 0x1E (the Pile framing)
 *   map/seps.bin  XSEP v1: 48 B header, u32 start deltas, u64 anchors every
 *                 STRIDE-th document (the layout src/docstarts.rs parses)
 *
 * usage: forward_only_fixture OUTDIR BYTES [DOCS]
 * Prints the corpus path and the doc count. No index code is linked: this is
 * the gate's own independent writer, so a wrong map cannot be hidden by a
 * reader that shares its arithmetic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define STRIDE 64

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void fput32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void fput64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s OUTDIR BYTES [DOCS]\n", argv[0]); return 2; }
    const char *dir = argv[1];
    uint64_t want = strtoull(argv[2], NULL, 10);
    uint64_t docs = argc > 3 ? strtoull(argv[3], NULL, 10) : 0;
    if (docs == 0) docs = want / 4096;              /* ~4 KiB documents */
    if (docs < 2) docs = 2;
    if (docs > 4000000) { fprintf(stderr, "too many docs\n"); return 2; }

    uint64_t doc_len = want / docs;
    if (doc_len < 64) { fprintf(stderr, "documents must be >= 64 bytes\n"); return 2; }

    char corpus[4096], mapdir[4096], seps[4096];
    snprintf(corpus, sizeof corpus, "%s/out.bin", dir);
    snprintf(mapdir, sizeof mapdir, "%s/map", dir);
    snprintf(seps, sizeof seps, "%s/seps.bin", mapdir);
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", mapdir);
    if (system(cmd) != 0) { fprintf(stderr, "mkdir failed\n"); return 1; }

    /* the corpus: a deterministic, aperiodic body (xorshift64) with one 0x1E
     * between documents. Aperiodicity matters: a periodic corpus has far fewer
     * runs and would not exercise the run directories. */
    FILE *c = fopen(corpus, "wb");
    if (!c) { perror(corpus); return 1; }
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint8_t *buf = malloc(doc_len);
    if (!buf) { fprintf(stderr, "oom\n"); return 1; }
    uint64_t wrote = 0;
    for (uint64_t d = 0; d < docs; d++) {
        for (uint64_t i = 0; i < doc_len; i++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            uint8_t b = (uint8_t)(s >> 24);
            /* never emit the separator inside a document: the map is the truth,
             * but keeping the framing law true lets consult arm its zero-locate
             * layers, which is the product shape. */
            /* the END format reserves byte 0x00 as the absent/sentinel symbol, so a
             * corpus that CONTAINS it cannot carry the order-preserving remap the
             * builder derives (the build refuses it); remap both it and the
             * separator out of the body. */
            if (b == 0x1e) b = 0x1f;
            if (b == 0x00) b = 0x01;
            buf[i] = b;
        }
        /* a few repeating words so seeds/mem have something recurring */
        memcpy(buf + (doc_len / 2), " the quick brown fox ", 21);
        if (fwrite(buf, 1, doc_len, c) != doc_len) { perror("write corpus"); return 1; }
        wrote += doc_len;
        if (d + 1 < docs) { fputc(0x1e, c); wrote++; }
    }
    fclose(c);
    free(buf);

    /* the map: doc 0 starts at 0, each following start = previous + doc_len + 1 */
    FILE *m = fopen(seps, "wb");
    if (!m) { perror(seps); return 1; }
    uint8_t h[48];
    memset(h, 0, sizeof h);
    memcpy(h, "XSEP", 4);
    put32(h + 4, 1);                 /* version */
    put32(h + 12, STRIDE);           /* anchor stride */
    put64(h + 16, docs);             /* doc_count */
    put64(h + 24, wrote);            /* corpus_bytes */
    uint64_t anchors = (docs + STRIDE - 1) / STRIDE;
    put64(h + 32, anchors);
    put64(h + 40, 0);                /* base: doc 0 starts at 0 */
    fwrite(h, 1, sizeof h, m);
    for (uint64_t d = 0; d < docs; d++) {
        uint32_t delta = (uint32_t)(d == 0 ? 0 : doc_len + 1);
        fput32(m, delta);
    }
    uint64_t pos = 0;
    for (uint64_t d = 0; d < docs; d++) {
        if (d % STRIDE == 0) fput64(m, pos);
        pos += doc_len + 1;
    }
    fclose(m);
    printf("FIXTURE corpus=%s map=%s bytes=%llu docs=%llu doc_len=%llu\n",
           corpus, mapdir, (unsigned long long)wrote, (unsigned long long)docs,
           (unsigned long long)doc_len);
    return 0;
}
