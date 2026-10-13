// check_pieces: C replacement for check_pieces.py (the M2 pieces gate).
// Reconstructs the dense ssa/ssa_t columns from the v2 band pieces and
// byte-compares against the banked reference, asserting exact run coverage
// [0,R). Streams band-by-band so RAM stays at O(band size) at any scale.
//
// Usage: check_pieces <prefix> <bank_prefix>   # prefix = merged/frag
// Exit 0 + PIECES_GATE_GREEN iff everything matches. Fail-loud otherwise.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static void die(const char* fmt, const char* arg, unsigned long long v) {
    fprintf(stderr, "FATAL check_pieces: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, " (%llu)\n", v);
    exit(1);
}
static void rd(void* p, size_t sz, size_t nm, FILE* f) {
    if (fread(p, sz, nm, f) != nm) { fprintf(stderr, "FATAL check_pieces: truncated read\n"); exit(1); }
}
static uint64_t ru64(const unsigned char* b) { uint64_t v; memcpy(&v, b, 8); return v; }
static uint32_t ru32(const unsigned char* b) { uint32_t v; memcpy(&v, b, 4); return v; }

typedef struct { int band, shard; char name[1700]; } Piece;

static int piece_cmp(const void* a, const void* b) {
    const Piece* x = a; const Piece* y = b;
    if (x->band != y->band) return x->band < y->band ? -1 : 1;
    if (x->shard != y->shard) return x->shard < y->shard ? -1 : 1;
    return strcmp(x->name, y->name);
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: check_pieces <prefix> <bank_prefix>\n"); return 2; }
    const char* prefix = argv[1]; const char* bank = argv[2];

    // --- runcols.idx: magic S R n w B, then S x (base,cnt) ---
    char path[1024];
    snprintf(path, sizeof path, "%s.runcols.idx", prefix);
    FILE* f = fopen(path, "rb");
    if (!f) die("cannot open %s", path, 0);
    unsigned char hdr[32]; rd(hdr, 1, 32, f);
    if (ru32(hdr) != 0x31494352u) die("bad RCI1 magic in %s", path, ru32(hdr));
    uint32_t S = ru32(hdr + 4);
    uint64_t R = ru64(hdr + 8);
    uint32_t w = ru32(hdr + 24), B = ru32(hdr + 28);
    uint64_t covered = 0;
    for (uint32_t s = 0; s < S; ++s) {
        unsigned char rec[16]; rd(rec, 1, 16, f);
        uint64_t base = ru64(rec), cnt = ru64(rec + 8);
        if (base != covered) die("idx shard base != running", path, base);
        covered += cnt;
    }
    fclose(f);
    if (covered != R) die("idx counts total != R", path, covered);

    // --- collect pieces ---
    snprintf(path, sizeof path, "%s.pieces", prefix);
    DIR* d = opendir(path);
    if (!d) die("cannot open pieces dir %s", path, 0);
    Piece* pieces = NULL; size_t np = 0, cap = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int band, shard;
        if (sscanf(e->d_name, "p-%d-%d", &band, &shard) != 2)
            die("stray file in pieces dir", e->d_name, 0);
        // strict name check: "p-<band>-<shard>" with no suffix
        char verify[512];
        snprintf(verify, sizeof verify, "p-%d-%d", band, shard);
        if (strcmp(verify, e->d_name) != 0)
            die("stray file in pieces dir", e->d_name, 0);
        if (np == cap) { cap = cap ? cap * 2 : 256; pieces = realloc(pieces, cap * sizeof(Piece)); }
        pieces[np].band = band; pieces[np].shard = shard;
        snprintf(pieces[np].name, sizeof pieces[np].name, "%s/%s", path, e->d_name);
        ++np;
    }
    closedir(d);
    if (!np) die("no pieces found in", path, 0);
    qsort(pieces, np, sizeof(Piece), piece_cmp);

    // --- banked dense columns (streamed) ---
    char hp[1024], tp[1024];
    snprintf(hp, sizeof hp, "%s.ssa", bank);
    snprintf(tp, sizeof tp, "%s.ssa_t", bank);
    FILE* fh = fopen(hp, "rb"); FILE* ft = fopen(tp, "rb");
    if (!fh) die("cannot open %s", hp, 0);
    if (!ft) die("cannot open %s", tp, 0);
    uint64_t h8; rd(&h8, 8, 1, fh); uint64_t t8; rd(&t8, 8, 1, ft);
    if (h8 != R) die("bank ssa header != R", hp, h8);
    if (t8 != R) die("bank ssa_t header != R", tp, t8);

    // --- stream bands in run order, compare ---
    uint64_t cursor = 0;
    size_t pi = 0;
    uint64_t nbands = (R + B - 1) / B;
    size_t words_max = 0;
    unsigned char* buf = NULL; uint64_t* wb = NULL;
    for (uint64_t band = 0; band < nbands; ++band) {
        while (pi < np && pieces[pi].band == (int)band) {
            FILE* pf = fopen(pieces[pi].name, "rb");
            if (!pf) die("cannot open piece", pieces[pi].name, 0);
            unsigned char ph[44]; rd(ph, 1, 44, pf);
            if (ru32(ph) != 0x31435250u) die("bad PRC1 magic in piece", pieces[pi].name, ru32(ph));
            uint64_t bnd = ru64(ph + 4);
            uint32_t sh = ru32(ph + 12);
            uint64_t firstRun = ru64(ph + 16), rc = ru64(ph + 24);
            uint32_t ww = ru32(ph + 32), zero = ru32(ph + 36);
            if (bnd != band) die("piece band field mismatch", pieces[pi].name, bnd);
            if (sh != (uint32_t)pieces[pi].shard) die("piece shard field mismatch", pieces[pi].name, sh);
            if (ww != w) die("piece w != idx w", pieces[pi].name, ww);
            if (zero != 0) die("piece reserved field nonzero", pieces[pi].name, zero);
            if (firstRun != cursor) die("piece firstRun != cursor", pieces[pi].name, firstRun);
            size_t words = (size_t)((w * (uint64_t)rc + 63) / 64);
            if (words > words_max) {
                words_max = words;
                buf = realloc(buf, rc + 4ull * rc + 16ull * words_max + 16);
                wb = realloc(wb, words_max * sizeof(uint64_t));
                if (!buf || !wb) die("OOM growing piece buffers", pieces[pi].name, words_max);
            }
            unsigned char* chars = buf;
            rd(chars, 1, rc, pf);
            unsigned char* lens = buf + rc;
            rd(lens, 4, rc, pf);  // lens are validated downstream by the finish; banded pieces exclude trimmed padding rows
            for (int col = 0; col < 2; ++col) {
                rd(wb, 8, words, pf);
                FILE* bf = col ? ft : fh;
                __uint128_t res = 0; int avail = 0; size_t wi = 0;
                for (uint64_t i = 0; i < rc; ++i) {
                    while (avail < (int)w) { res |= (__uint128_t)wb[wi] << avail; ++wi; avail += 64; }
                    uint64_t v = (uint64_t)(res & (((__uint128_t)1 << w) - 1));
                    res >>= w; avail -= w;
                    uint64_t ref; rd(&ref, 8, 1, bf);
                    if (v != ref) {
                        fprintf(stderr, "FATAL check_pieces: %s mismatch at run %llu (band %llu shard %u): piece %llu != bank %llu\n",
                                col ? "TAILS" : "HEADS",
                                (unsigned long long)(cursor + i), (unsigned long long)band, sh,
                                (unsigned long long)v, (unsigned long long)ref);
                        return 1;
                    }
                }
            }
            fclose(pf);
            cursor += rc;
            ++pi;
        }
    }
    if (pi != np) die("unconsumed pieces (band set mismatch?)", pieces[np - 1].name, np - pi);
    if (cursor != R) die("coverage != R", "pieces", cursor);
    if (fgetc(fh) != EOF || fgetc(ft) != EOF) die("bank columns longer than R", "bank", 0);
    fclose(fh); fclose(ft);
    printf("pieces=%zu bands=%llu runs=%llu w=%u COVERAGE_EXACT\n", np,
           (unsigned long long)nbands, (unsigned long long)cursor, w);
    return 0;
}
