"""Maintainer-only: snapshot pinned local Git checkouts, including initialized submodules.

Usage: python3 tools/vendor_xsa_upstreams.py /path/to/pinned/deps /fresh/raw-native.tar.gz
No build products or working-tree changes are copied. Refresh source hashes after.
"""
import pathlib, subprocess, tarfile, io, json, sys
root=pathlib.Path(__file__).resolve().parents[1]
base=pathlib.Path(sys.argv[1]).resolve()
lock=json.loads((root/'tools/upstream.lock.json').read_text())
provenance={}
with tarfile.open(pathlib.Path(sys.argv[2]),'w:gz',compresslevel=9) as out:
 def archive(repo,prefix,commit):
  raw=subprocess.check_output(['git','-C',str(repo),'archive',commit])
  with tarfile.open(fileobj=io.BytesIO(raw)) as src:
   for member in src:
    member.name=prefix+'/'+member.name
    if member.name.startswith(('pfp/tests/files/', 'teratools/data/', 'teratools/test/')):
     continue
    out.addfile(member,src.extractfile(member) if member.isfile() else None)
  listing=subprocess.check_output(['git','-C',str(repo),'ls-tree','-r',commit],text=True)
  for line in listing.splitlines():
   if line.startswith('160000 '):
    meta,path=line.split('\t'); subcommit=meta.split()[2]
    if (repo/path/'.git').exists():
     provenance[prefix+'/'+path]=subcommit
     archive(repo/path,prefix+'/'+path,subcommit)
 for name,pin in lock.items():
  provenance[name]=pin
  archive(base/name,name,pin['commit'])
(root/'xsa/vendor/native-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
