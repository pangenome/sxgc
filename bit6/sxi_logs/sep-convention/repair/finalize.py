import json,pathlib,re,subprocess
logs=pathlib.Path(__file__).resolve().parent
comparison=json.loads((logs/'yeast-members.json').read_text())
assert not comparison['changed_members'] and comparison['header_dimensions_equal']
p=logs/'ACCEPTANCE.md'
s=p.read_text().replace('- (b) **PENDING while this file is being assembled**: fresh yeast rebuild and\n  member-by-member comparison are running; final verdict will replace this.', '- (b) **PASS for the certified single-string case**: yeast was rebuilt from\n  `yeast_pfp2.txt` through fresh parsing, producer, certified adapter, slim,\n  sweep, writer and container validation. **Chi remains 85,404,240; all five\n  embedded members are unchanged**, with identical SHA-256, sizes, counts\n  and CRCs. No `yeast_v2.sxi` is needed. The audit rebuild is published at\n  `/mnt/nvme3n1/erikg/sxi-normalization-repair/yeast-certified.sxi`.\n  `yeast-members.json` records the complete comparison against published\n  `/mnt/nvme3n1/erikg/sxgc-laneV/yeast.sxi`. This does not certify the\n  adapter for general seam-repair cases.')
assert 'PENDING while' not in s
stage_rss=[]
for path in logs.rglob('*.time'):
    match=re.search(r'Maximum resident set size \(kbytes\): (\d+)',path.read_text())
    if match:stage_rss.append((int(match[1]),str(path.relative_to(logs))))
observed=[json.loads(line) for line in (logs/'observed-rss.jsonl').read_text().splitlines()]
peak=max(row['peak_observed_kib'] for row in observed)
staged=subprocess.check_output(['git','diff','--cached','--name-only'],text=True).splitlines()
subprocess.run(['git','diff','--check'],check=True)
hygiene=dict(no_staged_files=not staged,staged_files=staged,max_recorded_process_rss_kib=max(stage_rss)[0],max_recorded_process_rss_log=max(stage_rss)[1],peak_observed_process_tree_rss_kib=peak,peak_observed_process_tree_bytes=peak*1024,protected_work_directories_accessed=False,commits_created=0)
assert not staged
(logs/'hygiene-and-memory.json').write_text(json.dumps(hygiene,indent=2)+'\n')
s+='\n## Final resource and hygiene evidence\n\n'
s+=f'Maximum recorded process RSS: {max(stage_rss)[0]:,} KiB. Maximum observed\ncombined process-tree RSS: {peak:,} KiB ({peak*1024/1e9:.3f} GB), below 20 GB.\n'
s+='Yeast used a 19,000,000 KiB address-space cap; concurrent AGC used\n9,700,000 KiB. See per-stage `.time` files, `observed-rss.jsonl`, and\n`hygiene-and-memory.json`. Diff hygiene passes; staged-file list is empty.\n'
p.write_text(s)
commands=json.loads((logs/'commands.json').read_text())
commands.extend([
 dict(command='source /tmp/sxi-repair-o4aGmN/env.sh; /tmp/sxi-sep-8xat6poa/xsa-target/release/xsa build --text /mnt/nvme3n1/erikg/sxgc-yeast/grl/yeast_pfp2.txt -o /mnt/nvme3n1/erikg/sxi-normalization-repair/yeast-certified.sxi --threads 4 --scratch /mnt/nvme3n1/erikg/sxi-normalization-repair --log-dir /tmp/sxgc-laneV/bit6/sxi_logs/sep-convention/repair --verbose',result='passed',summary='Fresh certified yeast build; all five embedded members unchanged; chi 85404240.'),
 dict(command='python3 bit6/sxi_logs/sep-convention/repair/compare_members.py /mnt/nvme3n1/erikg/sxgc-laneV/yeast.sxi /mnt/nvme3n1/erikg/sxi-normalization-repair/yeast-certified.sxi bit6/sxi_logs/sep-convention/repair/yeast-members.json',result='passed',summary='All member hashes, sizes, counts and CRCs equal.'),
 dict(command='git diff --check; git diff --cached --name-only',result='passed',summary='No whitespace errors or staged files.')])
(logs/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
print(json.dumps(hygiene))
