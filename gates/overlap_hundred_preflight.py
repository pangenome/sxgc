"""Offline inventory, not a build driver. Exit 75 (hold) even on exact metadata."""
import hashlib
import json
from pathlib import Path
import stat
import struct
import sys

ROOT = Path('/mnt/nvme2n1/erikg')
BANK = ROOT / 'PILE100-REHEARSAL'
MAP = ROOT / 'pilemap100'
CACHE = Path('/home/erikg/.cache/xsa/13524d6bbedad508fdd8e30e7ab709dbf62ae5db64eb0cd8864dd79efa5b7b4c')
N, DOCS = 100_000_000_000, 16_119_767
REMAP = '0ce35ac950b471c0da0e9a7773a6d82871d53d5de5a496bb9ffda6078b896780'
CORPUS = 'de79fe96057b604a71513770111f246ab0e936d3cf56415bcc5a43bba60bbbfd'
TOOLS = {
    CACHE/'xsa_finish': 'bb929a7092cfeba7faa7c37ca999b71563e3af782d73fe693d1a35f5ecdbaaa9',
    CACHE/'scatter_sort': '4f73b65c22b4e750e8a7501e24a46c8e3e5d0d43321da5256e9dacf3ca33f753',
    CACHE/'cross_lcp_merge': '7571daf5eba713e7bad3e37c1eb33ed80b6e8650c7596ed99b7e1bdd9973539b',
    Path('/tmp/xsa_finish_cur'): '9a0bde6739ba1819f8d298beca2c8a9bf53480fab19f637b49cf6e3c8e200fa7',
    Path('/home/erikg/sxgc/xsa/target/release/xsa'): 'fdf5f97cb23d35e427161b60f9d7be47f5eeed6fd0bbf1a4bf4b81dbb6c0f1f0',
}


def need(ok, message):
    if not ok:
        raise ValueError(message)


def size(p):
    s = p.stat()
    need(stat.S_ISREG(s.st_mode), f'not regular: {p}')
    return s.st_size


def digest(data):
    return hashlib.sha256(data).hexdigest()


def small(p, limit):
    need(size(p) <= limit, f'oversize: {p}')
    with p.open('rb') as f:
        data = f.read(limit + 1)
    need(len(data) <= limit, f'oversize: {p}')
    return data


def layout(p, version, count, pin):
    end = size(p) - count * 40
    with p.open('rb') as f:
        header = f.read(64)
        f.seek(end)
        directory = f.read(count * 40)
        need(digest(header + directory) == pin, f'changed header/directory: {p}')
        magic, v, rows, _, runs, members, _, length, _, _ = struct.unpack('<4sIQQQIIQQQ', header)
        need((magic, v, members, length) == (b'SXI2', version, count, size(p)), 'header mismatch')
        table = {}
        for mid, codec, offset, length, items, crc in struct.iter_unpack('<IIQQQQ', directory):
            need(mid not in table and 64 <= offset <= offset + length <= end, 'member extent')
            table[mid] = dict(codec=codec, offset=offset, bytes=length, count=items, directory_crc=crc)
        if version == 10:
            f.seek(table[15]['offset'])
            need(struct.unpack('<8sIIQQ', f.read(32)) == (b'XDOCST01', 1, 64, DOCS, N), 'm15 head')
            f.seek(table[20]['offset'])
            head = struct.unpack('<22Q', f.read(176))
            need(head[:8] == (3, 8, 1000, N+1, N, 21707038, 551286102, 1000) and head[21] == 1, 'm20 head')
    return dict(path=str(p), version=v, rows=rows, runs=runs, members=table)


