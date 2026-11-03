// Independent occurrence counter for the seeds gate: one mmap pass per pattern,
// glibc memmem, OVERLAPPING occurrences counted (that is what a BWT interval
// width means). No index code, no shared implementation with xsa.
//
//   seeds_oracle CORPUS PATTERNS
//
// PATTERNS is a sequence of u32 little-endian length + that many bytes (the
// same framing `xsa mem --serve` uses). One unsigned count is printed per
// pattern, in input order, one per line.
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
    if (argc != 3) { fprintf(stderr, "usage: %s CORPUS PATTERNS\n", argv[0]); return 2; }
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
        unsigned long long count = 0;
        if (len == 0) {
            count = (unsigned long long)n + 1; // every boundary, as the index reports
        } else if (n >= len) {
            const unsigned char *h = c, *end = c + (n - len) + 1;
            // memmem counts OVERLAPPING occurrences because h advances by 1.
            while (h < end) {
                const unsigned char *m = memmem(h, (size_t)(end - h), pat, len);
                if (!m) break;
                count++;
                h = m + 1;
            }
        }
        free(pat);
        printf("%llu\n", count);
    }
    fclose(p);
    return 0;
}
