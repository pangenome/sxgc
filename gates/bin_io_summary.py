"""Summarize read-byte deltas between phase starts; never span level resets."""
import argparse, collections, re
p = argparse.ArgumentParser(); p.add_argument('log'); args = p.parse_args()
rb = collections.Counter(); seconds = collections.Counter(); bins = collections.Counter(); prev = None
pattern = re.compile(r'SS_BIN_IO level_stride=(\d+) bin=(\d+) stage=(\w+) rb=(\d+) sec=([.\d]+)')
with open(args.log) as f:
    for line in f:
        m = pattern.match(line)
        if not m: continue
        level, bin_no, stage, n, ts = m.groups(); n = int(n); ts = float(ts)
        if prev and level == prev[0]:
            _, oldbin, oldstage, oldn, oldts = prev
            assert n >= oldn and ts >= oldts
            rb[oldstage] += n-oldn; seconds[oldstage] += ts-oldts; bins[level,oldbin,oldstage] += n-oldn
        prev = (level,bin_no,stage,n,ts)
assert prev, 'no SS_BIN_IO markers'
for s,n in rb.most_common(): print(f'{s:16} read_bytes={n:14} sec={seconds[s]:.3f}')
print('highest read intervals aggregated per bin:', bins.most_common(5))
