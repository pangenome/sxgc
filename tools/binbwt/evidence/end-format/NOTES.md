Producer implementation only; Phase A is NOT complete.

Base: 4c4d086. Branch: phase-a-end-format. No edits to the Rust lane.

The scatter producer chooses a realization from a corpus preflight, keeps all
byte cells eight bits, keys only complete 24-byte spans, and sorts/inserts the
last <=24 finite suffixes through the full comparator. The comparator no longer
reads a virtual sentinel from mmap slack. Declared-code standalone output works
for all 256 bytes. --selftest EXPECT.tsv verifies rank, position and carried BWT
byte and exits nonzero on disagreement. A declared override is available only
with --selftest, allowing the exact inherited tiny/short/records oracles to be
used in both modes; automatic all-256-byte selection is separately exercised.
The inherited fixture writes a realization byte itself; the harness removes
that byte before passing its original corpus to scatter_sort. Its TSV uses raw
byte labels, not the future container byte+1 labels.

Reproduce (every command wrapped in timeout 600):

    python3 tools/binbwt/test_end_scatter.py
    g++ -O2 -std=c++17 -pthread tools/binbwt/scatter_sort.cpp -o /tmp/scatter-end
    g++ -O2 -std=c++17 tools/binbwt/sa_check.cpp -o /tmp/sa-check-end
    g++ -O2 -std=c++17 tools/binbwt/end_stream_check.cpp -o /tmp/end-stream-check
    /tmp/scatter-end --corpus /dev/shm/t150m.txt --out /tmp/end150.sa --scratch /tmp/end150-scatter --threads 8 --ram-gb 0.1 --par-bands 8
    /tmp/sa-check-end --corpus /dev/shm/t150m.txt --sa /tmp/end150.sa
    /tmp/end-stream-check /dev/shm/t150m.txt /tmp/end150.sa

The producer run took 13.97 s, n=150000002 (the provided corpus has 150000001
bytes), with 57279194 format-distinct BWT runs. The checker mechanically
constructs LF with rank zero reserved for END and verifies every decoded byte;
it does not use SA to perform the inverse walk. SA supplies only the exceptional
END-row rank. The existing SA checker now applies finite exhaustion for dollar
order so byte zero cannot collide with END.

G1: producer inverse BWT 150000001 bytes, 0 mismatches. Container G1 NOT RUN.
G2: NOT RUN; no historical chi/runs deltas are claimed.
G3: all 150000002 SA entries checked; carried BWT stream 0 mismatches.
G5: NOT RUN; the downstream seam producer was not changed.
G6: all six exact inherited fixture comparisons PASS, plus 70 binary cases,
remap and corrupt-oracle negative checks, exact LCP columns and inverse BWT.
ASan/UBSan: same six fixtures and 70 binary cases PASS (before adding the
standalone inverse-BWT checker to the harness; scatter code was identical).

Residual blockers: xsa_finish still has its original 256-cell m1 and container
header, and the cross/bin consumers still assume the 0xF5 remap. This branch
therefore refuses declared-code --tape-bands output explicitly rather than
sending an ambiguous byte-zero stream into those consumers. New free-byte
choices also require consumer integration. The complete end-to-end format,
container decode and G2/G5 gates remain unimplemented/unattested.
