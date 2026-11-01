# Chi study — pinned to 6dd06ef6e141a03522c5929d1e411cca468c7c01

Read-only product study; isolated `study/chi-structure` worktree. No corpus builds or product edits.
Verdict: **m5 is a position set, not a bounded-context → first-rank map.** 256 reusable slots do not bound total contexts.

## 1. Structure and exact production rule

- Mathematical χ is the minimum suffixient-set cardinality: cover every occurring extension `wc` of every right-maximal `w` by an ending position (`lean/Sxgc.lean:33`, `:53`, `:68`, `:73`, `:91`). Context length is unrestricted.
- Distinguish χ (count) from S (selected witness set). m5 stores S sorted, unique; it stores **no context, symbol, LCP, or SA rank** (`bit6/SXI_FORMAT.md:35`, `:39`; `bit6/xsa_finish.cpp:6289`).
- Literal set key = witness coordinate x; value = membership. EF's implicit ordinal i selects x_i. Universe: `[0,n]` in the format; cyclic finish emits `[0,n)` (`bit6/SXI_FORMAT.md:39`; `bit6/xsa_finish.cpp:2399`, `:2666`).
- `witness(sa) = (sa == 0 ? 0 : n-sa)` cyclically; noncyclic branch is `bigN-sa`. Thus **x is not rank**; run endpoints `v.h`, `v.t` are SA/text positions (`bit6/xsa_finish.cpp:2399`, `:2499`, `:2514`).
- Sweep state: `rr_len[c]`, `rr_pos[c]`, `rr_act[c]`, plus preceding character/end position and intervening LCP minimum (`bit6/xsa_finish.cpp:2342`, `:2389`).
- At a character change: `m3=min(previous minima,topLcp)`; emit active slots with `rr_len[c]>m3`, lower/reset them; install/replace each boundary candidate iff `topLcp>rr_len[c]` (`bit6/xsa_finish.cpp:2438`, `:2465`, `:2505`).
- Replacement overwrites an un-emitted candidate; EOF flushes active slots with threshold -1. Each actual `emit` increments χ; it is **not** a distinct-context insertion counter (`bit6/xsa_finish.cpp:2404`, `:2514`, `:2523`).
- EF: `l=max(0,floor(log2((n+1)/χ)))`; low bits = χl; high bits = `(n>>l)+χ+1`; 24-byte header plus separately byte-rounded vectors. Empty set: both bit counts zero (`bit6/xsa_finish.cpp:6168`, `:6284`; `xsa/src/sxi2.rs:107`).
- Writer sorts positions and rejects duplicates/non-increasing values; it does not deduplicate incorrect emissions (`bit6/xsa_finish.cpp:6174`, `:6267`; `bit6/SXI_FORMAT.md:43`).

## 2. Reader inventory and absence effects

| Rust source | Actual consumption |
|---|---|
| `xsa/src/sxi.rs:208`, `:316`, `:345`, `:388` | Count/shape checks, full decode/validation, `sxi-info --chi-out`, member statistics. |
| `xsa/src/sxi2.rs:107` | EF decoder; emits sorted u64 positions, validates order/range/count. |
| `xsa/src/main.rs:85`, `:818`, `:1296` | `stats`: embedded count or raw `.sA` file size; `tags`: raw positions → record/forward-position tags. |
| `xsa/src/witness.rs:24`, `:107` | Sidecar load checks m5 count/CRC; build decodes positions into bitmap, maps onto run-edge slots. |
| `xsa/src/product.rs:69`, `:292`, `:535`, `:1046` | Engine metadata count; `--first --witness-index` selects a chi run edge; `witness-build`. |
| `xsa/src/forks.rs:478`, `:628`, `:638` | Decode/filter witness positions; match BWT run edges for fork extraction. |
| `xsa/src/qlite.rs:18`, `:484` | **No m5 payload read/decode.** Only directory-presence/flag consistency; exact count/locate use other members. |

