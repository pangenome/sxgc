# Pairwise BCR tree results

| Fragment route | Wall (s) | Relative to serial |
| --- | ---: | ---: |
| Serial merge, 16 chunks | 20599.00 | 1.00× |
| Tree merge, 16 chunks | 19596.80 | 1.05× |

| Fragment level | Parallel pairs | Measured wall (s) |
| ---: | ---: | ---: |
| 1 | 8 | 817.96 |
| 2 | 4 | 846.48 |
| 3 | 2 | 1869.26 |
| 4 | 1 | 16063.11 |

## 50 GB / 26 chunk projection

The estimates below scale measured fragment level walls linearly by the ratio of bytes in the largest pair. They are scheduling estimates, not measurements; BCR memory and time can grow nonlinearly with run count.

| Level | Parallel pairs | Largest pair (GB) | Projected wall (s) |
| ---: | ---: | ---: | ---: |
| 1 | 13 | 3.85 | 23258 |
| 2 | 6 | 7.69 | 24069 |
| 3 | 3 | 15.38 | 53150 |
| 4 | 2 | 30.77 | 456738 |
| 5 | 1 | 50.00 | 742199 |

Projected fully parallel level sum: **1299413 s**; linear serial projection: **951780 s**.
Serial fragment peak RSS was 7.14 GB. Linear memory scaling to 50 GB would be about 330 GB for the final BCR state, above the 64 GB budget. Even a 15.38 GB level-3 pair projects to about 102 GB. The 50 GB run therefore needs a memory bounded BCR state before it can be attempted; the wall projection is conditional on that change.
