# Fork matrix feasibility audit

The requested `xsa forks` output cannot yet be assigned correct semantics from
`docs/GWAS_FORK_MATRIX.md`. No extractor or GWAS result was published. This
audit records the blocking issues so that a branch policy can be specified
before the 85.4 million / 2.25 billion column runs.

## Artifact facts

`model_audit.py` reads only the SXI header and member directory. The result is
in `model_audit.json`.

* Retained yeast235 is SXI2 v3, 9,901 named records, 100,905,045 BWT runs,
  and **85,404,336** sorted EF-chi witnesses. It has members 1, 4, 5, 6,
  8, 9, 10, 11, including the LF map and sparse anchors.
* The specified HPRC file is **SXI1 v1**, with 38,790 named records,
  2,739,737,285 runs, and 2,250,211,129 delta-coded chi witnesses. It has
  members 1 through 6, with no SXI2 move map or sparse anchors. Thus an
  extractor for the actual 466 smoke must support SXI1's raw run table and
  head/tail samples, not only the SXI2 path in the vision document.

These counts come from the retained files' directories, not from a decoded
member or a completed fork extraction. The audit does not validate CRCs.

## Semantic blockers

1. A chi member contains **text positions** only (`bit6/SXI_FORMAT.md`,
   `xsa/src/sxi2.rs::chi`). The current theory defines a coverage class as a
   set of **requirements `(w,c)` covered by a text position**
   (`lean/Sxgc.lean::covSet`), not as a set of collection record IDs. The
   artifact has no field specifying the right context `w`, its SA interval,
   or how to choose one context when a position witnesses several.
2. A BWT interval is over **occurrences**. A named record can occur in more
   than one following-character branch of the same context. The direct,
   single-pass scan of the first 100 named yeast235 records found all 100
   contain at least two distinct characters following `A` in the stored
   orientation. A `record_id -> one class_id` column is therefore not a
   partition without a rule for repeated occurrences (e.g. multi-label,
   presence/absence, or a chosen occurrence).
3. BWT runs are not branch classes. The self-contained cyclic-BWT fixture in
   `model_audit.py` has an `A` interval with 8 runs but only 4 distinct
   preceding characters. Two separated runs with the same BWT symbol take
   the same branch. Hence `n_classes = intersected_run_count` is incorrect
   even when a context interval is supplied.
4. The document calls rows "466 haplotypes" and also refers to 38,790
   sequences. The artifact names encode sequence records. A grouping from
   record ID to haplotype ID is a further choice if the paper's rows are
   intended to be haplotypes.

The scan was in-tool and sequential. An initial delimiter-based audit failed
because the corpus separator is not literal `0x1e` in that source file; the
corrected pass used the sidecar's record lengths, read a contiguous prefix
once, and stopped after 100 records. No retained artifact was modified.

## Decision needed before implementation

Define (a) the unique context / SA interval associated with each chi text
position, (b) how a record with occurrences in several branches is encoded,
and (c) whether output rows are sequence records or grouped haplotypes. The
requested partition and chi-square smoke depend on these definitions. In
particular, a multi-label or branch-presence matrix is not the specified
single-class partition and calls for a different association test.

The orchestration instruction requests `contact_supervisor` for this decision,
but no such tool was exposed in this run. This report is the reviewable
decision packet. No long-running process remains from this audit.
