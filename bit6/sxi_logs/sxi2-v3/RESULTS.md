# SXI2 v3 result: criterion solved; publication gates still blocked

## Diff summary

Only this `sxi2-v3` journal directory was added. `phi_oracle.py` extends the
v2 oracle with C, exhaustive and stress batteries, and escape roundtrips;
`criterion_probe.py` audits retained C tables; `escape_codec.py` and
`test_escape_codec.py` specify and test the reference bit-packed side-list.
`DESIGN.md`, this file, and the two output logs record the evidence. SXI1
writer, loader, query code and retained artifacts were untouched.

## Verified findings

The hybrid criterion is `C(i) := gcd(BWT byte frequencies)=1 OR the cyclic
SA-value domain of run i is a singleton`. Its proof and exact escape semantics
are in [DESIGN.md](DESIGN.md). The oracle computes a direct cyclic SA, checks
every passing domain point against the affine phi formula, sends every failed
run to the side-list, and round-trips every escaped successor through the
bit-packed reference codec.

| Oracle battery | Result |
|---|---:|
| Banked random aperiodic cases | 28,362; all pass |
| Banked periodic cases | 638; includes the same 126 phi counterexamples |
| Exhaustive ternary texts, lengths 2–10 | 88,569; all safe/escape checks pass |
| Added periodic stress cases | 2,000; all safe/escape checks pass |
| Banked `(ACG 0x1E)^4` fixture | 1 escaped run, 13 escaped SA values |
| Banked `(0x01 0x02 0x02)^2` fixture | 1 escaped run, 5 escaped SA values |

The read-only retained-artifact audit found gcd 1 in all three requested
SXI1 sources. Every run therefore passes C, and the exact escape-list member
would have zero entries and zero bytes of payload at these scales. The probe
does not validate source member CRCs or chi values; it reads the retained
header and C table. Its counts agree with the requested gates:

| Artifact | n | R | Header chi | g | Escaped runs | Escape payload |
|---|---:|---:|---:|---:|---:|---:|
| yeast235 | 3,336,986,759 | 100,905,045 | 85,404,336 | 1 | 0 | 0 B |
| k10 | 30,151,407,545 | 1,859,825,862 | 1,627,067,257 | 1 | 0 | 0 B |
| pile-frag remapped | 1,082,130,213 | 397,723,010 | 306,164,765 | 1 | 0 | 0 B |

## Space, query, and target verdict

These are **source-only estimates**, carried from the banked v2 probe. They
are not achieved SXI2 container member sizes.

| Artifact | Huffman heads + gamma lengths | Ideal EF chi | LF map | Phi map | Escape payload | Achieved SXI2 |
|---|---:|---:|---|---|---:|---|
| yeast235 | 0.099 GB | 0.077 GB | unmeasured | unmeasured | 0 B | absent |
| k10 | 2.227 GB | 1.252 GB | unmeasured | unmeasured | 0 B | absent |
| pile-frag remapped | 0.358 GB | 0.144 GB | unmeasured | unmeasured | 0 B | absent |
| HPRC 466 | unmeasured | 3.155 GB formula | unmeasured | unmeasured | unmeasured | absent |

The fragment ~0.6–0.7× text target and HPRC 466 6–8 GB target are **not
demonstrated**. The 466 corpus has no criterion audit or achieved codec
measurement here, so projecting a total from the fragment would be unsound.
The only defensible 466 numeric bound from the banked ledger is its 3.155 GB
ideal EF chi term before any BWT or map bytes. For a **1.31 TB full pile under
the fragment's exact density and run-distribution assumption**, the two
ideal source codecs alone scale from 0.502 GB to about 608 GB; LF, phi,
anchors and framing would increase it. This conditional pile figure is not
an artifact measurement or a projected achieved SXI2 size.

| Gate | SXI1 | SXI2 | Status |
|---|---|---|---|
| Byte-parity MEMs, three scales and multi-occurrence battery | retained baseline only | no query port | NOT RUN |
| Exact decoded chi stream | header counts only | no EF member | NOT RUN |
| HTTP serve parity | retained baseline only | no query port | NOT RUN |
| Format differential and malformed member rejection | existing SXI1 gate | no loader | NOT RUN |
| Count-heavy throughput | no paired run | no query port | NOT RUN |
| Locate-heavy throughput | no paired run | no query port | NOT RUN |

The reference escape member separately passed three roundtrip/malformed-input
unit tests, including cyclic wrap, truncated payload, nonzero padding and
unsorted run IDs. That is not a full SXI2 format differential gate.

For this exact-successor side-list, the outstanding construction problem is
deriving every interior successor of failed periodic domains from only `.ri4`,
head samples and chi without an `n`-row walk. The reference escape codec works
when supplied those values by the direct oracle; it is not a production
writer. The real inputs
need no escape entries, but the specified general hybrid publisher and
dual-format `xsa mems`/`xsa serve` loader have not been implemented. No SXI2
file was published, and no retained file was changed. The requested
`contact_supervisor` tool was not exposed by this runtime, so no background
gate was launched that would require handoff.

## Commands and outputs

- `python3 bit6/sxi_logs/sxi2-v3/phi_oracle.py > bit6/sxi_logs/sxi2-v3/phi-oracle.log` — PASS, results above.
- `python3 bit6/sxi_logs/sxi2-v3/criterion_probe.py <yeast235.sxi> <pile-frag.sxi> <k10.sxi> > bit6/sxi_logs/sxi2-v3/criterion-probe.json` — PASS, gcd 1 at all three scales.
- `python3 -m py_compile bit6/sxi_logs/sxi2-v3/*.py` — PASS.
- `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s bit6/sxi_logs/sxi2-v3 -p 'test_*.py' -v` — PASS, 3 tests.
- Inline assertion over `criterion-probe.json` — PASS, the three requested header chi counts, gcd certificates, and zero escape counts.
- `git diff --cached --stat` — empty; no staged files.
