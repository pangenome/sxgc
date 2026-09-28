#!/usr/bin/env python3
"""Finalize evidence only after publication, byte gates, and regressions pass."""
import hashlib,json,pathlib,subprocess
p=pathlib.Path(__file__).resolve().parent
members=json.loads((p/'yeast-published-members.json').read_text())
assert members['all_five_equal'] and len(members['members'])==5
journal=p/'yeast235-native-xsa-build-5najb_ty.jsonl'
stages=[json.loads(x) for x in journal.read_text().splitlines()]
assert stages[-1]['status']=='PASS'
results={x['stage']:x for x in stages if 'returncode' in x}
assert all(x['returncode']==0 for x in results.values())
assert 'verify-text-sample' in results and 'validate' in results
regressions=[json.loads(x) for x in (p/'regressions.jsonl').read_text().splitlines()]
assert len(regressions)==5 and all(x['returncode']==0 for x in regressions)
for file in ['completion-native-32.log','completion-native-64.log','completion-lce.log']:
    assert 'PASS' in (p/file).read_text(),file
hygiene=json.loads((p/'completion-hygiene.json').read_text())
assert all(v is True or v==[] for v in hygiene.values())
for path in ['/tmp/sxgc-laneV','/tmp/pfp-agc-fork','/home/erikg/pfp']:
    assert not subprocess.check_output(['git','-C',path,'diff','--cached','--name-only']).strip()
publication=json.loads((p/'publication-hygiene.json').read_text())
assert publication['no_collection_text'] and publication['no_fifo']
work=pathlib.Path(stages[-1]['work'])
assert (work/'fresh.agg').stat().st_size==3228961452
x=json.loads((p/'pending-product.json').read_text());x.pop('proc_stat',None);x.update(status='PASS: published, archive-audited, all five core members byte-identical',published=True,decision_pending=None,pipeline_pid=None,output=members['native']);(p/'pending-product.json').write_text(json.dumps(x,indent=2)+'\n')
(p/'completion-stages.json').write_text(json.dumps(results,indent=2)+'\n')
(p/'no-staged-files.json').write_text(json.dumps({'sxi':[],'fork':[],'protected':[],'no_staged_files':True},indent=2)+'\n')
old=(p/'ACCEPTANCE.before-completion.md').read_text()
architecture=old[old.index('## Memory and architecture'):old.index('The supplied contact_supervisor tool')]
text='''# Native PFP++ AGC lane — checked completion; external review pending

Implementation is in `/tmp/pfp-agc-fork`; the protected `/home/erikg/pfp`
checkout and binary remain identical to the saved baseline. No commits or staged
files. This completion changes evidence/PR materials only, with no implementation
scope expansion. See `PATCHES.md`, `patch-verification.json`, `PR-DESCRIPTION.md`,
and `COMMANDS.md` for the exact upstream and separate downstream diffs.

## Acceptance gates

- **(a) PASS — native canonical reader.** Every one of 3,336,986,759 yeast235
  bytes compares equal to the existing accepted file control. Parse, dictionary,
  names, RLE BWT and both SA sample files also compare byte for byte.
  Evidence: `yeast-reader.json`, `yeast-intermediates.json`.
- **(b) PASS — HPRC windows.** Twenty 100,000,000-byte windows spread over the
  466-sample / 1,403,221,068,481-byte canonical collection compare byte for byte
  against independent agc2flat forward-band extraction, reversed into canonical
  coordinates. Prior bounded TEST oracle files were removed after each window;
  no new expanded-text files were created for this completion. Evidence:
  `hprc-windows.json`, `check_hprc.py`, and the twenty oracle logs.
- **(c) PASS — steady throughput.** All six dataset/thread rows exceed 50% of
  the same-j file control. Per supervisor clarification, the HPRC short-window
  startup-inclusive miss is an amortization artifact, not a failing gate.
  The measured reader optimum is j=16. Full HPRC-466 reader projection is
  47.08 minutes including initialization; first parse ETA is 6.63 hours
  (6.65 hours conservatively adding HPRC initialization). These are explicitly
  projections, not a full HPRC build. See the complete table below.
- **(d) PASS — full native publication.** `/tmp/pfp-agc-gates/yeast235-native.sxi`
  was built directly from `yeast235.agc`, audited against archive ranges, validated,
  and atomically published. All five core members were compared byte by byte
  in bounded 1 MiB reads against `/tmp/sxi-stream/yeast/yeast235-control.sxi`.
  Evidence: `yeast-published-members.json`, `compare_published.py`,
  `yeast-completion.log`, the complete stage journal and `completion-stages.json`.
- **(e) PASS — regressions and write hygiene.** All three committed Python tests,
  both Cargo test commands, and all 216,000 committed LCE checks pass. Native
  32/64-bit seek/parse tests pass at j=1/16/48/96. The prior small default/native
  publication and corruption battery remains green. No collection.txt or FIFO
  exists in the full native scratch. Required r-space files, including the
  3,228,961,452-byte `.agg` and published SXI, are allowed by the clarified policy.
  No corpus-sized extracted text was written. Evidence: `regressions.jsonl`,
  `completion-native-32.log`, `completion-native-64.log`, `completion-lce.log`,
  `native-pipeline-battery.log`, `publication-hygiene.json`, `completion-hygiene.json`,
  and the earlier final-reader syscall trace `reader-writes.json`.

The previously stopped PID 3651283 no longer existed, so the full command was
restarted from the archive. The completed scratch is `/tmp/pfp-agc-gates/xsa-build-5najb_ty`.
The earlier literal artifact-size blocker is resolved by the explicit supervisor
clarification: the write prohibition applies to extracted collection text; all
r-space construction and product artifacts are unrestricted.

## Published core member comparison

| Member | Bytes | Count | Byte equal | SHA-256 |
|---|---:|---:|---|---|
'''
for m in members['members']:
    text+=f'| {m["member"]} | {m["bytes"]:,} | {m["count"]:,} | yes | `{m["sha256"]}` |\n'
