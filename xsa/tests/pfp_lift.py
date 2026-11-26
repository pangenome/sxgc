"""ID-level qualification: real parse + top sort + lift vs direct and brute rank.

Usage: python3 -BE pfp_lift.py BUNDLE_DIR. No raw bytes or corpus row producer.
"""
from array import array
import os
from pathlib import Path
import random
import re
import struct
import sys
import tempfile
from pfp_parse import model, run, trigger

ROOM = 64 << 20


def u64(path):
    return array('Q', path.read_bytes())


def check(tools, values, *, vary=False, shared=False, refuse=False):
    assert sys.byteorder == 'little' and array('Q').itemsize == 8
    values = tuple(values); n = len(values)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory); p = root/'parent'; q = root/'child'; q.mkdir()
        p.write_bytes(array('Q', values).tobytes())
        run([tools/'pfp_parse', 'ids', p, q, 1, ROOM, ROOM])
        tape, refs = model(values)
        owned = sum(s for _, s, _ in refs)
        assert u64(q/'parse.u64') == array('Q', tape)
        assert list(struct.iter_unpack('<3Q', (q/'phrases.ref').read_bytes())) == refs
        assert struct.unpack('<5Q', (q/'census').read_bytes()) == (n, len(tape), len(refs), owned, max(values))
        run([tools/'stream_rank', 'inverse', q/'parse.u64', q/'inverse', 1, ROOM])
        run([tools/'stream_rank', 'inverse', p, root/'direct', 1, ROOM])
        direct = u64(root/'direct')
        if n <= 1000:
            assert [direct[j] for j in sorted(range(n),key=lambda j:values[j:])] == list(range(1,n+1))
        cap = 16*owned+8*n

        def lift(name, workers, mem):
            out = root/name
            log = run([tools/'pfp_lift', p, q, out, workers, mem, cap])
            assert u64(out) == direct
            counts = dict((k.decode(),int(v)) for k,v in re.findall(rb'([a-z_]+)=(\d+)',log))
            assert counts['counted'] == counts['routed'] == counts['ordered'] == owned
            assert counts['upper'] == len(tape) and counts['lower'] == counts['ranks'] == n
            assert counts['array_bound'] <= mem and counts['posting_bytes'] == 16*owned
            assert not (q/'lift.offsets').exists()
            return counts

        one = lift('result', 1, ROOM)
        if shared: assert one['shared_groups'] > 0, one
        if vary:
            more = lift('small', 1, 512<<10)
            assert more['ranges'] > one['ranges'] and one['shared_groups'] == more['shared_groups']
            if len(os.sched_getaffinity(0)) >= 2: lift('parallel', 2, 4<<20)
            print(f"ranges {one['ranges']} -> {more['ranges']}; {owned} offsets counted/routed/ordered; {n} ranks identical")
        if refuse:
            def bad(mem, scratch):
                return run([tools/'pfp_lift', p, q, root/'absent', 1, mem, scratch], False)
            assert b'postings + lower inverse' in bad(ROOM,cap-1)
            assert b'budget' in bad(1024,cap)
            qi=u64(q/'inverse'); qi[0]=0; (q/'inverse').write_bytes(qi.tobytes())
            assert b'upper rank' in bad(ROOM,cap)
            assert not (root/'absent').exists() and not (q/'lift.offsets').exists()


def main(tools):
    tr = next((0,)*7+(i,) for i in range(256) if trigger((0,)*7+(i,)))
    for n in [1,7,8,9]: check(tools,[256]*n)
    check(tools,[44]*135,refuse=True)  # every window, including overlapping and final hits
    check(tools,tr+tr)
    # Numeric order differs from LE byte order; no reserved ID or u32 truncation.
    wide = [0,1,255,256,65536,2**32,2**48+1,2**63,2**64-1]
    rng = random.Random(447)
    check(tools,[rng.choice(wide) for _ in range(611)])
    tail=(3,1,4,1,5,9,2,6)
    check(tools,sum((tr+(i,)+tail for i in range(1,18)),())*3+tr,shared=True)
    for _ in range(16): check(tools,[rng.randrange(4) for _ in range(rng.randint(23,390))])
    check(tools,[rng.randrange(1<<14) for _ in range(24000)],vary=True)  # crosses 8192-ID scan pages
    print('PASS overlapping integer names, full inverse lift, END, wide IDs, continuation ties, disjoint ranges and refusal')


if __name__=='__main__': main(Path(sys.argv[1]).resolve())
