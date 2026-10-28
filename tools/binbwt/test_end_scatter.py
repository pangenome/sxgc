#!/usr/bin/env python3
"""Repeatable producer G6 gate; all subprocesses have a 600-second deadline.

The inherited fixture stores its realization byte on disk. Strip exactly that
byte before invoking the producer, whose input contract is the original corpus.
Its TSV labels ordinary bytes by their raw hex value (legacy fixture convention),
not by the container's byte+1 format code. The C++ --selftest normalizes the
carried byte through the inverse remap and recognizes END by SA position zero.
"""
import argparse
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

import end_fixture

STREAM_CHECKER = None


def run(argv, **kwargs):
    return subprocess.run(argv, timeout=600, stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, **kwargs)


def check(binary, root, name, text, declared=False, fixture=False, remapped=False):
    case = root / name
    case.mkdir()
    if fixture:
        generated = run([sys.executable, str(pathlib.Path(end_fixture.__file__)),
                         '--out', str(case), '--case', name.split('-')[0]])
        assert generated.returncode == 0, generated.stderr
        assert (case / 'corpus.bin').read_bytes() == text + b'\xf5'
    corpus = case / 'raw.bin'
    corpus.write_bytes(text)
    sa = sorted(range(len(text) + 1), key=lambda p: text[p:])
    rows = [(r, p, 0 if p == 0 else text[p - 1]) for r, p in enumerate(sa)]
    expected = '# rank\tposition\tbwt_symbol_hex\n' + ''.join(
        f'{r}\t{p}\t{c:02x}\n' for r, p, c in rows)
    oracle = case / 'expect.tsv'
    if fixture:
        assert oracle.read_text() == expected, 'independent oracle differs from inherited fixture'
    else:
        oracle.write_text(expected)
    output = case / 'sa'
    args = [str(binary), '--corpus', str(corpus), '--out', str(output),
            '--scratch', str(case / 'scratch'), '--threads', '2',
            '--lcp-out', str(case / 'lcp'), '--selftest', str(oracle)]
    if declared:
        args += ['--selftest-declared-end']
    table = bytes(range(256))
    if remapped:
        present = sorted(set(text))
        absent = sorted(set(range(256)) - set(text) - {245})
        assert 245 not in present
        table_list = [0] * 256
        for i, b in enumerate(present, 1):
            table_list[b] = i
        for i, b in enumerate(absent, 1 + len(present)):
            table_list[b] = i
        table = bytes(table_list)
        remap = case / 'remap'
        remap.write_bytes(table)
        args += ['--remap', str(remap)]
    result = run(args)
    assert result.returncode == 0, result.stderr
    assert 'mismatches=0' in result.stdout
    actual = output.read_bytes()[48:]
    assert actual == struct.pack(f'<{len(sa)}Q', *sa)
    assert (case / 'sa.sym').read_bytes() == bytes(0 if p == 0 else table[text[p-1]] for p in sa)
    lcp_data = (case / 'lcp').read_bytes()
    _, _, _, escape_count, _, _ = struct.unpack('<6Q', lcp_data[:48])
    lcps = list(lcp_data[48:48 + len(sa)])
    for i in range(escape_count):
        rank, value = struct.unpack_from('<QQ', lcp_data, 48 + len(sa) + 16 * i)
        lcps[rank] = value
    expected_lcps = [0]
    for a, b in zip(sa, sa[1:]):
        k = 0
        while a+k < len(text) and b+k < len(text) and text[a+k] == text[b+k]:
            k += 1
        expected_lcps.append(k)
    assert lcps == expected_lcps, (name, lcps, expected_lcps)
    expected_mode = 'declared-code' if declared or len(set(text)) == 256 else 'free-byte'
    assert f'END_SYMBOL mode={expected_mode}' in result.stderr
    assert f'TAIL_GROUP rows={min(24, len(text)+1)}' in result.stderr
    if STREAM_CHECKER:
        checker_args = [str(STREAM_CHECKER), str(corpus), str(output)]
        if remapped:
            checker_args.append(str(case / 'remap'))
        checked = run(checker_args)
        assert checked.returncode == 0, checked.stdout + checked.stderr
    if fixture:
        print(f'G6 case={name.split("-")[0]} mode={expected_mode} rows={len(sa)} mismatches=0')
    return args, oracle


def main():
    global STREAM_CHECKER
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=pathlib.Path)
    a = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='end-scatter-') as temp:
        root = pathlib.Path(temp)
        binary = a.binary.resolve() if a.binary else root / 'scatter_sort'
        if not a.binary:
            built = run(['g++', '-O2', '-std=c++17', '-pthread',
                         str(pathlib.Path(__file__).with_name('scatter_sort.cpp')),
                         '-o', str(binary)])
            assert built.returncode == 0, built.stderr
        STREAM_CHECKER = root / 'end_stream_check'
        built = run(['g++', '-O2', '-std=c++17',
                     str(pathlib.Path(__file__).with_name('end_stream_check.cpp')),
                     '-o', str(STREAM_CHECKER)])
        assert built.returncode == 0, built.stderr
        for name in ('tiny', 'short', 'records'):
            for declared in (False, True):
                check(binary, root, name + ('-declared' if declared else '-free'),
                      end_fixture.cases(name), declared, fixture=True)
        rng = random.Random(257)
        tests = [bytes(range(256)) + b'\x00\xff\x00',
                 bytes(range(256)) + b'\x00' * 600,
                 b'\x00' * 700, b'\xff' * 700,
                 b'\xf5' * 40 + b'\x00\xff',
                 b'abcabc' * 100 + b'abc']
        tests += [bytes(rng.randrange(256) for _ in range(n)) for n in range(1, 65)]
        for i, text in enumerate(tests):
            check(binary, root, f'binary-{i}', text)
        args, oracle = check(binary, root, 'remapped', end_fixture.cases('records'), remapped=True)
        oracle.write_text(oracle.read_text().replace('0\t427', '0\t426', 1))
        result = run(args)
        assert result.returncode != 0 and 'G6 disagreement' in result.stderr, result.stderr
        print(f'END_SCATTER_PASS fixtures=6 binary={len(tests)} remap=1 negative_oracle=1 lcp=exact inverse_bwt=exact')
    return 0


if __name__ == '__main__':
    sys.exit(main())
