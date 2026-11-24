# Resident dictionary sort: opt-in OpenMP

Default build and runtime are serial; no OpenMP headers or runtime needed.
To build the alternative producer on 64-bit Linux with GCC, `omp.h` and
libgomp (runtime loader must find `libgomp.so.1`):

```sh
XSA_BUILD_DICT_OPENMP=1 CC=gcc cargo build -j1 --locked --release \
  --manifest-path xsa/pfp/Cargo.toml --bin overlap_resident
```

This switch accepts only `0`/`1`; Cargo rebuilds the native archive on change.
It compiles both pinned C objects with `-fopenmp -DLIBSAIS_OPENMP`, links
`gomp`, and enables the matching Rust FFI. This is not a new parallel parse or
recursive integer residual API: callers of `libsais_int` still pass through
the serial entry point. Use the **non-OpenMP build** for the incumbent baseline.

At launch set, e.g., `XSA_RESIDENT_DICT_THREADS=2 OMP_NUM_THREADS=2
OMP_THREAD_LIMIT=2 OMP_DYNAMIC=FALSE OMP_WAIT_POLICY=PASSIVE GOMP_SPINCOUNT=0`.
These are process environment variables, not command arguments. Leave the
separate wide dictionary and radix switches at the values appropriate to the
arm. The caller must choose a thread request within its concurrent CPU grant,
accounting for other pipeline processes. There is no core reservation.

- Unset or `1`: serial entry point, no additional array grant. Explicit `2..8`
  requires an OpenMP build; invalid settings fail in planning before row output.
- Selected threads = min(request, Rust available parallelism, OpenMP max
  threads, OpenMP thread limit); do not pass upstream's auto (`0`). `1` after
  clamping also uses the serial API. The region runs synchronously before the
  radix phase; passive waiting prevents idle workers spinning during emit.
- `RESIDENT_DICT_SORT` on stderr reports the selected argument, not a measured
  team size for every loop: upstream serializes some loops on small inputs,
  short subproblems or dynamic teams. Set `OMP_DYNAMIC=FALSE` for comparisons.
- Narrow `libsais_omp`: signed i32 n, SA, fs, freq elements, threads and return.
  Wide `libsais64_omp`: the same fields are i64. Both use the original byte
  dictionary, `fs=0`, null freq and require status 0; no retry on failure.
  The wide API internally sorts with narrow coordinates and expands **in the
  existing wide output** for `1 < n <= INT32_MAX`. Only a larger dictionary
  exercises the native 64-bit sort. Lengths 0 and 1 return directly.

For `n > 1`, `t > 1`, the added dictionary-phase array grant and the logged
`thread_workspace_bytes` are the pinned allocator's additional requested bytes:
`t * (64 + (4*256 + 2*24576)*w) + 3*4097`, `w=4` up to `INT32_MAX`, else `8`.
This covers the three aligned allocations for thread state, buckets and pairs
of cached indices (including their malloc alignment padding). Otherwise add
zero. Original SA size, existing 64 MiB dictionary allowance, and other phase
array bounds are unchanged. **Not an RSS or address-space bound**: allow
separate headroom for worker stacks (`OMP_STACKSIZE`), runtime pool lifetime,
allocator and page rounding; measure those on any separately approved run.

## Small verification only

Run with a two-CPU grant (OpenMP environment as above):

```sh
XSA_BUILD_DICT_OPENMP=1 CC=gcc cargo test -j1 --locked \
  --manifest-path xsa/pfp/Cargo.toml --bin overlap_resident -- --test-threads=1
```

Repeat without `XSA_BUILD_DICT_OPENMP` for the default build. In
`index::tests::dict_sort_exact_order` an independent permutation and
head/successor-rank recurrence proves the entire SA; the two output widths
agree at every rank and parallel/serial arrays at the same width match
exactly. Fixtures include 65,536 equal bytes, 96,000 periodic bytes, and
131,073 seeded random bytes, i.e. not just the serial small-input shortcut.
The width-boundary allocation test is arithmetic only. These tests do not
certify native-wide output, throughput, RSS, IO savings or any larger rung.
