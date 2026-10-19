// PART A's two I/O probes. One binary, two arms, no cleverness:
//
//   scratch_io_probe write <path> <GiB>    the WINDOW-FILL shape: N interleaved
//                                          bin streams, 128 KiB writes, the same
//                                          pattern BinFillSink::flush produces -
//                                          buffered pwrite vs O_DIRECT pwrite vs
//                                          buffered + periodic DONTNEED.
//   scratch_io_probe read  <path> <MiB>    the SWEEP's shape: sequential 1 MiB
//                                          preads vs scattered 4 KiB preads in
//                                          random order vs the SAME scattered
//                                          4 KiB reads in ASCENDING order (what
//                                          the merged monotone front issues).
//
// Every line carries the load average it ran under, because this box is shared:
// a number without its load is a rumour. Bytes are always read/written in full;
// a short read/write is a hard error (the probes are allowed to fail, not to lie).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static double load1(void) { double v = -1; FILE* f = fopen("/proc/loadavg", "r"); if (f) { fscanf(f, "%lf", &v); fclose(f); } return v; }
static long meminfo_kb(const char* field) {
    FILE* f = fopen("/proc/meminfo", "r"); if (!f) return 0;
    char line[256]; long v = 0; const size_t n = strlen(field);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, field, n) && line[n] == ':') { v = atol(line + n + 1); break; }
    fclose(f); return v;
}
static void xwrite(int fd, const void* p, size_t len, off_t off) {
    const char* q = (const char*)p;
    while (len) {
        ssize_t z = pwrite(fd, q, len, off);
        if (z <= 0) { fprintf(stderr, "short pwrite errno=%d %s\n", errno, strerror(errno)); exit(2); }
        q += z; off += z; len -= (size_t)z;
    }
}
static size_t xread(int fd, void* p, size_t len, off_t off) {
    char* q = (char*)p; size_t got = 0;
    while (len) {
        ssize_t z = pread(fd, q, len, off);
        if (z < 0) { fprintf(stderr, "pread errno=%d %s\n", errno, strerror(errno)); exit(2); }
        if (z == 0) break;
        q += z; off += z; len -= (size_t)z; got += (size_t)z;
    }
    return got;
}

// ---- the window-fill shape: 256 interleaved streams, 128 KiB writes ----------
static int probe_write(const char* path, double gib) {
    const size_t total = (size_t)(gib * (1ull << 30));
    const size_t zones = 256, zoneBytes = total / zones, chunk = 128u << 10;
    void* buf = NULL;
    if (posix_memalign(&buf, 1u << 21, chunk)) return 1;
    memset(buf, 0xa5, chunk);
    for (int arm = 0; arm < 3; ++arm) {
        const int flags = O_RDWR | O_CREAT | O_TRUNC | (arm == 1 ? O_DIRECT : 0);
        int fd = open(path, flags, 0644);
        if (fd < 0) { fprintf(stderr, "arm=%d open errno=%d %s\n", arm, errno, strerror(errno)); return 2; }
        if (posix_fallocate(fd, 0, (off_t)total) != 0) { fprintf(stderr, "fallocate failed\n"); return 2; }
        const double t0 = now();
        size_t done = 0;
        for (size_t off = 0; off + chunk <= zoneBytes && done < total; off += chunk) {
            for (size_t z = 0; z < zones && done < total; ++z) {
                xwrite(fd, buf, chunk, (off_t)(z * zoneBytes + off));
                done += chunk;
            }
            // The DONTNEED arm: the fill's own chunks are dead once written and
            // the corpus is the tier the cache is for. Release the zones already
            // written every 256 MiB of progress.
            if (arm == 2 && (off % (256u << 20)) == 0 && off)
                posix_fadvise(fd, 0, (off_t)(off + chunk), POSIX_FADV_DONTNEED);
        }
        const double dt = now() - t0;
        printf("WRITE arm=%s scattered_128k bytes=%zu wall=%.3f gb_s=%.2f cached_gb=%.1f load1=%.1f\n",
               arm == 0 ? "buffered" : (arm == 1 ? "odirect" : "buffered_dontneed"),
               done, dt, done / 1e9 / dt, meminfo_kb("Cached") / (1024.0 * 1024.0), load1());
        // The fsync is what the window's seal pays in BOTH arms; report it apart.
        const double ts = now();
        if (fsync(fd) != 0) { fprintf(stderr, "fsync errno=%d\n", errno); return 2; }
        printf("WRITE_SYNC arm=%s wall=%.3f\n", arm == 0 ? "buffered" : (arm == 1 ? "odirect" : "buffered_dontneed"), now() - ts);
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close(fd);
    }
    free(buf);
    unlink(path);
    return 0;
}

// ---- the sweep's read shape -------------------------------------------------
static int probe_read(const char* path, double mib) {
    const size_t total = (size_t)(mib * (1ull << 20));
    const size_t page = 4096, seqChunk = 1u << 20;
    void* buf = NULL;
    if (posix_memalign(&buf, 1u << 21, seqChunk)) return 1;
    size_t npages = total / page;
    uint64_t* order = (uint64_t*)malloc(npages * sizeof *order);
    if (!order) return 1;
    for (size_t i = 0; i < npages; ++i) order[i] = i;
    // A cheap deterministic shuffle: xorshift over the index vector.
    uint64_t s = 0x9e3779b97f4a7c15ull;
    for (size_t i = npages; i > 1; --i) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        size_t j = (size_t)(s % i);
        uint64_t t = order[i - 1]; order[i - 1] = order[j]; order[j] = t;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open read errno=%d %s\n", errno, strerror(errno)); return 2; }
    // Arm 0: drop the file from the cache, then read it SEQUENTIALLY in 1 MiB
    // chunks - the merged front's shape (one ascending pass).
    for (int arm = 0; arm < 3; ++arm) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        const double t0 = now();
        size_t readBytes = 0;
        if (arm == 0) {
            for (off_t off = 0; (size_t)off < total; off += seqChunk)
                readBytes += xread(fd, buf, (size_t)((size_t)off + seqChunk <= total ? seqChunk : total - (size_t)off), off);
        } else {
            // Arm 1: scattered pages in a random permutation (the lazy arm's
            // demand faults, which follow the SA, not the file layout).
            // Arm 2: the SAME scattered pages in ascending order (a sorted,
            // uncoalesced front: skip-chunked but 4 KiB each).
            if (arm == 1) {
                for (size_t i = 0; i < npages; ++i)
                    readBytes += xread(fd, buf, page, (off_t)(order[i] * page));
            } else {
                for (size_t i = 0; i < npages; ++i)
                    readBytes += xread(fd, buf, page, (off_t)(i * page));
            }
        }
        const double dt = now() - t0;
        printf("READ arm=%s bytes=%zu wall=%.3f gb_s=%.2f cached_gb=%.1f load1=%.1f\n",
               arm == 0 ? "sequential_1m" : (arm == 1 ? "scattered_4k_random" : "scattered_4k_ascending"),
               readBytes, dt, readBytes / 1e9 / dt, meminfo_kb("Cached") / (1024.0 * 1024.0), load1());
    }
    close(fd);
    free(order); free(buf);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s write|read PATH GIB_OR_MIB\n", argv[0]);
        return 1;
    }
    const double sz = atof(argv[3]);
    if (!strcmp(argv[1], "write")) return probe_write(argv[2], sz);
    if (!strcmp(argv[1], "read")) return probe_read(argv[2], sz);
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 1;
}
