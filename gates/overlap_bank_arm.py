"""Bank bounded, replayable evidence, never corpus, product or row payloads.

Full originals stay in the private run. sink.log/finish.log in the bank are
explicitly selected lines; their original sizes, line counts and SHA are pinned.
A nonpassing score is evidence. Missing evidence is never synthesized as a pass.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

SMALL = ('binaries.sha', 'cold.log', 'exits.log', 'fixtures.log', 'seam-fixtures.log',
         'layout-test.log', 'front-end.json', 'pack.log', 'resident.log', 'scratch-plan.json',
         'build.json', 'build-source.log', 'oracle.json', 'oracle.log', 'identity.json', 'goals.json', 'summary.md',
         'stop.requested', 'arm.json', 'settings.json', 'matched.json', 'launch.json')
KEEP = {'sink.log': ('SS_SXLS_WRITER ', 'SS_PRIVATE_SXLS_RECLAIM ', 'SS_SCRATCH_PEAK ',
                     'SS_DONE_R ', 'STOP_OBSERVED', 'FATAL', 'thread '),
        'finish.log': ('XSA_FINISH_PFCK ', 'GATE_BITMAP ', 'XSA_FINISH_SWEEP_ANATOMY ',
                       'LCP_RUNS_ENCODING ', 'SXI2_MEMBER ', 'CONTAINER_FINAL ', 'XSA_FINISH_DONE ',
                       'XSA_FINISH_PEAK_RSS ', 'STOP_OBSERVED', 'FATAL')}

def bank(src, dest, incomplete=False):
    if not src.is_dir() or src.is_symlink() or dest.exists():
        raise ValueError('require original private arm directory and fresh destination')
    ident = json.loads((src/'identity.json').read_text()) if (src/'identity.json').exists() else {}
    build = json.loads((src/'build.json').read_text()) if (src/'build.json').exists() else {}
    if not incomplete:
        required = ('binaries.sha', 'cold.log', 'exits.log', 'front-end.json', 'resident.log',
                    'scratch-plan.json', 'oracle.json', 'oracle.log', 'sink.log', 'finish.log')
        if (not all((src/name).is_file() for name in required) or ident.get('verified') is not True or
                ident.get('reference_table_identical') is not True or build.get('stop') is not False or
                build.get('rc') != dict(resident=0, sink=0, finish=0) or
                (src/'exits.log').read_text().strip() != 'build=0 oracle=0'):
            raise ValueError('not a certified arm; preserve as --incomplete instead')
    dest.mkdir(parents=True)
    for name in SMALL:
        p = src/name
        if p.exists():
            if p.is_symlink() or not p.is_file() or p.stat().st_size > (16<<20):
                raise ValueError(f'not small receipt: {p}')
            shutil.copyfile(p, dest/name)
    logs = {}
    for name, prefixes in KEEP.items():
        p = src/name
        if not p.exists(): continue
        if p.is_symlink() or not p.is_file(): raise ValueError(f'not an original log: {p}')
        h = hashlib.sha256(); n = size = kept = kept_bytes = 0
        starts = tuple(x.encode() for x in prefixes)
        with p.open('rb') as f, (dest/name).open('xb') as g:
            for line in f:
                h.update(line); n += 1; size += len(line)
                if line.startswith(starts):
                    kept_bytes += len(line)
                    if kept_bytes > 4<<20: raise ValueError(f'unbounded selection: {name}')
                    g.write(line); kept += 1
        logs[name] = dict(sha256=h.hexdigest(), original_lines=n, original_bytes=size, selected_lines=kept)
    receipt = dict(source=str(src.resolve()), identity=ident.get('verified'), complete_arm=not incomplete,
                   score='see goals/comparison separately, not attested by this copy', selected_logs=logs)
    (dest/'source.json').write_text(json.dumps(receipt, indent=2, sort_keys=True)+'\n')
    return receipt

if __name__ == '__main__':
    ag = argparse.ArgumentParser(description=__doc__);ag.add_argument('source', type=Path)
    ag.add_argument('bank', type=Path);ag.add_argument('--incomplete', action='store_true');a=ag.parse_args()
    print(bank(a.source, a.bank, a.incomplete))
