"""Human-readable decomposition of completed matched-study receipts.
Only additive decompositions below: phases of a single producer, and measured
blocked/write vs other time *within* its emit clock. Concurrent walls never sum.
"""
import hashlib
import json
import math
from pathlib import Path
import re
import sys
from overlap_ten_score import verified_arm

def certificate(p):
    """Optional independent inotify observation; never use it in arm selection."""
    path=p/'certification-clock.json'
    if not path.is_file():return 'post-service certificate interval not instrumented'
    c=json.loads(path.read_text());src=json.loads((p/'source.json').read_text())['source']
    assert c['source']==src and c['kind']=='post-service certification envelope; not SHA kernel time' and c['profile']==p.name
    ev=c['close_events'];assert set(ev)=={'exits.log','identity.json'}
    for k,e in ev.items():assert len(e)==3 and hashlib.sha256((p/k).read_bytes()).hexdigest()==e[2]
    i,j=ev['exits.log'],ev['identity.json'];wall=(j[0]-i[0])/1e9;fs=(j[1]-i[1])/1e9
    assert math.isfinite(wall) and wall>=0 and fs>=0 and wall==c['observed_close_to_close_s'] and fs==c['filesystem_mtime_delta_s']
    return f'post-service certificate envelope {wall:.3f} s (mtime cross-check {fs:.3f} s)'

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
        monitor=f" Measured harness CPU {b['harness_cpu_s']:.1f} s." if 'harness_cpu_s' in b else ''
        print(f"- `{b['profile']}`: raw attributed worker R/W {raw[0]:.3f}/{raw[1]:.3f} GB; prepared {post[0]:.3f}/{post[1]:.3f} GB. Raw shared cgroup {cpu:.1f} CPU-s ({cpu/b['raw_to_product_s']:.2f} average CPUs); front + resident/sink/finish {child:.1f} CPU-s; difference {cpu-child:.1f}, not necessarily all monitor.{monitor}")
        v=oracle['process'];print(f"  Separate raw oracle: {oracle['wall_including_wait_s']:.2f} s including upstream wait, {v['cpu_s']:.2f} CPU-s, {v['read_bytes']/1e9:.3f}/{v['write_bytes']/1e9:.3f} GB attributed R/W. After both services: {certificate(p)}. Oracle wall overlaps construction; the later certification envelope is outside raw build wall.")
        print(f"  Cold raw front {f['wall_s']:.2f} s + cold handoff {f['cold_barrier_s']:.2f} s; prepared counted observed/reserved {b['counted_sampled_peak']:,}/{json.loads((p/'scratch-plan.json').read_text())['counted_bound']:,} B; whole raw directory sampled {b['raw_dir_sampled_allocated']:,} B incl. input copies/output. Front and prepared phases have distinct scratch peaks.")
        if w is not None:
            log=(p/'resident.log').read_text();plan=float(re.search(r' plan_s=([0-9.]+)',log)[1]);join=re.search(r'^RESIDENT_RADIX_JOIN .*?ranges=(\d+) blocks=(\d+).*lane_rows=(\[[^\]]+\]) serial_row_comparisons=0$',log,re.M);lane_rows=json.loads(join[3]);n=sum(lane_rows)
            print(f"  {int(join[1]):,} ordered ranges, {int(join[2]):,} blocks, {12*n/int(join[2]):,.0f} mean payload B/block (one separate END), zero join row comparisons. Plan {plan:.2f} s, packet waits {w:.2f} s ({1e9*w/n:.2f} ns/owned row), downstream sink + verifier write calls {s+a:.2f} s ({100*(s+a)/ph['emit']:.1f}% of emit). These timers are inside emit, concurrent with lane computation. Wait includes successful receives; do not infer worker compute by subtraction or call this a cat baseline.")
        else:print(f"  Serial emission outside the two measured write calls: {ph['emit']-s-a:.2f} s (not an isolated compute benchmark).")

