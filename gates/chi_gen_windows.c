// gates/chi_gen_windows.c — the G1 window-set generator.
//
// Generates a DOC-CONTAINED window set for the 10 GB parity container that
// exercises every layer of the chi cascade:
//   - RAMP windows at document starts (T2048 clamped to the document end):
//     empty spines, 1-byte spines, short spines, full 1024-byte spines;
//   - MID windows at document midpoints (full spines, the forced mass);
//   - TAIL windows ending at document ends (END-adjacent continuations);
//   - SHORT-LEFT windows (left=2 and left=16: short spines, wider intervals);
//   - SEP windows around INTERNAL 0x1E bytes (content separators inside
//     documents - the ENUM layer and the conservative alarm), found by
//     scanning the corpus;
//   - whole-doc windows for a few tiny documents.
//
// Windows carry {doc, base, start, len, left} with base = the TRUE document
// start from seps.bin (the container is the authority; consult dies on
// structure disagreement by design).
//
// usage: chi_gen_windows SEPSBIN CORPUS OUT.jsonl [NDOCS] [SEED]
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }

// xorshift for deterministic sampling
static uint64_t rng_state;
static uint64_t rnd(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}

// binary search: the index of the largest start <= p (or -1)
static int64_t doc_of(const uint64_t *starts, uint64_t docs, uint64_t p) {
    uint64_t lo = 0, hi = docs;
    while (lo + 8 < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (starts[mid] <= p) lo = mid; else hi = mid;
    }
    while (lo + 1 < docs && starts[lo + 1] <= p) lo++;
    return (int64_t)lo;
}

static FILE *out;
static uint64_t win_count = 0, pos_count = 0;

static void win(uint64_t doc, uint64_t base, uint64_t start, uint64_t len, uint32_t left) {
    if (len == 0) return;
    fprintf(out, "{\"doc\": %llu, \"base\": %llu, \"start\": %llu, \"len\": %llu, \"left\": %u}\n",
            (unsigned long long)doc, (unsigned long long)base,
            (unsigned long long)start, (unsigned long long)len, left);
    win_count++; pos_count += len;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s SEPSBIN CORPUS OUT.jsonl [NDOCS] [SEED]\n", argv[0]); return 1; }
    const char *seps = argv[1], *corpus = argv[2], *outp = argv[3];
    uint64_t ndocs = argc > 4 ? strtoull(argv[4], 0, 10) : 240;
    rng_state = argc > 5 ? strtoull(argv[5], 0, 10) : 0x9E3779B97F4A7C15ull;
    if (!rng_state) rng_state = 1;

    // ---- seps.bin (XSEP v1): 48B header, u32 deltas[docs], u64 anchors
    FILE *f = fopen(seps, "rb");
    if (!f) { perror("seps.bin"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { perror("read seps"); return 1; }
    fclose(f);
    uint64_t stride = rd32(b + 12), docs = rd64(b + 16), corpus_bytes = rd64(b + 24);
    uint64_t *starts = malloc(docs * 8);
    {
        uint64_t start = 0;
        for (uint64_t d = 0; d < docs; d++) {
            if (d % stride == 0) start = rd64(b + 48 + 4 * docs + 8 * (d / stride));
            else start += rd32(b + 48 + 4 * d);
            starts[d] = start;
        }
    }
    fprintf(stderr, "seps: docs=%llu corpus=%llu\n", (unsigned long long)docs, (unsigned long long)corpus_bytes);

    int fd = open(corpus, O_RDONLY);
    struct stat st; fstat(fd, &st);
    uint8_t *text = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (text == MAP_FAILED) { perror("mmap corpus"); return 1; }
    uint64_t text_len = (uint64_t)st.st_size < corpus_bytes ? (uint64_t)st.st_size : corpus_bytes;

    out = fopen(outp, "w");
    if (!out) { perror("out"); return 1; }

    // ---- 1) internal 0x1e positions (not the framing byte of any doc start):
    // scan the corpus; a position p is framing iff (p+1) is a doc start.
    uint64_t *internal_sep = malloc(256 * 8);
    uint64_t n_internal = 0;
    {
        // the doc-start set as a hash of "p is a start"
        uint8_t *is_start = calloc((text_len + 8), 1);
        for (uint64_t d = 0; d < docs; d++)
            if (starts[d] < text_len) is_start[starts[d]] = 1;
        const uint64_t CH = 1ull << 26; // 64 MiB chunks
        uint8_t *buf = malloc(CH);
        for (uint64_t off = 0; off < text_len && n_internal < 256; off += CH) {
            uint64_t len = CH < text_len - off ? CH : text_len - off;
            memcpy(buf, text + off, len);
            for (uint64_t p = 0; p < len && n_internal < 256; p++) {
                if (buf[p] == 0x1e) {
                    uint64_t abs = off + p;
                    if (abs + 1 < text_len && is_start[abs + 1]) continue; // framing
                    internal_sep[n_internal++] = abs;
                }
            }
        }
        free(buf); free(is_start);
    }
    fprintf(stderr, "internal 0x1e found: %llu (capped at 256)\n", (unsigned long long)n_internal);

    // ---- 2) the window set
    // a deterministic sample of ndocs documents spread over the corpus
    uint64_t step = docs / (ndocs ? ndocs : 1);
    if (step == 0) step = 1;
    for (uint64_t k = 0; k * step < docs; k++) {
        uint64_t d = k * step;
        uint64_t base = starts[d];
        uint64_t next = d + 1 < docs ? starts[d + 1] : text_len;
        if (next > text_len) next = text_len;
        uint64_t dlen = next - base;
        if (dlen == 0) continue;
        // RAMP at the doc start (T2048 clamped)
        win(d, base, 0, dlen < 2048 ? dlen : 2048, 1024);
        // MID window (full spines)
        if (dlen > 4096) win(d, base, dlen / 2, 1024, 1024);
        // TAIL window (END-adjacent)
        if (dlen > 3072) win(d, base, dlen - 1024, 1024, 1024);
        // SHORT-LEFT windows
        if (dlen > 512) {
            win(d, base, dlen / 2, 128, 2);
            win(d, base, dlen / 2 + 128, 128, 16);
        }
        // a few whole (tiny-ish) docs
        if (dlen <= 4096) win(d, base, 0, dlen, 1024);
    }
    // ---- SEP windows around the internal 0x1e positions (ENUM layer)
    for (uint64_t i = 0; i < n_internal; i++) {
        int64_t d = doc_of(starts, docs, internal_sep[i]);
        if (d < 0) continue;
        uint64_t base = starts[d];
        uint64_t rel = internal_sep[i] - base;
        uint64_t next = (uint64_t)d + 1 < docs ? starts[d + 1] : text_len;
        uint64_t dlen = next - base;
        if (dlen <= rel) continue;
        // the query sits right after the separator (spine crosses it) and a
        // little before it (the separator is deep in the spine)
        win(d, base, rel + 1 < dlen ? rel + 1 : dlen - 1, 64, 64);
        if (rel > 32) win(d, base, rel - 32, 64, 128);
    }
    fclose(out);
    printf("CHI_WINDOWS out=%s windows=%llu positions=%llu internal_sep=%llu\n",
           outp, (unsigned long long)win_count, (unsigned long long)pos_count,
           (unsigned long long)n_internal);
    munmap(text, st.st_size);
    return 0;
}
