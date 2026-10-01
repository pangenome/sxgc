# Pairwise BCR tree results

| Fragment route | Wall (s) | Status |
| --- | ---: | --- |
| Serial merge, 16 chunks | 20599.00 | Proven, four files and chi match |
| Tree merge, 16 chunks | Pending | `run_fragment.sh` in progress |

| 50 GB level | Parallel pairs | Largest pair (GB) | Conditional level wall |
| ---: | ---: | ---: | --- |
| 1 | 13 | 3.85 | 28.43 × measured fragment level 1 |
| 2 | 6 | 7.69 | 28.43 × measured fragment level 2 |
| 3 | 3 | 15.38 | 28.43 × measured fragment level 3 |
| 4 | 2 | 30.77 | 28.43 × measured fragment level 4 |
| 5 | 1 | 50.00 | 46.21 × measured fragment level 4 |

These are fully parallel, linear byte-scaling formulas. The run replaces
them with numerical walls after the four-file and chi gates pass. The serial
fragment merge peaked at 7.14 GB RSS. Linear scaling of its final state to
50 GB suggests about 330 GB, above the 64 GB budget. The level 3 pair alone
projects to about 102 GB, so the 50 GB route requires a memory bounded BCR
state before those walls can be attained.
