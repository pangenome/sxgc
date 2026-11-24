"""Human-readable decomposition of completed matched-study receipts.
Only additive decompositions below: phases of a single producer, and measured
blocked/write vs other time *within* its emit clock. Concurrent walls never sum.
"""
import json
from pathlib import Path
import re
import sys
from overlap_ten_score import verified_arm

def table(dirs):
    print('10 GB private-cold v8, same raw/rows/reference, 4 build CPUs + 1 oracle. Worker timings are concurrent.\n')
    print('Profile | raw s | prepared s | dict s | emit s | join: sink / audit write s | counted GB | shared GiB | sink syscw')
    print('--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---:')
    rows=[]
    for p in dirs:
        b,f,_=verified_arm(p);log=(p/'resident.log').read_text()
        ph={k:float(v) for k,v in re.findall(r'^RESIDENT_PHASE phase=(\w+) seconds=([\d.]+)',log,re.M)}
        def seconds(key):return float(re.search(r'\b'+key+r'=([\d.]+)',log)[1])
        a,s=seconds('audit_write_s'),seconds('sink_write_s');w=seconds('order_wait_s') if 'RESIDENT_RADIX_JOIN' in log else None
        rows.append((p,b,f,ph,a,s,w))
        print(f"{b['profile']} | {b['raw_to_product_s']:.2f} | {b['wall_s']:.2f} | {ph['dictionary_sort']:.2f} | {ph['emit']:.2f} | {s:.2f} / {a:.2f} | {b['raw_counted_highwater']/1e9:.3f} | {int(b['shared_after']['memory.peak'])/2**30:.3f} | {b['process']['sink']['syscw']:,}")
    print('\nPer-arm accounting (decimal GB, CPU-seconds):\n')
    for p,b,f,ph,a,s,w in rows:
        cg_cpu=lambda x:int(dict(y.split() for y in x['cpu.stat'].splitlines())['usage_usec'])/1e6
        cpu=cg_cpu(b['shared_after'])-cg_cpu(b['shared_before']);child=sum(j['cpu_s'] for j in b['process'].values())+f['process']['cpu_s']
        post=[sum(j[k] for j in b['process'].values())/1e9 for k in ('read_bytes','write_bytes')];raw=[b['raw_worker_io'][k]/1e9 for k in ('read_bytes','write_bytes')]
        oracle=json.loads((p/'oracle.json').read_text())
        print(f"- `{b['profile']}`: raw attributed worker R/W {raw[0]:.3f}/{raw[1]:.3f} GB; prepared {post[0]:.3f}/{post[1]:.3f} GB. Raw shared cgroup {cpu:.1f} CPU-s ({cpu/b['raw_to_product_s']:.2f} average CPUs); front + resident/sink/finish {child:.1f} CPU-s; difference {cpu-child:.1f}, not necessarily all monitor. Oracle separately {oracle['process']['cpu_s']:.2f} CPU-s.")
        if w is not None:
            print(f"  Ordered join waited {w:.2f} s for worker packets; downstream writes {s+a:.2f} s. These timers are inside emit, concurrent with lane computation. Do not infer worker compute by subtracting them.")
        else:print(f"  Serial emission outside the two measured write calls: {ph['emit']-s-a:.2f} s (not an isolated compute benchmark).")

if __name__=='__main__':table([Path(x) for x in sys.argv[1:]])
