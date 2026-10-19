// cold_cache: drop a file's clean pages from the page cache and PROVE it.
//
// PART C's trick: at 1 GB the page cache warms the whole corpus, so a warm arm
// says nothing about the pile regime. This tool posix_fadvise(DONTNEED)s the
// whole file, then samples residency twice - once by mincore() over a fresh
// read-only mapping, once from /proc/meminfo Cached - and prints both, so the
// "cold" claim is measured, never assumed. Advisory only: it cannot change a
// byte of the file, which is why every byte gate is unaffected by construction.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static long meminfo_kb(const char* field) {
    FILE* f = fopen("/proc/meminfo", "r"); if (!f) return 0;
    char line[256]; long v = 0; const size_t n = strlen(field);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, field, n) && line[n] == ':') { v = atol(line + n + 1); break; }
    fclose(f); return v;
}
// mincore the whole file through one read-only mapping, stride 256 KiB.
static void resident(const char* path, const char* label) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open %s errno=%d %s\n", path, errno, strerror(errno)); return; }
    struct stat st{};
    if (fstat(fd, &st) || st.st_size <= 0) { close(fd); return; }
    const size_t page = 4096, stride = 64 * page;
    void* p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { fprintf(stderr, "mmap errno=%d %s\n", errno, strerror(errno)); close(fd); return; }
    unsigned char vec[1];
    size_t sampled = 0, res = 0;
    for (size_t off = 0; off < (size_t)st.st_size; off += stride) {
        if (mincore((char*)p + off, page, vec) == 0) { ++sampled; res += vec[0] & 1; }
    }
    printf("COLD label=%s bytes=%llu sampled=%zu resident=%zu resident_pct=%.2f "
           "meminfo_cached_gb=%.2f meminfo_free_gb=%.2f\n",
           label, (unsigned long long)st.st_size, sampled, res,
           sampled ? 100.0 * (double)res / (double)sampled : 0.0,
           meminfo_kb("Cached") / (1024.0 * 1024.0), meminfo_kb("MemFree") / (1024.0 * 1024.0));
    munmap(p, (size_t)st.st_size);
    close(fd);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s FILE [FILE...]\n", argv[0]); return 1; }
    for (int i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "open %s errno=%d %s\n", argv[i], errno, strerror(errno)); return 2; }
        const double t0 = now();
        if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0) {
            fprintf(stderr, "fadvise %s errno=%d %s\n", argv[i], errno, strerror(errno));
            return 2;
        }
        const double dt = now() - t0;
        printf("COLD_DROP path=%s wall=%.3f\n", argv[i], dt);
        close(fd);
        resident(argv[i], "after_drop");
    }
    return 0;
}
