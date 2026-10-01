| Chunk | Sort wall (s) | Batch BCR insert wall (s) |
| ---: | ---: | ---: |
| 0 | 13.212 | 553.303 |
| 1 | 14.331 | 495.657 |
| 2 | 13.837 | 488.203 |
| 3 | 15.547 | 598.677 |
| 4 | 15.818 | 620.684 |
| 5 | 15.903 | 601.237 |
| 6 | 14.678 | 589.650 |
| 7 | 13.974 | 494.086 |
| 8 | 13.958 | 488.205 |
| 9 | 14.088 | 435.770 |
| 10 | 16.933 | 448.863 |
| 11 | 18.270 | 451.605 |
| 12 | 14.996 | 457.506 |
| 13 | 15.146 | 445.519 |
| 14 | 15.553 | 400.346 |
| 15 | 15.383 | 371.813 |

BCR prepends symbols. Batches were inserted in index order 15→0 so the final text is chunk 0 through chunk 15.
Chunk front end total: **268.78 s**.
Merge total including output and SA samples: **20599.00 s**.
Chunk route total: **20867.78 s**.
Monolithic PFP six-stage banked total: **4789.07 s**.

The route is a correctness gate, not a speed result. At this fragment scale, monolithic PFP is faster.
The value of chunk construction is a bounded-memory path when a full PFP dictionary cannot be held; an O(runs) cross-LCP merge remains open.
