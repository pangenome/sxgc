#!/usr/bin/env python3
from collections import Counter
import json,os,subprocess
from pathlib import Path
root=Path('/mnt/nvme2n1/erikg/sol8a-measure-oracle');root.mkdir(exist_ok=True)
map_path=Path('/mnt/nvme2n1/erikg/sol6c-gate100m/remap-256.bin');mapping=map_path.read_bytes()
for i,raw in enumerate([b'\x00',b'\x00'*17,b'\x00\x05\x00\x00\x05\x00',bytes(range(256))]):
    path=root/f'corpus-{i}';path.write_bytes(raw);text=raw.translate(mapping)
    result=subprocess.check_output(['/mnt/nvme2n1/erikg/sol8a-measure/measure',str(path),str(map_path)],env=dict(os.environ,CROSS_POOL_SLOTS='65536'),stderr=subprocess.DEVNULL,text=True)
    for line in result.splitlines():
        h=json.loads(line);counts=Counter(text[p:p+h['N']] for p in range(len(text)))
        bins=[0]*64
        for size in counts.values():bins[size.bit_length()-1]+=1
        assert h['singleton_count']==sum(size==1 for size in counts.values())
        assert h['residue_suffixes']==sum(size for size in counts.values() if size>1)
        assert h['residue_buckets']==sum(size>1 for size in counts.values())
        assert h['multiplicity_log2_bucket_counts']==bins
print('RADIX_MEASUREMENT_ORACLE_PASS cases=20 short_suffixes=1 NUL_padding_collision=1 production_remap=1 full_byte_alphabet=1 exact_multiplicities=1')
