# Fresh construction: unresolved dependency (2026-09-27)

The revised tail-only Phi construction has **not** been implemented. No
fresh yeast/k10/466 acceptance is claimed. This is a concrete dependency
problem in the proposed recipe, not a proof that every r-space algorithm
for the problem is impossible.

1. The installed producer contradicts the input premise. In
   `/home/erikg/r-pfbwt/include/rpfbwt_algorithm.hpp`, lines 668–670 select
   `run_heads_bitvector`, and lines 749–783 construct that vector and write
   `.ssa` as `[u64 R][R head positions]`. It does not emit the assumed tail
   samples. The earlier PHI_INVERSE_README audit already records this.
   The producer and its build have been left untouched.
2. The standard local r-index Phi implementation is not a primitive built
   from tail samples alone. In
   `/home/erikg/suffixient-array/experiments/toehold/internal/r_index.hpp`,
   lines 106–144 sort **first** samples, construct the predecessor structure
   from them and retain a mapping to run IDs. Phi at lines 195–220 uses that
   predecessor and the preceding run's **last** sample. Thus evaluating Phi
   at its interval starts to obtain missing head samples presupposes the
   very head samples needed to build that implementation's interval starts.
3. `lean/SxgcPhi.lean` proves `headFromTail` and inverse interval lookup
   given `PieceSound`, order, and coverage. It explicitly separates source
   table provenance from that proof. It does not provide a tail-only Phi
   constructor, or its time bound. The existing extractor decodes an
   already constructed table and sorts its images; that part remains valid.
4. The cited [r-index-f implementation](https://github.com/drnatebrown/r-index-f)
   documents LF/count operations. Its O(1) row-splitting claim concerns LF,
   not a construction of text-position Phi from tail-only samples. LF maps
   suffix-array rows; Phi maps text positions. Converting between those
   domains requires the missing suffix-array information.
5. The installed r-pfbwt single-string pilot and the newline-collection
   k10 oracle also have different run counts (1,859,825,862 versus
   1,859,825,801). Existing single-string outputs cannot honestly pass the
   prescribed collection head-byte gate through a container rename.

The new extractor `--from-front-end` mode fails before opening files, with
this dependency explained. Its original pilot table extraction remains
available; no pilot table is silently substituted into a fresh build.

To resume requires a validated r-space sample construction plus a front end
that actually supplies its specified inputs, or a revised endpoint-emission
and normalization contract. The final UX directive explicitly leaves the
front end untouched; this revision does not treat the older ledger's
HEAD+TAIL emission proposal as permission to change it. No impossibility
claim is made about other algorithms. The requested `contact_supervisor`
tool was searched for in the available tool catalog again and is absent.
No alternate external messaging channel was used.

The Rust `xsa build` CLI now recognizes `--agc`, `--fasta`, and `--text`,
enforces a single source/output and rejects existing output paths. It then
reports this missing dependency before launching stages or creating output.
This is a **fail-closed CLI boundary, not the requested build implementation**.
There is no managed stage orchestration or successful chi-gated fresh build.
`--verbose` explicitly says that no stage subprocess was launched.
The source gate attempts are in `sxi_logs/ux-source-gates.jsonl`.

No producer was run for this lane, no M/b_bwt/w_wt was constructed, no
large text walk was used, no unrelated PID was signaled, and no commit
was made. Under the final task's gate numbering, G0 (7/7 source builds),
G1 (fresh yeast), G2 (fresh k10 and RAM measurement), and G3 (fresh k10
head-byte equality) remain unpassed.
