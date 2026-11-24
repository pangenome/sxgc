"""Run only synthetic fixtures against the standalone census executable."""
from collections import Counter
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import time


def trigger(window):
    h = 1469598103934665603
    for b in window:
        h = (h ^ b) * 1099511628211 % 2**64
    return h % 31 == 0


def brute(text, base):
    ends = [i for i in range(1, len(text)-7) if trigger(text[i:i+8])]
    starts = [0, *ends]
    words = [(text[i:j+8], False) for i, j in zip(starts, ends)]
    words.append((text[starts[-1]:], True))
    distinct, mass = Counter(), Counter()
    for (raw, eof), f in Counter(words).items():
        for i in range(len(raw) - (0 if eof else 8)):
            key = raw[i]*257 + (raw[i+1]+1 if i+1 < len(raw) else 0)
            distinct[key] += 1
            mass[key] += f
    cells = {k: (v, mass[k]) for k, v in distinct.items()}
    k, u = len(set(words)), sum(distinct.values())
    d = u+9*k-7
    expected = dict(n=len(text), m=len(words), k=k, U=u, D=d, tape=d+3*k-1,
                    max_phrase=max(len(w) for w, _ in words), global_end_rows=1,
                    max_owned_stride=max(j-i for i, j in zip(starts, ends+[len(text)])),
                    alphabet=len(set(text)), starts_above_u32=sum(base+i>=2**32 for i in starts),
                    eof_start=base+starts[-1], eof_raw_length=len(text)-starts[-1])
    return expected, cells


def tagged_record_fixture(d):
    # A legitimate raw scan cannot produce this pair: the normal word ends
    # in a trigger which would split the EOF word. Exercise the scratch seam.
    src = Path(__file__).with_name('overlap_vocab_census.cpp').resolve()
    helper = d/'tag_test.cpp'
    helper.write_text(f'#define main census_entry\n#include "{src}"\n#undef main\n' + r'''
int main(){
    Census c; c.n=120; c.m=4; c.seg=64; c.deadline=UINT64_MAX; c.ioLimit=24*c.n;
    fs::create_directory("vocab.tmp"); Run r;
    Byte word[40]={};
    {Census::Writer w(c,r); for(unsigned h : {80,80,80,81}) w.put(h,word,0); w.close();}
    c.process(r);
    need(c.k==2 && c.sumF==4 && c.eofs==1 && c.u==72 && c.sumN==136 &&
         c.desc[1]==71 && c.occ[1]==135 && c.desc[0]==1 && c.occ[0]==1 &&
         c.rd==c.wr && !c.live && fs::is_empty("vocab.tmp"),"tag equality");
    fs::remove("vocab.tmp");
}
''')
    subprocess.run(['g++','-std=c++17','-O1',helper,'-o',d/'tag_test'],check=True)
    subprocess.run([d/'tag_test'],cwd=d,check=True)


def fixtures(exe):
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        tagged_record_fixture(d)
        def run(text, seg=8192, collisions=0, base=0, cap=2**63, deadline=None, refusal=None):
            (d/'corpus').write_bytes(text)
            args = [exe, '--fixture', str(int(time.time())+60 if deadline is None else deadline),
                    *map(str, (seg, collisions, base, cap))]
            r = subprocess.run(args, cwd=d, capture_output=True, text=True, timeout=65)
            assert not (d/'vocab.tmp').exists()
            if refusal:
                assert r.returncode == 75 and refusal in r.stderr and not r.stdout, r
                return
            assert r.returncode == 0, r.stderr
            head, *rows = r.stdout.splitlines()
            got = {a:int(b) for a,b in (v.split('=') for v in head.split())}
            cells = [(int(a), (int(b), int(c))) for a,b,c in (row.split() for row in rows)]
            expected, hist = brute(text, base)
            assert {k:got[k] for k in expected} == expected and cells == sorted(hist.items())
            assert got['peak'] <= 2*len(text) and got['scratch_read'] == got['scratch_write']
            return got
        rng = random.Random(342)
        t = next(i.to_bytes(8,'big') for i in range(10000) if trigger(i.to_bytes(8,'big')))
        run(b'x'); run(b'\xff'*40+t); run((b'ab'*14+t)*80, base=2**32-5)
        data = rng.randbytes(20000)*4
        assert run(data, 512, 1)['merge_levels'] >= 2
        assert run(data, 1024)['files'] > 64
        run(data, 32, refusal='record exceeds segment')
        run(data, cap=1, refusal='IO limit'); run(data, deadline=0, refusal='deadline')
        (d/'stop.requested').touch(); run(data, refusal='stop'); (d/'stop.requested').unlink()
        c = next(bytes([i]) for i in range(256) if trigger(bytes([i])*8))
        run(c*1000, refusal='scratch limit'); run(b'A'*(2**20+1), refusal='word limit')
        run(data, base=2**64-2, refusal='base overflow')
        assert subprocess.run([exe,'--internal-calibrate-10g',str(int(time.time())+60)],
                              cwd=d,capture_output=True).returncode == 75
    print('PASS exact fixture census, cells, forced collision/segmented merge, limits and cleanup')


if __name__ == '__main__':
    fixtures(Path(sys.argv[1]).resolve())
