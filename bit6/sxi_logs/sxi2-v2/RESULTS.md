# SXI2 v2 lane result: blocked, no compact container published

## Gate verdict

The requested SXI2 writer, dual-format loader and move-based `xsa mems` /
`xsa serve` port are not implemented. There is no SXI2 artifact. Accordingly
the byte-parity, chi-stream, HTTP, format differential, and old/new throughput
gates are **NOT RUN** and must not be reported as passed. The prior review
directory named in the task was not present in this checkout or the available
sibling worktrees. No retained artifact was modified.

The research gate found a correctness issue that a straightforward translation
of Nishimoto--Tabei would miss: the SXI1 cyclic BWT can have equal rotations,
whereas the paper assumes a unique terminator. `phi_oracle.py` compares the
tail-derived SA-value interval map with the true cyclic SA successor. It passes
all 28,362 random aperiodic texts tested and exhibits 126 periodic
counterexamples. The exact fixture `(ACG 0x1E)^4` predicts successor 1 for SA
value 0, but the true successor is 4. The fixture `(0x01 0x02 0x02)^2` also
breaks the map. This invalidates a general SXI2 locate proof until a periodic
exception scheme or explicit rejection certificate is designed. An LF-only
move table cannot repair it.

## Read-only retained-artifact facts

`size_probe.py` read the three retained SXI1 directories and RLBWT run
payloads. It did not decode or compare chi sets. The header counts match the
specified per-artifact gates; only cardinality was checked here.

| Artifact | n | R | header chi | SXI1 member 1 | member 2 | member 3 | member 5 | total |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| yeast235 | 3,336,986,759 | 100,905,045 | 85,404,336 | 0.505 GB | 0.404 GB | 0.807 GB | 0.089 GB | 1.804 GB |
| k10 | 30,151,407,545 | 1,859,825,862 | 1,627,067,257 | 9.299 GB | 8.137 GB | 14.879 GB | 1.647 GB | 33.962 GB |
| pile-frag **remapped** | 1,082,130,213 | 397,723,010 | 306,164,765 | 1.989 GB | 1.541 GB | 3.182 GB | 0.306 GB | 7.018 GB |

Member 2 is the mirrored tail vector and member 3 is raw head SA; neither
belongs in the target compact form. Pile-frag's chi is 306,164,765, not the
306,164,941 count of the separate 229-byte-filtered corpus.

## Per-member SXI2 space model, **not achieved sizes**

The Huffman head and gamma length counts are exact for the existing run
sequence, ignoring codebook, checkpoints and framing. The Elias--Fano figures
are the bit-vector length for one straightforward layout with `L=floor(log2((n+1)/chi))`;
again no index checkpoints or framing. No LF or phi codec was written or sized.

| Scale | Huffman heads + gamma lengths | EF chi | LF map | SA-value phi map | anchors/metadata | status |
|---|---:|---:|---|---|---|---|
| yeast235 | 0.099 GB | 0.077 GB | unknown | unknown | unknown | source measurements only |
| k10 | 2.227 GB | 1.252 GB | unknown | unknown | unknown | source measurements only |
| pile-frag remapped | 0.358 GB | 0.144 GB | unknown | unknown | unknown | source measurements only |
| HPRC 466 (n=1.403 TB, R=2.740 B, chi=2.250 B) | unknown; runs not retained here | 3.155 GB formula | unknown | unknown | unknown | projection incomplete |
| full pile (1.31 TB) | 434 GB if fragment density and run distribution held | 175 GB under the same assumption | unknown | unknown | unknown | conditional linear extrapolation |

The full pile scaling factor is 1,210.6 relative to this 1.082 GB fragment.
The fragment's measured ideal source codecs total 0.502 GB. If the prior
review's roughly 0.552 GB pre-locate estimate were achieved and a phi member
cost 0.15--0.25 GB, total before further overhead would be 0.702--0.802 GB,
or 0.65--0.74 times fragment text. This is a **scenario**, not an achieved
container. The 0.6--0.7 target is not yet demonstrated. Scaling that scenario
to 1.31 TB yields about 850--971 GB, contingent on identical corpus statistics
and member encoding. At 466, every additional bit per run costs 0.342 GB;
the 6--8 GB total claim has no measured support without its RLBWT and maps.

The task brief's approximate 0.5 GB chi EF estimate at fragment scale is
incorrect in units: 306,164,765 * (2 + log2(1,082,130,213 / 306,164,765))
bits is about 0.146 GB, and the straightforward EF layout is 0.144 GB.

## Throughput

| Workload | SXI1 | SXI2 | Verdict |
|---|---|---|---|
| count-heavy MEMs | not measured in this lane | unavailable | NOT RUN |
| locate-heavy MEMs | not measured in this lane | unavailable | NOT RUN |
| HTTP serve parity/latency | not measured in this lane | unavailable | NOT RUN |

No old/new throughput ratio is reported. The arbitrary-row `s_at_opt` path in
the current query product must be changed to carry a backward-search toehold
and enumerate with `phi^-1`; simply storing a phi member would not make the
current product use it.

## Commands and hygiene

- `python3 bit6/sxi_logs/sxi2-v2/size_probe.py <retained yeast> <retained pile-frag> <retained k10> > bit6/sxi_logs/sxi2-v2/size-probe.json` — passed after one local dtype correction; source-only measurements.
- `python3 bit6/sxi_logs/sxi2-v2/phi_oracle.py > bit6/sxi_logs/sxi2-v2/phi-oracle.log` — passed, with the counterexamples above.
- `python3 -m py_compile bit6/sxi_logs/sxi2-v2/size_probe.py bit6/sxi_logs/sxi2-v2/phi_oracle.py` — passed.
- `python3` assertion over `size-probe.json` — passed all three per-artifact header cardinalities.
- `git status --short` — only untracked `bit6/sxi_logs/sxi2-v2/`, no staged files.

No long-running gate was launched, so none remains to hand off. The requested
supervisor contact tool was not exposed among available tools in this run.
