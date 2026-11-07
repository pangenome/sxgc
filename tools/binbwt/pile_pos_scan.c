// pile_pos_scan: an independent exact-match scanner over one corpus file.
//
// Why this exists: `xsa query --count` and `LC_ALL=C grep -a -o -F` disagreed by
// 192 on the Pile for one pattern (" function ", 4,785,152 vs 4,784,960) while
// three other patterns agreed exactly. Adjudicating that needs a THIRD
// implementation that shares no code with either, and that can enumerate every
// match position so each one can be checked against the bytes.
//
// The corpus is read through WINDOWED mmap only (the project's rule: the corpus
// file is the only text and nothing is materialised). Windows are MAP_PRIVATE,
// PROT_READ, MADV_RANDOM, and adjacent windows overlap by pattern_len-1 bytes so
// a match that straddles a window boundary is found exactly once (matches are
// only reported when they START inside the window's own first `advance` bytes).
//
// Output (stdout, TSV unless --offsets is given):
//   SCAN pattern=<escaped> bytes=<n> matches=<count> windows=<n>
//   PREV <byte-or-EOF> <count>      one line per distinct preceding byte
//   NEXT <byte-or-EOF> <count>      one line per distinct following byte
// With --offsets PATH the raw little-endian u64 offsets go to PATH instead and
// stdout keeps the summary lines.
//
// Build: cc -O2 -o pile_pos_scan pile_pos_scan.c
// Use:   ./pile_pos_scan corpus.bin " function " [--offsets out.u64] [--window-mb 1024]

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void die(const char *what) {
    fprintf(stderr, "pile_pos_scan: %s: %s\n", what, strerror(errno));
    exit(1);
}

// Match the pattern at p (pattern_len bytes available). Deliberately a plain
// byte loop: this scanner must not share code or a corpus contract with grep's
// Boyer-Moore or with the index, and an O(n*m) comparison at m<=16 with a
// first-byte skip is fast enough on 100 GB (measured below in the receipt).
static inline int matches(const unsigned char *p, const unsigned char *pat, size_t m) {
    return memcmp(p, pat, m) == 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s FILE PATTERN [--offsets OUT] [--window-mb N]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    const unsigned char *pat = (const unsigned char *)argv[2];
    size_t m = strlen(argv[2]);
    if (m == 0) {
        fprintf(stderr, "pile_pos_scan: empty pattern\n");
        return 2;
    }
    const char *offsets_path = NULL;
    size_t window = (size_t)1024 << 20;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--offsets") && i + 1 < argc) {
            offsets_path = argv[++i];
        } else if (!strcmp(argv[i], "--window-mb") && i + 1 < argc) {
            window = (size_t)strtoull(argv[++i], NULL, 10) << 20;
        } else {
            fprintf(stderr, "pile_pos_scan: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (window < (1u << 20)) window = 1u << 20;

    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open");
    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat");
    size_t n = (size_t)st.st_size;

    uint64_t prev_hist[257];
    uint64_t next_hist[257];
    memset(prev_hist, 0, sizeof prev_hist);
    memset(next_hist, 0, sizeof next_hist);
    // index 256 means "no byte there" (start of file / end of file)

    FILE *out = NULL;
    if (offsets_path) {
        out = fopen(offsets_path, "wb");
        if (!out) die("fopen --offsets");
    }

    uint64_t matches_total = 0, windows = 0;
    size_t pos = 0;
    while (pos < n) {
        size_t want = window;
        if (pos + want > n) want = n - pos;
        // One extra pattern_len-1 bytes of lookahead so a match starting in the
        // window's own bytes is visible even when it ends in the next window.
        size_t map_len = want;
        if (pos + map_len + m - 1 > n) map_len = n - pos;
        else map_len = want + (m - 1);

        void *base = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, (off_t)pos);
        if (base == MAP_FAILED) die("mmap");
        madvise(base, map_len, MADV_RANDOM);
        const unsigned char *p = base;
        size_t limit = map_len - (m - 1) < want ? map_len - (m - 1) : want; // matches must START here
        size_t i = 0;
        while (i < limit) {
            const unsigned char *hit = memchr(p + i, pat[0], limit - i);
            if (!hit) break;
            size_t k = (size_t)(hit - p);
            if (k + m > map_len) break; // needs bytes past the window; next window sees it
            if (matches(p + k, pat, m)) {
                uint64_t g = (uint64_t)pos + k;
                matches_total++;
                prev_hist[(g == 0) ? 256 : p[k - 1]]++;
                // the byte AFTER the match: inside this window, or the file end
                uint64_t after = g + m;
                next_hist[(after >= n) ? 256 : p[k + m]]++;
                if (out) {
                    if (fwrite(&g, sizeof g, 1, out) != 1) die("fwrite");
                }
                i = k + 1; // overlapping matches are impossible for this pattern,
                           // but +1 (not +m) keeps the scanner honest about that
            } else {
                i = k + 1;
            }
        }
        if (munmap(base, map_len) != 0) die("munmap");
        windows++;
        pos += want;
    }
    if (out && fclose(out) != 0) die("fclose --offsets");

    printf("SCAN pattern=%s bytes=%zu matches=%llu windows=%llu\n", argv[2], n,
           (unsigned long long)matches_total, (unsigned long long)windows);
    for (int b = 0; b < 257; b++) {
        if (prev_hist[b]) {
            if (b == 256) printf("PREV EOF %llu\n", (unsigned long long)prev_hist[b]);
            else printf("PREV 0x%02x %llu\n", b, (unsigned long long)prev_hist[b]);
        }
    }
    for (int b = 0; b < 257; b++) {
        if (next_hist[b]) {
            if (b == 256) printf("NEXT EOF %llu\n", (unsigned long long)next_hist[b]);
            else printf("NEXT 0x%02x %llu\n", b, (unsigned long long)next_hist[b]);
        }
    }
    close(fd);
    return 0;
}