def projected(m, k, d):
    # Same liveness equations as overlap_ten_front; assumes no radix extras.
    tape = d + 3*k - 1
    retained = 12*(k+1) + 21*m + 16*((m+15)//16)
    return dict(m=m, k=k, D=d, packed=tape, parse_column=4*m, signed_extent=m+6*k,
                parse_arrays=tape+8*m+40*k, aux_arrays=tape+d+33*(k+1)+56*(m+1),
                sort_arrays=9*d+retained+(64<<20),
                emit_arrays=9*d+16*((d+63)//64)+retained+40*k+(8<<20))


def inventory():
    corpus = ROOT/'pile-head100.txt'
    need(size(corpus) == N, 'corpus size')  # Never open the corpus.
    manifest = json.loads(small(MAP/'MANIFEST.json', 65536))
    need((manifest['pile_sha256_file'], manifest['corpus_bytes'], manifest['doc_count']) ==
         (CORPUS, N, DOCS), 'map receipt')
    verification = manifest['verification']
    need((verification['documents_compared'], verification['mismatches'], verification['content_digest_sha256']) ==
         (DOCS, 0, '2a6f07ba80a6930231ef921d2d2621bbd603bcd9bf80baae1f86f2ee4f94a45d'), 'map verification receipt')
    with (MAP/'seps.bin').open('rb') as f:
        need(struct.unpack('<4sIIIQQ', f.read(32)) == (b'XSEP', 1, 1, 64, DOCS, N), 'map separator head')
    with (MAP/'meta.bin').open('rb') as f:
        need(struct.unpack('<4sIQQ', f.read(24)) == (b'XPM1', 1, DOCS, N), 'map head')
    for name, length in [('meta.bin',548073068), ('seps.bin',66494092), ('titles.bin',277583886), ('names.bin',578428935)]:
        need(size(MAP/name) == length, f'map component {name}')
    remap = BANK/'remap-256.bin'
    need(digest(small(remap, 256)) == REMAP, 'remap')
    tool_hashes = {}
    for p, pin in TOOLS.items():
        need(size(p) <= 64<<20, f'oversize tool: {p}')
        with p.open('rb') as f:
            h = hashlib.sha256()
            for _ in range(1024):  # hard read cap, including on file replacement
                block = f.read(65536)
                if not block:
                    break
                h.update(block)
        need(h.hexdigest() == pin, f'tool drift: {p}')
        tool_hashes[str(p)] = pin
    hundred = layout(BANK/'pile-head100.sxi2', 10, 10, 'f6f15d33c46370f12d4e910a30d549685d05137c0d26104a68934e6ded559804')
    ten = layout(ROOT/'lcpruns-gate/g3/scratch/pf-10gb.sxi2', 8, 9, '20dcf301b649f5501305ad9b88b6d6975728759d985774a46cd78189806a5e98')
    receipt = small(Path(__file__).parent/'receipts/packed-front-10g/front-end.json', 16384)
    need(digest(receipt) == '5e875c44f9293e35471e2b30645f15cb02c8a44164aa4adcd679144e688c06bf', 'front receipt')
    c = json.loads(receipt)['census']
    return dict(corpus_stat_bytes=N, corpus_path=str(corpus), manifest_only_sha256=CORPUS,
                map_path=str(MAP), remap_path=str(remap), remap_sha256=REMAP, observed_tool_sha256=tool_hashes,
                observed_directories=[hundred, ten], measured_10g_front=c,
                fixed_100g_bytes=dict(scratch_limit=2*N, bitmap=(N+8)//8, full_sa_tap=8*(N+1),
                                      oracle_pipe_volume=12*(N+1)),
                independent_gates_not_scored_by_metadata_inventory=['published_verified_10g_product', 'small_exact_v10_replay',
                                  'wide_parse_and_bounded_dictionary_order', 'bounded_concurrent_tap',
                                  'matched_resource_time_and_verification_plan', 'published_measured_default'],
                projected_linear_10g_not_100g_measurement=projected(*(10*c[key] for key in ('m','k','D'))))


if __name__ == '__main__':
    report = dict(launch_authorized=False, status='HOLD')
    try:
        report.update(inventory(), metadata_match=True)
    except (OSError, ValueError, KeyError, TypeError, struct.error) as e:
        report.update(metadata_match=False, error=str(e))
    print(json.dumps(report, indent=2))
    sys.exit(75)
