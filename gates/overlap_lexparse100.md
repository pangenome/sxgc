# Segmented lexical front (default fixture entry only; 100 GB refused)

`overlap_lexparse100.cpp` is a separate front, not a new default or a change to
lexparse10. The executable rejects all but positive regular files of at most
10,000,000 bytes *before opening the input*. Run in a fresh private directory
on an ordinary filesystem with a stable, immutable source. Do not share or
modify its working files. Successful stdout is a census, not a product receipt.

```
g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_lexparse100.cpp -o /tmp/lex100
g++ -std=c++17 -O2 gates/overlap_lexparse10.cpp -o /tmp/lex10
python3 gates/overlap_lexparse100_test.py /tmp/lex100 /tmp/lex10
```

## Invariants and fit

* Two buffered sequential raw scans, u64 byte positions. At a lexparse10 FNV
  trigger strictly after position zero, emit through the eight trigger bytes,
  retaining those bytes as the next prefix. Emit the last word with END even
  on a final trigger. Partition ownership strips the repeated eight bytes
  from non-END words only. First scan proves `m <= UINT32_MAX` and word length
  <= 1 MiB, before writing occurrence records. `k <= m`; IDs use `0..k-1`,
  leaving a u32 missing-ID sentinel. No downstream rank is computed. Consumers
  using `m+1`, signed extents, dictionary positions or wide suffix ranks must
  separately check their arithmetic (notably `parse_extent=m+6k`).
* No sampled splitters, no fixed-capacity lexical buckets. Sort (tag,j,payload)
  by complete unsigned-byte payload, then END, ignoring j. Equal keys can span
  arbitrary input chunks and merge levels; all are consecutive in the final
  order. Assign one new zero-based ID per key change across the entire final
  stream. No hash is used to decide equality. A 64-way interval router groups
  (j,id) pairs by **text** interval, independent of key skew. At <=2^20 rows
  invert in RAM, detecting repeats and holes, and append the interval in order.
  Neither sort nor restoration materializes suffix rows, SA or raw positions.
* Disk ledger includes ALL logical file bytes in the new directory, including
  both final columns. Charge before write (even before buffered writes), fail
  without writing if `delta > 2N-live`. Only credit the actual size after
  writer and reader are closed and unlink succeeds. No sparse preallocation,
  outside temp files, or fixture allowance. Inductively live never exceeds 2N;
  failed or partial writes can overcharge, never undercharge. Consumed files
  may already be retired on stop: a refusal is evidence, not a resumable
  checkpoint. Preserve directory and stderr (including live/peak/cap on stop);
  require exit 0 **and** complete LEX100 line to accept. Input excluded from
  scratch, read-only. Physical blocks, inode costs, page cache and receipts
  are not logical bytes and need independent measurement/capacity planning.
* Let `S=N+16m-8` (sum of all occurrence records with header and overlap),
  `L=1MiB+8` (largest segment), `T=D+3k-1` (dictionary). Initial writes <=S;
  intermediate merge live <= S+16L: only bytes already copied from the one
  not-yet-retired segment per reader can duplicate live input. In final merge
  at most 4 extra bytes per consumed occurrence (8-byte assignment plus at
  most 4-byte tag and payload vs 8-byte input header and payload), so <=
  S+4m+16L. Single-chunk case needs no external runs. Restoration <=
  T+8m+8*ceil(m/64) for a routed root: at most one root child is temporarily
  copied before retirement; finished parse costs less than retired pairs. For
  a RAM-only root the final bound is T+4m. Bounds are sufficient, not required:
  the online ledger, not a presumed duplicate rate, decides. Some inputs
  cannot fit (even final outputs can exceed 2N); no fallback relaxes the cap.
* Census reports m,k,D and `descriptors` (sum of unique owned word lengths),
  with `D=descriptors+9k-7`, parse `4m` and dictionary `T` checked on close.
  Feed measured m,k,D to `overlap_hundred_preflight.projected` for downstream
  model terms; those estimates do not prove downstream fit or ordering.

## Cost and hard limits

Normal chunk <=16 MiB record arena + <=4 MiB u32 offsets (<=2^20 records).
At most 65536 initial runs (or stop), metadata only, fan-in 16 and one <=1 MiB
payload per reader. Split files at whole-record boundaries with target 1 MiB;
retire at segment EOF. No run-size limit tied to lexical mass. Router opens at
most 64 writers simultaneously (parents closed before recursion); final merge
also opens <=16 readers and the dictionary. Only bounded many streams and
buffers, <=4 MiB leaf IDs, no sample or corpus mapping. `--fixture-small`
shrinks chunk/offset/segment/leaf limits to 1024 B / 64 / 512 B / 64 IDs to
exercise real multi-level merges and routing. Single Linux thread pinned to one
allowed CPU; 192 MiB RLIMIT_AS; `mlockall(CURRENT|FUTURE)` or refuse, including
on fixtures. These process limits do not limit filesystem cache. A real gate
must additionally enforce/measure cgroup memory (192 MiB candidate), swap=0,
1 CPU and IO, not equate address-space bounds with total RAM used.

With R initial runs: initial sort O(m log(chunk rows)) comparisons, then
ceil(log16 R) merge levels for R>1, each O(m log 16) comparisons; a comparison
can examine the entire word. Logical run traffic up to S written and S read per
level (count initial write + final read as one level). Router depth at most 2
for u32 m and 2^20 leaves; up to 8m written and read per level, plus final
4m and T writes. Seek locality between segments/streams, actual IO, directory
costs, CPU time and cache behavior are unmeasured at scale. No timing claim.

For illustration only, 10 times the archived 10 GB `m` would give
3,181,644,880 occurrences at 100,000,000,000 bytes: `S` about 150.91 GB and
`S+4m+16L` about 163.65 GB (decimal). This is **not** a measured 100 GB fit;
the distribution, width checks and true output sizes can differ. Even if this
projection holds, multiple reads/writes of S may be prohibitive.

## Before a cold front

Keep 100 GB closed. Require independent review and published typed default /
10 GB product promotion evidence; validate exact fixture and skew/refusal
paths, measured intermediate-rung parity at matching verification and phase
boundaries, IO amplification and resource/time envelope. Only the parent may
approve a separate fail-closed gate: verify pinned input manifest/environment,
immutable source, exact rung, free scratch (including filesystem overhead),
CPU/memory/no-swap cgroup, wall/physical read/write and stop guards, cold-cache
conditions, and capture receipts without destroying evidence. No such gate is
supplied for 100 GB. For supervised intermediate studies only, see the
[1/10 GB qualification gates](overlap_segmented_studies.md). Success of a cold front would establish exact counts only, not a
bounded product, suffix-order proof, performance promotion or larger launch.
