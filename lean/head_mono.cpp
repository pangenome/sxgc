// Offline measurement: are run head positions monotone along the BWT run stream?
// Decodes the saved raw rlebwt (deterministic emit) and walks the SA tape.
// usage: head_mono <rlebwt> <sa_tape> [k]
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;

static void die(const char* m) { fprintf(stderr, "FATAL %s\n", m); exit(1); }

int main(int argc, char** argv) {
    if (argc < 3) die("usage: head_mono <rlebwt> <sa> [k]");
    const u64 k = argc > 3 ? strtoull(argv[3], nullptr, 10) : 64;
    // --- SA tape
    int sfd = open(argv[2], O_RDONLY);
    if (sfd < 0) die("open sa");
    struct stat sst{}; if (fstat(sfd, &sst)) die("stat sa");
    u8* smap = (u8*)mmap(nullptr, sst.st_size, PROT_READ, MAP_PRIVATE, sfd, 0);
    if (smap == MAP_FAILED) die("mmap sa");
    u32 magic; memcpy(&magic, smap, 4);
    if (magic != 0x41535853u) die("sa magic");
    u64 n; memcpy(&n, smap + 8, 8);
    if (u64(sst.st_size) != 48 + 8 * n) die("sa size");
    const u64* sa = (const u64*)(smap + 48);
    // --- rlebwt
    int rfd = open(argv[1], O_RDONLY);
    if (rfd < 0) die("open rlebwt");
    struct stat rst{}; if (fstat(rfd, &rst)) die("stat rlebwt");
    u8* rmap = (u8*)mmap(nullptr, rst.st_size, PROT_READ, MAP_PRIVATE, rfd, 0);
    if (rmap == MAP_FAILED) die("mmap rlebwt");

    u64 row = 0, runs = 0, rows = 0;
    u64 back = 0, fwdSteps = 0, maxFwd = 0, reentrySteps = 0, anchorSteps = 0;
    u64 carriedPos = 0, have = 0, tailBack = 0, tailFwd = 0;
    u64 firstHead = 0;
    u64 sampleEvery = 1;
    u64 checked = 0;
    // single pass over words
    u64 pos = 0, len = rst.st_size;
    const u64 W1 = 10;   // padding rows
    while (pos + 4 <= len) {
        u32 w; memcpy(&w, rmap + pos, 4); pos += 4;
        u8 c = u8(w & 255);
        u64 L = (w >> 8) & 0x7fffff;
        while (w >> 31) {
            memcpy(&w, rmap + pos, 4); pos += 4;
            if ((w & 255) != c) die("continuation char");
            L += (w >> 8) & 0x7fffff;
        }
        ++runs;
        u64 r0 = row;
        row += L;
        if (row > n + 10) die("row overflow");
        if (r0 < W1) continue;                    // padding prefix
        // this run's head position = sa[r0 - 10]; tail = sa[r0 - 10 + L - 1]
        u64 h = sa[r0 - 10];
        u64 t = sa[r0 - 10 + L - 1];
        if (r0 + L - 10 > n) continue;             // trailing straddle
        if (!have) { firstHead = h; carriedPos = h; have = 1; }
        else {
            if (h < carriedPos) {
                ++back;
                u64 a = h - (h % k);
                reentrySteps += (h - a);           // walk up from the anchor
                carriedPos = a;                    // re-entered at the anchor
            }
            u64 step = h - carriedPos;
            fwdSteps += step;
            if (step > maxFwd) maxFwd = step;
            carriedPos = h;
        }
        // tails, with the same carried cursor (tail query right after head)
        if (t < carriedPos) ++tailBack;
        else { tailFwd += t - carriedPos; carriedPos = t; }
        anchorSteps += h % k;                      // the per-query alternative
        ++checked;
        (void)c;
        (void)sampleEvery;
    }
    if (row != n + 10) { fprintf(stderr, "row=%llu n=%llu\n", (unsigned long long)row, (unsigned long long)n); die("row coverage"); }
    fprintf(stderr, "HEAD_MONO n=%llu runs=%llu raw_runs=%llu checked=%llu k=%llu\n",
            (unsigned long long)n, (unsigned long long)runs, (unsigned long long)runs,
            (unsigned long long)checked, (unsigned long long)k);
    fprintf(stderr, "HEAD_MONO backwards=%llu (%.4f%%) carried_fwd_steps=%llu max_fwd_gap=%llu "
            "reentry_steps=%llu total_carried=%llu (%.3fn)\n",
            (unsigned long long)back, 100.0 * double(back) / double(checked),
            (unsigned long long)fwdSteps, (unsigned long long)maxFwd,
            (unsigned long long)reentrySteps,
            (unsigned long long)(fwdSteps + reentrySteps),
            double(fwdSteps + reentrySteps) / double(n));
    fprintf(stderr, "HEAD_MONO tail_backwards=%llu tail_fwd_steps=%llu anchor_query_steps=%llu (%.1f per run avg)\n",
            (unsigned long long)tailBack, (unsigned long long)tailFwd,
            (unsigned long long)anchorSteps, double(anchorSteps) / double(checked));
    fprintf(stderr, "HEAD_MONO first_head=%llu\n", (unsigned long long)firstHead);
    return 0;
}
