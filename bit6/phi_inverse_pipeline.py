#!/usr/bin/env python3
"""Resume after successful G0 dump, enforcing the remaining gate ladder.

All products go to /tmp/laneS; all baseline inputs remain read-only.
"""
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
LOG = ROOT/'bit6/phi_inverse_logs'
OUT = pathlib.Path('/tmp/laneS')
YEAST = pathlib.Path('/mnt/nvme3n1/erikg/sxgc-yeast/grl')
K10 = pathlib.Path('/mnt/nvme3n1/erikg/sxgc-pilot/k10')
K466 = pathlib.Path('/mnt/nvme3n1/erikg/sxgc-pilot/k466')
CHECK = ROOT/'bit6/phi_inverse_check.py'
EXTRACT = OUT/'phi_inverse_heads'
XSA = ROOT/'xsa/target/release/xsa'
state = json.loads((LOG/'state.json').read_text()) if '--resume-g1' in sys.argv else {}

def note(message):
    print(time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), message, flush=True)

def run(name, command, diagnostic=False):
    note(f'START {name}: '+ ' '.join(map(str, command)))
    start = time.monotonic()
    with open(LOG/(name+'.log'), 'w') as f:
        result = subprocess.run(['/usr/bin/time', '-v', *map(str, command)], stdout=f, stderr=subprocess.STDOUT)
    state[name] = {'exit': result.returncode, 'wall_seconds': time.monotonic()-start}
    (LOG/'state.json').write_text(json.dumps(state, indent=2)+'\n')
    note(f'END {name}: {state[name]}')
    if result.returncode and not diagnostic:
        raise RuntimeError(f'STOP: gate {name} failed; see {LOG/(name+".log")}')
    return (LOG/(name+'.log')).read_text()

if '--wait-g0' in sys.argv:
    deadline = time.monotonic()+7200
    while '\tExit status:' not in (LOG/'G0.dump.log').read_text():
        assert time.monotonic() < deadline, 'timed out waiting for owned G0 dump'
        time.sleep(5)
assert '\tExit status: 0' in (LOG/'G0.dump.log').read_text(), 'G0 dump must finish successfully first'
assert 'PASS saFirst byte identity' in (LOG/'G0.head-byte.log').read_text()
if '--resume-g1' not in sys.argv:
    run('G0.agg-diagnostic', ['cmp', OUT/'yeast.phi.agg', '/tmp/laneY/yeast_pfp2.agg'], diagnostic=True)
    text = run('G0.sweep', [XSA, 'chi-rspace', '--stream-agg', '--ri4', '/tmp/laneQ/pass3/yp2real.ri4', '--agg', OUT/'yeast.phi.agg', '-o', OUT/'yeast.phi.sA'])
    assert 'chi = 85404240 ' in text, 'STOP: incorrect yeast chi'
    run('G0.sets', ['python3', CHECK, 'sets', OUT/'yeast.phi.sA', YEAST/'chi_yeast_pfp2.sA', '85404240', OUT/'yeast-sets'])
    note('G0 PASS END-TO-END chi=85404240 witness sorted-set equality=True')
else:
    assert state['G0.sets']['exit'] == 0 and state['G0.sweep']['exit'] == 0
    assert '\tExit status: 0' in (LOG/'G0.parallel-byte.log').read_text()
    note('RESUME G1 after parallel decoder re-gate: yeast heads byte-identical; retained successful G0 end-to-end result')
run('G1.extract', [EXTRACT, K10/'h10rt.lcp_index.lcp_index', K10/'h10new2.ri4', OUT/'h10.phi.head_sa', '64'])
run('G1.byte', ['cmp', OUT/'h10.phi.head_sa', K10/'h10.head_sa'])
note('G1 PASS head-SA byte identity')
run('G2.extract', [EXTRACT, K466/'h466rt.lcp_index.lcp_index', K466/'h466.ri4', OUT/'h466.head_sa', '64'])
run('G2.anchors', ['python3', CHECK, 'anchors', K466/'h466.ri4', OUT/'h466.head_sa', K466/'h466.anchors'])
note('G2 PASS range and anchor checks')
# A present file alone is insufficient: the producer must have finished.
parse = K466/'h466rl_pfp.parse'
processes = subprocess.check_output(['ps', '-eo', 'pid,args'], text=True)
active = [line for line in processes.splitlines() if '/home/erikg/pfp/build/pfp++' in line and 'h466rl_pfp' in line]
state['G3.parse'] = {'exists': parse.exists(), 'size': parse.stat().st_size if parse.exists() else None, 'active_producers': active}
(LOG/'state.json').write_text(json.dumps(state, indent=2)+'\n')
if not parse.exists() or active:
    note('G3 NOT RUN: 466 parse incomplete; stopped after G2 as instructed')
else:
    note('G3 parse present and producer exited; completion and <150 GB dump plan require inspection before launch')
    sys.exit(3)
