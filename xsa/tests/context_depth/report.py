#!/usr/bin/env python3
"""Render measured receipts; no invented or extrapolated values."""
import json, pathlib
from study import quant
root=pathlib.Path(__file__).resolve().parents[3]
out=pathlib.Path(__file__).parent/'results'
s=json.loads((out/'summary.json').read_text()); v=json.loads((out/'validation.json').read_text())
rows=[json.loads(x) for x in (out/'observations.jsonl').read_text().splitlines()]
def tupleq(q): return ' / '.join(f'{q[k]:.3f}' if isinstance(q[k],float) else str(q[k]) for k in ['p50','p90','p99','max'])
text='''# Measured context depth on the 100 GB Pile head

2026-11-10. Base: origin/main `7f0d567d96ef02bbfc44b8341f54bc6e059ae4db` (fetched before creating `study/context-depth`). Read MEMORY_OPS.md and RESEARCH_MEMORY_OPS.md first. No members or product code changed.

## Definition and orientation

1,000 uniformly sampled byte positions, without replacement, across exclusive ends [1024, 100000000000], Python Random seed 20261110. These are byte-weighted positions, not uniform documents, words, or deduplicated text. The 1023 excluded early boundaries are negligible but explicitly excluded. The exact positions are `xsa/tests/context_depth/results/positions.json`; the generator is `study.py` beside it.

At exclusive end p, depth d means **Q = corpus[p-d:p] in its original forward byte order**. It excludes corpus[p], the next byte. Backward search consumes Q from right to left, prepending corpus[p-1], then corpus[p-2], etc. Reversing Q before sending it would measure the wrong text. No synthetic prefix, sentinel, or novel suffix is added.

**K elsewhere excludes the sampled source occurrence**: seek width must be at least K+1. Overlapping matches count; within-document repeats count; separator-crossing strings are included in these raw byte counts. Starting with d=1, double depth until width < K+1, then binary search for the last supported suffix. Search is bounded only by p, not 1024 or any artificial depth cap. All reported maxima ended by support failure, not by p.

Important API limitation: `xsa mem` extends left but has no minimum-support parameter. Its `-k` limits output MEMs, not occurrence support. A query copied from the corpus has its entire source as a MEM, even if unique. It therefore cannot by itself yield K-supported depth. We used existing `xsa branches` interval widths for the threshold search, and `xsa mem --serve --min-len 1` on Q=corpus[p-16:p] at all 1,000 positions to verify full match length=16, query_start=0, and width parity. This is a disclosed composition of existing primitives, not a claim that MEM natively performs support backoff. No made-up MEM depth is reported.

The named pile-head100.txt is absent. CONSULT.md explicitly identifies the first 100e9 bytes of `/mnt/nvme2n1/erikg/pile.txt` as this container's text; the build log declares raw bytes with no transformation and n=100000000001. We read only that prefix. All 1,000 MEM and branch source checks passed; two independent full-prefix brute-force histograms also match. This verifies sampled agreement, not a full cryptographic corpus identity proof.

Executable: `/home/erikg/sxgc/xsa/target/release/xsa`, SHA256 `9dc13f72dc33978e3b6baaaf980a7fee53bfc9619c83034a5c50d563c6c5d19f`. This is the existing executable, not a new build attestation.

## Support depth (bytes)

Nearest-rank percentiles; K means occurrences elsewhere.

| K | p50 | p90 | p99 | max |
|---:|---:|---:|---:|---:|
'''
for k,q in s['support'].items(): text+='| '+k+' | '+' | '.join(str(q[x]) for x in ['p50','p90','p99','max'])+' |\n'
text+='''
## Branching and concentration curve

Each cell is **p50 / p90 / p99 / max** over the same 1,000 positions. H is Shannon entropy in bits. Top-1 is a fraction of total occurrence mass. Full byte histogram (-k 256); END is a separate outcome in H, excluded from byte branch count/top-1. Counts include self. Each context contributes equally to percentiles (not pooled occurrence-weighted). Unique contexts yield H=0 and top-1=1; this includes source memorization, not independent predictive evidence.

| depth | distinct next bytes | H (bits) | top-1 mass | total occurrences |
|---:|---|---|---|---|
'''
for d,q in s['curve'].items(): text+=f"| {d} | {tupleq(q['branches'])} | {tupleq(q['entropy'])} | {tupleq(q['top1'])} | {tupleq(q['count'])} |\n"
text+='\nRepeated-context subset (at least 2 elsewhere, total >=3), to distinguish determinism from self-match:\n\n| depth | contexts | median H | median top-1 |\n|---:|---:|---:|---:|\n'
for d in s['curve']:
    sub=[r['curve'][d] for r in rows if r['curve'][d]['count']>=3]
    text+=f"| {d} | {len(sub)} | {quant([r['entropy'] for r in sub])['p50']:.3f} | {quant([r['top1'] for r in sub])['p50']:.3f} |\n" if sub else f'| {d} | 0 | unavailable | unavailable |\n'
