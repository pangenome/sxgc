# The fit check prices the shape the run will execute (2026-11-07)

Branch `fix/tape-fit-check` off `pile-week-v1` (`c46a852`). Every line below is
the literal stdout/stderr of the command above it on this box
(2x AMD EPYC 7713, 256 threads, 1007 GiB RAM). Box load during the plan-only
runs: **load average 10.3 / 10.7 / 10.7** (other jobs on the box; the plan-only
gates are 0.04 s of `survey` + arithmetic, so the load does not touch them).
The 150 MB product gate ran with that same background load.

## The defect

`--tape` was gated on the BIN shape's all-phase peak (2.642 n: rlebwt + rolling
window files + piece window + container + witness spill) while the same output
carried a correct tape envelope (48.00 GiB arena, 96 bands in flight, 709 waves).
The tape route runs the band sorter, whose scratch peak was MEASURED at
`0.789 n` on the Pile's first 100 GB (`tools/binbwt/RESULTS_PILE_100GB.md`:
78,880,730,679 B at n = 100,000,000,001). Two shapes were priced as one.

## The fix, in the plan's own words

- `plan::Shape` (`bin` | `tape` | `chunk-merge` | `fan-in`): the printed header,
  the gating number and the process are the same shape. `--tape` prints the tape
  plan and `--bin` the bin plan; `--tape --bin` is refused (two shapes, one
  price). The other shape's number survives only as an explicitly labelled
  `XSA_PLAN_ALTERNATIVE shape=bin ...` / `shape=tape ...`.
- The tape shape's model, journaled on one line:
  `XSA_PLAN_DISC_MODEL shape=tape factor=1.00 measured_basis=0.789n@pile-head-100GB
  base=peak_scratch_measured covers=band-window+container+journal
  container_inside_scratch_peak=1 ...` — peak scratch = `1.00 n`, the factor
  covering the band window, the container being published and the journal (the
  container is INSIDE the scratch peak).
- Every `XSA_PLAN_DISC` / `XSA_PLAN_RAM` refusal is ONE actionable sentence
  (`plan::refusal`): shape, peak vs limit in the same unit and in bytes, the
  shortfall, the exact override flag (or `override=none` with `--skip-fit-check`
  named as NOT applying), and the knob that fixes it. Sub-GiB limits print in MiB
  so a 5 MiB tmpfs is not rounded to `0.0 GiB`.
- `--skip-fit-check` overrides ONLY the absolute peak-vs-free check and prints
  `DISC_CHECK_SKIPPED basis=operator peak=<n> free=<m> ...` in the plan block
  (which the run journals verbatim). `--allow-infeasible` is a silent alias.
  The RAM term and the bin shape's 2.5 n output-side bound have no override.

## F1 — `--tape --plan-only` on the pile (plan-only wall 0.04 s, maxrss 15.4 MiB)

```sh
xsa build --tape --input /mnt/nvme2n1/erikg/pile.txt --scratch /tmp/g-f1 \
          --threads 96 --memory-gb 96 --plan-only
```

