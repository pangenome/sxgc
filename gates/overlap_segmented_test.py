"""Small positive/corrupt packed columns and fail-closed CLI tests. No bank IO."""
import hashlib
from pathlib import Path
import random
import struct
import subprocess as sp
import sys
import tempfile

from overlap_lexparse100_test import oracle
from overlap_segmented_gate import baseline, prerequisite, stat_io


def main(front, verify):
    prerequisite()
    assert baseline('1g')[0]==30.38 and baseline('10g')[0]>350
    assert stat_io('8:0 rbytes=9 wbytes=4\n8:1 wbytes=2 rbytes=7')==dict(rbytes=16,wbytes=6)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp); raw=d/'raw'; cols=d/'columns'; cols.mkdir()
        text=random.Random(617).randbytes(4096)*3
        raw.write_bytes(text); h=hashlib.sha256(text).hexdigest()
        tape,ids,_=oracle(text); p=cols/'dictionary.packed'; q=cols/'parse.u32'
        def check(t,i,why=None,grant=1<<20,sha=h):
            p.write_bytes(t); q.write_bytes(i)
            r=sp.run([verify,cols,str(len(text)),sha,str(grant)],capture_output=True)
            if why is None:assert r.returncode==0,r.stderr
            else:assert r.returncode!=0 and why.encode() in r.stderr,(why,r)
        check(tape,ids);check(tape,ids,'metadata grant',0);check(tape,ids,'SHA mismatch',sha='0'*64)
        check(tape,ids[:-1],'extent');check(tape,ids[:-4],'END');check(tape,ids[4:],'incomplete decode')
        check(tape,ids[:4]+b'\xff'*4+ids[8:],'range');check(tape,ids[-4:]+ids[4:],'END');check(tape[:-1],ids,'length')
        records=[];pos=0
        while pos<len(tape):
            tag,=struct.unpack_from('<I',tape,pos);end=pos+4+(tag&0x7fffffff);records.append(tape[pos:end]);pos=end
        check(records[1]+records[0]+b''.join(records[2:]),ids,'strictly lexical')
        check(tape[:4]+tape[4:6],ids,'length');check(struct.pack('<I',2)+b'ab',ids,'length')
        check(struct.pack('<I',32)+b'A'*32,ids,'boundary');check(struct.pack('<I',0x80000002)+b'xy',ids,'range')
        parse=list(x for (x,) in struct.iter_unpack('<I',ids));end_id=parse[-1]
        check(tape,b''.join(struct.pack('<I',x) for x in [parse[2]]+parse[1:]),'overlap')
        # Insert a valid, lexically smallest unreachable word, renumbering every ID.
        sample=oracle(b'\0'*128+text)[0];length=struct.unpack_from('<I',sample)[0];extra=sample[:4+length]
        check(extra+tape,b''.join(struct.pack('<I',x+1) for x in parse),'unused ID')
        endword=records[end_id]; t=b''.join(r if j!=end_id else struct.pack('<I',len(r)-4)+r[4:] for j,r in enumerate(records))
        check(t,ids,'boundary' if len(endword)>12 else 'length')
        q.unlink();r=sp.run([verify,cols,str(len(text)),h,'10000'],capture_output=True);assert r.returncode!=0
        for mode in ('--intermediate-1g','--intermediate-10g','--intermediate-100g'):
            r=sp.run([front,raw,d/'refused',mode],capture_output=True);assert r.returncode and not (d/'refused').exists()
    print('PASS offline verdict/baselines; exact decode, metadata, malformed words, truncated/missing/unused IDs, EOF, overlap, rung refusals')


if __name__=='__main__':main(*sys.argv[1:])