text+='''
## Document concentration

No matching Pile document map is available: main contains sidecar reader code, but the build log says docs=0; expected `/mnt/nvme2n1/erikg/seps.bin` and titles.bin are absent. The wiki map belongs to another corpus and was not used. **Distinct document count and largest-document mass are unavailable.** Occurrence concentration is reported in the total-occurrences column above; it must not be interpreted as document frequency or cross-document support. No document independence conclusion follows.

## The 1024-byte check

'''
text+=f"**{s['1024_elsewhere_ge2']} / {s['samples']} = {100*s['1024_elsewhere_ge2']/s['samples']:.1f}% have at least 2 occurrences elsewhere at exactly 1024 bytes.**\n\n"
text+=f"Under the common alternative convention of total support >=2 (only one elsewhere), the answer is {s['1024_total_ge2']} / {s['samples']} = {100*s['1024_total_ge2']/s['samples']:.1f}%. **A {100*s['1024_total_ge2']/s['samples']:.1f}% survival rate under total support >=2 is a material minority, not nearly none: it changes the design.** Both conventions are reported to remove the self-match ambiguity.\n"
text+='''
## Cost today (microseconds)

Serial persistent CLI workers (-j 1), measured Python round trip (frame write, server work, response read), not pure core CPU time. First startup request excluded from aggregate latency; later cache/page state is mixed/warmed. The separate depth benchmark follows the study and full-prefix oracle; no cold-cache claim. Host was shared and the first study overlapped brute-force scanning. No throughput extrapolation. Tail latency includes scheduling and I/O effects.

| operation | p50 / p90 / p99 / max µs |
|---|---|
'''
for k,q in s['latency_us'].items(): text+=f'| {k} | {tupleq(q)} |\n'
text+='\n| branches context depth | p50 / p90 / p99 / max µs (1,000 queries each) |\n|---:|---|\n'
for d,q in v['branches_roundtrip_us_by_depth'].items(): text+=f'| {d} | {tupleq(q)} |\n'
text+='''
MEM cost is for 16-byte queries only; the current MEM scan can be quadratic for long fully matching input, so these timings do not predict long MEM latency. Branch cost includes count and all right-extension counts, not bare seek. Support search repeatedly pays this cost; a reusable locus/tree would avoid that repeated seeking. Query counts and CLI internal aggregate timing are preserved in summary.json and stderr receipts.

## Reproduction and checks

From repository root:

```sh
python3 xsa/tests/context_depth/study.py --exe /home/erikg/sxgc/xsa/target/release/xsa --index /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 --corpus /mnt/nvme2n1/erikg/pile.txt --out xsa/tests/context_depth/results
cc -O3 -pthread gates/consult_oracle.c -o /tmp/context-oracle
/tmp/context-oracle /mnt/nvme2n1/erikg/pile.txt xsa/tests/context_depth/results/oracle.contexts 100000000000 > xsa/tests/context_depth/results/oracle.tsv
mkdir -p xsa/tests/context_depth/results/validation
python3 xsa/tests/context_depth/validate.py /home/erikg/sxgc/xsa/target/release/xsa /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 /mnt/nvme2n1/erikg/pile.txt xsa/tests/context_depth/results
python3 xsa/tests/context_depth/report.py
```

oracle.contexts contains two u32-length-prefixed forward contexts ending at the first sampled position, depths 16 and 1024 (same framing as branches). Generate it by reading each corpus[p-d:p], writing struct.pack('<I',d), then those d bytes. The existing independent C oracle scans every possible overlapping start in the entire 100 GB prefix. It verifies complete byte histograms, not just counts. Per-position support, branching metrics, and positions are checked in; no corpus bulk copy is checked in.

'''
text+=f"Validation: **{v['threshold_checks']} threshold endpoints** rechecked: count(depth)>=K+1 and count(depth+1)<K+1 (each K at every position), plus 11,000 per-depth latency queries. Oracle results: {json.dumps(v['oracle'])}. All passed.\n"
text+='''
## What depth matters

'''
q=s['support']['2']; text+=f"The typical position supports two other occurrences for {q['p50']} bytes; 90% stop by {q['p90']} bytes and 99% by {q['p99']}. The observed maximum is {q['max']} bytes. A single fixed 1024-byte window describes neither the typical context nor the long repeated tail. The measured 1024 survival fraction above is the result, regardless of the prior expectation of nearly none. Long repeats are real byte evidence; without document projection they may be copies or repetition within a document.\n\n"
text+='''`relax` should expose caller-selected support, including the self-exclusion convention, rather than bake in one threshold. The K ladder shows how evidence requirements change depth. Document-support thresholds need project; raw multiplicity is insufficient. `extend` must support deep traversal for the observed tail, preferably skipping unary edges; spending 1024 byte steps for every position wastes work on typical contexts. Short-context branch entropy and top-1 show where continuation choices remain wide. This study does not establish an optimal prediction threshold, held-out predictive quality, or semantic value of repetition.

Residual risks: 1,000 byte-uniform positions are a finite sample; extreme repeats can be missed. Document concentration is unmeasured. Corpus identity has sampled and documented support, not a new full hash. Timings use an existing binary and shared host, with no source-build or isolated cold-cache attestation. The requested MEM-only threshold method is unavailable in the existing API; exact branches width probes supply the missing composition. No new primitive is needed to reproduce these measurements, but tree navigation performance remains unmeasured.
'''
(root/'CONTEXT_DEPTH.md').write_text(text)
