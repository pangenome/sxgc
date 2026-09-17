#!/usr/bin/env python3
"""Map flat-text byte offsets (as returned by suffixient-array locate/MEMs)
into the AGC sequence name space using the .names.tsv sidecar from agc2flat.

Usage:
  mappos.py <names.tsv> <flat_offset> [<flat_offset> ...]
  mappos.py <names.tsv> --occs <results.occs>     # 'pos len' pairs
  mappos.py <names.tsv> --mems <results.mems>     # 'pstart mlen tpos' triples

Offset p maps to contig (name, p - start) where start <= p < start + len.
Offsets falling on a '$' separator are reported as boundary markers."""
import sys, bisect

def load(tsv):
    names, starts, lens = [], [], []
    with open(tsv) as f:
        for line in f:
            n, s, l = line.rstrip("\n").split("\t")
            names.append(n); starts.append(int(s)); lens.append(int(l))
    return names, starts, lens

def mappos(names, starts, lens, p):
    i = bisect.bisect_right(starts, p) - 1
    if i < 0:
        return f"offset {p}: before first contig"
    off = p - starts[i]
    if off >= lens[i]:
        return f"offset {p}: '$' separator (end boundary of {names[i]})"
    return f"offset {p}: {names[i]}:{off} (contig len {lens[i]})"

def main():
    names, starts, lens = load(sys.argv[1])
    total = starts[-1] + lens[-1] + 1
    args = sys.argv[2:]
    if not args:
        print(f"{len(names)} contigs, flat length {total}"); return
    if args[0] == "--occs":
        for line in open(args[1]):
            parts = line.split()
            if not parts: continue
            p, mlen = int(parts[0]), int(parts[1])
            print(mappos(names, starts, lens, p), f"match length {mlen}")
    elif args[0] == "--mems":
        for line in open(args[1]):
            parts = line.split()
            if not parts: continue
            pstart, mlen, tpos = map(int, parts[:3])
            print(mappos(names, starts, lens, tpos),
                  f"MEM length {mlen}, pattern offset {pstart}")
    else:
        for p in map(int, args):
            print(mappos(names, starts, lens, p))

if __name__ == "__main__":
    main()
