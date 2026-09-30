# SXI2 v3 hybrid phi criterion and publication status

**Status: criterion proved and audited; container publication blocked.** This
note extends the banked [v2 design](../sxi2-v2/DESIGN.md). It does not declare
an SXI2 wire format or query implementation that has not passed the gates.

## Criterion C

Let `T` be the length-`n` cyclic indexed byte string. Let `f_c` be each byte's
frequency, obtained from the retained RLBWT `C` table, and let
`g = gcd({f_c : f_c > 0})`. For BWT run `i`, let `u_i = SA[tail_i]`, reconstructed
from its mirrored tail sample, and `v_i = SA[(tail_i+1) mod n]`, the next run's
head sample. Sort `(u_i, v_i, i)` by `u_i`. Let `D_i` be the nonempty cyclic
integer interval from `u_i` inclusive to the next `u` exclusive. Define

```
C(i) := (g = 1) OR (|D_i| = 1).
```

Both tests use only permitted publish inputs. The `g = 1` branch applies to
**every** run. The singleton branch is a conservative certificate for a run
even if `g > 1`. This is sufficient, not necessary: an aperiodic text may
have `g > 1` and then its nonsingleton runs go to the escape path.

**Proof.** If `T=U^m` for some `m>1`, every byte count is divisible by `m`.
The BWT is a permutation of the text bytes, so its counts are the same. Thus
`g=1` rules out periodicity. All cyclic rotations are then distinct, so
the order-preserving LF argument underlying the Nishimoto--Tabei SA-value
interval map has no equal-rotation ties. It gives
`phi^-1(x) = (v_i + x - u_i) mod n` for every `x in D_i`. If `|D_i|=1`, its
only point is `u_i`, and the same formula returns the directly sampled
`v_i`, independently of periodicity. The cyclic wrap is taken modulo `n`.
The oracle checks the formula against a directly sorted cyclic SA for every
point, not just endpoints.

## Escape semantics

For each failed run, an exact side-list must be sorted by BWT run ID. The
member may be omitted only when every run passes C, with `g=1` recorded as
the certificate; then its on-disk size is exactly zero. Each
entry needs the SA-value domain and **the true `SA` successor for every
`x in D_i`**, in increasing cyclic-domain order. `escape_codec.py` provides a
reference member: a 40-byte header (`SXESC3` magic, `n`, `R`, entry count,
`ceil(log2 n)` width); 32-byte directory records `(run_id, domain_start,
domain_length, bit_offset)` sorted by run ID; then all successors bit-packed
at that width. Binary search finds the run in `O(log E)` and an indexed bit
read returns one successor in `O(1)`. This is exact and compact relative to
u64 values, though it may be large on periodic texts with wide failed
domains. The oracle round-trips it on every test case. A single explicit head
or tail SA for a failed run does not repair interior values. The example
`(ACG 0x1E)^4` has four runs; C escapes one domain containing **13** of the
16 SA values. `(0x01 0x02 0x02)^2` escapes a domain of five of six values.
An escape lookup must return the side-list value before trying affine phi.
The list can be delta/range coded, but its random access and exactness must
be specified and tested before claiming a compact container.

A naive LF walk to one explicit run-tail sample is not a general substitute
for this list under the direct cyclic-SA tie rule. On `(0x01 0x02 0x02)^2`,
SA row 1 has value 3, yet one LF step reaches sampled row 3 with SA value 5;
adding one predicts 0 rather than 3. A specialized periodic-class locator
could avoid the dense successor list, but it would require its own proof and
gate. This observation does not change the shipped SXI1 contract.

The retained `.ri4`, `head_sa`, and chi inputs do not provide a ready stream
of these interior true successors. Generating them by visiting all `n` BWT
rows would violate the publish-path law. An O(R polylog R) construction for
periodic failed domains, or a certified additional build-time sidecar, is
still required. The v2 proposal's one-value-per-run exception would be
incorrect. This is why the implementation gate remains blocked despite the
new criterion's success on the requested real artifacts.

## Real-artifact audit

`criterion_probe.py` reads the SXI1 header and 256-entry `C` table without
walking BWT rows. The retained yeast235, remapped pile fragment, and k10
artifacts all have `g=1`. Therefore every run passes C and the exact escape
count and bytes for **these inputs** are zero. See `criterion-probe.json`.

| Artifact | n | R | chi | g | Safe runs | Escaped runs |
|---|---:|---:|---:|---:|---:|---:|
| yeast235 | 3,336,986,759 | 100,905,045 | 85,404,336 | 1 | 100,905,045 | 0 |
| k10 | 30,151,407,545 | 1,859,825,862 | 1,627,067,257 | 1 | 1,859,825,862 | 0 |
| pile-frag remapped | 1,082,130,213 | 397,723,010 | 306,164,765 | 1 | 397,723,010 | 0 |

This establishes the phi correctness precondition on those source texts. It
does **not** validate a writer, decoder, chi byte stream, or query outputs.
The probe checks header counts and the C-table arithmetic; the banked v2
probe supplies the source-codec size estimates, not achieved SXI2 bytes.

## Intended container and query algorithm

Keep SXI1 canonical. A separate SXI2 magic/version should carry independently
checksummed members for Huffman run heads plus Elias/gamma run lengths, LF
move intervals, SA-value phi intervals, sparse toehold anchors, the sorted
run-ID escape side-list, Elias--Fano chi, and optional names/remap. The
directory must include codec IDs, counts, exact bit lengths, byte bounds, and
CRC. The writer must validate member decode and the chi stream against its
source, fsync a partial file, then atomically link without clobbering an
existing artifact. `xsa mems` and `xsa serve` must carry a known SA toehold
through backward search and enumerate matches by phi; arbitrary-row LF walks
remain a different latency regime. Periodic failures first consult the
escape list. The old loader must continue to accept SXI1 byte-for-byte.

## Space and throughput verdict

No SXI2 member bytes or query benchmarks were produced in this lane. The
banked v2 source-only codec estimates remain: Huffman+gamma 0.099/2.227/0.358
GB and ideal EF chi 0.077/1.252/0.144 GB for yeast/k10/fragment respectively.
The new **exact escape bytes are zero for all three**, but LF and phi map
encoding costs are unknown. The fragment target of ~0.6--0.7 times text and
the 466 target of 6--8 GB cannot be claimed as achieved or projected from
these inputs. The 466 symbol-count gcd has not been audited, and its run-head
codec and maps have not been measured. There is no honest SXI1/SXI2 throughput
ratio without an SXI2 query path.

## Relation to run-edge theory

This criterion concerns equal cyclic rotations and the order-preserving
SA-value map; `RunEdgeHit` in `lean/SxgcRunEdge.lean` concerns the existence
of a run-edge witness for each inclusion-maximal coverage class. They share
run boundaries but C does not imply the open `RunEdgeDominate` statement O1.
A Lean statement could define `Primitive T`, `BwtRun T i`, `PhiDomain T i`,
and `PhiSuccessor T x`, then prove
`(Primitive T ∨ card (PhiDomain T i)=1) → ∀ x ∈ PhiDomain T i,
 phiFormula T i x = PhiSuccessor T x` under the chosen tie rule. A separate
lemma could derive `Primitive T` from `gcd(byteFrequencies T)=1`. No Lean
proof was attempted.
