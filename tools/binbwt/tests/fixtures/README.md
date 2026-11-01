The 10 GB fixture patterns are the existing `/tmp/g10/pats.fa` oracle inputs.
`query10-oracle.tsv` contains the 20 low-count rows selected by the previously
banked Q5 gate from `/tmp/g10/oracle.tsv` (count <= 3, ties by original ordinal;
the historical gate skips the first source line). A comment header replaces
that skipped line. Pattern ordinals are unchanged. High-count rows are omitted
to avoid committing 170 MB of unrelated positions. These expected answers
predate the patched query reader.
Corpus: `/mnt/nvme2n1/erikg/extcols-scratch/real10b-ref/pile-10b-snap.txt`,
10,000,017,258 bytes; container n=10,000,017,259.
