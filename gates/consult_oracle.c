// gates/consult_oracle.c — the BRUTE-FORCE oracle for `xsa consult`.
//
// For each context in the framed contexts stream (u32 LE length + bytes, the
// same frames `xsa branches --serve` reads), scan the WHOLE corpus text and
// count the byte that FOLLOWS every occurrence, plus the occurrences followed
// by the end of the text. That is exactly the row `xsa consult` must emit, so
// this is an independent answer computed with no index, no FM search and no
// shared code with the thing under test.
//
// No text is materialised: the corpus is mmapped read-only and scanned in place.
//
//   usage: consult_oracle CORPUS CONTEXTS [SCAN_BYTES]
//          SCAN_BYTES restricts the scan to a PREFIX of the file, which is what a
//          container built over a prefix of a bigger corpus declares as its text:
//          `xsa consult` bounds positions and the EOF test by the container's
//          n-1, so the oracle must scan exactly the same bytes.
//   out:   idx<TAB>ctxlen<TAB>width<TAB>eof<TAB>b1:c1,b2:c2,...   (count desc, byte asc)
//
// width is the number of occurrences of the context, eof the number of them at
// the end of the text, and the pairs are the next-byte histogram restricted to
// nonzero counts - the same ordering the row layout uses.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const unsigned char *g_corpus;
static uint64_t g_len;

static uint64_t *g_counts; /* [job][256] */
static uint64_t *g_width;
static uint64_t *g_eof;

struct ctxs {
    uint64_t n;
    const unsigned char **ctx;
    uint32_t *len;
};

static struct ctxs g_ctxs;
static pthread_mutex_t g_next = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_next_i;

static void *run_one(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_next);
        uint64_t j = g_next_i++;
        pthread_mutex_unlock(&g_next);
        if (j >= g_ctxs.n) return 0;
        const unsigned char *ctx = g_ctxs.ctx[j];
        uint32_t len = g_ctxs.len[j];
        uint64_t *c = &g_counts[j * 256];
        memset(c, 0, 256 * sizeof(uint64_t));
        uint64_t width = 0, eof = 0;
        const unsigned char first = ctx[0];
        const unsigned char *p = g_corpus;
        const unsigned char *end = g_corpus + g_len;
        if (len == 1) {
            /* every occurrence is a hit: one linear histogram pass */
            for (const unsigned char *q = p; q < end; ++q) {
                if (*q == first) {
                    width++;
                    if ((uint64_t)(q - p) + 1 == g_len) eof++;
                    else c[*(q + 1)]++;
                }
            }
        } else {
            while (p < end) {
                const unsigned char *hit = memchr(p, first, (size_t)(end - p));
                if (!hit) break;
                /* a match must fit inside the text */
                if ((uint64_t)(end - hit) >= len && memcmp(hit, ctx, len) == 0) {
                    width++;
                    if ((uint64_t)(hit - g_corpus) + len == g_len) eof++;
                    else c[hit[len]]++;
                }
                p = hit + 1;
            }
        }
        g_width[j] = width;
        g_eof[j] = eof;
    }
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s CORPUS CONTEXTS [SCAN_BYTES]\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno)); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { fprintf(stderr, "stat: %s\n", strerror(errno)); return 1; }
    g_len = (uint64_t)st.st_size;
    if (argc == 4) {
        char *endp = 0;
        unsigned long long want = strtoull(argv[3], &endp, 10);
        if (!endp || *endp || want == 0 || want > g_len) {
            fprintf(stderr, "bad SCAN_BYTES %s (must be 1..%llu)\n", argv[3],
                    (unsigned long long)g_len);
            return 1;
        }
        g_len = (uint64_t)want;
    }
    void *m = mmap(0, (size_t)g_len, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { fprintf(stderr, "mmap: %s\n", strerror(errno)); return 1; }
    g_corpus = (const unsigned char *)m;

    FILE *cf = fopen(argv[2], "rb");
    if (!cf) { fprintf(stderr, "open %s: %s\n", argv[2], strerror(errno)); return 1; }
    size_t cap = 64, n = 0;
    const unsigned char **ctx = malloc(cap * sizeof(*ctx));
    uint32_t *len = malloc(cap * sizeof(*len));
    unsigned char *arena = 0;
    size_t arena_len = 0, arena_cap = 0;
    for (;;) {
        uint32_t l = 0;
        if (fread(&l, 1, 4, cf) != 4) break;
        if (l == 0 || l > (1u << 16)) { fprintf(stderr, "bad context length %u\n", l); return 1; }
        if (arena_len + l > arena_cap) {
            arena_cap = (arena_cap ? arena_cap * 2 : (1 << 20)) + l;
            arena = realloc(arena, arena_cap);
        }
        if (fread(arena + arena_len, 1, l, cf) != l) { fprintf(stderr, "truncated contexts\n"); return 1; }
        if (n == cap) { cap *= 2; ctx = realloc(ctx, cap * sizeof(*ctx)); len = realloc(len, cap * sizeof(*len)); }
        ctx[n] = arena + arena_len;
        len[n] = l;
        arena_len += l;
        n++;
    }
    if (n == 0) { fprintf(stderr, "no contexts\n"); return 1; }
    g_ctxs.n = n;
    g_ctxs.ctx = ctx;
    g_ctxs.len = len;
    g_counts = calloc(n * 256, sizeof(uint64_t));
    g_width = calloc(n, sizeof(uint64_t));
    g_eof = calloc(n, sizeof(uint64_t));

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int threads = ncpu > 0 && ncpu < (long)n ? (int)ncpu : (int)n;
    if (threads < 1) threads = 1;
    pthread_t *tid = malloc(sizeof(pthread_t) * threads);
    for (int i = 0; i < threads; ++i) pthread_create(&tid[i], 0, run_one, 0);
    for (int i = 0; i < threads; ++i) pthread_join(tid[i], 0);

    for (uint64_t j = 0; j < n; ++j) {
        const uint64_t *c = &g_counts[j * 256];
        printf("%llu\t%u\t%llu\t%llu\t", (unsigned long long)j, len[j],
               (unsigned long long)g_width[j], (unsigned long long)g_eof[j]);
        /* count desc, byte asc: the row layout's ordering */
        int order[256];
        uint64_t nn = 0;
        for (int b = 0; b < 256; ++b)
            if (c[b]) order[nn++] = b;
        for (uint64_t a = 0; a < nn; ++a)
            for (uint64_t b = a + 1; b < nn; ++b) {
                int x = order[a], y = order[b];
                if (c[y] > c[x] || (c[y] == c[x] && y < x)) { order[a] = y; order[b] = x; }
            }
        for (uint64_t a = 0; a < nn; ++a)
            printf("%s%d:%llu", a ? "," : "", order[a], (unsigned long long)c[order[a]]);
        printf("\n");
    }
    return 0;
}
