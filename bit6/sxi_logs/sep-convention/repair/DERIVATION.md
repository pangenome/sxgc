# Padding removal: certificate and obstruction to local repair

This lane does **not** implement the general padded-to-cyclic transform.
The adapter now certifies its existing normalization or rejects before opening
outputs. Rejection is not a successful repair/publication gate.

## What the producer parameters mean

Read-only investigation of the installed producer and parser:

- `/home/erikg/r-pfbwt/include/rpfbwt_algorithm.hpp:525`: `suffix_length`
  is the length of a proper suffix of a level-1 dictionary phrase. It varies
  during enumeration; it is not a CLI option, a desired circular period, or
  the number of bytes of corpus to trim.
- Lines 663–665 subtract that phrase suffix length from the expanded phrase
  end coordinate, modulo `l1_n`. They change the coordinate, not the lexicographic
  ordering computed from the padded dictionary/parse.
- `/home/erikg/pfp/src/pfp_algo.cpp:534,565`: the text parser starts with a
  dollar and unconditionally appends `params.w` dollars. The dictionary loader
  (`pfp/dictionary.hpp:110–116` in r-pfbwt's pfp_ds dependency) expands the initial
  dollar prefix to `w` dollars. A consistent parse/window still represents
  `T + dollar^w`, for any supported window. The parser CLI restricts
  window size to 3 through 200, so it cannot request zero padding.
- `rpfbwt --help` has no suffix-length, no-padding, or circular-text flag.
  `--w1` is the dictionary overlap/window parameter; changing it alone does
  not reinterpret an existing parse as cyclic T. With a normal one-dollar
  dictionary prefix, zero window makes `w - n_dollars` negative in the
  dictionary loader and cannot provide a valid zero-padding invocation.

No producer, parser, tap, or producer invocation was changed. The help outputs
are retained. A no-pad or pad-transparent supported invocation was not found.
This is a source-based finding, not a claim to have tested every invalid flag
combination or to prove that every possible custom parse construction fails.

## Sufficient certificate in O(r) time and O(1) extra space

Let S be the suffix array of the *finite* T (shorter suffix first), obtained by
removing the first w pad rows. Its BWT still contains one dollar, at row z with
S[z]=0. Let c=T[n-1], and q be the number of c bytes strictly before z in this
remaining BWT. Replacing that dollar by c induces the circular predecessor edge.

If q=0, the new LF sends z to the first F-row for c, which is the row of suffix
n-1. Every other c-predecessor is after z, so the new c at z replaces exactly
its old preceding c in the deleted pad rows. Their LF destinations remain the
same after subtracting w. The destinations for other letters also remain the
same. Thus LF maps every SA value x to x-1 modulo n. F remains sorted, proving
lexicographic cyclic rotation order by repeated stable backward extension.
These certified frames have no unresolved periodic tie ordering.

The adapter checks q=0 from a streaming raw-RLE pass, verifies the unique
remaining dollar's endpoint is SA=0, and rejects if q>0. It also handles the
case in which that dollar shares the final pad run: exactly one remaining
row, with known head and tail SA=0. All existing output scans remain O(r),
with no LF walk or full-text access. Producer samples remain untouched.

Exhaustive audit: every binary text of lengths 1 through 9 (1,022 cases).
252 certified outputs match the independent cyclic BWT, run heads **and**
packed tails; 770 rejected cases produce neither output. See
`certificate-exhaustive.json` and its reproducible test-only driver.
The certificate is sufficient; rejecting a frame is not a proof that no
other compressed algorithm could normalize it.

## Why a fixed pad-sized row repair is insufficient

For even m >= 2, T = A^m B A^m has length 2m+1. Its padded BWT has exactly
six runs, independent of m (w=10). Suffix order lists the final A-only suffixes
shortest first, at positions 2m,2m-1,...,m+1. Cyclic order lists those rotations
longest A-prefix first, at positions m+1,m+2,...,2m. Exactly m array rows change.
The straightforward backwards LF-driven remove/reinsert scheme needs m-1
row relocations. The oracle independently measures m=8,32,128,512, including
running the dense test-only relocation simulation and checking its final SA.
Thus the row cascade can be Theta(n) even for constant r; it is not bounded
by w or by the number of runs. Purely periodic fixtures also expose equal
rotations, where BWT equality alone does not certify the endpoint tie order.
The oracle uses increasing starting position to break exact rotation ties.

This is a lower bound for enumerating affected rows / this local scheme,
**not** an impossibility proof for an r-space transform using compressed
interval operations. Such a general transformation remains required. No
O(n) fallback, capped walk disguised as a repair, or full-text adapter scan
was added.

## Slim collection semantics and downstream boundary

Slim auto-detects 0x1e from the existing RLE alphabet; no-newline input is
also a single cyclic text. Legacy newline-only input retains its old
collection ends and truncation. In cyclic mode the dictionary scan does
not allocate per-record end lists: there is one virtual collection end.
LCE compares separator bytes and wraps the corpus seam, capping each raw
PFP LCE before dollar padding. A query crosses at most two seams (three raw
LCE calls). Direct test-only comparisons include repetitive, near-periodic,
no-separator, and the original three-record fixture.

Fixing slim reveals a further production boundary: tiny AGC's certified
frame and raw control pass slim and the sweep, but the sweep emits witness
20002 for n=20001. `sxi_write` correctly rejects `chi out of range`.
`xsa/src/main.rs:976` uses N=n+1 for its existing collection convention.
This observation does not by itself justify changing N, applying modulo,
or deleting the witness; that needs its own mathematical derivation and
scope decision. The front end and writer were left untouched.

The precise sweep incompatibility is visible at `xsa/src/main.rs:1012`:
newline is mapped to sentinel class 0, which is excluded from emission loops.
0x1e remains ordinary class 30, as required for cyclic corpus bytes, so an
SA=0 endpoint can emit N-0 = n+1. The tiny AGC failure is therefore not cured
by merely relabeling slim's end count. Mapping all record separators to
sentinels would reinstate collection semantics and violate the cyclic-T
contract; it was not used as a workaround.
