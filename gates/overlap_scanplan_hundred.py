"""Conditional fixed-input 100 GB scan only. No product or downstream dispatch."""
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time

import overlap_defaults as defaults
import overlap_hundred_preflight as contract
import overlap_scanplan_gate as gate
from overlap_scanplan_census import CELLS, TEN, census, plan
from overlap_segmented_gate import need

CUTOFF = 1795531169
ROOT = gate.HERE.parent
LIVE = Path.home()/'sxgc'
DOCS = ('OVERLAP_DEFAULTS.md', 'OVERLAP_10G_STUDY.md', 'OVERLAP_100G_CONTRACT.md',
        'docs/overlap_scanplan.md')
DEFAULT = 'gates/receipts/scale-10g/default.json'
ONE_DEFAULT = 'gates/receipts/radix-full-1g/default.json'
SCAN = 'gates/receipts/scanplan-10g/scan.json'
FRONT = 'gates/receipts/packed-front-10g/front-end.json'
CALIBRATION_SHA = 'fea28d298533a0096945441477b2c9778b0dff471bbe260796f45be43922b049'
FRONT_SHA = '5e875c44f9293e35471e2b30645f15cb02c8a44164aa4adcd679144e688c06bf'
FIELDS = ('n', 'm', 'starts_above_u32', 'alphabet', 'max_phrase')


def git(*args):
    return subprocess.check_output(['git', '-C', ROOT, *args], timeout=60).decode().strip()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def cg_check(cg):
    need((cg['memory.max'], cg['memory.swap.max'], cg['cpu.max']) ==
         (str(192<<20), '0', '100000 100000') and 0 < int(cg['memory.peak']) <= 192<<20 and
         {'oom 0', 'oom_kill 0'} <= set(cg['memory.events'].splitlines()), 'scan grant/peak/OOM')


