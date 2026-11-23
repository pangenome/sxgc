"""Small exact byte oracle, including final triggers and unequal spans with equal tails."""
import itertools
from pathlib import Path
import random
import struct
import subprocess
import tempfile

BIN = Path(__file__).resolve().parents[1] / 'xsa/target/release/overlap_resident'

def trigger(s):
    h = 1469598103934665603
    for c in s:
        h = ((h ^ c) * 1099511628211) % (1 << 64)
    return h % 31 == 0

def prepare(d, text):
    starts = [0] + [i for i in range(1, len(text)-7) if trigger(text[i:i+8])]
    end = len(starts)-1
    words = [(text[s:starts[j+1]+8] if j < end else text[s:], j == end) for j,s in enumerate(starts)]
    refs = {w: (s, len(w[0]) | (w[1] << 31)) for w,s in zip(words, starts)}
    ids = {w:i for i,w in enumerate(sorted(refs))}
    parse = [ids[w] for w in words]
    (d/'corpus').write_bytes(text)
    (d/'dictionary.refs').write_bytes(b''.join(struct.pack('<II', *refs[w]) for w in ids))
    (d/'parse.u32').write_bytes(struct.pack('<%dI'%len(parse), *parse))
    inv = [0]*len(parse)
    for r,i in enumerate(sorted(range(len(parse)),key=lambda j:parse[j:]), 1): inv[i] = r
    (d/'expected').write_bytes(struct.pack('<%dI'%len(inv), *inv))

def main():
    rng = random.Random(1837)
    t = next(a.to_bytes(8,'big') for a in range(10000) if trigger(a.to_bytes(8,'big')))
    cases = [b'x', b'a'*35, b'\x00\xff'+t, (b'ab'+t)*50, t*32+t[:-1], b'abcd'*300]
    for j in range(12):
        alphabet = bytes(rng.sample(range(256), [3,15,50][j%3])); cases.append(rng.choices(alphabet,k=1400)); cases[-1] = bytes(cases[-1])
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp); args=[BIN,d,d/'stop','200000000',d/'expected']
        for text in cases:
            prepare(d,text); n=len(text)
            rc=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            assert rc.returncode==0, rc.stderr
            rows=list(struct.iter_unpack('<III',rc.stdout)); sa=sorted(range(n+1),key=lambda i:text[i:]); assert len(rows)==len(sa)
            for r,(p,h,b) in enumerate(rows):
                want=sum(1 for _ in itertools.takewhile(lambda pair:pair[0]==pair[1],zip(text[p:],text[sa[r-1]:]))) if r else 0
                assert (p,h,b)==(sa[r],want,text[p-1] if p else 256)
        (d/'expected').write_bytes(bytes((d/'expected').stat().st_size)); wrong=subprocess.run(args,capture_output=True); assert wrong.returncode and b'external rank mismatch' in wrong.stderr and not wrong.stdout
        small=subprocess.run([*args[:3],'32'],capture_output=True); assert small.returncode and not small.stdout
        prepare(d,bytes(range(256))); wide=subprocess.run(args,capture_output=True); assert wide.returncode and b'two free codes' in wide.stderr and not wide.stdout
        (d/'stop').touch(); assert subprocess.run(args,capture_output=True).returncode==75
    print(f'PASS {len(cases)} complete SA/LCP/BWT streams; wrong rank, width, budget refusals; stop=75')

if __name__=='__main__': main()
