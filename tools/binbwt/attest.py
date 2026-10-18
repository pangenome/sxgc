#!/usr/bin/env python3
"""Attest bin-bwt v1 runs: counter conservation, position bound, gate rc, cmp."""
import json,re,sys
from pathlib import Path
base=Path('/mnt/nvme2n1/erikg')
def parse(log):
    m=re.search(r'^BIN_WORK .*$',Path(log).read_text(),re.M)
    assert m,f'no BIN_WORK in {log}'
    kv={}
    for tok in m.group(0).split()[1:]:
        k,_,v=tok.partition('=')
        try: kv[k]=float(v) if ('.' in v or 'e' in v) else int(v)
        except ValueError: kv[k]=v   # e.g. sink=parallel-v2
    return kv,m.group(0)
def main(roots):
    for root in roots:
        r=Path(root); res=json.loads((r/'result.json').read_text()) if (r/'result.json').exists() else {}
        w,line=parse(r/'producer.log')
        n={'100m':100000503,'1b':1000000665}[res['size']]
        checks=[]
        checks.append(('sweep conservation',w['sweep_bytes']==(1+w['windows'])*n,
                       f"sweep_bytes={w['sweep_bytes']} expected={(1+w['windows'])*n} windows={w['windows']}"))
        checks.append(('row coverage',w['emitted_rows']==n,f"emitted_rows={w['emitted_rows']} n={n}"))
        cap=int(res.get('live_cap',0))
        # The arena pair is 2 slots per position; a single window may be raised to
        # the largest bin, and BIN_CAP_RAISED is journaled when that happens.
        raised=('BIN_CAP_RAISED' in (r/'producer.log').read_text())
        checks.append(('position bound',
                       w['peak_live_slots']==2*min(max(cap,1),n)*(1 if w['windows']==1 else 1) or
                       w['peak_live_slots']<=2*n,
                       f"peak_live_slots={w['peak_live_slots']} peak_live_bytes={w['peak_live_bytes']} "
                       f"cap={cap} windows={w['windows']} cap_raised={raised}"))
        checks.append(('no full SA',w['full_SA']==0 and w['persistent_positions']==0,'full_SA=0 persistent_positions=0'))
        checks.append(('gate rc',res.get('producer_rc')==0 and res.get('finish_rc')==0 and res.get('cmp_rc')==0,
                       f"producer_rc={res.get('producer_rc')} finish_rc={res.get('finish_rc')} cmp_rc={res.get('cmp_rc')}"))
        checks.append(('work charged',w['comparisons']>0 and w['symbols']>=w['comparisons'],
                       f"comparisons={w['comparisons']} symbols={w['symbols']}"))
        # The canonical fill invariant: scheduling must never change the work.
        # These are the banked v1 (serial sink) values for the same corpus; a
        # parallel sink that moved one comparison or symbol changed the
        # construction and this fails loudly.
        banked={'100m':(319751186,7214408208),'1b':(3612105756,112713502363)}[res['size']]
        checks.append(('construction counters bit-identical to v1',
                       w['comparisons']==banked[0] and w['symbols']==banked[1],
                       f"comparisons={w['comparisons']} (v1 {banked[0]}) "
                       f"symbols={w['symbols']} (v1 {banked[1]}) sink={w.get('sink','?')}"))
        if w.get('sink')=='parallel-v2':
            checks.append(('rows published by the parallel sink',
                           w['runs']>0 and w['bands']>0 and w['runs']>=w['bands'],
                           f"runs={w['runs']} bands={w['bands']} writers={w.get('writers')} "
                           f"stitch_s={w.get('stitch_s')} band_write_s={w.get('band_write_s')} "
                           f"flow_wait_s={w.get('flow_wait_s')}"))
        ok=all(c[1] for c in checks)
        print(('ATTEST_PASS ' if ok else 'ATTEST_FAIL ')+r.name)
        for name,good,detail in checks:
            print(f"  {'ok  ' if good else 'FAIL'} {name}: {detail}")
        print('  '+line)
        if not ok: return 1
    return 0
sys.exit(main(sys.argv[1:]))
