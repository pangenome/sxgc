#!/usr/bin/env python3
"""Regenerate the lane's evidence table from the measured stage journals."""
from pathlib import Path
import json
import re
here=Path(__file__).resolve().parent

def journal(tag):
    p=here/tag/'journal.jsonl'
    return [json.loads(s) for s in p.read_text().splitlines()] if p.exists() else []

def timing(tag,stage):
    p=here/tag/(stage+'.time')
    if not p.exists() or not p.stat().st_size:return None,None
    text=p.read_text();m=re.search(r'Elapsed .*\): ([0-9:.]+)',text)
    seconds=0.0
    if m:
        for part in m[1].split(':'):seconds=60*seconds+float(part)
    rss=re.search(r'Maximum resident set size \(kbytes\): (\d+)',text)
    return seconds,int(rss[1])*1024 if rss else None

rows=[]
for tag in ['yeast-parser-checked','yeast-final','web-parser-bounded','cyclic-checked']:
    for event in journal(tag):
        if event.get('status')!='PASS' or 'peak_rss_bytes' not in event or event['stage'].startswith('cmp'):continue
        stage=event['stage'];wall,rss=timing(tag,stage)
        rows.append(dict(case=tag,stage=stage,wall_seconds=wall,rss_bytes=rss,
            sampled_allocated_disk_bytes=event['peak_allocated_disk_bytes'],
            address_limit_bytes=event['address_space_limit_bytes'],journal=str(Path(tag)/'journal.jsonl')))
wall,rss=timing('yeast-parser-checked','parse-l2')
if rss:
    rows.insert(1,dict(case='yeast-parser-checked',stage='parse-l2',wall_seconds=wall,rss_bytes=rss,
        sampled_allocated_disk_bytes=None,address_limit_bytes=3500000000,
        journal='yeast-parser-checked/parse-l2.time'))
(here/'measurements.json').write_text(json.dumps(rows,indent=2)+'\n')
inputs=here/'yeast-parser-checked/checked-inputs.json'
input_ok=inputs.exists() and json.loads(inputs.read_text())['status']=='PASS'
front_ok=any(e.get('stage')=='gate' and e.get('status')=='PASS' for e in journal('yeast-final'))
verdicts={'a_yeast': 'PASS' if input_ok and front_ok else 'PENDING',
    'b_k10':'NOT_RUN','c_pile_fragment_end_to_end':'NOT_RUN','d_10_to_50_GB':'NOT_RUN',
    'small_cyclic_end_to_end':'PASS','full_pile_fit':'NOT_ESTABLISHED; measured-ratio projection exceeds RAM/disk/ID limits',
    'independent_review':'PENDING'}
