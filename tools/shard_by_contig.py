#!/usr/bin/env python3
"""Shard the flat AGC collection text by contig name for suffixient-array
index builds. Reads <flat>.names.tsv (name \t start \t len), groups contigs
by contig part (text after the sample#haplotype prefix, i.e. after the 2nd
'#'), extracts each group's slices from the flat text, and writes:
  <outdir>/<contig>.txt          shard text ('$'-separated, same as flat)
  <outdir>/<contig>.names.tsv    name \t shard_offset \t len (shard-relative)
Usage: shard_by_contig.py <flat.txt> <flat.names.tsv> <outdir>"""
import os, sys, collections
from bisect import bisect_right

def main():
    flat, tsv, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    groups = collections.defaultdict(list)
    with open(tsv) as f:
        for line in f:
            name, start, ln = line.rstrip("\n").split("\t")
            parts = name.split("#")
            contig = parts[2] if len(parts) >= 3 else parts[-1]
            groups[contig].append((name, int(start), int(ln)))

    n_shards = 0
    for contig, contigs in sorted(groups.items()):
        total = sum(ln for _, _, ln in contigs) + len(contigs)
        with open(f"{outdir}/{contig}.txt", "wb") as out, \
             open(f"{outdir}/{contig}.names.tsv", "w") as outtsv, \
             open(f"{outdir}/{contig}.samples.txt", "w") as outsamples:
            offset = 0
            samples = set()
            for name, start, ln in contigs:
                with open(flat, "rb") as f:
                    f.seek(start)
                    seq = f.read(ln)
                out.write(seq); out.write(b"$")
                outsamples.write(f"{name}\t{ln}\n")
                samples.add(name.split("#")[0])
                outtsv.write(f"{name}\t{offset}\t{ln}\n")
                offset += ln + 1
        n_shards += 1
        print(f"{contig}: {len(contigs)} contigs, {len(samples)} samples, {offset} bytes")

    # group contigs appearing in few samples into a remainder shard? report only
    sizes = [(c, sum(ln for _, _, ln in g) + len(g)) for c, g in groups.items()]
    print(f"--- {n_shards} shards, largest:", max(sizes, key=lambda x: x[1]))

if __name__ == "__main__":
    main()
