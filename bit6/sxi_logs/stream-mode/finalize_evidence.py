import difflib,hashlib,json,pathlib,re,subprocess
root=pathlib.Path('/tmp/sxgc-laneV');log=root/'bit6/sxi_logs/stream-mode'
assert 'PASS full yeast235 stream/control' in (log/'yeast-gate.log').read_text()
assert 'PASS duplicate names across samples' in (log/'battery-final-source.log').read_text()
assert 'PASS exactly one writer' in (log/'fifo.log').read_text()
regressions=[json.loads(x) for x in (log/'regressions.jsonl').read_text().splitlines()]
assert len(regressions)==5 and all(x['returncode']==0 for x in regressions)
assert '216000 exact capped LCE checks' in (log/'lce-regression.log').read_text()
files=json.loads((log/'changed-files.json').read_text());diff=[]
for name in files:
 p=root/name;baseline=log/'baseline'/p.name
 old=baseline.read_text() if baseline.exists() else ''
 diff.extend(difflib.unified_diff(old.splitlines(keepends=True),p.read_text().splitlines(keepends=True),fromfile='a/'+name,tofile='b/'+name))
(log/'lane.diff').write_text(''.join(diff))
assert not subprocess.check_output(['git','diff','--cached','--name-only'],cwd=root).strip()
subprocess.run(['git','diff','--check'],cwd=root,check=True)
protected=json.loads((log/'protected-component-hashes.json').read_text())
assert all(hashlib.sha256((root/p).read_bytes()).hexdigest()==h for p,h in protected.items())
builds=[json.loads(x) for x in (log/'yeast-gate.log').read_text().splitlines() if x.startswith('{')]
assert len(builds)==2 and all(x['returncode']==0 and x['tree_peak_rss_kib']*1024<20_000_000_000 for x in builds)
stages=[]
for journal in sorted(log.glob('yeast235-*-xsa-build-*.jsonl')):
 for row in map(json.loads,journal.read_text().splitlines()):
  if 'returncode' in row:stages.append(dict(journal=journal.name,**row))
