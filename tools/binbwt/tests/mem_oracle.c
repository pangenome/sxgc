/*
 * mem_oracle — INDEPENDENT exact-MEM oracle for `xsa mem` (gate X1).
 *
 * Reads the corpus and the queries and computes the true MEM set from corpus
 * bytes alone: no container, no BWT, no rank column, no code shared with the
 * index (all it shares is the definition).
 *
 * Method. A match query[a..b] (inclusive) occurs iff some corpus position p has
 * lcp(corpus[p:], query[a:]) >= b-a+1. ONE pass over the corpus therefore
 * answers every (start, length) question at once:
 *
 *   - prefilter: an 8-byte anchor. Any match of at least 8 bytes contains its
 *     own first 8 bytes at the alignment, so a position p can only start a match
 *     for start a if corpus[p..p+8) == query[a..a+8). All query anchors go into
 *     a 2^26-bit filter keyed by a multiplicative hash of that word; a filter hit
 *     is verified against the actual query anchors before any extension work, so
 *     the filter can only cost time, never change an answer.
 *   - for a verified hit, extend rightward to the mismatch and record, per start
 *     a: the maximum match length any corpus position gives, and how many
 *     positions attain that maximum.
 *
 * Then, straight from the definition:
 *   lcp[a]  = max over p of lcp(corpus[p:], query[a:])      (0 if no p)
 *   MEM(a)  exists iff lcp[a] >= minlen and (a == 0 or lcp[a-1] <= lcp[a])
 *           (left-maximal: query[a-1 .. a+lcp[a]-1] does not occur)
 *           and is right-maximal by construction: one more byte at the right end
 *           is empty because lcp[a] is the maximum at a
 *           (unless a+lcp[a] == qlen, the boundary case of the definition).
 *   occurrences(a) = #{p : lcp(corpus[p:], query[a:]) >= lcp[a]}, which is
 *           exactly the count of positions attaining the maximum.
 *
 * Overlapping occurrences count separately, as the index counts them.
 *
 * Usage: mem_oracle CORPUS QUERIES MINLEN
 *   QUERIES is a file of repeated (u32 len)(bytes).
 * Output: one "Q <qid> <qlen>" line per query, then "A <qid> <a> <lcp> <occ>"
 * for every start a with lcp > 0 (MEM filtering is left to the gate, so the gate
 * can check maximality itself rather than trusting this program's selection).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define HASHBITS 26
#define HASHSIZE (1u << HASHBITS)

static void die(const char *m) { fprintf(stderr, "mem_oracle: %s\n", m); exit(1); }

static inline uint64_t load8(const unsigned char *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static inline uint32_t hash8(uint64_t v) {
    uint64_t h = v * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> (64 - HASHBITS));
}

/* One query's per-start record. */
struct Clip {
    uint32_t *lcp;   /* max match length at this start */
    uint32_t *occ;   /* positions attaining that max */
};

int main(int argc, char **argv) {
    if (argc != 4) die("usage: mem_oracle CORPUS QUERIES MINLEN");
    const char *corpus_path = argv[1];
    const char *query_path = argv[2];
    int minlen = atoi(argv[3]);
    if (minlen < 8) die("minlen must be >= 8 (the anchor width)");

    /* ---- queries ---- */
    int qf = open(query_path, O_RDONLY);
    if (qf < 0) die("open queries");
    struct stat qs;
    if (fstat(qf, &qs)) die("stat queries");
    unsigned char *qb = mmap(NULL, (size_t)qs.st_size, PROT_READ, MAP_PRIVATE, qf, 0);
    if (qb == MAP_FAILED) die("mmap queries");
    size_t qo = 0, qcap = 64, nq = 0;
    unsigned char **q = malloc(qcap * sizeof(*q));
    uint32_t *ql = malloc(qcap * sizeof(*ql));
    while (qo + 4 <= (size_t)qs.st_size) {
        uint32_t len;
        memcpy(&len, qb + qo, 4);
        qo += 4;
        if (qo + len > (size_t)qs.st_size) die("truncated query file");
        if (nq == qcap) {
            qcap *= 2;
            q = realloc(q, qcap * sizeof(*q));
            ql = realloc(ql, qcap * sizeof(*ql));
        }
        q[nq] = qb + qo;
        ql[nq] = len;
        qo += len;
        nq++;
    }
    if (nq == 0) die("no queries");

    /* ---- corpus ---- */
    int cf = open(corpus_path, O_RDONLY);
    if (cf < 0) die("open corpus");
    struct stat cs;
    if (fstat(cf, &cs)) die("stat corpus");
    size_t n = (size_t)cs.st_size;
    if (n < 8) die("corpus shorter than the anchor");
    unsigned char *c = mmap(NULL, n, PROT_READ, MAP_PRIVATE, cf, 0);
    if (c == MAP_FAILED) die("mmap corpus");
    madvise(c, n, MADV_SEQUENTIAL);

    struct Clip *clip = calloc(nq, sizeof(*clip));
    for (size_t i = 0; i < nq; i++) {
        clip[i].lcp = calloc((size_t)ql[i] + 1, sizeof(uint32_t));
        clip[i].occ = calloc((size_t)ql[i] + 1, sizeof(uint32_t));
        if (!clip[i].lcp || !clip[i].occ) die("alloc per-start records");
    }

    /* ---- anchor filter over every query start ---- */
    uint64_t *bits = calloc(HASHSIZE / 64, sizeof(uint64_t));
    if (!bits) die("alloc filter");
    size_t anchors = 0;
    for (size_t i = 0; i < nq; i++)
        for (uint32_t a = 0; a + 8 <= ql[i]; a++) {
            uint32_t h = hash8(load8(q[i] + a));
            bits[h >> 6] |= 1ull << (h & 63);
            anchors++;
        }

    /* ---- the single pass ---- */
    size_t hits = 0, verified = 0;
    for (size_t p = 0; p + 8 <= n; p++) {
        uint32_t h = hash8(load8(c + p));
        if (!(bits[h >> 6] & (1ull << (h & 63)))) continue;
        hits++;
        uint64_t w = load8(c + p);
        for (size_t i = 0; i < nq; i++) {
            if (ql[i] < (uint32_t)minlen) continue;
            for (uint32_t a = 0; a + 8 <= ql[i]; a++) {
                if (load8(q[i] + a) != w) continue;
                verified++;
                uint32_t len = 8;
                while (a + len < ql[i] && p + len < n && q[i][a + len] == c[p + len]) len++;
                if (len > clip[i].lcp[a]) {
                    clip[i].lcp[a] = len;
                    clip[i].occ[a] = 1;
                } else if (len == clip[i].lcp[a]) {
                    clip[i].occ[a]++;
                }
            }
        }
    }

    for (size_t i = 0; i < nq; i++) {
        printf("Q %zu %u\n", i, ql[i]);
        for (uint32_t a = 0; a < ql[i]; a++)
            if (clip[i].lcp[a] > 0)
                printf("A %zu %u %u %u\n", i, a, clip[i].lcp[a], clip[i].occ[a]);
    }
    fprintf(stderr,
            "mem_oracle: corpus_bytes=%zu queries=%zu anchors=%zu filter_hits=%zu "
            "verified_alignments=%zu minlen=%d\n",
            n, nq, anchors, hits, verified, minlen);
    return 0;
}
