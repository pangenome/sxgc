# 466 dictionary-SA failure: address-space ceiling, not a 32-bit wrap

The copied-input reproduction proves the first failure is a legitimate allocation
denied by `RLIMIT_AS=149000000000`. No 32-bit repair to r-pfbwt is justified by
this failure. The original executable remains unchanged.

## Evidence

- Retained dictionary file: 18,611,972,661 bytes, including phrase delimiters.
  The parser's 18,522,063,705 count excludes those delimiters.
- After dollar padding, SA has 18,611,972,670 entries of eight bytes each.
- Traced failing allocation: **148,895,781,360 bytes**, `malloc` returns null,
  errno 12, inherited address-space limit **149,000,000,000 bytes**.
- Dictionary vector capacity is already 37,223,945,322 bytes; phrase-boundary
  bitvector/rank/select and other mappings also occupy address space.
- `bit6/sxi_pipeline.py` sets that hard limit before launching child stages.
  `xsa/runtime/bit6/sxi_pipeline.py` ships the same behavior.
- At 850 GB the identical allocation returns a non-null pointer; see
  `full466-rpfbwt.log`. The executable SHA256 is unchanged:
  `588ea73817f6513dc1467161f93fc550e2968a899aeeba853657f7934a649242`.
- Reproduction: `repro-149gb.log` and `repro-149gb.time`, exit 134,
  wall 2:06.71, peak RSS 36,347,904 KiB (37.22 decimal GB).
- Original failure timing was **1:31.36 = 91.36 seconds**, not 1 hour 31 minutes.
  Original peak was 36,354,048 KiB, not an allocation-size measurement.

## Width audit

In the fork's PFP-DS dependency, `utils.hpp` defines `long_type=uint64_t`.
`read_file` uses `stat.st_size` and `size_t`; dictionary `d.size()` feeds
`vector<long_type>` directly. SA/LCP/ISA/DA loops and phrase offsets use
`long_type`; dictionary SA storage chooses five bytes at this size.
`gsacak_templated` takes 64-bit length and SA pointers. CMake's propagated
`-DM64` makes gSACA-K's `uint_t`/`int_t` 64-bit in both caller and library.
The failure occurs before gSACA-K is entered.
The "Using 8 bytes" message is unconditional in dictionary construction; it
does not itself establish a size-dependent width-selection event.

Remaining 32-bit fields represent phrase/alphabet IDs, not dictionary offsets:
the parse elements, colex phrase IDs and gSACA-K top-level alphabet size.
89,908,955 first-level phrases fit those fields. Integer dictionary level two
contains 2,294,697,296 symbols, and its parse contains 1,357,077,877 IDs; their
lengths are also propagated as 64-bit values. Prefix-dollar counters are `int`
but count the bounded window padding, not dictionary length.

The retained k10 `h10ss_pfp.dict` is 5,483,754,416 bytes, also above 2^32.
Its accepted frontend log reports five-byte SA storage and successful completion.
The currently running k10 comparison uses that complete retained parse/output
set; `h10rl_pfp` has first-level inputs but no retained level-two files.

## Repair and validation status

The concrete repair adds `xsa build --address-space-gb N`, retaining the 149 GB
default and respecting lower inherited hard limits. The effective limit is
journaled. Validation uses an 850 GB ceiling and the identical r-pfbwt binary.
The release build passed. Three real tiny builds (default 149 GB, explicit
850 GB, and inherited 1 GB) produced identical indexes with chi=1401;
invalid limits were rejected. See `memory-limit-tests.log`.

Yeast235 frontend PASS: `.rlebwt`, metadata, `.ssa`, and `.ssa_t` are all
byte-identical to retained accepted outputs. Wall 10:02.11; peak RSS
8,820,112 KiB. See `regression-gates.jsonl` and `yeast-rpfbwt.time`.
The user-requested upstream 64-bit patch is not fabricated: no relevant wrap
has been demonstrated.

Full validation runs from independent copies in `/tmp/rpfbwt-64-466`, with
timed frontend/endpoints/slim/sweep/audit/write/validate stages. It is explicitly
**validation only**, not the from-zero milestone. Final gates and chi remain
pending until `validation.jsonl` records PASS. The driver never edits retained
inputs or upstream r-pfbwt, and no commit is created.

The requested `contact_supervisor` tool is absent from the available tool
catalog; progress is recorded here and in the task thread instead.
