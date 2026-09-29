#!/usr/bin/env python3
"""Regenerate reviewer tables from journaled observations; no fitted scaling law."""
import json,pathlib,re
h=pathlib.Path(__file__).resolve().parent
rows=[json.loads(x) for x in (h/'measurements.jsonl').read_text().splitlines() if json.loads(x).get('stage')=='measurement']
gates=[json.loads(x) for x in (h/'gates.jsonl').read_text().splitlines()]
def peak(tag):
    p=h/(tag+'.time')
    if not p.exists():return 'pending'
    m=re.search(r'Maximum resident set size \(kbytes\): (\d+)',p.read_text())
    return f'{int(m[1])*1024/1e9:.4f}' if m else 'pending'
s=['# Measurements and scaling\n','All GB/TB values below are decimal. D includes dictionary separators and EOF.\n',
   '| Input MB | w/p | distinct phrases | D bytes | D/n | parse entries | parse bytes | distinct length mean / p50 / p90 / p99 / max |',
   '|---:|:---:|---:|---:|---:|---:|---:|:---|']
for r in rows:
    q=r['phrase_length_quantiles'];lengths=' / '.join(f'{x:.1f}' for x in [r['phrase_length_mean'],q['p50'],q['p90'],q['p99'],q['max']])
    s.append(f"| {(r['input_bytes']+r['removed_bytes'])/1e6:.0f} | {r['w']}/{r['p']} | {r['distinct_phrases']:,} | {r['dictionary_bytes']:,} | {r['D_over_n']:.6f} | {r['parse_entries']:,} | {r['parse_bytes']:,} | {lengths} |")
weighted=h/'phrase-statistics.json'
if weighted.exists():
    s += ['\nOccurrence-weighted length distributions are recorded separately in `phrase-statistics.json`; each occurrence-count sum is checked against the parse length. Distinct-phrase quantiles above intentionally give each dictionary phrase one vote.\n']
s+=['\n## Explicit constant-ratio projection to 1.31 TB\n',
    'This is an extrapolation from a single prefix, **not** a fitted full-corpus deduplication estimate. Phrase diversity can change with source mixture and scale. It is a capacity warning, not a rigorous lower bound on unseen data.\n',
    '| Calibration | projected D TB | hypothetical 24D TB | selected SA width | changed-path SA-packing live set TB (D + tmp + packed SA only) | projected phrase IDs (billions) |',
    '|:---|---:|---:|---:|---:|---:|']
for r in rows:
    if r['input_bytes']<900_000_000:continue
    d=r['D_over_n']*1.31e12
    width=(int(d).bit_length()+7)//8
    s.append(f"| w={r['w']}, p={r['p']} | {d/1e12:.4f} | {24*d/1e12:.4f} | {width} B | {(1+8+width)*d/1e12:.4f} | {r['distinct_phrases']/r['input_bytes']*1.31e12/1e9:.3f} |")
s+=['\nThe latter column excludes boundary indexes, L2 structures, and merge workspace. These scenarios all exceed 900 GB. For default w=10/p=100, the projected dictionary itself exceeds RAM and the 40-bit address range. Even an actual M5 implementation could not represent that projected D. Projected phrase counts also exceed existing uint32_t phrase IDs; lowering p is not a complete solution.\n',
    '## Frontend byte gates and R\n',
    '| Case | n including padding | R | R/n | baseline peak GB | memory peak GB |',
    '|:---|---:|---:|---:|---:|---:|']
for r in gates:
    if r.get('status')=='GATE_PASS' and 'r' in r:
        tag=r['stage'];s.append(f"| {tag} | {r['n']:,} | {r['r']:,} | {r['R_over_n']:.6f} | {peak(tag+'-baseline')} | {peak(tag+'-memory')} |")
s+=['\nAt the measured 100 MB R/n=0.38647, a constant-ratio projection gives ~506.3 billion runs for 1.31 TB: the RLE plus two 8-byte endpoint arrays alone would be about **10.13 TB** (20 bytes/run, ignoring headers and unusually long-run continuation records). This is another disk-capacity scenario to validate at larger scale, not a proven full-corpus R estimate. R is a property of the text/BWT, not a tunable parse compression factor. Window changes affect artificial padding and can perturb a few boundary runs; an intrinsic R reduction should not be inferred from changing w/p. `gates.jsonl` lists only completed comparisons as GATE_PASS.\n',
    '## 466 redo projection\n',
    'With D=18,611,972,670 symbols, removing L1 DA saves 4D=74.448 GB and releasing L1 ISA saves 5D=93.060 GB during all subsequent L2/merge phases. Applying those **resident-array** savings to the supplied 505.9 GB peak yields a conditional **338.4 GB** late-phase estimate. The changed L1 SA packing stage is about (1+8+5+0.16)D=263.5 GB; The retained completed run logs four-byte L1 LCP, so ISA/LCP/colex construction is about (1+5+5+4+0.16)D=282.2 GB plus phrase-ID storage. Its GNU time file actually reports 509,998,396 KiB = 522.238 GB, versus the task calibration of 505.9 GB. Using that measured RSS instead yields **354.7 GB** for the late phase. Budget **380 GB** for a redo experiment; neither 338.4 nor 354.7 is measured for the changed binary. This is not a whole-process upper bound: baseline peak phase, L2/merge allocations and allocator retention still require a full redo or richer telemetry. The read-only reference extracts are retained466.time and retained466-widths.log. Dictionary capacity savings are deliberately excluded from RSS savings.\n',
    '## Semi-external SA probe\n',
    'The standalone probe uses the first 9,999,977 dictionary bytes ending at a phrase boundary, M32 gSACA-K, and a 39,999,908-byte MAP_SHARED SA on nvme2n1. The in-memory and disk variants produced hash fa607383921036d5. With systemd MemoryMax=32M and MemorySwapMax=0, the disk probe completed at a recorded cgroup peak of 32 MiB.\n',
    '- Memory: 2.399 s, maxRSS 49,152 KiB, no block I/O.\n',
    '- Disk: 122.942 s (51.2x), 2,170 major faults, 491,704,320 input bytes and 2,046,705,664 output bytes (Linux block counters ×512); maxRSS 34,824 KiB, cgroup peak 32 MiB. Timing includes final synchronous writeback.\n',
    'This demonstrates correctness-preserving OS paging for an SA workspace, **not an integrated, scalable external constructor**. Random-I/O amplification is substantial; do not linearly promise its runtime at TB scale. Default projected 64-bit temporary SA alone is ~11.3 TB, exceeding nvme2n1 space before input/final SA/ISA/LCP. A mapped temporary SA alone cannot close the pile gate. It needs a fundamentally different external construction/storage plan; no disk fallback is silently enabled in the frontend.\n']
(h/'RESULTS.md').write_text('\n'.join(s)+'\n')