(log/'build-stages.json').write_text(json.dumps(stages,indent=2)+'\n')
(log/'hygiene-and-memory.json').write_text(json.dumps(dict(no_staged_files=True,no_commits=True,protected_component_hashes_unchanged=True,yeast_builds=builds,peak_stage_rss_kib=max(x.get('peak_rss_kib') or 0 for x in stages)),indent=2)+'\n')
peak=max(x['tree_peak_rss_kib'] for x in builds)
(log/'ACCEPTANCE.md').write_text(f'''# Stream-mode acceptance — checked; external review pending

Implemented default AGC FIFO streaming, explicit `--materialize`, and archive-backed
sampled witness auditing. Scope: seven source/test/documentation files listed in
`changed-files.json`; pre-existing dirty work retained. `lane.diff` isolates this lane.
No tap, PFP frontend, endpoint producer, repair, or core format implementation was changed.
No commits or staged files. The running 466 tree, scratch and processes were not modified.

## Gates

- **Battery PASS:** five admitted three-record fixtures (2k/20k/200k random,
  600k perturbed duplicates, 18k satellite), plus duplicate names across two
  samples. Stream/file builds have identical six SXI members; the first five
  fixtures also compare parse and dictionary bytes. Random ranges and ranges
  crossing separators match legacy materialized bytes. See `battery-final-source.log`.
- **Full yeast235 PASS:** fresh `/tmp/sxi-stream/yeast/yeast235-stream.sxi` and
  `/tmp/sxi-stream/yeast/yeast235-control.sxi` publish; all five core members are
  compared byte for byte by `run_yeast.py`. Hashes/counts/lengths are in
  `yeast-members.json`. The requested old yeast235 path was absent, so the
  control was built fresh in this lane's scratch with `--materialize`.
- **Archive audit PASS:** both fresh yeast builds verify 32 witnesses before
  publication. A prior-artifact preflight also passes with `/does-not-exist`
  as the text argument. Corrupted range bytes fail the real audit and block
  publication. Malformed/short requests and invalid names fail loudly.
- **Lifecycle PASS:** exactly one producer invocation; byte-exact FIFO transfer;
  failed writer, failed reader, early reader exit, failed journal write, and
  SIGTERM clean up the FIFO and owned children. Default AGC scratch contains
  no `collection.txt`. See `fifo.log` and battery failure stage logs.
- **Regressions PASS:** all three committed Python regressions, both Cargo
  test commands, and 216,000 committed LCE primitive checks. See
  `regressions.jsonl`, `lce-regression.log` and individual logs.
- **Memory PASS:** sequential full builds have maximum observed process-tree
  RSS {peak:,} KiB ({peak*1024/1e9:.3f} GB), below 20 GB. Stage RSS/timing and
  monitor results are in `build-stages.json` and `hygiene-and-memory.json`.

## Review and limits

The audit remains sampled, not a proof of complete chi-set correctness. Its
range service retains O(record count) metadata and the AGC decoder working set;
it never materializes collection text. SIGKILL/host loss cannot run cleanup;
any leftover FIFO is confined to the unique failed scratch directory and is
never reused. The unchanged endpoint repair policy rejects an exact-duplicate
exploratory fixture in legacy file mode; final fixtures stay within admitted policy.
The full stream was already running when failure-path cleanup was hardened to
unlink the FIFO even if journaling fails. Final-source battery and lifecycle
gates were rerun afterward; streamed bytes and downstream construction were
unchanged. External reviewer approval is still required. No review agent was invoked.
The requested `contact_supervisor` tool was unavailable in this session.

Reproduction commands: `COMMANDS.md`. Structured evidence: `changed-files.json`,
`source-and-tool-hashes.json`, `protected-component-hashes.json`, stage journals,
and all retained `.time`/`.log` files under this directory.
''')
print(json.dumps(dict(status='PASS',peak_tree_rss_kib=peak,no_staged_files=True,changed_files=files)))
report={
 'criteriaSatisfied':[
  dict(id='criterion-1',status='satisfied',evidence='Default single-writer AGC FIFO streaming, explicit --materialize, same-pass names, archive-backed audit, all requested gates passed; seven scoped source/test/documentation files.'),
  dict(id='criterion-2',status='satisfied',evidence='bit6/sxi_logs/stream-mode/ contains lane.diff, baselines, commands, stage journals/timings, yeast member byte comparisons, memory measurements, regression and failure-gate logs.')],
 'changedFiles':files,
 'testsAddedOrUpdated':['bit6/test_sxi_stream.py','bit6/test_sxi_fifo.py'],
 'commandsRun':[
  dict(command='cargo build --release --manifest-path agc2flat/Cargo.toml --target-dir /tmp/sxi-stream/agc-target',result='passed',summary='Isolated archive streamer/range-service build.'),
  dict(command='cargo build --release --manifest-path xsa/Cargo.toml --target-dir /tmp/sxi-stream/xsa-target',result='passed',summary='Isolated CLI build with --materialize.'),
  dict(command='g++ -O3 -std=c++17 -Wall -Wextra bit6/sxi_text_audit.cpp -o /tmp/sxi-stream/tools/sxi_text_audit',result='passed',summary='Archive-backed audit compiled.'),
  dict(command='python3 bit6/test_sxi_stream.py --work /tmp/sxi-stream/battery-final-source --xsa /tmp/sxi-stream/xsa-target/release/xsa --tools /tmp/sxi-stream/tools --log-dir bit6/sxi_logs/stream-mode',result='passed',summary='Six fixture sets: member equivalence, archive ranges, corrupt-range publication rejection, malformed ranges/names, producer/consumer failures.'),
  dict(command='python3 bit6/test_sxi_fifo.py',result='passed',summary='Single writer, exact bytes, broken pipe, journal failure and SIGTERM cleanup.'),
  dict(command='python3 bit6/sxi_logs/stream-mode/run_yeast.py',result='passed',summary='Full fresh stream/control publications; all five core members byte-identical; both archive audits pass.'),
  dict(command='python3 bit6/sxi_logs/stream-mode/run_regressions.py',result='passed',summary='Three committed Python regressions and both Cargo test commands passed.'),
  dict(command='bash tools/build_slim_dump.sh /tmp/sxi-stream/tools/slim_lce_test tools/slim_lce_test.cpp && /tmp/sxi-stream/tools/slim_lce_test',result='passed',summary='216000 exact capped LCE checks passed.'),
  dict(command='git diff --check && git diff --cached --name-only',result='passed',summary='No whitespace errors or staged files.')],
 'validationOutput':[
  'Full yeast235: chi=85404336; 32/32 archive witnesses verified in each fresh build.',
  'Five yeast core members compared byte for byte; full parse and dictionary are also byte-identical.',
  f'Maximum observed full-build process-tree RSS: {peak} KiB ({peak*1024/1e9:.3f} GB), below 20 GB.',
  'Default AGC builds contain no collection.txt; FIFOs are removed after success and tested failures.'],
 'residualRisks':['Independent reviewer approval remains required.','Witness auditing is sampled; SIGKILL or host failure can leave an unused FIFO in abandoned scratch.'],
 'noStagedFiles':True,
 'diffSummary':'Adds AGC stdout/FIFO streaming and bounded archive range access for witness auditing; preserves legacy materialization, sidecar format, PFP/producer, and SXI core construction.',
 'reviewFindings':['No blockers found in focused self-review; external review pending.'],
 'manualNotes':'No commits. Prior dirty work preserved; the running 466 tree, scratch and processes were not modified. contact_supervisor was unavailable. An initial exact-duplicate fixture hit the existing file-mode endpoint refusal; final admitted fixtures pass. Failure-path cleanup was hardened after the full stream started; final-source battery and lifecycle gates were rerun afterward.'}
(log/'acceptance-report.json').write_text(json.dumps(report,indent=2)+'\n')
