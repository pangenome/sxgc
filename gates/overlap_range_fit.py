"""Dry fit of an intermediate resident-dictionary, bounded-descriptor sort.
Uses the archived exact lexical interval masses (never touches the 1 GB text).
Counts *logical* distribution visits for one dictionary traversal per batch;
not a runtime, physical read, end-to-end grant, or a beyond-RAM dictionary test.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

N,M,K,D = 1_000_000_665,31_784_026,25_336_659,1_166_209_205

def load(path):
    buf=path.read_bytes(); n,=struct.unpack_from('<I',buf); at=4; windows=[]
    for _ in range(n):
        desc,key_bytes,rows,tag=struct.unpack_from('<QQQI',buf,at)
        at+=28+(tag&0x7fffffff); windows.append((desc,key_bytes,rows))
    assert at==len(buf) and len(windows)==3340 and sum(x[0] for x in windows)==938_179_281
    assert sum(x[1] for x in windows)==40_925_082_224 and sum(x[2] for x in windows)==N
    return windows, hashlib.sha256(buf).hexdigest()

def pack(windows,arena,record_bytes,copy_keys):
    batches=[]; used=rows=0
    for desc,key_bytes,r in windows:
        cost=desc*record_bytes+(key_bytes if copy_keys else 0)
        if cost>arena: raise ValueError('exact interval exceeds arena: refine (never split equal keys) or refuse')
        if used+cost>arena: batches.append((used,rows)); used=rows=0
        used+=cost; rows+=r
    if rows: batches.append((used,rows))
    assert sum(r for _,r in batches)==N
    return dict(arena=arena,record_bytes=record_bytes,copy_tail_bytes=copy_keys,batches=len(batches),
                peak_payload=max(u for u,_ in batches),largest_batch_rows=max(r for _,r in batches),
                logical_dictionary_scan_bytes=D*len(batches),distinct_output_rows_plus_end=N+1)

def main():
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument('plan',type=Path); args=cli.parse_args()
    w,digest=load(args.plan)
    # No dense owner/SA: the source word is known when a descriptor is generated.
    # Only valid if the dictionary and these lists remain explicitly resident.
    core=D+8*(K+1)+4*(K+1)+9*M+4*M+8*((M+15)//16)
    # Each equal tail group lies in one exact source window, one cursor per descriptor;
    # account for BinaryHeap geometric growth from capacity 4. Not a row-mass estimate.
    largest=max(x[0] for x in w); heap_bytes=20*(1<<(max(largest,4)-1).bit_length())
    variants=[pack(w,a,record,copy) for a in (256<<20,1<<30) for record,copy in [(8,False),(16,False),(16,True)]]
    print(json.dumps(dict(plan_sha256=digest,intervals=len(w),resident_dict_lists_rmq_bytes=core,
                          heap_bound_bytes=heap_bytes,routing_model='one full physical-order dictionary traversal per macro batch, no disk IO inferred',
                          excludes='source parser, finish, allocator, control, cache, oracle; not a speed or cgroup pass',profiles=variants),indent=2))

if __name__=='__main__': main()
