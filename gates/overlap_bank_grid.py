"""Archive a native-grid receipt without copying corpora, columns or target/."""
import argparse
import hashlib
import json
from pathlib import Path

from overlap_ten_dict_grid import replay

FILES=('inputs.json','admission.json','binaries.log','build.log','fixtures.log','cold.log','resident.log','exit.json','result.json','stop.requested')

def bank(src,dest,stopped=False):
    if not src.is_dir() or src.is_symlink() or dest.exists():raise ValueError('require fresh grid bank and original directory')
    if stopped:
        if (src/'result.json').exists() or not (src/'stop.requested').is_file() or json.loads((src/'exit.json').read_text())['rc']!=75:
            raise ValueError('expected cooperatively stopped grid with no result')
    else:replay(src)
    data={}
    for name in FILES:
        path=src/name
        if path.exists():
            if path.is_symlink() or not path.is_file() or path.stat().st_size>(8<<20):raise ValueError('not a small receipt: '+str(path))
            data[name]=path.read_bytes()
    dest.mkdir(parents=True)
    for name,b in data.items():(dest/name).write_bytes(b)
    meta=dict(source=str(src.resolve()),scored=not stopped,sha256={k:hashlib.sha256(b).hexdigest() for k,b in data.items()})
    (dest/'source.json').write_text(json.dumps(meta,indent=2,sort_keys=True)+'\n')
    if not stopped:replay(dest)
    print('BANKED_GRID',dest,'scored='+str(not stopped))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('bank',type=Path);p.add_argument('--stopped',action='store_true');args=p.parse_args()
    bank(args.source,args.bank,args.stopped)
