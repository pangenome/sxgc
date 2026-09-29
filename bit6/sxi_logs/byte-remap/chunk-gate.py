from pathlib import Path
import random,subprocess,json
root=Path(__file__).resolve().parents[3];work=root/'vendor/byte-remap-gates/chunks';work.mkdir(exist_ok=True)
rng=random.Random(731);data=bytes(rng.choice(b'ACGT') for _ in range(2*1024*1024+13))+b'\x1e'
text=work/'identity.txt';text.write_bytes(data)
rows=[]
for label,tool in [('old','/home/erikg/sxgc/vendor/pfp-agc-fork/build/pfp++'),('new',root/'vendor/pfp-agc-fork/build/pfp++')]:
 cmd=list(map(str,[tool,'-t',text,'-o',work/label,'-w',10,'-p',100,'--tmp-dir',work]))
 with (work/(label+'.log')).open('wb') as log:subprocess.run(cmd,stdout=log,stderr=log,check=True)
 rows.append(cmd)
for suffix in ['.dict','.parse']:subprocess.run(['cmp',str(work/('old'+suffix)),str(work/('new'+suffix))],check=True)
assert (work/'new.remap').read_bytes()==bytes(range(256))
# Source byte 255 occurs on the next read after its identity code was assigned
# to original byte 0. Existing mappings must remain stable across that seam.
b=bytearray(data);b[(1<<20)-1]=0;b[1<<20]=255;b[(1<<20)+1]=1
text=work/'seam.txt';text.write_bytes(b)
cmd=list(map(str,[root/'vendor/pfp-agc-fork/build/pfp++','-t',text,'-o',work/'seam','-w',10,'-p',100,'--tmp-dir',work]))
with (work/'seam.log').open('wb') as log:subprocess.run(cmd,stdout=log,stderr=log,check=True)
s=(work/'seam.remap').read_bytes();assert s[0]==255 and s[255]==254 and s[1]==253 and s[30]==30
rows.append(cmd)
(root/'bit6/sxi_logs/byte-remap/chunk-commands.json').write_text(json.dumps(rows,indent=2)+'\n')
print('PASS >2 MiB file: old byte-read and new chunked parse/dictionary identical; displaced high code across 1 MiB read boundary')
