#!/usr/bin/env python3
"""Counter identity: the ONE-ENTRY journal vs the hand-driven direct path.

  single_entry_compare.py <front root> <direct root>

Compares the WORK counters only (never a wall): the bin producer's BIN_WORK
construction/publication fields and the finish's SLIM_FP/SLIM_SEEDS fields. The
two runs execute the same bundled binaries on the same corpus, so any difference
is a routing defect (byte-identity alone has hidden two of those before).
"""
import json
import sys
from pathlib import Path

# The construction/publication work. Timing fields are deliberately absent.
BIN_FIELDS = ['bins', 'windows', 'sweep_bytes', 'sweep_passes', 'level_passes',
              'level_elems', 'bin_passes', 'comparisons', 'symbols', 'cert_decided',
              'ties', 'leaf_sorts', 'leaf_elems', 'peak_live_slots', 'peak_live_bytes',
              'emitted_rows', 'runs', 'bands', 'target_hits', 'sink']
FINISH_FIELDS = ['queries', 'verified_symbols', 'hash_probes', 'verification_work',
                 'hash_probe_charges', 'suffix_symbol_reads', 'total_work',
                 'max_verified', 'max_requested_lce']


def fields(line):
    out = {}
    for token in line.split():
        if '=' in token:
            key, _, value = token.partition('=')
            out[key] = value
    return out


def slim(rows_text):
    """SLIM_FP lines carry the label as a bare token, not key=value."""
    out = {}
    for line in rows_text:
        parts = line.split()
        if len(parts) >= 3 and parts[0] == 'SLIM_FP':
            out[parts[1]] = fields(line)
        elif parts and parts[0] == 'SLIM_SEEDS':
            out['seeds'] = fields(line)
    return out


def main():
    front_root, direct_root = Path(sys.argv[1]), Path(sys.argv[2])
    journal = (front_root / 'xsa-build.log').read_text().splitlines()
    # The front door journals the producer line verbatim after V2_BIN_WORK.
    bin_front = [l.split('BIN_WORK ', 1)[1] for l in journal if 'BIN_WORK dollar=' in l]
    slim_front = slim([l.split('V2_FINISH_COUNTER ', 1)[1]
                       for l in journal if l.startswith('V2_FINISH_COUNTER ')])
    producer_log = (direct_root / 'producer.log').read_text().splitlines()
    finish_log = (direct_root / 'finish.log').read_text().splitlines()
    bin_direct = [l for l in producer_log if l.startswith('BIN_WORK ')]
    slim_direct = slim(finish_log)

    assert len(bin_front) == 1 and len(bin_direct) == 1, (len(bin_front), len(bin_direct))
    front, direct = fields(bin_front[0]), fields(bin_direct[0])
    problems = []
    for key in BIN_FIELDS:
        if front.get(key) != direct.get(key):
            problems.append(f'BIN_WORK {key}: front={front.get(key)} direct={direct.get(key)}')
    for label in sorted(set(slim_front) | set(slim_direct)):
        a, b = slim_front.get(label, {}), slim_direct.get(label, {})
        if label == 'seeds':
            for key in ('queries', 'lce_wall', 'repair_wall', 'wit_ram_values', 'wit_spills', 'wit_disc_bytes'):
                if key.endswith('_wall'):
                    continue      # a wall, not work
                if a.get(key) != b.get(key):
                    problems.append(f'SLIM_SEEDS {key}: front={a.get(key)} direct={b.get(key)}')
            continue
        for key in FINISH_FIELDS:
            if a.get(key) != b.get(key):
                problems.append(f'SLIM_FP[{label}] {key}: front={a.get(key)} direct={b.get(key)}')
    summary = dict(front_bin={k: front.get(k) for k in BIN_FIELDS},
                   direct_bin={k: direct.get(k) for k in BIN_FIELDS},
                   front_finish={label: {k: v.get(k) for k in FINISH_FIELDS}
                                 for label, v in slim_front.items() if label != 'seeds'},
                   direct_finish={label: {k: v.get(k) for k in FINISH_FIELDS}
                                  for label, v in slim_direct.items() if label != 'seeds'},
                   problems=problems)
    (front_root / 'counter-identity.json').write_text(json.dumps(summary, indent=2) + '\n')
    if problems:
        print('COUNTER_IDENTITY_FAIL')
        for p in problems:
            print('  ' + p)
        sys.exit(1)
    print('COUNTER_IDENTITY_PASS fields=%d finish_fields=%d*%d'
          % (len(BIN_FIELDS), len(FINISH_FIELDS), len([k for k in slim_front if k != 'seeds'])))
    for key in ('comparisons', 'symbols', 'emitted_rows', 'peak_live_slots', 'runs', 'bands'):
        print(f'  BIN_WORK {key}={front.get(key)}')


if __name__ == '__main__':
    main()
