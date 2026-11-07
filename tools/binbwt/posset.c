// posset: position-set arithmetic and byte verification for query certificates.
//
// The pile's search certificate compares three independent enumerations of the
// same occurrences (index locate, GNU grep -b -o, and a windowed-mmap scan).
// Comparing them means set arithmetic on millions of u64 offsets, and proving
// each offset is a real occurrence means checking the corpus bytes at it. Both
// belong in one small C program that touches nothing but the files it is given.
//
//   posset fromtext FILE.txt OUT.u64     "offset:..." lines -> little-endian u64
//   posset sort IN.u64 OUT.u64           sort ascending
//   posset stat IN.u64                   count, unique count, first/last offset
//   posset diff A.u64 B.u64 OUT.u64      offsets in A that are not in B (A sorted)
//   posset verify CORPUS PATTERN IN.u64  every offset must carry the exact bytes
//   posset context CORPUS PATTERN IN.u64 N   N bytes of context around each offset
//
// `verify` reads the corpus with pread: a few million independent 10-byte reads
// are the point (this is the check, not a scan).
//
// Build: cc -O2 -o posset posset.c
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void die(const char *what) {
    fprintf(stderr, "posset: %s: %s\n", what, strerror(errno));
    exit(1);
}

static uint64_t *load(const char *path, size_t *n_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die(path);
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz < 0) die("lseek");
    if (lseek(fd, 0, SEEK_SET) != 0) die("lseek");
    if (sz % (off_t)sizeof(uint64_t) != 0) {
        fprintf(stderr, "posset: %s is not a u64 array (%lld bytes)\n", path, (long long)sz);
        exit(1);
    }
    size_t n = (size_t)sz / sizeof(uint64_t);
    uint64_t *v = malloc(n ? n * sizeof(uint64_t) : 1);
    if (!v) die("malloc");
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, v + got, (n - got) * sizeof(uint64_t));
        if (r < 0) die("read");
        if (r == 0) {
            fprintf(stderr, "posset: %s short read\n", path);
            exit(1);
        }
        got += (size_t)r / sizeof(uint64_t);
    }
    close(fd);
    *n_out = n;
    return v;
}

static void store(const char *path, const uint64_t *v, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) die(path);
    if (n && fwrite(v, sizeof(uint64_t), n, f) != n) die("fwrite");
    if (fclose(f) != 0) die("fclose");
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

// read a corpus window containing [off, off+len) without mapping the file
static void pread_exact(int fd, uint64_t off, unsigned char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = pread(fd, buf + got, len - got, (off_t)(off + got));
        if (r < 0) die("pread");
        if (r == 0) {
            fprintf(stderr, "posset: short pread at %llu (file ends early)\n",
                    (unsigned long long)(off + got));
            exit(1);
        }
        got += (size_t)r;
    }
}