```
XSA_PLAN v=6 pipeline=v2tape (tape shape: band sorter -> ordered band merge -> decomposed fused finish; the RAM term is the band arena, the scratch peak is the measured 0.789n envelope x 1.00) corpus_bytes=1307910802540 chunks=1 kway=1 threads=96 cap_gb=96 cores=256 cx16=true mem_available_gb=818
XSA_PLAN_TAPE envelope_gb=48.00 memory_gb=96 bytes_per_row=90 bands_in_flight=96 rows_per_band=4809757 rows_per_band_budget=4809757 binding=budget bands=384 waves=709 rows_per_wave=1846946688 arena_gb=38.70 fixed_gb=0.47 stage_gb=2.00 peak_gb=41.17 peak_ratio_pct=85 bound=90 arena_planned_gb=38.70 headroom_pct=85 rule=rows_per_band=(envelope-fixed-stage)x85%/(bytes_per_row x bands_in_flight) n_is_in_no_numerator=1 n=1307910802541
XSA_PLAN_TAPE_SHAPE phase_table=none wall_model=none note=this route's scratch peak is the measured band window (XSA_PLAN_DISC_MODEL below), not a per-phase sum; the bin shape's per-term table is printed by `--bin --plan-only` and appears here only as a labelled alternative
XSA_PLAN_DISC_MODEL shape=tape factor=1.00 measured_basis=0.789n@pile-head-100GB base=peak_scratch_measured covers=band-window+container+journal container_inside_scratch_peak=1 measured_n_bytes=100093934467 measured_peak_scratch_bytes=78880730679 measured_ratio=0.788 measured_ratio_arithmetic=78880730679/100093934467=0.788 (the same peak is 0.789 of the 100000000001-byte pile-head-100 gate corpus, where it was 78880730679 B) n_bytes=1307910802540 peak_bytes=1307910802540 peak_gb=1218.1 peak_n=1.000 free_gb=995.4 rule=peak=TAPE_DISC_FACTOR x n (the measured band-window envelope; the container is itself inside the scratch peak, and the journal is the term the factor carries beside it)
XSA_PLAN_DISC INFEASIBLE shape=tape: the tape route's measured scratch peak (band window + container + journal): peak 1218.1 GiB (1307910802540 B) vs free scratch 995.4 GiB (1068784988160 B) -> SHORT 222.7 GiB (22.4% over); override --skip-fit-check to run anyway (that flag skips ONLY this absolute peak-vs-free check; the RAM term, the band law and every receipt keep their teeth); fix: free scratch space or point --scratch at a device that holds 1.00n (the tape peak is a measured n-multiple: --threads and --memory-gb do not shrink it; --bin prices the other shape)
XSA_PLAN_RAM FEASIBLE shape=tape: the band arena is 38.7 GiB = 96 bands in flight x 4809757 rows x 90 B/row + 0.5 GiB fixed + 2.0 GiB staging = 41.17 GiB, 85% of the 48.00 GiB envelope (--memory-gb 96, the bound is 90%); 384 bands per wave, 709 waves; the producer asserts the same ratio at exit (SS_TAPE_PEAK) and the front door refuses the run if the two plans disagree
XSA_PLAN_ALTERNATIVE shape=bin all_phase_peak_gb=3218.6 = 2.642n free_gb=995.4 verdict=DOES NOT FIT note=this is the BIN shape's peak, NOT this run's gating number (--tape runs the band sorter, priced by XSA_PLAN_DISC_MODEL above); `--bin --plan-only` prints its whole per-term table
XSA_PLAN_ALTERNATIVE free or add scratch space for the tape shape's 1.00n peak (it is a measured n-multiple: --threads does not shrink it), or pass --skip-fit-check to run anyway
```

It still refuses on **this** scratch device, and correctly: `1.00 n` =
1218.1 GiB against 995.4 GiB free. The number being refused is now the tape
route's own, and the refusal says so with its shortfall and its override.

## F2 — the same command with `--skip-fit-check`

```sh
xsa build --tape --input /mnt/nvme2n1/erikg/pile.txt --scratch /tmp/g-f2 \
          --threads 96 --memory-gb 96 --plan-only --skip-fit-check
```

```
XSA_PLAN_DISC INFEASIBLE shape=tape: the tape route's measured scratch peak (band window + container + journal): peak 1218.1 GiB (1307910802540 B) vs free scratch 995.4 GiB (1068785217536 B) -> SHORT 222.7 GiB (22.4% over); override --skip-fit-check to run anyway (that flag skips ONLY this absolute peak-vs-free check; the RAM term, the band law and every receipt keep their teeth); fix: free scratch space or point --scratch at a device that holds 1.00n (the tape peak is a measured n-multiple: --threads and --memory-gb do not shrink it; --bin prices the other shape)
DISC_CHECK_SKIPPED basis=operator shape=tape peak=1307910802540 peak_gb=1218.09 free=1068785217536 free_gb=995.38 short_gb=222.70 short=222.7 GiB override=--skip-fit-check note=only the absolute peak-vs-free check is skipped; the RAM term, the band law and every receipt still refuse this run
```

Plan-only never starts a phase and never refuses, so the journaled form was also
exercised end to end on a 4 MiB pile slice (`/tmp/f4-corpus.txt`, n = 4,194,304,
`--threads 8 --memory-gb 8`, scratch `/tmp/tf-journal`; walls: order-tape /
emit-tape / finish-v2 all under 10 s, exit 0). `/tmp/tf-journal/xsa-build.log`
contains:

```
DISC_CHECK_SKIPPED basis=operator shape=tape peak=4194304 peak_gb=0.00 free=1068911460352 free_gb=995.50 short_gb=0.00 short=0.0 MiB override=--skip-fit-check note=only the absolute peak-vs-free check is skipped; the RAM term, the band law and every receipt still refuse this run
XSA_BUILD_DONE chi=1295046 pipeline=v2tape container=/tmp/tf-journal/f4-corpus.sxi2
```

and the RAM term still has no override (`--threads 256 --memory-gb 2` on the
pile, plan-only):

