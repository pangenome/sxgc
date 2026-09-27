#!/usr/bin/env python3
"""Stream SXI member hashes and collect the fresh build journals for review."""
import hashlib
import json
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parent

def members(path):
    path = pathlib.Path(path)
    with path.open('rb') as f:
        header = f.read(64)
        n, k, r = struct.unpack_from('<3Q', header, 8)
        count = struct.unpack_from('<I', header, 32)[0]
        directory = [struct.unpack('<IIQQQII', f.read(40)) for _ in range(count)]
        result = {}
        for member, codec, offset, size, number, crc, _ in directory:
            f.seek(offset)
            h = hashlib.sha256()
            remaining = size
            while remaining:
                data = f.read(min(remaining, 8 << 20))
                assert data
                remaining -= len(data)
                h.update(data)
            result[str(member)] = dict(codec=codec, bytes=size, count=number, crc32=crc, sha256=h.hexdigest())
    return dict(path=str(path), n=n, k=k, r=r, members=result)


def journal(label):
    files = list(ROOT.glob(label+'-xsa-build-*.jsonl'))
    assert len(files) == 1, files
    rows = [json.loads(line) for line in files[0].read_text().splitlines()]
    assert rows[-1].get('status') == 'PASS', rows[-1]
    return dict(journal=str(files[0]), publication=rows[-1],
                stages=[row for row in rows if 'returncode' in row])


def main():
    builds = {label: journal(label) for label in ('yeast', 'yeast235', 'yeast235-control')}
    previous = members('/mnt/nvme3n1/erikg/sxgc-laneV/yeast.sxi')
    current = members(builds['yeast']['publication']['output'])
    assert builds['yeast']['publication']['chi'] == 85404240
    assert all(previous['members'][str(i)] == current['members'][str(i)] for i in range(1, 6))
    assert all(previous[key] == current[key] for key in ('n', 'k', 'r'))
    yeast = dict(previous=previous, current=current, changed_members=[], chi_changed=False)
    (ROOT/'yeast-members.json').write_text(json.dumps(yeast, indent=2)+'\n')
    agc, control = [members(builds[name]['publication']['output']) for name in ('yeast235', 'yeast235-control')]
    assert all(agc['members'][str(i)] == control['members'][str(i)] for i in range(1, 6))
    assert all(agc[key] == control[key] for key in ('n', 'k', 'r'))
    assert builds['yeast235']['publication']['chi'] == builds['yeast235-control']['publication']['chi']
    (ROOT/'yeast235-members.json').write_text(json.dumps(dict(agc=agc, text_control=control, changed_core_members=[]), indent=2)+'\n')
    (ROOT/'build-stages.json').write_text(json.dumps(builds, indent=2)+'\n')
    print(json.dumps(dict(yeast_chi=builds['yeast']['publication']['chi'], yeast_all_five_unchanged=True,
                          yeast235_chi=builds['yeast235']['publication']['chi'], yeast235_all_five_control_equal=True)))

if __name__ == '__main__':
    main()
