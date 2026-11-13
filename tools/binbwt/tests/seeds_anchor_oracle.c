// Independent occurrence oracle for the seeds ANCHOR gate: one mmap pass per
// pattern, glibc memmem, OVERLAPPING occurrences (that is what a BWT interval
// width means), printing both the count and every occurrence START so the gate
// can check that each reported corpus_offset really is an occurrence.
//
//   seeds_anchor_oracle CORPUS PATTERNS [MAX_STARTS]
//
// PATTERNS is a sequence of u32 little-endian length + that many bytes (the same
// framing `xsa mem --serve` uses). One line per pattern, in input order:
//
//   <count> <printed> <start0> <start1> ...
//
// No index code, no shared implementation with xsa (seeds_oracle.c counts; this
// one also localises, because the anchor table's corpus_offset is a claim about
// WHICH occurrence, not only how many).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *m) { perror(m); exit(1); }

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s CORPUS PATTERNS [MAX_STARTS]\n", argv[0]);
        return 2;
    }
    unsigned long long max_starts = 200000;
    if (argc == 4) max_starts = strtoull(argv[3], 0, 10);
    int fc = open(argv[1], O_RDONLY);
    if (fc < 0) fail("corpus open");
    struct stat sc;
    if (fstat(fc, &sc)) fail("corpus stat");
    const unsigned char *c = mmap(0, (size_t)sc.st_size, PROT_READ, MAP_PRIVATE, fc, 0);
    if (c == MAP_FAILED) fail("corpus mmap");
    const size_t n = (size_t)sc.st_size;

    FILE *p = fopen(argv[2], "rb");
    if (!p) fail("patterns open");
    for (;;) {
        uint32_t len = 0;
        size_t got = fread(&len, 1, 4, p);
        if (got == 0) break;
        if (got != 4) { fprintf(stderr, "truncated length header\n"); return 1; }
        unsigned char *pat = malloc(len ? len : 1);
        if (!pat) fail("malloc");
        if (len && fread(pat, 1, len, p) != len) { fprintf(stderr, "truncated pattern\n"); return 1; }
        unsigned long long count = 0, printed = 0;
        // count first, then the starts: buffer the starts of one pattern only,
        // which is bounded by MAX_STARTS and never by the corpus.
        unsigned long long *starts = malloc(sizeof(*starts) * (size_t)(max_starts ? max_starts : 1));
        if (!starts) fail("malloc starts");
        if (len == 0) {
            // every boundary, as the index reports
            count = (unsigned long long)n + 1;
            for (unsigned long long i = 0; i <= n && printed < max_starts; i++, printed++) starts[printed] = i;
        } else if (n >= len) {
            const unsigned char *h = c, *end = c + (n - len) + 1;
            while (h < end) {
                const unsigned char *m = memmem(h, (size_t)(end - h), pat, len);
                if (!m) break;
                if (printed < max_starts) starts[printed++] = (unsigned long long)(m - c);
                count++;
                h = m + 1; // overlap: a BWT interval counts overlapping occurrences
            }
        }
        printf("%llu %llu", count, printed);
        for (unsigned long long i = 0; i < printed; i++) printf(" %llu", starts[i]);
        printf("\n");
        free(starts);
        free(pat);
    }
    return 0;
}