| C++ source | Actual consumption |
|---|---|
| `bit6/sxi_format.hpp:54`, `:71`, `:74` | SXI1 mandatory-m5 count/size checks and generic CRC byte read; not a semantic chi decoder. |
| `bit6/sxi2_write.cpp:38`, `:423` | Convert delta chi → EF; read exported decoded chi and compare every value. |
| `bit6/sxi_write.cpp:45` | Read raw `.sA`, range-check/sort/reject duplicates, encode member 5. |
| `bit6/sxi_text_audit.cpp:90` | Read/sample raw chi witnesses for text audit. |
| `bit6/xsa_finish.cpp:6141`, `:6162` | Read banked `.sA` count/samples; consume generated witness buckets into m5. |
| `bit6/chi_rspace_dump.cpp:106`, `:211`, `:544`; `bit6/test_sxi_run_width.cpp:19` | Indirect SXI1 Container validation/CRC only; no chi algorithm consumer. |

- Packaged duplicates: `xsa/runtime/bit6/{sxi_format.hpp:71,sxi_write.cpp:45,sxi_text_audit.cpp:90,xsa_finish.cpp:6141,chi_rspace_dump.cpp:106}` carry the same reader paths.
- Archived readers also searched: `bit6/sxi_logs/query-product/baseline/{sxi.rs:244,main.rs:661,sxi_format.hpp:45,sxi_write.cpp:44,chi_rspace_dump.cpp:104}`; `bit6/sxi_logs/rope-compat/baseline/product.rs:68`; `bit6/sxi_logs/sep-convention/repair2/baseline/main.rs:661`; `bit6/sxi_logs/stream-mode/baseline/sxi_text_audit.cpp:37`.
- Indirect Rust callers `main.rs:64,:252,:1105` use full Container validation, so legacy RI4/SXI loads and legacy chi-rspace inputs inherit its m5 requirement. Producer-only `scan-rs`, `teralcp_chi`, and aggregate builders are not chi payload readers.
- Absent m5: full Rust open refuses explicitly (`xsa/src/sxi.rs:99`); therefore stats/export, forks, witness-build, legacy Engine MEM/MS/serve/first paths fail before querying. Legacy SXI1 C++ rejects missing m5 and does not support v6 at all (`bit6/sxi_format.hpp:56`, `:59`).
- Exact qlite count/locate remain supported with flag 64 set and chi-complete cleared (`xsa/src/qlite.rs:484`). Legacy v6 failures can additionally involve missing phi/m8, independent of chi (`xsa/src/main.rs:1436`).
- Query classes using chi: suffixient requirement-witness reporting, fork extraction, optional chi-assisted locate-one/MEM/MS paths. **MEM/MS/locate are not intrinsically chi-dependent**: existing first path has a toehold fallback (`xsa/src/product.rs:297`).
- `--chi-expect`, witness sample checks and bridge export require chi; claims checking does not (`bit6/xsa_finish.cpp:5214`; `bit6/SXI_FORMAT.md:238`).

## 3. Key universe: bounded slots, unbounded identities

- There is **no constructed stable context key** in SweepImpl: `c=v.c` indexes a mutable slot; unrestricted `topLcp` and endpoint identify its current candidate (`bit6/xsa_finish.cpp:2494`, `:2514`).
- Semantic requirement key is `(w,c)`, constructed from **all substrings**, not one/two bytes (`lean/Sxgc.lean:62`, `:68`). A full context can instead be represented by its global suffix interval/depth plus extension c; these IDs grow with the text.
- Requirements need not be one-to-one with emitted witnesses. For a χ-sized semantic universe use maximal coverage classes, not all requirements; the finite positive-text model has exactly χ such classes (`lean/Sxgc.lean:776`, `:788`, `:1863`). This is a semantic characterization, not a precomputed ID table in finish.
- `rr_len[c]` can rise, fall, and recur in another interval; even `(c,LCP length)` is not a stable identity. Slot reuse after reset is explicit (`bit6/xsa_finish.cpp:2450`, `:2465`, `:2514`).
- Therefore neither 256 nor 65,536 keys works. At most 255 nonzero **active** candidates coexist; arbitrarily many successive candidates/classes reuse them. Active-state bound ≠ emitted-key-universe bound.
- Array payload is exactly 6,400 bytes: 256 × (i64 length + u64 position + bool active + two i32 heap columns), plus scalar state/sink; reset scratch includes 1,024-byte fired list and 32-byte masks (`bit6/xsa_finish.cpp:2342`, `:2352`, `:2443`).