def study(bank):
    """Complete portable bank only. The report has no authority to dispatch."""
    from overlap_scaling_policy import evaluate,experiment_order,one_series
    from overlap_ten_dict_grid import replay
    from overlap_ten_score import get
    pair=bank/'radix-full-1g';grid=bank/'native-grid-10g';old=bank/'packed-serial-10g';out=bank/'scale-10g'
    native=replay(grid);order=experiment_order(pair,grid);arms=[out/name for name in order];result=evaluate(old,pair,grid,arms)
    assert result['complete_lane_sweep'];print('# 10 GB matched scaling decision\n')
    print('Selected:', '`'+result['selected']+'`' if result['selected'] else '**none eligible**', '; <2×/byte target at both endpoints:',result['performance_target_met'])
    print('\n## Isolated native dictionary sort\n')
    print('Requested/selected threads | sort s | speed vs 1 | process CPU s | mean CPUs during sort | peak RSS GiB | physical R/W bytes | post-clock digest s')
    print('---: | ---: | ---: | ---: | ---: | ---: | ---: | ---:')
    for a in native['arms']:
        print(f"{a['threads']} | {a['wall_s']:.3f} | {native['arms'][0]['wall_s']/a['wall_s']:.3f}× | {a['cpu_s']:.3f} | {a['cpu_s']/a['wall_s']:.2f} | {a['peak_rss']/2**30:.3f} | {a['read_bytes']}/{a['write_bytes']} | {a['hash_s']:.2f}")
    print(f"\nAll {native['arms'][0]['entries']:,} LE i64 entries hashed after each clock; one matching SHA `{native['arms'][0]['sha256']}`. Chose {native['selected_threads']} (fewest within 5% of fastest with no extra sort IO vs one). Average CPUs are measured process CPU/wall, not an observed OpenMP team count.\n\n## Full product\n")
    ident=get(arms[0],'identity.json');src=(arms[0]/'build-source.log').read_text().splitlines()[0];harness=(arms[0]/'binaries.sha').read_text().splitlines()[0]
    print(f"All four verified 10,000,000,001 raw rows, including 5,705,032,705 positions above u32, and all nine members of the {ident['bytes']:,}-byte v8 container, SHA `{ident['sha256']}`. New worker source `{src}`, harness `{harness}`. Resource grant: 200 GiB shared, 140 GiB arrays, 4 build CPUs, 2 finish threads, separate 14,000 MiB / 1 CPU verifier, no swap; counted scratch ≤20 GB.\n")
    table([old,*arms]);base=next(r for r in result['rows'] if r['lanes']==1);_,_,small=one_series(pair);one=min(small,key=lambda v:v[-1]['raw'])
    print(f"\n## Performance gates\n\nMatched 1 GB reference: `{one[0]['profile']}`, {one[-1]['raw']:.3f} s raw, {one[-1]['prepared']:.3f} s prepared. New-arm per-byte ratios use the same endpoint/verification.\n")
    print('Lanes | raw vs old serial | same-binary raw speed vs 1 lane | build CPU vs 1 lane | raw/prepared per-byte vs matched 1 GB | raw/prepared per-byte vs 3 GB | prepared R/W vs old serial | eligible | target')
    print('---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---:')
    for r,p in zip(result['rows'],arms):
        n=r['normalized_to_fastest_verified_1g'];v=r['post_io_ratio'];three=get(p,'goals.json')['normalized_wall']['packed-3g-bitmap']
        print(f"{r['lanes']} | {r['raw_ratio_to_historical_serial']:.3f}× | {base['raw_s']/r['raw_s']:.3f}× | {r['build_cpu_s']/base['build_cpu_s']:.3f}× | {n['raw']:.3f}× / {n['prepared']:.3f}× | {three['raw_to_product_s']:.3f}× / {three['wall_s']:.3f}× | {v['read_bytes']:.4f}× / {v['write_bytes']:.4f}× | {r['eligible']} | {r['under_2x']}")
    print('\nThe historical comparison combines code, sink and sort changes and the previously documented frozen background difference. Only the three new arms isolate lane count. This selection applies to the tested resident v8 grant, not an arbitrary machine or a v10 100 GB build.')

if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='--study':study(Path(sys.argv[2]))
    else:table([Path(x) for x in sys.argv[1:]])
