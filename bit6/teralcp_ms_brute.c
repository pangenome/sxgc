// teralcp_ms_brute.c — gate: brute-force matching statistics per read via
// SA interval search, for byte-comparison with TeraMS output.
// Convention (BCR multi-string, same as the chi gates): the i-th '\n' acts
// as sentinel $i (distinct, sorted before the alphabet). MS-len[i] = the
// longest k such that read[i..i+k) occurs in the text; MS-pos[i] = a text
// position whose suffix matches read[i..] for exactly MS-len[i] chars
// (ties broken toward the SA neighbor with the larger match; if TeraMS
// reports a different but equally valid pos, the gate compares validity).
// Usage: teralcp_ms_brute <text> <reads.fa> <out.len> <out.pos>
// Output format mirrors TeraMS: per read, u64 count then u32 lens /
// u64 poss, with a '>' header line before each array block (ASCII).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
extern int divsufsort(const unsigned char* T, int32_t* SA, int32_t n);
typedef int32_t saidx_t;

static void die(const char* m) { fprintf(stderr, "ms_brute: %s\n", m); exit(1); }

int main(int argc, char** argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s <text> <reads.fa> <out.len> <out.pos>\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) die("open text");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* T = malloc(n);
    if (fread(T, 1, n, f) != (size_t)n) die("read text");
    fclose(f);
    int k = 0, present[256]; memset(present, 0, sizeof present);
    for (long i = 0; i < n; ++i) { if (T[i] == '\n') ++k; else present[T[i]] = 1; }
    unsigned char remap[256]; int next = k + 1;
    for (int c = 0; c < 256; ++c) if (present[c]) remap[c] = (unsigned char)(next++);
    k = 0;
    for (long i = 0; i < n; ++i) {
        if (T[i] == '\n') { ++k; T[i] = (unsigned char)k; }
        else T[i] = remap[T[i]];
    }
    saidx_t* SA = malloc(n * sizeof(saidx_t));
    if (divsufsort(T, SA, (saidx_t)n) != 0) die("divsufsort");

    // suffix compare of read (remapped on the fly) against T[s..]
    // returns: >0 if read-part > suffix, <0 if <, 0 if equal to read length
    FILE* rf = fopen(argv[2], "r");
    if (!rf) die("open reads");
    FILE* fl = fopen(argv[3], "w");
    FILE* fp = fopen(argv[4], "w");
    if (!fl || !fp) die("open outputs");
    char name[4096], line[70000];
    long reads = 0, lens_bad = 0, pos_invalid = 0, pos_diff = 0;
    while (fgets(line, sizeof line, rf)) {
        if (line[0] != '>') continue;
        sscanf(line + 1, "%4095s", name);
        if (!fgets(line, sizeof line, rf)) die("short read file");
        long m = (long)strlen(line);
        while (m && (line[m-1] == '\n' || line[m-1] == '\r')) line[--m] = 0;
        if (!m) die("empty read");
        unsigned char* R = malloc(m);
        for (long i = 0; i < m; ++i) {
            unsigned char c = (unsigned char)line[i];
            if (c == '\n') { die("newline in read"); }
            R[i] = present[c] ? remap[c] : 0xff;   // unknown char -> no match
        }
        uint32_t* lens = malloc(m * 4);
        uint64_t* poss = malloc(m * 8);
        for (long i = 0; i < m; ++i) {
            // binary search read[i..] in SA: find insertion point
            long lo = 0, hi = n;
            while (lo < hi) {
                long mid = (lo + hi) / 2;
                long s = SA[mid]; long j = 0;
                while (j < m - i && s + j < n && T[s + j] == R[i + j]) ++j;
                int cmp;
                if (j == m - i) cmp = 0;               // read suffix exhausted
                else if (s + j >= n) cmp = -1;         // text suffix exhausted -> read bigger
                else cmp = (R[i + j] < T[s + j]) ? -1 : 1;
                if (cmp > 0) lo = mid + 1; else hi = mid;
            }
            // neighbors: SA[lo-1] and SA[lo] bracket the insertion point
            long best = 0; long bpos = -1;
            for (long nb = lo - 1; nb <= lo; ++nb) {
                if (nb < 0 || nb >= n) continue;
                long s = SA[nb]; long j = 0;
                while (j < m - i && s + j < n && T[s + j] == R[i + j]) ++j;
                if (j > best) { best = j; bpos = s; }
                else if (j == best && j > 0 && bpos < 0) bpos = s;
            }
            lens[i] = (uint32_t)best;
            poss[i] = (uint64_t)(bpos < 0 ? 0 : bpos);
        }
        fprintf(fl, ">%s\n", name);
        fprintf(fp, ">%s\n", name);
        uint64_t cnt = (uint64_t)m;
        fwrite(&cnt, 8, 1, fl); fwrite(lens, 4, m, fl); fputc('\n', fl);
        fwrite(&cnt, 8, 1, fp); fwrite(poss, 8, m, fp); fputc('\n', fp);
        ++reads;
        free(R); free(lens); free(poss);
    }
    fclose(rf); fclose(fl); fclose(fp);
    fprintf(stderr, "ms_brute: %ld reads done\n", reads);
    (void)lens_bad; (void)pos_invalid; (void)pos_diff;
    return 0;
}