## 4. First-lexicographic ownership and concat

- Proposed rule: for stable final key k, `owner(k)=min{band b : k appears in b}`; `r(k)=min{rank in owner(k) belonging to k}`; emit `(k,r(k))` once. This presupposes **final** class identity/membership is already known.
- Generic online seen test: dense IDs in U need exactly `ceil(|U|/8)` bitmap bytes (32 bytes for 256; 8,192 for 65,536); hashed IDs need Θ(D) stored entries for D distinct encountered keys, plus exact key storage/collision resolution. For maximal-class IDs D=χ; arbitrary requirement keys can have D>χ.
- A bitmap on literal m5 coordinates needs `ceil((n+1)/8)` bytes, not constant memory. General exact membership needs information about the subset; a fixed-size fingerprint is not an exact replacement.
- These are costs of the generic seen-set scheme, **not a universal streaming lower bound**: sorted/grouped equal keys can avoid a global seen set; today's sweep already avoids one through interval lifetimes.
- First occurrence is not today's final representative: later higher LCP replaces it without emission. Changing representative may preserve cardinality yet change S and break byte-identity gates (`bit6/xsa_finish.cpp:2514`; `tools/binbwt/tests/chi_deferral_test.py:52`).
- Band closure is not context closure. Correct serial carry copies the complete sweep state, does not flush intermediate bands, and flushes once at EOF (`bit6/xsa_finish.cpp:2602`).
- Real `banana+NUL` emits **ranks 3,2,0**; sorted early pair still gives 2,3,0. Synthetic four-symbol fixture holds two unresolved slots while blocking 8,192 determined ranks (`tools/binbwt/tests/chi_deferral_test.py:55`, `:88`).
- Thus **immutable, immediately finalized rank-band concat is not established/correct for today's sweep**; no constant bitmap repairs it. Synthetic growing-buffer evidence is not a realizable-text lower-bound proof (`tools/binbwt/CHI_BOUNDED_DEFERRAL.md:57`).
- Even a correct rank-concat stream cannot be inserted into m5: EF requires sorted **text coordinates**, and rank→SA→witness is not monotone (`bit6/xsa_finish.cpp:2399`; `xsa/src/sxi2.rs:119`).
- Cheapest correct no-format-change baseline: carry sweep state and externally order emitted positions; existing value buckets + bounded-fan-in merges do this (`bit6/xsa_finish.cpp:2616`, `:2777`). Fixed RAM is possible with disk/replay, not with the proposed constant-universe seen bitmap.

## 5. Ranked paths and costs (incremental chi work; shared LCP/endpoint production excluded)

Let R=run records, B=chosen buffer bytes, K=spill runs per value bucket. Proposals below are study-only.

