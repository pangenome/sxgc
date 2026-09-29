# Alphabet measurement

Count-only, bounded 8 MiB buffers, one sequential pass per selected region. Inputs opened read-only. No transformed text was written.

| Source | Offset | Bytes | Distinct | Counts for 0,1,2,3,4,5 | 0x1E count |
|---|---:|---:|---:|---|---:|
| /home/erikg/sxgc-piletest/pile-frag.txt | 0 | 1082130213 | 231 | [0, 130, 29, 3, 67, 0] | 177753 |
| /mnt/nvme2n1/erikg/pile.txt | 0 | 268435456 | 219 | [0, 90, 0, 2, 0, 0] | 44940 |
| /mnt/nvme2n1/erikg/pile.txt | 653955401270 | 268435456 | 219 | [0, 3, 6, 2, 3, 1] | 43981 |
| /mnt/nvme2n1/erikg/pile.txt | 1307642367084 | 268435456 | 230 | [0, 205, 0, 1414, 0, 0] | 44478 |

Missing values, /home/erikg/sxgc-piletest/pile-frag.txt at 0:
00, 05, 06, 0b, 0e, 15, 1c, 1d, c0, c1, f1, f2, f3, f4, f5, f6, f7, f8, f9, fa, fb, fc, fd, fe, ff

Missing values, /mnt/nvme2n1/erikg/pile.txt at 0:
00, 02, 04, 05, 06, 0b, 0e, 0f, 14, 15, 16, 17, 1a, 1b, 1c, 1d, 1f, 7f, c0, c1, de, df, f1, f2, f3, f4, f5, f6, f7, f8, f9, fa, fb, fc, fd, fe, ff

Missing values, /mnt/nvme2n1/erikg/pile.txt at 653955401270:
00, 0e, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 1a, 1b, 1c, 1d, 1f, c0, c1, dc, dd, de, f1, f2, f3, f4, f5, f6, f7, f8, f9, fa, fb, fc, fd, fe, ff

Missing values, /mnt/nvme2n1/erikg/pile.txt at 1307642367084:
00, 02, 04, 05, 06, 08, 0e, c0, c1, dd, de, f1, f2, f3, f4, f5, f6, f7, f8, f9, fa, fb, fc, fd, fe, ff

The fragment has 229 forbidden-byte occurrences. Every measured region admits an injective single-byte mapping while fixing 0x1E. The union of measured alphabets has 237 values. The measurements do not prove the alphabet of the full 1,307,910,802,540-byte pile.

Capacity correction: {6..255} contains 250 values, and removing 0x1E leaves 249 payload slots. With 0x1E reserved, the supported observed alphabet has at most 249 nonseparator values. A permutation of ALL 256 values into that allowed set is impossible; the implementation only requires observed values to lie in the allowed set.

Reproduction: compile measure.cpp with c++ -O3 and invoke its binary with PATH OFFSET LENGTH as recorded in alphabet.jsonl. The complete 256-bin counts are retained there.
