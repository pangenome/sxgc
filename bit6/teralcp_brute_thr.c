// teralcp_brute_thr.c — gate 4a.1 mid-scale brute force.
// Multi-string BCR convention (validated byte-identical vs grlBWT on the
// fresh tiny gate, bit6/teralcp_gate_tiny.py): the i-th '\n' (string order)
// acts as sentinel $i, sentinels distinct, ordered by string index, all
// sorting before the alphabet. Limit: k <= 255 (sentinels are bytes below
// the alphabet; with k strings the last sentinel value is k <= 255... in
// practice k < 0x21 required when lowercase present — see driver).
// Emits brute.thr / brute.thr_pos (5-byte LE per split run), TeraLCP
// semantics: first run of char c -> (0,0); else min LCP over the gap
// (prev c-run end, run head], earliest argmin.
// Usage: teralcp_brute_thr <text-file> <out-prefix>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
// divsufsort ABI (libdivsufsort.a in ~/usr); header avoided (variant drift)
extern int divsufsort(const unsigned char* T, int32_t* SA, int32_t n);
typedef int32_t saidx_t;

static void die(const char* m) { fprintf(stderr, "brute_thr: %s\n", m); exit(1); }

int chi_mode(int argc, char** argv);

int main(int argc, char** argv) {
    if (argc == 4 && !strcmp(argv[2], "chi")) return chi_mode(argc, argv);
    if (argc != 3) { fprintf(stderr, "usage: %s <text> <outprefix>\n       %s <text> chi <out.chi>\n", argv[0], argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) die("open text");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* T = malloc(n);
    if (fread(T, 1, n, f) != (size_t)n) die("read text");
    fclose(f);

    // pass 1 (no modification): count strings, collect distinct real symbols
    int k = 0;
    int present[256]; memset(present, 0, sizeof present);
    for (long i = 0; i < n; ++i) {
        if (T[i] == '\n') ++k;
        else present[T[i]] = 1;
    }
    int nsym = 0;
    for (int c = 0; c < 256; ++c) if (present[c]) ++nsym;
    if (k <= 0) die("no endmarkers");
    if (k + nsym > 255) die("k + alphabet exceeds byte range");
    unsigned char remap[256];
    int next = k + 1;
    for (int c = 0; c < 256; ++c) if (present[c]) remap[c] = (unsigned char)(next++);
    // pass 2: i-th '\n' -> sentinel (i+1); real symbols -> remapped block
    k = 0;
    for (long i = 0; i < n; ++i) {
        if (T[i] == '\n') { ++k; T[i] = (unsigned char)k; }
        else T[i] = remap[T[i]];
    }
    fprintf(stderr, "n=%ld k=%d realsym=%d (alphabet remapped to %d..%d)\n", n, k, nsym, k+1, k+nsym);

    // suffix array
    saidx_t* SA = malloc(n * sizeof(saidx_t));
    if (divsufsort(T, SA, (saidx_t)n) != 0) die("divsufsort");

    // BWT with sentinels mapped back to '\n'
    unsigned char* BWT = malloc(n);
    for (long i = 0; i < n; ++i) {
        long p = SA[i] - 1; if (p < 0) p += n;
        unsigned char c = T[p];
        BWT[i] = (c <= k && c >= 1) ? '\n' : c;
    }

    // Kasai LCP over the mapped text
    int32_t* LCP = calloc(n, sizeof(int32_t));
    int32_t* RANK = malloc(n * sizeof(int32_t));
    for (long i = 0; i < n; ++i) RANK[SA[i]] = i;
    long h = 0;
    for (long i = 0; i < n; ++i) {
        if (RANK[i] > 0) {
            long j = SA[RANK[i] - 1];
            while (i + h < n && j + h < n && T[i + h] == T[j + h]) ++h;
            LCP[RANK[i]] = (int32_t)h;
            if (h) --h;
        } else h = 0;
    }
    free(RANK); free(SA);

    // split runs + thresholds
    // last_end[c]: BWT end row of previous run of c; seen[c]
    long last_end[256]; int seen[256];
    memset(last_end, -1, sizeof last_end); memset(seen, 0, sizeof seen);

    char path[512];
    snprintf(path, sizeof path, "%s.thr", argv[2]);
    FILE* fthr = fopen(path, "wb");
    snprintf(path, sizeof path, "%s.thr_pos", argv[2]);
    FILE* fpos = fopen(path, "wb");
    if (!fthr || !fpos) die("open outputs");

    long i = 0; uint64_t cnt = 0;
    while (i < n) {
        long j = i;
        while (j < n && BWT[j] == BWT[i]) ++j;
        unsigned char c = BWT[i];
        long run_start = i, run_end = j - 1;
        long nruns = (c == '\n') ? (run_end - run_start + 1) : 1;
        for (long rr = 0; rr < nruns; ++rr) {
            long s = (c == '\n') ? run_start + rr : run_start;
            uint64_t thr, pos;
            if (!seen[c]) {
                thr = 0; pos = 0; seen[c] = 1;
            } else if (last_end[c] + 1 > s) {
                die("internal: run order invariant violated");
            } else {
                int32_t best = INT32_MAX; long bpos = -1;
                for (long q = last_end[c] + 1; q <= s; ++q)
                    if (LCP[q] < best) { best = LCP[q]; bpos = q; }
                thr = (uint64_t)best; pos = (uint64_t)bpos;
            }
            unsigned char ob[5];
            for (int b = 0; b < 5; ++b) ob[b] = (thr >> (8*b)) & 0xFF;
            fwrite(ob, 5, 1, fthr);
            for (int b = 0; b < 5; ++b) ob[b] = (pos >> (8*b)) & 0xFF;
            fwrite(ob, 5, 1, fpos);
            ++cnt;
            last_end[c] = (c == '\n') ? s : run_end;
        }
        i = j;
    }
    fclose(fthr); fclose(fpos);
    fprintf(stderr, "split runs=%llu\n", (unsigned long long)cnt);
    free(T); free(BWT); free(LCP);
    return 0;
}

// (chi_mode appended at end of file; forward-declared here)

// ---- chi mode: brute-force per-row scan machine (scan-rs port, Bit-2 gated
// semantics; sentinel chars map to 0, never emitted). Usage:
//   teralcp_brute_thr <text> <outprefix> chi <out.chi>
// Emits u64 LE positions (N - sa convention, N = n + 1) in emission order.
int chi_mode(int argc, char** argv) {
    const char* textPath = argv[1];
    FILE* f = fopen(textPath, "rb");
    if (!f) die("open text");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* T = malloc(n);
    if (fread(T, 1, n, f) != (size_t)n) die("read text");
    fclose(f);
    int k = 0; int present[256]; memset(present, 0, sizeof present);
    for (long i = 0; i < n; ++i) { if (T[i] == '\n') ++k; else present[T[i]] = 1; }
    int nsym = 0;
    for (int c = 0; c < 256; ++c) if (present[c]) ++nsym;
    unsigned char remap[256]; int next = k + 1;
    for (int c = 0; c < 256; ++c) if (present[c]) remap[c] = (unsigned char)(next++);
    k = 0;
    for (long i = 0; i < n; ++i) {
        if (T[i] == '\n') { ++k; T[i] = (unsigned char)k; }
        else T[i] = remap[T[i]];
    }
    saidx_t* SA = malloc(n * sizeof(saidx_t));
    if (divsufsort(T, SA, (saidx_t)n) != 0) die("divsufsort");
    int32_t* RANK = malloc(n * sizeof(int32_t));
    for (long i = 0; i < n; ++i) RANK[SA[i]] = i;
    int32_t* LCP = calloc(n, sizeof(int32_t));
    long h = 0;
    for (long i = 0; i < n; ++i) {
        if (RANK[i] > 0) {
            long j = SA[RANK[i] - 1];
            while (i+h < n && j+h < n && T[i+h] == T[j+h]) ++h;
            LCP[RANK[i]] = (int32_t)h;
            if (h) --h;
        } else h = 0;
    }
    // machine
    #define CHI_SIGMA 256
    long long rlen[CHI_SIGMA]; long long rpos[CHI_SIGMA]; int ract[CHI_SIGMA];
    for (int c = 0; c < CHI_SIGMA; ++c) { rlen[c] = -1; rpos[c] = 0; ract[c] = 0; }
    long long MAXI = (long long)0x7fffffffffffffffLL;
    FILE* fo = fopen(argv[3], "wb");
    if (!fo) die("open chi out");
    uint64_t N = (uint64_t)n + 1;
    long long m = MAXI; long long p = -1; uint64_t p_sa = 0;
    uint64_t cnt = 0;
    // buffered emit
    uint64_t buf[65536]; int bn = 0;
    #define EMIT(x) do { buf[bn++] = (x); if (bn == 65536) { fwrite(buf, 8, bn, fo); bn = 0; } ++cnt; } while (0)
    for (long i = 0; i < n; ++i) {
        int c = (SA[i] > 0) ? T[SA[i] - 1] : T[n - 1];   // BWT char (mapped)
        if (c <= k) c = 0;                                // sentinels -> 0
        long long l = LCP[i]; uint64_t s = (uint64_t)SA[i];
        if (p < 0) { p = c; p_sa = s; continue; }
        long long m2 = m < l ? m : l;
        if (c != p) {
            // eval
            for (int cc = 1; cc < CHI_SIGMA; ++cc)
                if (m2 < rlen[cc]) { if (ract[cc]) EMIT(rpos[cc]); rlen[cc] = m2; rpos[cc] = 0; ract[cc] = 0; }
            // upds
            if (l > rlen[p]) { rlen[p] = l; rpos[p] = (long long)(N - p_sa); ract[p] = 1; }
            if (l > rlen[c]) { rlen[c] = l; rpos[c] = (long long)(N - s); ract[c] = 1; }
            m = MAXI;
        } else m = m2;
        p = c; p_sa = s;
    }
    for (int cc = 1; cc < CHI_SIGMA; ++cc)
        if (-1 < rlen[cc]) { if (ract[cc]) EMIT(rpos[cc]); }
    if (bn) fwrite(buf, 8, bn, fo);
    fclose(fo);
    fprintf(stderr, "chi brute: %llu positions\n", (unsigned long long)cnt);
    return 0;
}
