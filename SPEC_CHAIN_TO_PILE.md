# SPEC: one binary, one product, to pile scale

Objective: `xsa build --tape --input <corpus> --scratch <dir> --threads N --timeout S`
produces a queryable suffixient-array container for 1.31e12 suffixes on one machine,
with bounded scratch, and every rung carries a certificate that would have caught
tonight's locate bug.

Binding rules
- One entry point: `xsa build`. Every tool it uses is compiled into the bundle by
  `cargo build --release` and hashed in MANIFEST.sha256. No side binaries, no shell
  consumers, no gate scripts in the run path.
- The product is the arbiter. A rung is accepted only if its certificate table is full.
- Fail loud, never infer. If a precondition cannot be checked, the run refuses.

---

## Phase A — close the representation (code, hours)

A1. DECLARED END SYMBOL (semantics, not widening).
    - The format declares k = 257 codes: symbol 0 = END, symbols 1..256 = byte values.
    - Data path stays 8-bit. No u16 symbols anywhere in keys, rows, or comparisons.
    - Realization is a preflight decision, recorded in the journal:
        * free-byte mode: some byte value never occurs (valid-UTF-8 text: 0xC0,0xC1,0xF5..0xFF)
          -> append that byte once at the end; it maps to symbol 0.
        * declared-code mode: all 256 byte values occur (arbitrary binary)
          -> the END symbol exists only in the frame and the container declaration.
    - Assertion: exactly one of the two modes is chosen and stated; a corpus that fits
      neither is impossible, so reaching neither is an internal error.

A2. THE TAIL GROUP (the only place END collides with data).
    - The final <= 24 positions cannot be keyed without a pad below all bytes. They are
      sorted as their own small group by the full comparator (which already implements
      finite order), and the END row is emitted explicitly at its rank.
    - No reliance on zero padding being "below" anything.

A3. CONTAINER.
    - Alphabet declared k = 257, symbol 0 = END.
    - m1 canonical-Huffman codebook: 256 -> 257 cells (one weight-1 symbol; weight 0 floors to 1).
    - Directory/member set unchanged (lite: m1,m4,m5,m7,m10,m11).

A4. DELETE.
    - padding frame n+10, the 9 dollar rows, counts[2]==10, 2->terminal, trim_padding,
      terminal>=6 guard: DONE.
    - comparator `dollar`/shorter-first branches become THE END RULE (documented, not removed).

A-gates (at 150 MB unless noted)
  G1 decode round-trip == corpus bytes                     (PASS 2026-10-28)
  G2 exactly the END row changes; chi/runs delta explained  (PASS, chi -1, runs +7 -> must be
                                                            re-derived by hand, not accepted
                                                            from a report)
  G3 SA_CHECK_PASS over all entries + stream == BWT(SA)      (PASS, 0 mismatches)
  G5 seam classes discovered == 0, repair inert              (PASS, raw_classes=0)
  G6 TAIL FIXTURE: a hand-built corpus ending mid-record, <=24 positions, exact SA and
     BWT, both realizations (free-byte and declared-code)       (NEW, mandatory)

---

## Phase B — close the query path (blocking; nothing ships before this)

B1. READER accepts the lite container: 6 members (m1,m4,m5,m7,m10,m11), v6, no phi,
    no escape member, non-contiguous member offsets (directory sorted by id, written in
    another order), and k = 257.

B2. LOCATE IS THE PRODUCT. Positions must be exact, not merely counts.
    - Decide and document the anchor convention: member 11 is "one u64 tail SA per 1024th
      RUN". The reader must consume it as run-indexed, or the writer must store rank-indexed
      anchors. Whichever is chosen, the other side's assumption is deleted.
    - No cyclic predecessor anywhere: the END symbol terminates every walk.

B3. WITNESS PATH. witness-build / mems --first / forks currently require phi (member 8),
    which the lite container does not carry. Provide an LF-walk locate fallback; that path
    becomes the supported one for lite containers.

B-gates
  G4  >= 100 patterns taken from RAW corpus bytes (never re-encoded): counts AND positions
      exact vs brute force, zero dropped. 24/24 was the bar that exposed the bug; 100/100
      is the bar for shipping.
  G4b known-offset test: a pattern lifted from a known offset must report that offset.

---

## Phase C — close the front door (code, small)

C1. chain.rs PRODUCTION_REMAP -> the sentinel table (or the realization the preflight chose).
C2. Planner models the TAPE producer (wave cap, pending bands, live residency) instead of
    printing the bin shape for a run that is not the bin route.
C3. `--verify` post-conditions inside the binary: G1, G3, G5, G4-sample. Then delete
    tools/binbwt/fused_gate.py from the run path entirely.
C4. ROUTE SELECTION (decided):
    - DEFAULT = tape. The tape producer is the product route; it is what `xsa build` runs
      when no route flag is given.
    - `--tape` REMAINS a valid, explicit flag and selects exactly the default. It is kept
      deliberately: an explicit invocation must be possible for legacy scripts, for
      reproduced commands in papers and journals, and so that "which route?" is never a
      silent inference. `--tape` is idempotent, not deprecated.
    - `--bin` selects the bin producer: the A/B arm, kept exactly like `--legacy-merge`.
      `--tape` and `--bin` are mutually exclusive; giving both is an error.
    - PROVENANCE: the journal records both the route AND how it was chosen:
          XSA_BUILD_ROUTE route=tape source=default      (no flag given)
          XSA_BUILD_ROUTE route=tape source=explicit
          XSA_BUILD_ROUTE route=bin  source=explicit
      During the transition, a run that takes the NEW default prints a one-line notice that
      the default changed (previously bin), so no existing script silently changes meaning.
      Remove the notice once nothing in the wild relies on "no flag = bin".

C-gates
  - `xsa build --tape` with no extra flags reproduces the gated 150 MB result.
  - `grep` finds no hand-built binary or script in any run path.

---

## Phase D — the ladder (machine time; each rung must fill the certificate)

Certificate columns, every rung:
  chi | runs | container sha256 | closure 0/0/0 | decode round-trip | stream==BWT(SA) sampled |
  query: counts+positions exact on a sample | peak intermediate / n | wall | tier

D1. 1 GB   — record the first full certificate under the final representation.
D2. 10 GB  — text-order invariants must match the recorded baseline up to the END row;
             wire `--lcp-column` and measure BOTH arms (LCE vs sorter column) with
             work/run, so the superlinear-vs-flat claim is measured, not assumed.
D3. 100 GB — the rung that refused at band 90,113 of 1,638,400 on the 128*n LCE budget.
             Success = completes, work/run flat, certificate full, peak/n recorded.

---

## Phase E — the wall (only after D3)

E1. `perf record` the finish. We have never attributed the 200 ns/run and 17 us CPU/run.
E2. Then, in order of evidence: parallel prefix-scan for the rank cursor (it IS a prefix
    sum); K/wave tuning for the 516 s piece-wait now that they are plan parameters.
E3. Re-measure 10 GB, N >= 3 idle repeats, product sha unmoved.

---

## Phase F — pile

F1. Space: ~2.18n scratch (2.9 TB) + container (~0.45n, 590 GB). Free it when we get there.
F2. Run with the certificate above; T3 claims checker as the certificate for verification
    that cannot be exhaustive at 2e12.
F3. Publish: reproducible command, container sha, chi, runs, wall, peak/n, and the
    certificate table. Anything not in the table is not claimed.

---

## Kill list (delete, do not maintain)
  fused_gate.py in the run path · consume.sh (deleted) · scatter_sort_routeb (deleted) ·
  superseded containers/worktrees · any flag that is documented as deprecated.
