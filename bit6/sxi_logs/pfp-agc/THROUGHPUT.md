| Dataset | j | AGC MB/s | File MB/s | Ratio | AGC init s | AGC MB/s incl. init |
|---|---:|---:|---:|---:|---:|---:|
| yeast235 | 16 | 470.671 | 647.037 | 72.7% | 0.905 | 417.377 |
| yeast235 | 48 | 509.017 | 625.647 | 81.4% | 0.976 | 443.032 |
| yeast235 | 96 | 534.771 | 615.706 | 86.9% | 1.059 | 457.160 |
| HPRC-466-20-windows | 16 | 509.109 | 692.746 | 73.5% | 68.439 | 27.637 |
| HPRC-466-20-windows | 48 | 464.330 | 666.796 | 69.6% | 76.616 | 24.715 |
| HPRC-466-20-windows | 96 | 437.640 | 730.069 | 59.9% | 82.213 | 23.046 |

Decimal MB/s, identical FNV checksums in both paths. The acceptance gate is steady reader throughput: every row exceeds 50% of its same-j file control. The 68–82 s HPRC compressed-index startup dominates a 2 GB benchmark window; its startup-inclusive miss is a benchmark-window artifact, per the supervisor clarification. Startup is paid once and amortizes across the full corpus; it is not a decoded-text scan. File preparation is excluded.

HPRC-466 canonical size is 1,403,221,068,481 bytes (archive layout, including separators). The measured optimum among j=16/48/96 is **j=16 at 509.109 MB/s**. Projected reader time is **45.94 min**, or **47.08 min including 68.439 s startup**, yielding **496.774 MB/s** over the full corpus.

**466 parse-ETA: 6.63 hours at j=16** by linear scaling of the measured 56.80 s full yeast235 first parse (3,336,986,759 bytes); conservatively adding full HPRC startup gives **6.65 hours**. This is a planning estimate: the serial phrase parser, dictionary growth, and different repetitiveness can change it. The 47-minute reader projection is not a parse or full-index build claim. Only the reader thread optimum was swept; full HPRC parsing was not run. Formulae and inputs: `hprc466-eta.json`; raw measurements: `throughput.json`.
