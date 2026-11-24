"""Ten-GB scan-only calibration. No production input path overrides."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from overlap_scanplan_census import TEN, census, plan
from overlap_segmented_gate import (CG, RECEIPTS, RUNGS, SANDBOX, cg_info, cold,
                                    current_io, exclusive, identity, io_delta, need)
from overlap_ten_matched_score import metrics
from overlap_scanplan_test import fixtures

HERE = Path(__file__).resolve().parent
MARGIN = 64 << 20


def worker_io():
    counters = dict(s.split(': ') for s in Path('/proc/self/io').read_text().splitlines())
    return {k: int(counters[k]) for k in ('read_bytes', 'write_bytes')}


class Phase:
    def __init__(self, d, report, name, deadline, read_cap, write_cap):
        self.record = report[name] = dict(read_cap=read_cap, write_cap=write_cap)
        self.d, self.deadline = d, deadline
        self.cpu = time.process_time()
        self.origin = worker_io()
        self.devices = current_io(CG)
        self.start = self.next_poll = time.monotonic()

    def guard(self, force=False):
        now = time.monotonic()
        need(time.time() < self.deadline and not (self.d/'stop.requested').exists(), 'deadline/stop')
        if not force and now < self.next_poll:
            return
        self.next_poll = now + .25
        r = self.record
        r['wall_s'] = now-self.start
        r['worker_cpu_s'] = time.process_time()-self.cpu
        r['worker_io'] = {k:v-self.origin[k] for k,v in worker_io().items()}
        delta = r['per_device_io'] = io_delta(self.devices, current_io(CG))
        for read, write in [(r['worker_io']['read_bytes'], r['worker_io']['write_bytes']),
                            *((d['rbytes'], d['wbytes']) for d in (delta or {}).values())]:
            need(0 <= read <= r['read_cap'] and 0 <= write <= r['write_cap'], 'phase IO guard')
        need(shutil.disk_usage(self.d).free >= MARGIN, 'free space guard')


def build_kernel(d, deadline, report, live):
    lib = d/'chunk.so'
    live(); subprocess.run(['/usr/bin/cc','-std=c11','-O3','-Wall','-Wextra','-Werror','-fPIC','-shared',
                            HERE/'overlap_scanplan_chunk.c','-o',lib],check=True,timeout=deadline-time.time())
    fixtures(lib); live(); report.update(brute_fixture='passed', kernel_sha256=hashlib.sha256(lib.read_bytes()).hexdigest())
    return lib


def calibrate(d, deadline, report):
    stop = d/'stop.requested'
    def live():
        need(time.time() < deadline and not stop.exists(), 'deadline/stop')
    live(); exclusive()
    cg = report['cgroup_before'] = cg_info()
    need((cg['cpu.max'], cg['memory.max'], cg['memory.swap.max']) ==
         ('100000 100000', str(192<<20), '0') and
         (CG/'cgroup.procs').read_text().split() == [str(os.getpid())], 'wrong dedicated grant')
    need(__debug__, 'optimized Python disables receipt checks')
    control = RECEIPTS/'packed-serial-10g'
    need(metrics(control)['profile'] == 'packed-10g-bitmap', 'exact full control required')
    ref = (RECEIPTS/'packed-front-10g/front-end.json').read_bytes()
    need(hashlib.sha256(ref).hexdigest() == '5e875c44f9293e35471e2b30645f15cb02c8a44164aa4adcd679144e688c06bf', 'bank changed')
    bank = json.loads(ref)['census']
    report.update(commit=subprocess.check_output(['git','-C', HERE, 'rev-parse','HEAD'], text=True).strip(),
                  control=str(control), cgroup=str(CG))
    lib = build_kernel(d, deadline, report, live)
    _, source, _ = RUNGS['10g']; raw = d/'corpus'
    original = identity(source)
    free = shutil.disk_usage(d).free
    need(source.is_file() and original[2] == TEN and free >= TEN+(1<<30), 'source/space')
    report.update(source=str(source), source_stat=original, private_copy_budget=TEN,
                  counted_column_scratch_bytes=0, free_before=free)
    prep = Phase(d, report, 'copy_cold', deadline, TEN+MARGIN, TEN+MARGIN)
    h = hashlib.sha256()
    with source.open('rb') as src, raw.open('xb') as dest:
        left = TEN
        while left:
            prep.guard(); data = src.read(min(left, 1<<20)); need(data, 'short source')
            dest.write(data); h.update(data); left -= len(data)
        need(not src.read(1), 'long source')
    need(identity(source) == original, 'source changed')
    raw.chmod(0o400); cold(raw); prep.guard(True)
    raw_id = identity(raw)
    report.update(copy_sha256=h.hexdigest(), private_copy_bytes=raw_id[2], mincore_resident_pages=0,
                  mincore_checked_pages=(TEN+os.sysconf('SC_PAGE_SIZE')-1)//os.sysconf('SC_PAGE_SIZE'))
    scan = Phase(d, report, 'raw_scan', deadline, TEN+MARGIN, MARGIN)
    with raw.open('rb') as stream:
        c = census(lib, stream, TEN, scan.guard)
    scan.guard(True)
    need(scan.record['worker_io']['read_bytes'] > 0 and identity(raw) == raw_id and c['sha256'] == h.hexdigest(), 'scan IO/identity')
    report['banked_fields'] = {k:bank[k] for k in ('m','starts_above_u32','n','alphabet','max_phrase')}
    need(all(c[k] == v for k,v in report['banked_fields'].items()), 'bank mismatch')
    report.update(census=c, models=[plan(c, size) for size in (256<<20, 1<<30)],
                  free_after=shutil.disk_usage(d).free, pre_receipt_whole_dir_allocated=sum(p.stat().st_blocks*512 for p in [d,*d.iterdir()]))
    after = report['cgroup_after'] = cg_info()
    need({'oom 0','oom_kill 0'}.issubset(after['memory.events'].splitlines()), 'OOM')
    live(); report['status'] = 'CALIBRATED_10G_SCAN_ONLY'


def main(deadline):
    need(time.time() < deadline, 'expired')
    with (Path(os.environ['XDG_RUNTIME_DIR'])/'overlap-ten-timing.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)
        d = Path(tempfile.mkdtemp(prefix='scanplan-10g-',dir=SANDBOX))
        print(f'scan-only receipt={d}',flush=True)
        report = dict(status='REFUSED',deadline=deadline,launch_authorized=False)
        try:
            calibrate(d, deadline, report)
        except Exception as exc:
            report['error'] = repr(exc); raise
        finally:
            (d/'scan.json').write_text(json.dumps(report, separators=(',',':'))+'\n')


if __name__ == '__main__':
    main(int(sys.argv[1]))
