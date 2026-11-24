"""Native-wide dictionary component only. Never emits rows or writes an SA."""
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess as sp
import sys
import threading
import time

from overlap_resident_gate import ROOT, TEN, cg_info, save
from overlap_scaling_policy import one_series
from overlap_ten_matched_score import metrics as certified_ten
from private_cold import cold
from overlap_timing_idle import exclusive

SANDBOX = ROOT / 'sandbox'
SOURCE = SANDBOX / 'packed-front-10g-YebiCY'
REPO = Path(__file__).resolve().parents[1]
GRID = dict(XSA_RESIDENT_DICT_GRID='1', XSA_RESIDENT_PACKED='1', XSA_RESIDENT_WIDE_DICT='1',
            OMP_DYNAMIC='FALSE', OMP_WAIT_POLICY='PASSIVE', GOMP_SPINCOUNT='0',
            OMP_NUM_THREADS='4', OMP_THREAD_LIMIT='4')


def load(d, name):
    return json.loads((d / name).read_text())


def before(deadline):
    assert time.time() < int(deadline), 'absolute deadline expired'


def prerequisites(ten, pair, deadline):
    before(deadline)
    exclusive()
    i = load(ten, 'identity.json')
    assert i['verified'] and i['sha256'] == i['expected'] == TEN.sha and i['reference_table_identical'] and len(i['members']) == 9
    assert (ten / 'exits.log').read_text().strip() == 'build=0 oracle=0'
    assert load(ten, 'build.json')['profile'] == 'packed-10g-bitmap' and load(ten, 'oracle.json')['rc'] == 0
    assert f'ORACLE rows={TEN.n+1} independent_final_rank={TEN.n+1} all_adjacent_sa_lcp_bwt_verified=1' in (ten/'oracle.log').read_text()
    assert certified_ten(ten)['profile']=='packed-10g-bitmap'
    one_series(pair)  # same attestation and score replay as the full-product policy
    ref = load(REPO/'gates/receipts/packed-front-10g', 'front-end.json')
    assert load(SOURCE, 'front-end.json') == ref and ref['columns'] == load(ten, 'front-end.json')['columns']
    assert all((SOURCE/'columns'/k).stat().st_size == n for k,n in ref['columns'].items())
    assert shutil.disk_usage(SANDBOX).free >= sum(ref['columns'].values()) + (2 << 30)
    mem = int(re.search(r'^MemAvailable:\s+(\d+)', Path('/proc/meminfo').read_text(), re.M)[1])*1024
    assert mem >= 232 << 30
    return ref


def score(log, entries):
    rows = []
    for line in log.splitlines():
        if not line.startswith('DICT_GRID '):
            continue
        row = dict(x.split('=') for x in line.split()[1:])
        for k in ('threads', 'entries', 'peak_rss', 'read_bytes', 'write_bytes'):
            row[k] = int(row[k]); assert row[k] >= 0
        for k in ('wall_s', 'cpu_s', 'hash_s'):
            row[k] = float(row[k]); assert math.isfinite(row[k]) and row[k] >= 0
        assert row['wall_s'] > 0 and row['peak_rss'] > 0 and row['entries'] == entries and re.fullmatch('[0-9a-f]{64}', row['sha256'])
        rows.append(row)
    assert [r['threads'] for r in rows] == [1,4,2] and len({r['sha256'] for r in rows}) == 1
    fastest = min(r['wall_s'] for r in rows)
    picks = [r['threads'] for r in rows if r['wall_s'] <= 1.05*fastest and
             all(r[k] <= rows[0][k] for k in ('read_bytes','write_bytes'))]
    return dict(arms=rows, selected_threads=min(picks, default=None), scope='in-memory dictionary sort only; no product, prep or hash time in arm wall')


def worker(d, exe, ten, pair, deadline):
    ref = prerequisites(ten, pair, deadline)
    cg = cg_info()
    assert (cg['memory.max'], cg['cpu.max'], cg['memory.swap.max']) == (str(200<<30),'400000 100000','0')
    save(d/'inputs.json', dict(source=str(SOURCE), columns=ref['columns'], receipt10=str(ten), pair1=str(pair), deadline=int(deadline), cgroup=cg, array_grant=140<<30, env=GRID))
    col = d/'columns'; col.mkdir()
    flag = d/'stop.requested'
    for k in ('dictionary.packed','parse.u32'):
        if flag.exists(): raise SystemExit(75)
        before(deadline)
        sp.run(['cp','--reflink=never',SOURCE/'columns'/k,col/k], check=True)
        assert not (col/k).samefile(SOURCE/'columns'/k) and (col/k).stat().st_size == ref['columns'][k]
        cold(col/k)
    (d/'cold.log').write_text('private column copies: mincore zero resident pages\n')
    before(deadline); exclusive()
    if flag.exists() or int(deadline)-time.time()<4200:
        save(d/'admission.json',dict(started=False,remaining_s=int(deadline)-time.time(),required_s=4200));raise SystemExit(75)
    env = dict(PATH=os.defpath, **GRID, XSA_GATE_PACKED_10G='1', XSA_GRID_DEADLINE=str(deadline))
    # stderr through a pipe so receipt logging writes are not charged to the sort.
    with open('/dev/full','wb') as no_rows, (d/'resident.log').open('xb') as log:
        p = sp.Popen([exe,col,d/'stop.requested',str(140<<30)], env=env, stdout=no_rows, stderr=sp.PIPE)
        def drain():
            for line in p.stderr:
                log.write(line); log.flush()
                if line.startswith((b'DICT_GRID ',b'RESIDENT_DICT_SORT ')):
                    print(line.decode().rstrip(),flush=True)
        reader = threading.Thread(target=drain); reader.start()
        try:
            while p.poll() is None:
                # No signals to an in-flight native sort. It checks the flag at
                # the next phase/hash boundary. Reserve ample time at admission.
                if not flag.exists():
                    try: before(deadline); exclusive(p.pid)
                    except (AssertionError,RuntimeError) as e:
                        flag.write_text(str(e)+'\n'); print('STOP_REQUEST',e,flush=True)
                try: p.wait(timeout=5)
                except sp.TimeoutExpired: pass
        finally:
            if p.poll() is None: flag.touch()
            p.wait(); reader.join()
            cg = cg_info(); save(d/'exit.json',dict(rc=p.returncode,cgroup=cg))
    if flag.exists() or p.returncode == 75: raise SystemExit(75)
    assert p.returncode == 0 and int(cg['memory.peak']) <= 200<<30 and {'oom 0','oom_kill 0'} <= set(cg['memory.events'].splitlines())
    log = (d/'resident.log').read_text(); assert 'RESIDENT_STREAM' not in log and log.count('kernel=native_wide') == 3
    assert f"RESIDENT_CHECK recurrence_ranks={ref['census']['m']} virtual_end=0" in log
    result = score(log,ref['census']['D']); before(deadline); save(d/'result.json',result)
    if result['selected_threads'] is None: raise SystemExit(75)


if __name__ == '__main__':
    assert not sys.flags.optimize
    if sys.argv[1] == 'preflight': prerequisites(Path(sys.argv[2]),Path(sys.argv[3]),sys.argv[4])
    elif sys.argv[1] == 'worker': worker(*(Path(x) for x in sys.argv[2:6]),sys.argv[6])
    else: raise SystemExit('unknown grid command')