text+=f'\nPublished SXI size: {members["native_bytes"]:,} bytes. Audit output: `{pathlib.Path(results["verify-text-sample"]["log"]).name}`.\n\n## Throughput and 466 parse-ETA\n\n'+(p/'THROUGHPUT.md').read_text()+'\n'+architecture
text+=f'\nCompletion stage peak RSS: {max(r["peak_rss_kib"] or 0 for r in results.values()):,} KiB. The process-tree monitor began during BWT and observed {publication["observed_tree_peak_rss_kib"]:,} KiB; first-parse RSS is recorded separately. These figures do not claim a whole-run sampled tree bound.\n'
text+='''
## Review and residual limits

No local acceptance blockers remain. External reviewer approval is still required
and is not claimed here. The upstream patch clean-applies to its recorded base
and exactly reproduces all eleven files in the tested fork. The upstream PR draft
is prepared locally; nothing was committed or remotely published.

AGC support is coupled to pinned format-3 internal APIs. Archive auditing samples
32 witnesses rather than proving the complete chi set. Full HPRC parsing/index
construction was not run; HPRC coverage is the 20 byte proofs plus reader timings,
and the parse ETA remains a yeast-derived extrapolation. The contact_supervisor
tool was unavailable in this session; no coordination decision was needed beyond
the supplied supervisor clarification.
'''
(p/'ACCEPTANCE.md').write_text(text)
pr=(p/'PR-DESCRIPTION.before-completion.md').read_text();pr=pr[:pr.index('Validation evidence and remaining gates')]
pr+='''Validation (complete local checked evidence; external review pending):

- All 3,336,986,759 yeast235 canonical bytes match the accepted file control.
  The native build publishes a complete SXI; all five core members compare
  byte for byte with the accepted control, and 32 archive-backed witnesses pass.
  See `ACCEPTANCE.md`, `yeast-reader.json`, `yeast-published-members.json`, and
  `completion-stages.json` in the accompanying acceptance bundle.
- Twenty distributed 100 MB HPRC-466 windows (2 GB total) match an independent
  agc2flat oracle byte for byte (`hprc-windows.json`). No full HPRC build is claimed.
- `THROUGHPUT.md` and `throughput.json` record j=16/48/96 reader results.
  Steady AGC/file ratios are 59.9–86.9%. HPRC's measured optimum is j=16,
  509.109 MB/s; its 68–82 s initialization dominates the 2 GB test window but
  amortizes over the full 1.403 TB canonical corpus. The full-reader projection
  is 47.08 minutes including initialization. First-parse ETA is 6.63 hours,
  extrapolated from measured yeast parsing; this is not measured HPRC parsing.
- Native 32/64-bit regressions at j=1/16/48/96 cover parse/dictionary/names,
  random seeks, chunk seams, separators, EOF and invalid ranges. Existing
  Python/Cargo/LCE regressions pass; the AGC-disabled/file path also passed.
- No expanded collection text is created in the native product path. The
  `.agg`, `.ri4`, samples and `.sxi` are intended r-space artifacts and may exceed
  1 GB. Protected PFP sources/binary remain untouched; no commits or staged files.

`pfp-agc.patch` is the complete upstream diff against
`1a5f114ae026c18e7c0049ceace1a5eabc8be44a`; `series` lists it and
`patch-verification.json` verifies clean application and exact tested-file equality.
The separate SXI pipeline diff selects native AGC parsing, retains XSA_PFP and
explicit --fifo fallback, and labels --materialize forensic-only. It is not part
of this upstream PR. Reviewer attention: pinned internal AGC API coupling and
format-3 compatibility; no additional archive versions are claimed.
'''
(p/'PR-DESCRIPTION.md').write_text(pr)
changed=json.loads((p/'changed-files.json').read_text())
commands=[{'command':'XSA_PFP=/tmp/pfp-agc-fork/build/pfp++ XSA_TOOLS=/tmp/sxi-stream/tools XSA_PIPELINE=/tmp/sxgc-laneV/bit6/sxi_pipeline.py /tmp/sxi-stream/xsa-target/release/xsa build --agc /home/erikg/yeast/yeast235.agc -o /tmp/pfp-agc-gates/yeast235-native.sxi --scratch /tmp/pfp-agc-gates --log-dir /tmp/sxgc-laneV/bit6/sxi_logs/pfp-agc --threads 16 --verify-text-sample 32 --verbose','result':'passed','summary':'All stages pass, archive audit green, validated SXI published'}, {'command':'python3 bit6/sxi_logs/pfp-agc/compare_published.py','result':'passed','summary':'Five core members byte-identical to accepted control'}, {'command':'python3 bit6/sxi_logs/pfp-agc/run_regressions.py','result':'passed','summary':'Three committed Python tests and both Cargo suites pass'}, {'command':'/tmp/sxi-stream/tools/slim_lce_test','result':'passed','summary':'216000 exact LCE checks'}, {'command':'python3 /tmp/pfp-agc-fork/tests/test_agc.py --pfp /tmp/pfp-agc-fork/build/pfp++ --probe /tmp/pfp-agc-fork/build/pfp_source_probe --agc /home/erikg/agc/bin/agc --agc2flat /tmp/sxi-stream/tools/agc2flat (also pfp++64)','result':'passed','summary':'Both widths pass at j=1/16/48/96'}, {'command':'git apply --check pfp-agc.patch; git apply pfp-agc.patch (fresh base export)','result':'passed','summary':'All eleven patched files exactly match tested fork; no staging'}]
report={'criteriaSatisfied':[{'id':'criterion-1','status':'satisfied','evidence':'Full native yeast publication completed without code/scope expansion or extracted collection text; all five core members byte-identical'}, {'id':'criterion-2','status':'satisfied','evidence':'Acceptance bundle contains stage logs, direct byte comparisons and hashes, regressions, throughput/466 ETA, verified patch, PR draft and hygiene checks'}], 'changedFiles':[str(pathlib.Path('/tmp/pfp-agc-fork')/f) for f in changed['fork']]+changed['pipeline']+['bit6/sxi_logs/pfp-agc/ACCEPTANCE.md','bit6/sxi_logs/pfp-agc/THROUGHPUT.md','bit6/sxi_logs/pfp-agc/PR-DESCRIPTION.md','bit6/sxi_logs/pfp-agc/PATCHES.md','bit6/sxi_logs/pfp-agc/COMMANDS.md','bit6/sxi_logs/pfp-agc/compare_published.py','bit6/sxi_logs/pfp-agc/monitor_completion.py','bit6/sxi_logs/pfp-agc/finalize_completion.py','bit6/sxi_logs/pfp-agc/series'], 'testsAddedOrUpdated':['/tmp/pfp-agc-fork/tests/test_agc.py','/tmp/pfp-agc-fork/tests/source_probe.cpp','bit6/sxi_logs/pfp-agc/compare_published.py'], 'commandsRun':commands,'validationOutput':[f'Published {members["native"]}: {members["native_bytes"]} bytes','All five core members byte-identical; yeast-published-members.json contains hashes/counts/lengths','All 3336986759 yeast reader bytes and 20 x 100000000 HPRC window bytes matched (prior proofs retained)','Archive-backed 32-witness audit passes; no collection.txt/FIFO in native scratch','All regressions pass; protected checkout and binary unchanged','HPRC optimum j=16: 509.109 MB/s steady; startup-amortized reader projection 47.08 min; parse ETA 6.63 h'],'residualRisks':['External reviewer approval remains required','Pinned internal AGC APIs; format 3 only','Archive audit is sampled; HPRC full parse/index not run; parse ETA is extrapolated'],'noStagedFiles':True,'diffSummary':'Optional native AGC TextSource and tests in isolated PFP fork; separate SXI integration; completed publication and evidence materials, no commits','reviewFindings':['No local acceptance blockers; independent reviewer gate pending'],'manualNotes':'Supervisor clarified text-only write prohibition and steady-throughput gate. Original stopped PID was gone; full native build was restarted. Upstream PR is a local draft only.'}
(p/'acceptance-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: finalized acceptance, PR draft, state and structured report')
