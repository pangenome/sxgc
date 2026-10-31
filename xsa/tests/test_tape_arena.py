#!/usr/bin/env python3
"""Reservation flag failures must retry plain mmap before permitting shrink."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "runtime/tools/binbwt/scatter_sort.cpp"
HARNESS = r'''#include <sys/mman.h>
#include <cerrno>
#include <cassert>
static int mode = 0, calls = 0;
static void* checked_mmap(void* a, size_t n, int p, int f, int d, off_t o) {
    ++calls;
    if (mode == 2 || (mode == 1 && (f & MAP_NORESERVE))) {
        errno = mode == 1 ? EAGAIN : ENOMEM;
        return MAP_FAILED;
    }
    return mmap(a, n, p, f, d, o);
}
#define mmap checked_mmap
#define main scatter_sort_main
#include "SOURCE"
#undef main
#undef mmap
int main(int argc, char** argv) {
    mode = std::atoi(argv[1]);
    if (mode == 3) {
        // Real kernel ENOMEM: existing mappings count against RLIMIT_AS even
        // when untouched and MAP_NORESERVE is used. No large corpus is needed.
        struct rlimit lim{64u << 20, 64u << 20};
        assert(setrlimit(RLIMIT_AS, &lim) == 0);
        void* corpus = mmap(nullptr, 48u << 20, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(corpus != MAP_FAILED);
        assert(!tape_arena_reserve(32u << 20));
        assert(calls == 2);
        munmap(corpus, 48u << 20);
        assert(tape_arena_reserve(32u << 20));
        return 0;
    }
    assert(tape_arena_reserve(4096) == (mode != 2));
    assert(calls == (mode ? 2 : 1));
}
'''
with tempfile.TemporaryDirectory(prefix="tape-arena-test-") as tmp:
    cpp = pathlib.Path(tmp) / "test.cpp"
    exe = pathlib.Path(tmp) / "test"
    cpp.write_text(HARNESS.replace("SOURCE", str(SOURCE)))
    subprocess.run(["timeout", "900", "g++", "-O2", "-std=c++17", "-pthread",
                    str(cpp), "-o", str(exe), "-latomic"], check=True)
    for mode in range(4):
        run = subprocess.run(["timeout", "900", str(exe), str(mode)],
                             capture_output=True, text=True, check=True)
        log = run.stderr
        if mode == 0:
            assert "mechanism=anon fallback=0" in log
            assert "FALLBACK" not in log
        elif mode == 1:
            assert "errno=11 strerror=Resource temporarily unavailable" in log
            assert "SS_TAPE_ARENA_FALLBACK" in log
            assert "mechanism=anon fallback=1" in log
        else:
            assert log.count("SS_TAPE_ARENA_FAILED") == 2
            assert "errno=12 strerror=Cannot allocate memory" in log
            assert "flags=MAP_PRIVATE|MAP_ANONYMOUS errno=" in log
            assert ("SS_TAPE_ARENA bytes=" in log) == (mode == 3)
        print(f"mode={mode} PASS\n{log}", end="")
