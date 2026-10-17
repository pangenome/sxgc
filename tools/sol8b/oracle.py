#!/usr/bin/env python3
"""Independent exact contributor/mass oracle for the measurement extension."""
import json, math, pathlib, random, struct, subprocess, sys, tempfile
binary = sys.argv[1]
rng = random.Random(812)
fixtures = [b'\x00\x05', b'\x1e\x0a\x1e'*13, b' '*97,
            b'ab'*51, bytes(range(256)), b'abc\0abc\0abc\0',
            bytes(rng.randrange(256) for _ in range(101))]
cases = 0
with tempfile.TemporaryDirectory(prefix='sol8b-oracle-') as tmp:
    root = pathlib.Path(tmp)
    for data in fixtures:
        for nonidentity in (False, True):
            remap = bytearray(range(256))
            if nonidentity: remap[0], remap[5] = 5, 0
            corpus = root/'corpus'; corpus.write_bytes(data)
            mapping = root/'remap'; mapping.write_bytes(remap)
            chunks = root/'chunks'; chunks.mkdir(exist_ok=True)
            for old in chunks.iterdir(): old.unlink()
            cuts = sorted(set([0, len(data)//3, len(data)//2, len(data)]))
            cuts = [x for i,x in enumerate(cuts) if not i or x != cuts[i-1]]
            for i,(lo,hi) in enumerate(zip(cuts,cuts[1:])):
                # Synthetic compact headers: the measurement reads lengths only.
                (chunks/f'chunk-{i}.pos').write_bytes(
                    b'SXP4'+struct.pack('<IQQQQ',1,lo,hi-lo,hi-lo,0)+bytes(24))
            output = subprocess.check_output([binary,'--hybrid',str(corpus),str(mapping),str(chunks)],text=True,stderr=subprocess.DEVNULL)
            mapped = bytes(remap[c] for c in data)
            for row in map(json.loads,output.splitlines()):
                d = row['d']; expected = {}
                for p in range(len(data)):
                    # Python byte strings distinguish finite suffixes from NUL padding.
                    key = mapped[p:p+d]
                    c = next(i for i,hi in enumerate(cuts[1:]) if p < hi)
                    mass, contributors = expected.setdefault(key,[0,set()])
                    expected[key][0] += 1; contributors.add(c)
                cb = [0]*len(cuts); cm = [0]*len(cuts)
                for mass,contributors in expected.values():
                    cb[len(contributors)] += 1; cm[len(contributors)] += mass
                assert row['contributor_buckets'] == cb, (row,cb)
                assert row['contributor_mass'] == cm, (row,cm)
                assert row['buckets'] == len(expected)
                assert row['singleton_buckets'] == sum(m==1 for m,c in expected.values())
                assert row['one_chunk_mass'] == cm[1]
                assert row['all_chunks_mass'] == cm[-1]
                assert row['max_mass'] == max(m for m,c in expected.values())
                logmass = sum(m*math.log2(len(c)) for m,c in expected.values())
                assert abs(row['mass_log2_contributors']-logmass) < 1e-6
                cases += 1
print(f'HYBRID_PRICING_ORACLE_PASS cases={cases} d=4,6,8 NUL=1 byte05=1 wrap_prefix=1 periodic=1 full_alphabet=1 nonidentity_remap=1 finite_suffixes=1 contributor_counts=exact mass=exact log_tax=exact')