def calibration_clock(s: dict, bank: dict) -> float:
    c = s['census']; cells = c['cells']; n = c['n']; page = os.sysconf('SC_PAGE_SIZE')
    need(s['status'] == 'CALIBRATED_10G_SCAN_ONLY' and s['brute_fixture'] == 'passed' and
         'error' not in s and s['launch_authorized'] is False and
         re.fullmatch('[0-9a-f]{64}', s['kernel_sha256']) and
         re.fullmatch('[0-9a-f]{64}', c['sha256']) and s['copy_sha256'] == c['sha256'], 'calibration identity')
    need(n == TEN == s['private_copy_budget'] == s['private_copy_bytes'] == s['source_stat'][2] and
         s['source'] == str(gate.RUNGS['10g'][1]) and s['counted_column_scratch_bytes'] == 0 and
         s['mincore_checked_pages'] == (n+page-1)//page and s['mincore_resident_pages'] == 0 and
         s['free_before'] >= n+(1<<30) and s['free_after'] >= gate.MARGIN, 'calibration copy/cold')
    need(s['banked_fields'] == {k:bank[k] for k in FIELDS} == {k:c[k] for k in FIELDS}, 'front census mismatch')
    need(0 < len(cells) <= CELLS and all(type(k) is int and type(v) is int and
         0 <= k < CELLS and v > 0 for k,v in cells) and
         [k for k,_ in cells] == sorted({k for k,_ in cells}) and sum(v for _,v in cells) == n and
         sum(v for k,v in cells if k%257 == 0) == c['global_end_rows'] == 1 and
         len({k//257 for k,_ in cells}) == c['alphabet'] and
         8 <= c['eof_raw_length'] == n-c['eof_start'] <= c['max_owned_stride'] <= c['max_phrase'] and
         c['max_phrase'] <= c['max_owned_stride']+8 and
         s['models'] == [plan(c, size) for size in (256<<20, 1<<30)], 'histogram/planning mismatch')
    for field in ('cgroup_before', 'cgroup_after'):
        cg_check(s[field])
    clock = 0
    for phase in ('copy_cold', 'raw_scan'):
        p = s[phase]; rc = n+gate.MARGIN; wc = rc if phase == 'copy_cold' else gate.MARGIN
        need((p['read_cap'], p['write_cap']) == (rc, wc) and math.isfinite(p['wall_s']) and
             p['wall_s'] > 0 and math.isfinite(p['worker_cpu_s']) and p['worker_cpu_s'] > 0, 'phase caps/clock')
        def io(r,w):
            need(type(r) is int and type(w) is int and 0 <= r <= rc and 0 <= w <= wc, 'phase IO')
        worker = p['worker_io']; io(worker['read_bytes'], worker['write_bytes'])
        need(worker['read_bytes'] > 0 if phase == 'raw_scan' else worker['write_bytes'] >= n, 'missing physical IO')
        if p['per_device_io'] is not None:
            need(isinstance(p['per_device_io'], dict) and p['per_device_io'], 'invalid device IO')
            for dev,v in p['per_device_io'].items():
                need(re.fullmatch(r'\d+:\d+',dev), 'invalid device'); io(v['rbytes'], v['wbytes'])
        clock += p['wall_s']
    return clock


def budget(deadline: int, clock: float) -> int:
    required = math.ceil(15*clock+180)
    need(time.time()+required <= deadline <= CUTOFF, 'insufficient calibration headroom or deadline beyond cutoff')
    return required


def verify_default(data: bytes, docs: str, decision: dict, rung: str='10g') -> None:
    # Structural JSON equality distinguishes false from 0, unlike dict equality.
    need(json.dumps(json.loads(data),sort_keys=True) == json.dumps(decision,sort_keys=True) and
         decision['rung'] == rung and ((rung=='10g' and type(decision['normalized_speed_target_met']) is bool) or
         (rung=='1g' and decision['normalized_speed_target_met'] is None)) and
         decision['profile'] in docs and sha(data) in docs, 'undocumented/stale default')


def check_sweep(directory: Path, selected: str) -> None:
    profile = re.fullmatch(r'packed-10g-l[124](-b[01]-t[124]-r1)',selected)
    need(profile is not None, 'wrong sweep profile')
    for lanes in (1,2,4):
        arm = directory/f'packed-10g-l{lanes}{profile[1]}'
        origin = json.loads((arm/'source.json').read_bytes())
        need(origin['complete_arm'] is True and origin['identity'] is True and
             not (arm/'stop.requested').exists() and
             json.loads((arm/'build.json').read_bytes())['profile'] == arm.name, 'incomplete/misfiled sweep arm')


def v10_fixture(bank):
    # Product identity of the small rehearsal, NOT a production resource bound.
    for wire in ('narrow','row43'):
        v=json.loads((bank/f'{wire}.json').read_bytes());need(v['wire']==wire and v['n']==6311445 and
            v['rows']==6311446 and v['verified_equal_bytes']==8847248 and v['boundary_queries_passed'] is True and
            v['members']==[1,4,5,7,10,11,12,13,15,20] and v['full_sha256']=='55eea26f9d735664204fb58f33f7c601477bfbecfb8f34d298e6425ce92bcb07' and
            v['relay']=='relay rows=6311446 bands=193 credit=1' and v['finish_sha256']==contract.TOOLS[contract.CACHE/'xsa_finish'],'small v10 relay identity')
    s=json.loads((bank/'stalled.json').read_bytes());need(s['product_published'] is s['verification_published'] is False and s['relay']=='STOP_OBSERVED phase=v10-relay','small v10 stalled credit')

def admitted(deadline: int) -> dict:
    need(__debug__, 'assertions required'); budget(deadline, 0)
    rev = defaults.published_main(LIVE)
    need(git('rev-parse','HEAD') == rev and not git('status','--porcelain') and
         not git('status','--porcelain','--ignored','--','gates/receipts',*DOCS), 'checkout drift')
    need(all(line.startswith('100644 blob ') for line in
             git('ls-tree','-r',rev,'--','gates/receipts').splitlines()), 'nonregular receipt in bank')
    files = {}
    for name in (*DOCS, DEFAULT, ONE_DEFAULT, SCAN, FRONT):
        entry=git('ls-tree',rev,'--',name).split();need(len(entry)==4,'missing published evidence: '+name)
        mode, kind, obj, path = entry
        need((mode,kind,path)==('100644','blob',name), 'unpublished evidence')
        need(int(git('cat-file','-s',obj)) <= 8<<20, 'oversize admission document/receipt')
        files[name] = subprocess.check_output(['git','-C',ROOT,'cat-file','blob',obj])
    hashes = {k:sha(v) for k,v in files.items()}
    need(hashes[FRONT] == FRONT_SHA and hashes[SCAN] == CALIBRATION_SHA, 'reviewed calibration/front changed')
    for tag in ('serial','parallel','batch'):
        need((defaults.BANK/'radix-full-1g'/tag/'source.json').is_file(), 'portable one-GB bank required')
    # replay() landed with the full grid bank contract. Never accept the older
    # result.json-only experiment_order on a partial checkout.
    from overlap_ten_dict_grid import replay
    replay(defaults.BANK/'native-grid-10g')
    decision = defaults.decision('10g')
    check_sweep(defaults.BANK/'scale-10g',decision['profile']);v10_fixture(defaults.BANK/'v10-relay')
    docs = files[DOCS[0]].decode()
    verify_default(files[DEFAULT],docs,decision)
    verify_default(files[ONE_DEFAULT],docs,defaults.decision('1g'),'1g')
    s = json.loads(files[SCAN]); clock = calibration_clock(s,json.loads(files[FRONT])['census'])
    need(re.fullmatch('[a-f0-9]{40}', s['commit']), 'invalid calibration revision')
    git('merge-base','--is-ancestor', s['commit'], rev)
    git('diff','--exit-code',s['commit'],rev,'--','gates/overlap_scanplan_chunk.c','gates/overlap_scanplan_test.py')
    return dict(commit=rev, hashes=hashes, receipt_tree=git('rev-parse',rev+':gates/receipts'),
                profile=decision['profile'], under_2x=decision['normalized_speed_target_met'],
                calibration_kernel=s['kernel_sha256'], calibration_clock_s=clock, required_s=budget(deadline,clock))


def reservation(n):
    return n+(192<<20)+gate.MARGIN

def scan(d: Path, deadline: int, pin: str, report: dict) -> None:
    def live():
        need(time.time() < deadline and not (d/'stop.requested').exists(), 'deadline/stop')
    before = report['cgroup_before'] = gate.cg_info(); cg_check(before)
    need((gate.CG/'cgroup.procs').read_text().split() == [str(os.getpid())], 'dedicated service required')
    ticket = admitted(deadline)
    need(sha(json.dumps(ticket, sort_keys=True).encode()) == pin, 'admission changed across service transition')
    report.update(admission=ticket, cgroup=str(gate.CG), workload='100 GB raw copy+cold and single sequential census only',
                  product_promotion=False, counted_column_scratch_bytes=0)
    lib = gate.build_kernel(d, deadline, report, live)
    need(report['kernel_sha256'] == ticket['calibration_kernel'], 'calibrated kernel changed')
    gate.exclusive(); live()
    need(defaults.published_main(LIVE) == ticket['commit'], 'publication moved')
    budget(deadline,ticket['calibration_clock_s']); live()
    metadata = contract.inventory(); source = contract.ROOT/'pile-head100.txt'; n = contract.N
    need(metadata['corpus_stat_bytes'] == n and metadata['corpus_path'] == str(source) and
         metadata['manifest_only_sha256'] == contract.CORPUS, 'contract metadata mismatch')
    original = gate.identity(source); free = shutil.disk_usage(d).free
    need(stat.S_ISREG(source.lstat().st_mode) and original[2] == n and free >= n+(1<<30), 'source/space')
    # Even if the private unlinked copy is charged, its known N bytes plus the
    # entire working RAM grant and private helper/receipt margin stay under 2N.
    reserve=reservation(n);need(reserve<=2*n,'conservative scan reservation')
    report.update(metadata_match=True, manifest_sha256=contract.CORPUS, source=str(source),
                  source_stat=original, free_before=free, private_copy_budget=n,
                  conservative_scan_reservation=reserve, counted_limit=2*n)
    budget(deadline,ticket['calibration_clock_s']); live()
    with tempfile.TemporaryFile(dir=d) as raw:
        fd = raw.fileno(); st = os.fstat(fd)
        need(stat.S_ISREG(st.st_mode) and st.st_nlink == st.st_size == 0 and
             st.st_uid == os.getuid() and (st.st_dev,st.st_ino) != original[:2], 'not a new anonymous private file')
        prep = gate.Phase(d,report,'copy_cold',deadline,n+gate.MARGIN,n+gate.MARGIN)
        h = hashlib.sha256(); left = n
        with open(source,'rb',opener=lambda p,f:os.open(p,f|os.O_NOFOLLOW)) as src:
            ident = os.fstat(src.fileno()); need((ident.st_dev,ident.st_ino,ident.st_size)==original[:3], 'source replaced')
            while left:
                prep.guard(); block = src.read(min(left,1<<20)); need(block,'short source')
                raw.write(block); h.update(block); left -= len(block)
            need(not src.read(1) and gate.identity(source)==original, 'source changed')
        raw.flush(); need(os.fstat(fd).st_size == n and h.hexdigest()==contract.CORPUS, 'manifest/copy mismatch')
        os.fchmod(fd,0o400)
        with open(f'/proc/self/fd/{fd}','rb') as frozen:
            raw.close()  # the only writable reference to the anonymous inode
            fd=frozen.fileno();need(fcntl.fcntl(fd,fcntl.F_GETFL)&os.O_ACCMODE==os.O_RDONLY,'unsealed copy')
            gate.cold(Path(f'/proc/self/fd/{fd}')); prep.guard(True)
            report.update(copy_sha256=h.hexdigest(), private_copy_bytes=n, immutable_read_fd=True, mincore_resident_pages=0,
                          mincore_checked_pages=(n+os.sysconf('SC_PAGE_SIZE')-1)//os.sysconf('SC_PAGE_SIZE'))
            phase = gate.Phase(d,report,'raw_scan',deadline,n+gate.MARGIN,gate.MARGIN)
            c = census(lib,frozen,n,phase.guard); phase.guard(True)
            need(c['sha256'] == contract.CORPUS and phase.record['worker_io']['read_bytes'] > 0, 'scan SHA/cold IO')
            alloc=os.fstat(fd).st_blocks*512
            report.update(census=c,models=[plan(c,size) for size in (256<<20,1<<30)], private_copy_allocated=alloc,
                          whole_run_allocated_before_receipt=alloc+sum(p.stat().st_blocks*512 for p in [d,*d.iterdir()]),
                          free_after_scan=shutil.disk_usage(d).free)
    after = report['cgroup_after'] = gate.cg_info(); cg_check(after); live()
    report['status'] = 'MEASURED_100G_SCAN_ONLY'


def worker(deadline, pin):
    with defaults.timing_lease():
        d = Path(tempfile.mkdtemp(prefix='scanplan-100g-',dir=gate.SANDBOX))
        print(f'scan-only receipt={d}', flush=True)
        report = dict(status='REFUSED',deadline=deadline,launch_authorized=False)
        try:
            scan(d,deadline,pin,report)
        except Exception as exc:
            report['error'] = repr(exc); raise
        finally:
            (d/'scan.json').write_text(json.dumps(report,separators=(',',':'))+'\n')


if __name__ == '__main__':
    try:
        if len(sys.argv)==3 and sys.argv[1]=='admit':
            ticket = admitted(int(sys.argv[2]))
            print(ticket['commit'], sha(json.dumps(ticket,sort_keys=True).encode()))
        elif len(sys.argv)==4 and sys.argv[1]=='worker' and re.fullmatch('[0-9a-f]{64}',sys.argv[3]):
            worker(int(sys.argv[2]),sys.argv[3])
        else:
            raise ValueError('expected fixed scan-only admission or dedicated worker')
    except Exception as exc:
        print('100 GB SCAN HOLD:',repr(exc),file=sys.stderr); sys.exit(75)
