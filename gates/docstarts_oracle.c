/* gates/docstarts_oracle.c — a THIRD, independent decoder of the document map.
 *
 * Reads an XSEP v1 seps.bin and a positions file (u64 LE array), and prints one
 * PROBE line per position:
 *
 *   PROBE pos=<p> docid=<d> start=<s> depth=<p-s>
 *
 * in exactly the format `xsa docstarts probe` prints, so a gate can diff the
 * two outputs byte for byte. No container, no Rust, no code shared with the
 * member decoder: the anchor table is bisected and the u32 delta stream is
 * scanned with plain C, written from the seps.bin layout description in
 * RESULTS_PILE_DOCMAP.md / xsa/src/pilemap.rs.
 *
 *   usage: docstarts_oracle SEPS_BIN POSITIONS_U64LE
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t *map;
static uint64_t map_len;

static uint32_t u32at(uint64_t o) {
    uint32_t v;
    memcpy(&v, map + o, 4);
    return v;
}
static uint64_t u64at(uint64_t o) {
    uint64_t v;
    memcpy(&v, map + o, 8);
    return v;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: docstarts_oracle SEPS_BIN POSITIONS_U64LE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    map_len = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    map = malloc(map_len);
    if (!map || fread((void *)map, 1, map_len, f) != map_len) {
        fprintf(stderr, "docstarts_oracle: short read on %s\n", argv[1]);
        return 2;
    }
    fclose(f);
    if (map_len < 48 || memcmp(map, "XSEP", 4) || u32at(4) != 1) {
        fprintf(stderr, "docstarts_oracle: %s is not an XSEP v1 map\n", argv[1]);
        return 2;
    }
    const uint64_t stride = u32at(12);
    const uint64_t docs = u64at(16);
    const uint64_t corpus_bytes = u64at(24);
    const uint64_t anchors = u64at(32);
    const uint64_t base = u64at(40);
    if (stride == 0 || docs == 0 || base != 0 ||
        anchors != (docs + stride - 1) / stride ||
        map_len != 48 + 4 * docs + 8 * anchors) {
        fprintf(stderr, "docstarts_oracle: %s fails the XSEP v1 layout check\n", argv[1]);
        return 2;
    }
    const uint64_t deltas_off = 48;
    const uint64_t anchors_off = 48 + 4 * docs;

    FILE *pf = fopen(argv[2], "rb");
    if (!pf) { perror(argv[2]); return 2; }
    uint64_t pos;
    while (fread(&pos, 8, 1, pf) == 1) {
        if (pos >= corpus_bytes) {
            fprintf(stderr, "docstarts_oracle: position %llu past the map's corpus\n",
                    (unsigned long long)pos);
            return 2;
        }
        /* largest anchor <= pos: binary search over the anchor table */
        uint64_t lo = 0, hi = anchors;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (u64at(anchors_off + 8 * mid) <= pos) lo = mid + 1;
            else hi = mid;
        }
        uint64_t block = lo - 1;
        uint64_t doc = block * stride;
        uint64_t start = u64at(anchors_off + 8 * block);
        /* walk the deltas forward: delta[i] = start[i] - start[i-1] */
        while (doc + 1 < docs) {
            uint64_t next = start + u32at(deltas_off + 4 * (doc + 1));
            if (next > pos) break;
            start = next;
            doc++;
        }
        printf("PROBE pos=%llu docid=%llu start=%llu depth=%llu\n",
               (unsigned long long)pos, (unsigned long long)doc,
               (unsigned long long)start, (unsigned long long)(pos - start));
    }
    fclose(pf);
    return 0;
}