```
XSA_PLAN_RAM INFEASIBLE shape=tape RAM: the band arena is 0.0 MiB = 256 bands in flight x 1 rows x 90 B/row + 1.0 GiB fixed + 64.0 MiB staging = peak 1.1 GiB (1191200784 B) = 0.001n vs the 921.6 MiB usable envelope (90% of --memory-gb 2 = 966367641 B) -> SHORT 214.4 MiB (23.3% over); the band unit itself is UNDER the fold floor (the arena cannot hold 256 bands in flight of 65536 rows x 90 B/row); rows_per_band=1 floor=65536 (short 65535 rows); override=none: no flag skips the RAM term, --skip-fit-check does NOT touch it; fix: raise --memory-gb until the usable envelope covers 1.2 GiB; 1024 bands per wave, 1277256644 waves
```

with the same exit 1 refusal **even when `--skip-fit-check` is passed**.

## F3 — `--bin --plan-only` (plan-only wall 0.04 s)

```sh
xsa build --bin --input /mnt/nvme2n1/erikg/pile.txt --scratch /tmp/g-f3 \
          --threads 96 --memory-gb 96 --plan-only
```

```
XSA_PLAN v=6 pipeline=v2bin (bin shape: one sweep -> 256 byte-bins -> ordered stitch -> decomposed fused finish; no chunk phase, no merge walk) corpus_bytes=1307910802540 chunks=1 kway=1 threads=96 cap_gb=96 cores=256 cx16=true mem_available_gb=819
XSA_PLAN_DISC_MODEL shape=bin factor=1.00 base=all_phase_peak(phase_rows) measured_basis=terms-at-1GB-G2 (rlebwt 1.546n, window credit x cap x 8 B = 0.00 GiB, piece window, container 0.468n, witness spill EXTRAPOLATED at 0.3x overflow) covers=rolling-window-files+rlebwt+piece-window+container+witness-spill cap=1 windows=1307910802540 n_bytes=1307910802540 peak_bytes=3455994969836 peak_gb=3218.6 peak_n=2.642 free_gb=995.4
XSA_PLAN_DISC INFEASIBLE shape=bin: the bin shape's ALL-PHASE scratch peak (rolling window FILES + rlebwt + piece window + container + witness spill): peak 3218.6 GiB (3455994969836 B) vs free scratch 995.4 GiB (1068785197056 B) -> SHORT 2223.3 GiB (223.4% over); override --skip-fit-check to run anyway (that flag skips ONLY this absolute peak-vs-free check; the RAM term, the band law and every receipt keep their teeth); fix: free scratch space, or plan the shape that has the measured envelope (--tape, whose peak is the measured 0.789n band window rather than this tree)
```

(The bin plan's own phase table, `XSA_PLAN_V2_*` per-term block and
`XSA_PLAN_V2_HOSTING` line are all still printed above these lines; only the
tail is quoted here.)

## F4 — a 5 MiB tmpfs scratch and a 4 MiB corpus, real run (exit 1)

```sh
head -c 4194304 /mnt/nvme2n1/erikg/pile.txt > /tmp/f4-corpus.txt
mkdir /run/lock/tapefit-f4          # tmpfs, 5,242,880 B free
xsa build --input /tmp/f4-corpus.txt --scratch /run/lock/tapefit-f4 --threads 8 --memory-gb 8
```

```
XSA_PLAN_DISC_MODEL shape=bin factor=1.00 base=all_phase_peak(phase_rows) measured_basis=terms-at-1GB-G2 (rlebwt 1.546n, window credit x cap x 8 B = 0.00 GiB, piece window, container 0.468n, witness spill EXTRAPOLATED at 0.3x overflow) covers=rolling-window-files+rlebwt+piece-window+container+witness-spill cap=1 windows=4194304 n_bytes=4194304 peak_bytes=140702137 peak_gb=0.1 peak_n=33.546 free_gb=0.0
XSA_PLAN_DISC INFEASIBLE shape=bin: the bin shape's ALL-PHASE scratch peak (rolling window FILES + rlebwt + piece window + container + witness spill): peak 134.2 MiB (140702137 B) vs free scratch 5.0 MiB (5242880 B) -> SHORT 129.2 MiB (2583.7% over); override --skip-fit-check to run anyway (that flag skips ONLY this absolute peak-vs-free check; the RAM term, the band law and every receipt keep their teeth); fix: free scratch space, or plan the shape that has the measured envelope (--tape, whose peak is the measured 0.789n band window rather than this tree)
XSA_PLAN_RAM FEASIBLE shape=bin: RAM term = ONE BUILD GROUP ~0.0 GiB (...) = 0.3 GiB against the usable 90% of --memory-gb 8 GiB = 7.2 GiB (fits); ...
XSA_PLAN_ALTERNATIVE free scratch for the all-phase bin-shape peak
xsa: error: build: PLAN_INFEASIBLE: XSA_PLAN_DISC INFEASIBLE shape=bin: the bin shape's ALL-PHASE scratch peak (rolling window FILES + rlebwt + piece window + container + witness spill): peak 134.2 MiB (140702137 B) vs free scratch 5.0 MiB (5242880 B) -> SHORT 129.2 MiB (2583.7% over); override --skip-fit-check to run anyway (that flag skips ONLY this absolute peak-vs-free check; the RAM term, the band law and every receipt keep their teeth); fix: free scratch space, or plan the shape that has the measured envelope (--tape, whose peak is the measured 0.789n band window rather than this tree)
The XSA_PLAN_ALTERNATIVE lines above name the knobs; --skip-fit-check overrides ONLY the absolute peak-vs-free check (it does NOT touch the RAM term)
```