| Rank / option | RAM and storage | Passes / ordering / verdict |
|---|---|---|
| 0: omit chi when only qlite is needed | No chi state/output | Zero chi passes; already supported, but **does not build chi** (`bit6/SXI_FORMAT.md:213`). |
| 1: (c) position bitmap when n/8 fits | `ceil((n+1)/8)` bytes + O(256) sweep; no 8χ-byte spool | One sweep sets bit x, rejecting duplicate; one ascending bitmap scan emits identical EF after χ known. O(R log256 + χ + n/word) plus EF output. No context map, no comparison sort; theoretical proposal. |
| 2: (c) existing value-range buckets | B value buffers + O(P+spill-run-count) metadata; compressed spill storage O(χ) words worst case | One sweep, in-memory sorts if resident; otherwise spill-write/read and 64-way merges, O(log_64 K) levels. Final sorted pass emits EF (`bit6/xsa_finish.cpp:2647`, `:2689`, `:2777`, `:6181`). |
| 3: (b) per-symbol append-only files | 255 fixed output buffers + O(256) sweep; 8χ bytes positions, or 16χ for `(rank,position)` | One shared sweep; append only **finalized** witnesses into symbol files. Coordinates remain unsorted: still external-sort by position + EF pass. Extra symbol partition buys no current-m5 ordering advantage. |
| rejected: (a) bounded-bitmap first-lex concat | Hypothetical 32/8,192-byte seen table is insufficient; Θ(χ) stable-class IDs or text-dependent bitmap instead | First-band decisions need future context; rank order differs from m5 coordinate order. No valid pass/cost claim for this algorithm. |

- Per-symbol candidate ranks move forward with boundary traversal; retaining ranks permits a 255-way rank merge after EOF, but that is a **different representation**, still needing position reorder for m5 (`bit6/xsa_finish.cpp:2494`, `:2514`). No rank-format change is recommended here.
- Existing bucket budget is not a strict total-RSS bound: vector growth/encoding temporaries, spill descriptors, and parallel merge workers add memory (`bit6/xsa_finish.cpp:2669`, `:2693`, `:2708`, `:2733`). Strict fixed-RAM alternative: on-disk run manifests + fixed fan-in external sort; or bounded coordinate windows and repeated sweep replay (ceil(n/(8B)) passes).

| Banked corpus | n | χ | Raw 8χ bytes | Bitmap bytes (n+1 bits) | EF payload bytes |
|---|---:|---:|---:|---:|---:|
| 8 MB | 8,000,002 | 6,544,643 | 52,357,144 | 1,000,001 | 1,818,105 |
| 150 MB | 150,000,002 | 44,034,230 | 352,273,840 | 18,750,001 | 20,383,582 |
| 10 GB | 10,000,017,259 | 2,676,929,445 | 21,415,435,560 | 1,250,002,158 | 1,294,233,465 |

- n/χ provenance: `tools/binbwt/RESULTS_RECURSIVE_PLANNER.md:24`, `:55`, `:147`. Memory/EF columns are calculated, not measured; EF excludes directory/alignment and uses l=0,1,1.
- All actual construction alternatives must retain the sweep's emission count **and exact sorted S**: assert these χ constants, decode m5 and compare every u64 with today's artifact; count-only agreement cannot validate representative selection. Existing checks: `bit6/xsa_finish.cpp:6125`, `:6148`, `:6284`; full conversion comparison: `bit6/sxi2_write.cpp:54`.
- (a) fails before corpus qualification; (b) must compare sorted symbol-file union; bitmap must compare popcount=emits=χ and all sorted set bits; buckets must compare decoded sorted output. Then compare deterministic m5 bytes and run consumer gates. Omission instead validates absence flags and qlite behavior, not χ=0 as a complete set.

## Evidence and residual limits

- Ran existing `python3 tools/binbwt/tests/chi_deferral_test.py`: PASS; real ranks 3,2,0; synthetic blocked outputs 2/256/8,192 with only two unresolved slots. Tiny extracted C++ fixture only; no corpus build.
- Repo-wide Rust/C++ chi/member-reader searches included runtime and archived baselines. Product tree and running 100-GB scratch were untouched; only this document changed in the isolated worktree.
- Not measured: proposed bitmap/per-symbol wall time or corpus equivalence; no 8-MB/150-MB/10-GB reruns. Banked counts are cited historical evidence. No claim of a general impossibility theorem for all bounded-RAM algorithms.
- Supervisor contact tool was unavailable in this session's tool catalog; no decision or approval was needed. All five questions answered; implementation/large-scale qualification intentionally outside this study.
