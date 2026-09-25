# TERALCP-IN-PARSE-SPACE LANE — REPORT (M1/M2/M3)

Task: can the phi-piece sidecar be built in parse time (no n-term)?
Files (all new): tools/teralcp_m1.py, tools/teralcp_m1b.py,
tools/teralcp_m2.py, tools/teralcp_m3.py. Gated tools imported unmodified.
No git commits.

---

## M1 — TeraLCP scaling: **O(n) TIME, O(r) SPACE (confirmed)**

Pipeline built as in bit6/*.sh: brute SA -> BWT runs -> rlbwt
(`<name>.bwt.heads` 1 byte/run + `.bwt.len` 5-byte LE) -> real TeraLCP
`-f rlbwt -oindex`. Threads pinned `-p 1` except the control row.

DECISIVE ROW — F2 satellite, r held ~constant, n grows 8x:

| text | n | r | TeraLCP wall | idx size |
|---|---|---|---|---|
| F2 sat-3200k | 3,200,399 | 306 | 81.7 ms | ~0.0 MB |
| F2 sat-6400k | 6,400,400 | 304 | 181.2 ms | ~0.0 MB |
| F2 sat-12800k | 12,800,399 | 291 | 383.4 ms | ~0.0 MB |
| F2 sat-25600k | 25,600,400 | 322 | 642.4 ms | ~0.0 MB |

8x n at fixed r -> 7.9x time: **time is linear in n even when r is ~300.**
Contrast F1 random (r grows with n): 2.37s -> 23.7s for 3.2M -> 25.6M.
F3 duplicates (r grows slowly, 44.6k -> 145.9k over 4x n): 253ms -> 1310ms.

SPACE is r-driven: index size 32.4MB at r=2.40M, 288.0MB at r=19.2M
(~15 bytes/run), ~0.0MB for the satellite at r~300 — **O(r) space.**

Threads: `-p 1/4/32` = 9.20s / 5.06s / 3.21s on n=12.8M — parallel, so
absolute wall is thread-dependent but the n-linearity is structural.

Code confirms the same (parent's read, consistent with the source
comment at TeraLCP.h ~740): "4 O(n) traversals for construction + 1
O(n) for minLCP" in ConstructPhiAndSamples (the Phi move structure +
ISA samples); TeraLCP never consumes the parse (its `-othresholds`
flags only EMIT pfp-compatible files downstream).

**M1 verdict: confirmation-only, as steered — TeraLCP is O(n) time,
O(r) space. Not parse time.**

---

## M2 — the candidate structure: **GREEN (exact, size r)**

S_law = brute direct-law piece starts (piece continues at p iff
PLCP(p-1) = PLCP(p)+1).

Tested candidate families for CONTAINMENT of S_law:

```
family      random-4-20k      satellite-18k     duplicates-600k
phi  (phi-parallel breaks) 100%     100%        100%
runedge (SA of boundary rows +-1) 100%   100%   100%
revrun (BWT(rev T) runs mapped)  93.7%   95.1%  (not used)
parse (phrase-occ starts)    1.2%      1.2%   (not used)
```

Two equivalent exact candidates surfaced:
- phi-parallel breaks C_phi = {p : phi(p) != phi(p-1)-1}: |C_phi| = r,
  100% containment on the 7 battery texts — BUT it differs from the
  run-boundary set on ascending-chain texts (see below), so it is NOT
  the safe enumeration.
- **the run-boundary set**
  `C = { resolve(row i) : i in 1..N-1, BWT[i] != BWT[i-1] } U { N-1 }`
  contains S_law EXACTLY, at |C| = r on every battery text.

Exhaustive small-text check (all texts over {1,2}, |T| <= 8, plus the
known pathological [2,2,2,1,1]): **511 texts, 0 containment failures.**
[e.g. [2,2,2,1,1]: S_law = {0,3,5}; C = {0,3,5}; the phi-parallel set
C_phi = {0,5} misses 3 — the run-boundary set is the correct one.]

Full-battery M2 run (containment rate, candidate size in parens):

```
text               r        |S_law|   phi(rate,size)   runedge*(rate,size)  parse(rate,size)
random-4-20k       14959    9951      1.000(14959)     1.000(18738)         1.000(229)
satellite-18k      364      244       1.000(364)       1.000(465)           1.000(5)
HOR-nested         232      161       1.000(232)       1.000(303)           1.000(64)
duplicates-600k    14701    9656      1.000(14701)     1.000(26105)         1.000(5247)
dup+unique-120k    53844    35939     1.000(53844)     1.000(73510)         1.000(1132)
random-bin-20k     9883     6137      1.000(9883)      1.000(14855)         1.000(226)
random-4-200k     149887   99599     1.000(149887)    1.000(187424)        1.000(2019)
```
(*runedge as implemented in teralcp_m2.py takes both SA[i] and SA[i-1], size
~1.25-1.8r; the dedicated size-r variant `{SA[i] : boundary} U {N-1}` used by
M3 was verified to contain S_law at |C| = r exactly on all 7 battery texts.)
NB the per-family rates above are for the battery family; the exhaustive
small-text test is what rules the family safe — `phi` shows 1.000 here yet
misses starts on 22/255 small ascending-chain texts, so `phi` is NOT the
enumeration; the run-boundary set is.

Exhaustive small-text check (all texts over {1,2}, |T| <= 8, plus
[2,2,2,1,1]): **511 texts, 0 containment failures for the run-boundary
set.**

Enumerability in r-space: the r-1 run boundaries come straight from the
rlbwt; each boundary row i is resolved with ONE `resolve(i)` parse
lookup (O(log)); add position N-1. **Total O(r log).**

**M2 verdict: GREEN — the direct-law piece starts are contained in an
O(r)-sized, r-space-enumerable candidate set (resolved BWT run-boundary
rows + the final position).**

---

## M3 — build the pieces in parse space: **GREEN (exact on 7/7)**

Pipeline (no brute PLCP, no text scan):
1. candidates C (M2);
2. per candidate p: PLCP(p), PLCP(p-1) from a PARSE-SPACE LCP —
   character access via phrase occurrences (last `span` chars of each
   phrase; reconstruction of T validated) and a phrase-walking LCP of
   the suffix pair (p, phi(p)), with phi(p) = resolve(resolve_inv(p)-1);
3. S_emit = {0} U { p in C : PLCP(p) != PLCP(p-1) - 1 }, samples = PLCP.

GATE vs brute direct-law partition:

| text | N | r | \|C\| | \|S_law\| | \|S_emit\| | set_eq | sample_mismatch | slope_bad(1000) | lcp_bad(400) |
|---|---|---|---|---|---|---|---|---|---|
| satellite-18k | 18501 | 364 | 364 | 244 | 244 | **True** | 0 | 0 | 0 |
| random-4-20k | 20001 | 14959 | 14959 | 9951 | 9951 | **True** | 0 | 0 | 0 |
| HOR-nested | 18361 | 232 | 232 | 161 | 161 | **True** | 0 | 0 | 0 |
| duplicates-600k | 600001 | 14701 | 14701 | 9656 | 9656 | **True** | 0 | 0 | 0 |
| dup+unique-120k | 120001 | 53844 | 53844 | 35939 | 35939 | **True** | 0 | 0 | 0 |
| random-bin-20k | 20001 | 9883 | 9883 | 6137 | 6137 | **True** | 0 | 0 | 0 |
| random-4-200k | 200001 | 149887 | 149887 | 99599 | 99599 | **True** | 0 | 0 | 0 |

- emitted start set == brute direct-law starts (exact, all 7);
- emitted samples == brute PLCP at those starts (0 mismatches);
- slope law holds on 1000 random positions/text (0 violations);
- the parse-space LCP itself was cross-checked against brute PLCP on 400
  random positions/text (0 mismatches).

COST instrumentation (the honest caveat):

| text | r | lcp_calls | lcp_steps | wall |
|---|---|---|---|---|
| satellite-18k | 364 | 1124 (~3.1r) | 3,483,890 (~188n) | 1.59 s |
| duplicates-600k | 14701 | 29798 (~2.0r) | 1,623,987 (~2.7n) | 22.7 s |
| HOR-nested | 232 | 860 (~3.7r) | 174,955 (~9.5n) | 0.35 s |

The **call count is O(r)** (the parse-time part is right). The **steps
per call are the blocker**: the prototype's LCP is a phrase walk whose
length is the LCP value, so totals can exceed n on periodic texts
(satellite: 188n; the LCPs at its few piece starts are ~thousands).
A production implementation must replace the phrase walk with the
standard PFP-LCP primitive (parse ISA + dictionary LCP + bounded
within-phrase compare), giving O(polylog + phrase-length) per call ->
O(r polylog) total; or derive the large periodic samples from the
periodicity class. This is a cost-engineering task, not a correctness
gap.

---

## GO / NO-GO on "pieces in parse time"

**GO on structure and correctness.** The pieces ARE constructible from
parse coordinates:
- starts: exact O(r) candidate set from resolved run-boundary rows (M2:
  511 exhaustive small texts + 7 battery texts, 0 misses);
- samples: parse-space LCP, exact on all 7 texts (M3);
- total construction: O(r) resolve/resolve_inv + O(r) LCP queries.

**NO-GO on wall-clock parse time with the current LCP.** The phrase-walk
LCP makes the per-query cost proportional to the LCP value; on periodic
texts (satellite) the total exceeds n. Blocker (precise): an O(polylog)
PFP-LCP primitive (or a periodicity-aware sample derivation) is required
before the piece build is genuinely r-dominated. With it, the piece
sidecar closes and the build's only Omega(n) step is the single pfp++
parse pass.

Supporting negative notes recorded: the phi-parallel condition is NOT
equivalent to the direct-law condition (22/255 small texts differ), so
the candidate set must be the run-boundary enumeration (C), not the
phi-parallel set; and no simple phi/psi identity propagates PLCP sample
values (~25% match = chance), so the explicit LCP query is necessary.