No phase was spawned: the refusal is the plan's, before `Journal::open`.

## F5 — no product change (150 MB, `--with-chi`)

```sh
xsa build --tape --input /dev/shm/t150m.txt --scratch /tmp/tf-f5/s150chi \
          --threads 16 --memory-gb 24 --with-chi
```

```
V2_CONTAINER_PUBLISHED path=/tmp/tf-f5/s150chi/t150m.sxi2 bytes=118891088
PEAK_INTERMEDIATE peak_bytes=311822763 n=150000001 peak_intermediate_over_n=2.0788 lcp_source=sorter-bands pending_bands=8
XSA_BUILD_DONE chi=44034230 pipeline=v2tape container=/tmp/tf-f5/s150chi/t150m.sxi2
$ sha256sum /tmp/tf-f5/s150chi/t150m.sxi2
f6abd41da6e3c3797abae2e288596a6042337a894869f573fecf51c53f624b3e
```

Walls on the same box (labelled: load average 10.3 from other jobs):
order-tape `0:58.99` (peak RSS 1,318,172 KiB), emit-tape `0:58.98`
(349,724 KiB), finish-v2 `1:01.12` (155,260 KiB). chi and the container sha256
are the banked values, i.e. the fit check is plan-time only: no producer or
finish argument, no env knob, no format byte changed.

## Counters, exact

- F1/F2/F3 plan-only: wall 0.04 s, maxrss 15,696 KiB each (survey = `df` +
  `/proc/meminfo` + `stat`, then arithmetic).
- F2 journal run (4 MiB slice, 8 threads): `chi=1295046`, container published,
  exit 0.
- F5: `chi=44034230`, container 118,891,088 B,
  sha256 `f6abd41d…53f624b3e`, `peak_intermediate_over_n=2.0788`.
- `cargo build --release`: clean (24 warnings, all pre-existing).
- `cargo test --release`: 79 passed, 0 failed — including 7 new ones
  (`plan::tests::the_tape_fit_check_prices_the_measured_tape_envelope`,
  `…the_two_shapes_are_priced_separately_and_the_bin_number_never_gates_the_tape_run`,
  `…every_refusal_names_shape_peak_free_shortfall_and_override`,
  `…no_flag_skips_the_ram_term_and_the_tape_refusal_says_so`,
  `…the_skip_line_carries_the_numbers_and_the_plan_journals_it`,
  `chain::tests::skip_fit_check_is_the_flag_and_allow_infeasible_is_a_silent_alias`,
  `chain::tests::the_shape_is_chosen_explicitly_and_only_one_shape_is_priced`).

### Two notes on replicating these transcripts

- `free_gb=` / `mem_available_gb=` are LIVE survey values (`df` on the scratch
  device, `/proc/meminfo`): other jobs move them, so a re-run of F1-F4 differs
  only in those figures and in the shortfall that follows from them (995.3-995.7
  GiB free was seen during this session). Every structural field — shape, peak,
  n-multiple, model line, shortfall arithmetic, override text — is identical.
- F5 was run TWICE, the second time on the final tree of this branch: chi
  44,034,230 and container sha256 `f6abd41d…53f624b3e` both times, so the last
  two commits (plan-print only) changed no product byte.

## Residual risks

- The tape peak is a MEASURED n-multiple on one corpus family (the Pile at
  100 GB). A corpus that is far denser in distinct contexts (more runs, a bigger
  container) could push the real peak above `1.00 n`; the factor is the operator's
  single knob on that line, and the measured basis (0.789 n) is printed beside it.
- `0.789 n` was measured at n = 100,000,000,001 (pile-head-100); the model line
  prints both bytes and ratio so the arithmetic can be checked, and it does not
  extrapolate a different ratio for a different corpus.
- `--plan-only` reports and exits 0 even when the plan is infeasible: the refusal
  is the real run's. That is unchanged behaviour, and it is documented in
  REPRODUCE.md §9.
- The bin shape's fit check still prices an EXTRAPOLATED witness spill (0.3x of
  the overflow); that is unchanged by this work and is labelled EXTRAPOLATED on
  its own `XSA_PLAN_DISC_MODEL` line.
