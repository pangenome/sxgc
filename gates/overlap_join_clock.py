"""Observed ordered-publisher clocks, normalized to the matched 1 GB batched arm.
Only disjoint stopwatches on the synchronous ordered publisher are subtracted.
Never estimate lane compute by subtracting its concurrent writer's waits.
"""
import json
from pathlib import Path
import re
import sys
from overlap_resident_gate import ONE,TEN
from overlap_radix_product_score import metrics as one_metrics
from overlap_ten_score import verified_arm

def fields(log,tag):
    line,=[l for l in log.splitlines() if l.startswith(tag+' ')];return dict(re.findall(r'(\w+)=(\S+)',line))

def clocks(d,one=False):
    if one:
        b,*_=one_metrics(d);n=ONE.n;assert b['profile']=='packed-radix-4-batch'
    else:
        b,_,_=verified_arm(d);n=TEN.n
    log=(d/'resident.log').read_text();assert 'RESIDENT_ASYNC_AUDIT' not in log # these are synchronous branches
    s=fields(log,'RESIDENT_STREAM');p=fields(log,'RESIDENT_RADIX_PLAN');j=fields(log,'RESIDENT_RADIX_JOIN');emit=float(fields(log,'RESIDENT_PHASE phase=emit')['seconds'])
    assert int(s['rows'])==n+1 and int(j['serial_row_comparisons'])==0 and int(j['ranges'])==int(p['ranges']) and int(j['lanes'])==int(p['lanes'])>1
    t=dict(plan=float(p['plan_s']),packet_receive=float(j['order_wait_s']),sink_write=float(s['sink_write_s']),audit_write=float(s['audit_write_s']));t['other_publisher_elapsed']=emit-sum(t.values());assert min(t.values())>=0
    return dict(profile=b['profile'],n=n,emit_s=emit,ordered_join_s=t,ranges=int(j['ranges']),blocks=int(j['blocks']),max_range_rows=int(j['max_range_rows']),mean_payload_bytes=12*n/int(j['blocks']),queue_bytes=int(p['row_queue_reserved']))

def report(pair,arms):
    base=clocks(pair,True)
    print('# Ordered publisher: where did emit wall change?\n\nSynchronous batched sink and independent audit, one global END plus the same owned-row wire width (12 B). Different texts, row layouts and sorting kernels across rungs. **Not a sinkless `cat` benchmark.**\n')
    for d in arms:
        j=clocks(d);scale=j['n']/base['n'];print(f"## {j['profile']} vs {base['profile']}\n\nInterval | 1 GB observed s | 10 GB observed s | 10 GB minus byte-linear 1 GB (s) | 10 GB ns/owned row")
        print('---|---:|---:|---:|---:')
        for k,a in {**base['ordered_join_s'],'**emit**':base['emit_s']}.items():
            v=j['emit_s'] if k=='**emit**' else j['ordered_join_s'][k];print(f'{k} | {a:.3f} | {v:.3f} | {v-scale*a:+.3f} | {1e9*v/j["n"]:.2f}')
        print(f"\n{j['ranges']:,} whole-key ranges, {j['blocks']:,} ready data packets (mean {j['mean_payload_bytes']:,.0f} B, largest range {j['max_range_rows']:,} rows), {j['queue_bytes']:,} B queue. At 1 GB: {base['ranges']:,} ranges, {base['blocks']:,} packets (mean {base['mean_payload_bytes']:,.0f} B, largest {base['max_range_rows']:,} rows). No per-row comparisons or patching at join. Plan is before the data writer, packet receive includes fast successful calls, and other elapsed includes untimed setup/accounting. The sink and verifier may work while lanes compute. The table locates wall on the publisher, not causes of stalls, worker CPU or removable wall.")
if __name__=='__main__':report(Path(sys.argv[1]),list(map(Path,sys.argv[2:])))