(here/'gate-verdicts.json').write_text(json.dumps(verdicts,indent=2)+'\n')
lines=['# Parsed-space lane results','',
'The three dictionary-memory changes are implemented as an opt-in experimental toolchain. '
'The full-pile acceptance contract is **not satisfied**: gates b–d and independent review remain outstanding, '
'and the current full-pile projection exceeds capacity. No slice is claimed as a substitute for the full pile.','',
'## Gates','',
f"- **(a) Yeast235: {verdicts['a_yeast']}.** Final bounded parser outputs are byte-identical to both the accepted retained inputs and the exact inputs used by the external frontend. The frontend compares `.rlebwt`, `.rlebwt.meta`, `.ssa`, and `.ssa_t` against `/tmp/rpfbwt-64-yeast/parse`. Every measured yeast stage runs under RLIMIT_AS=3,500,000,000 bytes.",
'- **(b) K10: NOT RUN.** Its retained parse and the supervisor-owned memory-gate directories were not modified. The historical retained prefix has no tail reference; the recipe records PARTIAL until all four output references are available.',
'- **(c) Pile fragment: NOT RUN end to end.** The requested 1,082,130,213-byte raw input was copied into the worktree. The continuation fixture reserves a unique terminal (`fixtures/pile-terminal-cleaning.json`). A 100 MB prefix of the earlier cleaned fixture (`fixtures/pile-cleaning.json`) was used only for parser timing. No web chi/n is claimed.',
'- **(d) 10–50 GB: NOT RUN.** Ordered, no-clobber recipes are in `RECIPES.sh`, including a 10 GB prepared measurement slice. Only run after a–c pass and after checking the downstream run-ID limit.',
'- **Small complete regression: PASS.** The 200,001-byte cyclic fixture traversed parse → external front end → endpoints → slim → sweep → 1,000-witness audit → `.sxi` → validation. chi=136,189, chi/n=0.6809415953. This synthetic DNA fixture is not a web-text datum.','',
'## Per-stage measurements','',
'GNU time wall/RSS below; disk is cumulative allocated construction storage in that experiment’s work and scratch directories, sampled every 0.5 s (plus a final sample). Sampling can miss short transient peaks. Input files, compiler outputs and read-only references are excluded. The final L2 parser has time/RSS evidence but no separately polled disk peak; the earlier identical-input L2 run measured 789,766,144 bytes (`yeast-agc/journal.jsonl`).','',
'| Case | Stage | Wall s | Peak RSS MiB | Sampled disk MiB | Address cap GB |',
'|---|---|---:|---:|---:|---:|']
for row in rows:
    disk='not sampled' if row['sampled_allocated_disk_bytes'] is None else f"{row['sampled_allocated_disk_bytes']/2**20:.2f}"
    lines.append(f"| {row['case']} | {row['stage']} | {row['wall_seconds']:.2f} | {row['rss_bytes']/2**20:.2f} | {disk} | {row['address_limit_bytes']/1e9:.1f} |")
lines += ['', '## Correctness evidence','',
'- `dictionary-test-checked.log`: four randomized byte dictionaries, every SA and LCP entry over three replay passes, rank/select membership checks, and a 70,000-byte repeated-prefix fixture checking all SA entries plus sampled exact LCPs beyond the 16-bit hint limit.',
'- `phrase-store-test.log`: 100,000 distinct phrases plus 100,000 duplicates, all lexical ranks/emissions, disk-chain rehashing, and a forced hash collision rejected.',
'- `frontend-final-test.log`: 16 byte comparisons against the pinned accepted tools across DNA/ASCII fixtures with 32 KiB SA blocks and a 64 KiB dictionary cache; unsupported properties and oversize phrases refused; owned scratch cleaned.',
'- `measurement-test.log`: all 256 byte values exercise the measurement cleaner, terminal uniqueness, raw-copy identity, manifest hash and overwrite refusal.',
'- `cyclic-checked/audit.log`: TEXT_SAMPLE_PASS requested=1000 verified=1000; writer and reader validation also passed.',
'- `provenance.json`, stage journals and build logs retain executable hashes, argv and build dependencies. `git diff --check`, Python syntax checks and recipe shell syntax checks passed. No commit or staging operation was performed.','',
'## Limits and continuation','',
'`ARCHITECTURE.md` documents all three changes, dictionary-access audit, width limits and residency assumptions. `BUDGET.md` and `full-pile-budget.json` give the measured disk placement and the full-pile projection. The endpoint tap and file formats are unchanged; the experimental merge enforces one thread/one chunk. The product pipeline has not been switched to this experimental toolchain. The current phrase is capped by `SXI_MAX_PHRASE_BYTES` (64 MiB default); the SA block budget must accommodate complete padded phrases.',
'',
'The requested `contact_supervisor` tool was not available in the session catalog. No live long-running gate is handed off via this report. Rebuild and launch later gates from a durable checkout using the supplied recipes; do not launch work that will outlive an automatically reaped worktree.',
'',
'Exploratory logs are preserved, but are not counted as passing gates: the first yeast attempt used a different newline input than the accepted AGC reference; three frontend attempts were explicitly stopped on this lane’s own PIDs while improving buffering, LCP reuse and boundary lookup reuse. `scratch-cleanup.json` records removal of only those owned stopped-job scratch directories.',
'',
'Independent reviewer approval is still required.']
(here/'REPORT.md').write_text('\n'.join(lines)+'\n')
