# Fresh-source SXI build

Build tools (no commits and no upstream edits):

```sh
bash tools/build_rpfbwt_tap.sh /home/erikg/r-pfbwt /tmp/rpfbwt-sxgc
bash tools/build_sxi_tools.sh /tmp/laneV/tools
xsa/target/release/xsa build --text collection.txt -o collection.sxi \
  --threads 8 --scratch /path/on/source/filesystem --verbose
xsa/target/release/xsa build --agc collection.agc -o collection.sxi --verbose
```

The copy destination must not exist. `build_rpfbwt_tap.sh` copies upstream
including the cached dependency sources, applies `patches/rpfbwt_emit_tails.patch`,
and uses its own CMake build at `/tmp/rpfbwt-sxgc/build-sxgc/rpfbwt`.
The patch changes only the merge's endpoint-selection/range filter and adds
`.ssa_t` output (`u64 count`, then `count` native-endian u64 values, little-endian
on this host). It leaves `.ssa` writes unchanged. The tail merge skips empty
temporary streams to avoid setting the output failbit. Upstream's unchanged
head merge has that empty-stream bug; builds smaller than 1 MB use one chunk,
while large builds use 50. Both choices are printed with `--verbose`.

`XSA_PFP`, `XSA_RPFBWT`, `XSA_TOOLS`, `XSA_AGC2FLAT`, and `XSA_PIPELINE`
override tool paths. The Rust launcher otherwise locates the Python script
relative to the compile-time repository path; shipping a standalone binary
requires installing the script/tools and setting these paths.

Exact stages:

1. AGC only: committed `agc2flat --revlines --upper -o SCRATCH/collection.txt`.
   It streams one reversed contig per line and writes `collection.txt.names.tsv`.
   The text is materialized on the same filesystem as the archive.
2. `pfp++ -t TEXT -o PREFIX -w 10 -p 100 -j THREADS --tmp-dir SCRATCH`.
3. `pfp++ -i PREFIX.parse -w 5 -p 11 -j THREADS --tmp-dir SCRATCH` (second level).
4. Patched `rpfbwt --l1-prefix PREFIX --w1 10 --w2 5 --threads THREADS
   --chunks CHUNKS --tmp-dir SCRATCH`.
5. `rpfbwt_endpoints PREFIX fresh.ri4 fresh.head_sa`: bounded-buffer O(r)
   normalization, remove ten padding rows, remap byte 2 to newline, coalesce
   adjacent normalized runs, convert tails to `n-1-SA`, pack the existing v4
   transport. The `.ssa` and `.ssa_t` source files are retained untouched.
6. Committed `slim_dump --slim --resolve-ri4 --dict-stream --ri4 fresh.ri4
   --head-sa fresh.head_sa --parse PREFIX -t THREADS -o fresh.agg`.
   Heads/tails resolve directly; no new Phi, M/b_bwt/w_wt artifacts or LF walks.
   rpfbwt retains its own existing internal second-level structures.
7. `xsa chi-rspace --stream-agg --ri4 fresh.ri4 --agg fresh.agg -o fresh.sA`.
   Gate logged count against file length; enforce `--expect-chi` if supplied.
8. Committed `sxi_write` consumes these fresh endpoint inputs plus chi. It
   checks sample ranges/singletons, run counts, chi range/uniqueness and CRCs.
   `xsa sxi-info` validates the candidate before atomic no-clobber publication.

Each subprocess has its own `/usr/bin/time -v` file and stage log under
`bit6/sxi_logs` (override `--log-dir`). The JSONL journal records argv, return
code, wall seconds, and stage peak RSS. Stages run sequentially with a 149 GB
address-space ceiling. Scratch is intentionally retained for independent
review. Failed stages or gates never publish the requested output path.

`--expect-heads RAW` and `--expect-ri4 FILE` are acceptance-only comparisons,
run **after** constructing fresh endpoints. The latter checks the entire
run table and packed tails. Neither oracle supplies construction inputs.

Collection text must already have the pilot orientation and end in newline.
Do not replace inter-contig newlines by `!` or use the old single-string pilot
as a collection oracle. PFP's suffix order over concatenated newline text can
still differ from BCR's independent-string ordering; endpoint gates must
establish equivalence for each collection. The adapter does not repair that
ordering. For more than one string, both `--expect-heads` and `--expect-ri4`
must pass; otherwise publication is refused. General oracle-free multi-contig
AGC construction is therefore still unavailable with this front end.
`--fasta` is recognized but rejected with a conversion instruction because
its orientation/separator contract is unspecified by this task.

Reproduction tests:

```sh
python3 bit6/test_endpoint_tap.py
python3 bit6/test_sxi_build.py
python3 bit6/test_source_battery.py --work /tmp/laneV/new-battery
```

The tiny suffix-array oracle in `test_endpoint_tap.py` is test-only; no such
algorithm is used by the production pipeline. The legacy Phi extractor and
the existing SXI format remain unchanged.