static void escape(const unsigned char *p, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < cap; i++) {
        unsigned char c = p[i];
        if (c == '\\' || c == '\t' || c == '\n' || c == '\r') {
            out[o++] = '\\';
            out[o++] = (c == '\\') ? '\\' : (c == '\t' ? 't' : (c == '\n' ? 'n' : 'r'));
        } else if (c >= 0x20 && c < 0x7f) {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, cap - o, "\\x%02x", c);
        }
    }
    out[o] = 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: posset fromtext|sort|stat|diff|verify|context ...\n");
        return 2;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "fromtext")) {
        if (argc != 4) return 2;
        FILE *f = fopen(argv[2], "r");
        if (!f) die(argv[2]);
        FILE *o = fopen(argv[3], "wb");
        if (!o) die(argv[3]);
        char line[512];
        uint64_t n = 0;
        while (fgets(line, sizeof line, f)) {
            char *colon = strchr(line, ':');
            if (!colon) {
                fprintf(stderr, "posset: no offset in line: %s", line);
                return 1;
            }
            *colon = 0;
            uint64_t v = strtoull(line, NULL, 10);
            if (fwrite(&v, sizeof v, 1, o) != 1) die("fwrite");
            n++;
        }
        fclose(f);
        if (fclose(o) != 0) die("fclose");
        fprintf(stderr, "fromtext lines=%llu\n", (unsigned long long)n);
        return 0;
    }
    if (!strcmp(cmd, "sort")) {
        if (argc != 4) return 2;
        size_t n = 0;
        uint64_t *v = load(argv[2], &n);
        qsort(v, n, sizeof(uint64_t), cmp_u64);
        store(argv[3], v, n);
        fprintf(stderr, "sort n=%zu\n", n);
        return 0;
    }
    if (!strcmp(cmd, "stat")) {
        if (argc != 3) return 2;
        size_t n = 0;
        uint64_t *v = load(argv[2], &n);
        size_t uniq = 0;
        for (size_t i = 0; i < n; i++) {
            if (i == 0 || v[i] != v[i - 1]) uniq++;
        }
        printf("STAT count=%zu unique=%zu first=%llu last=%llu\n", n, uniq,
               n ? (unsigned long long)v[0] : 0ULL,
               n ? (unsigned long long)v[n - 1] : 0ULL);
        if (uniq != n) {
            // report the first few duplicates: a duplicate position is never
            // legitimate and must be named, not summarised
            size_t shown = 0;
            for (size_t i = 1; i < n && shown < 8; i++) {
                if (v[i] == v[i - 1]) {
                    printf("DUP offset=%llu at index %zu\n", (unsigned long long)v[i], i);
                    shown++;
                }
            }
        }
        return 0;
    }
    if (!strcmp(cmd, "diff")) {
        if (argc != 5) return 2;
        size_t na = 0, nb = 0;
        uint64_t *a = load(argv[2], &na);
        uint64_t *b = load(argv[3], &nb);
        qsort(a, na, sizeof(uint64_t), cmp_u64);
        qsort(b, nb, sizeof(uint64_t), cmp_u64);
        uint64_t *out = malloc((na ? na : 1) * sizeof(uint64_t));
        if (!out) die("malloc");
        size_t k = 0, j = 0;
        for (size_t i = 0; i < na; i++) {
            while (j < nb && b[j] < a[i]) j++;
            if (j < nb && b[j] == a[i]) continue;
            out[k++] = a[i];
        }
        store(argv[4], out, k);
        printf("DIFF a=%zu b=%zu a_not_b=%zu\n", na, nb, k);
        return 0;
    }
    if (!strcmp(cmd, "kmer")) {
        // kmer CORPUS IN.u64 BEFORE AFTER OUT.txt: one raw window per line, for
        // each offset, so an index can be asked for that exact window with
        // --limit 1 and its located position compared with the offset. A window
        // that contains 0x1E or NUL cannot travel in a pattern file or argv, so it
        // is REFUSED here rather than silently mangled.
        if (argc != 7) return 2;
        size_t before = (size_t)strtoull(argv[4], NULL, 10);
        size_t after = (size_t)strtoull(argv[5], NULL, 10);
        size_t n = 0;
        uint64_t *v = load(argv[3], &n);
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) die(argv[2]);
        off_t sz = lseek(fd, 0, SEEK_END);
        if (sz < 0) die("lseek");
        FILE *o = fopen(argv[6], "wb");
        if (!o) die(argv[6]);
        unsigned char buf[512];
        size_t len = before + after;
        if (len > sizeof buf) {
            fprintf(stderr, "posset: window too wide\n");
            return 2;
        }
        for (size_t i = 0; i < n; i++) {
            uint64_t lo = v[i] > before ? v[i] - before : 0;
            uint64_t hi = v[i] + after;
            if ((off_t)hi > sz) hi = (uint64_t)sz;
            size_t l = (size_t)(hi - lo);
            pread_exact(fd, lo, buf, l);
            int usable = 1;
            for (size_t k = 0; k < l; k++) {
                if (buf[k] == 0x1e || buf[k] == 0) {
                    usable = 0;
                    break;
                }
            }
            if (!usable) {
                // A window carrying a record separator or a NUL cannot travel as a
                // pattern. It is SKIPPED, and one KEEP/SKIP line per input offset
                // goes to stderr in input order, so a caller can line the emitted
                // windows up with its own offsets without guessing.
                fprintf(stderr, "SKIP %llu\n", (unsigned long long)v[i]);
                continue;
            }
            fprintf(stderr, "KEEP %llu\n", (unsigned long long)v[i]);
            if (fwrite(buf, 1, l, o) != l) die("fwrite");
            fputc('\0', o);  // NUL-terminated: a window may contain a newline
        }
        if (fclose(o) != 0) die("fclose");
        close(fd);
        return 0;
    }
    if (!strcmp(cmd, "verify")) {
        if (argc != 5) return 2;
        const unsigned char *pat = (const unsigned char *)argv[3];
        size_t m = strlen(argv[3]);
        size_t n = 0;
        uint64_t *v = load(argv[4], &n);
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) die(argv[2]);
        off_t sz = lseek(fd, 0, SEEK_END);
        if (sz < 0) die("lseek");
        unsigned char buf[64];
        size_t bad = 0, shown = 0;
        for (size_t i = 0; i < n; i++) {
            if ((off_t)(v[i] + m) > sz) {
                if (shown < 8) {
                    printf("BAD offset=%llu reason=past-eof\n", (unsigned long long)v[i]);
                    shown++;
                }
                bad++;
                continue;
            }
            pread_exact(fd, v[i], buf, m);
            if (memcmp(buf, pat, m) != 0) {
                if (shown < 8) {
                    char esc[256];
                    escape(buf, m, esc, sizeof esc);
                    printf("BAD offset=%llu bytes=\"%s\"\n", (unsigned long long)v[i], esc);
                    shown++;
                }
                bad++;
            }
        }
        printf("VERIFY pattern=%s corpus=%s n=%zu ok=%zu bad=%zu\n", argv[3], argv[2], n,
               n - bad, bad);
        close(fd);
        return bad ? 1 : 0;
    }
    if (!strcmp(cmd, "context")) {
        if (argc != 6) return 2;
        const unsigned char *pat = (const unsigned char *)argv[3];
        size_t m = strlen(argv[3]);
        size_t want = (size_t)strtoull(argv[5], NULL, 10);
        size_t n = 0;
        uint64_t *v = load(argv[4], &n);
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) die(argv[2]);
        off_t sz = lseek(fd, 0, SEEK_END);
        if (sz < 0) die("lseek");
        unsigned char buf[4096];
        if (want > sizeof buf) want = sizeof buf;
        for (size_t i = 0; i < n; i++) {
            uint64_t lo = v[i] > want ? v[i] - want : 0;
            uint64_t hi = v[i] + m + want;
            if ((off_t)hi > sz) hi = (uint64_t)sz;
            size_t len = (size_t)(hi - lo);
            pread_exact(fd, lo, buf, len);
            char esc[8192];
            escape(buf, len, esc, sizeof esc);
            printf("CTX offset=%llu lo=%llu hi=%llu bytes_ok=%d text=\"%s\"\n",
                   (unsigned long long)v[i], (unsigned long long)lo,
                   (unsigned long long)hi, memcmp(buf + (v[i] - lo), pat, m) == 0, esc);
        }
        close(fd);
        return 0;
    }
    fprintf(stderr, "posset: unknown command %s\n", cmd);
    return 2;
}
