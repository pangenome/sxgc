# Context bounds do not imply ordered-output bounds

The requested bounded-deferral rank-concat implementation is **not implemented**.
The existing position-valued member 5 and production sweep remain unchanged.
This note records an executable obstruction, not a successful E1 gate.

Run:

```sh
python3 tools/binbwt/tests/chi_deferral_test.py
python3 tools/binbwt/tests/chi_deferral_test.py --require-concat
```

The first command passes by checking the counterexamples. The second deliberately
fails (exit 1) on the actual production sweep's `1,2,0,3` emission. The test extracts
`SweepImpl` directly from `bit6/xsa_finish.cpp`, so it cannot silently drift onto a
copied implementation. It uses rank-labelled synthetic endpoints to isolate
scheduling; no production coordinate representation is changed.

## Why flushing active contexts is incorrect

At a boundary, `rr_act[c]` does not mean the candidate is already a final witness.
A later `lcp_b > rr_len[c]` replaces `rr_pos[c]` without emitting the previous
candidate. The test forks one identical prefix into two continuations: EOF emits
rank 0, while returning to context 1 at a higher threshold replaces rank 0.
Consequently early unconditional emission changes the final witness set.

## Why bounded unresolved contexts are insufficient

Start with contexts 1 and 2 at threshold 1, then alternate contexts 3 and 4
at thresholds 2 and 1. Context 1's rank 0 stays active. Each pair determines two
later ranks. The test grants ascending emission within each band by sorting its
two newly determined entries, before checking the obstruction.

| Two-run bands | Unresolved contexts at band close (peak) | Determined ranks blocked by rank 0 |
| --- | --- | --- |
| 1 | 2 | 2 |
| 128 | 2 | 256 |
| 4096 | 2 | 8192 |

The alphabet is fixed at four nonzero symbols. Every band close checks the
255-context bound. Increasing the number of pairs increases the blocked output
without increasing that context bound. The fixture's retained output vectors are
test oracles, **not** a proposed bounded production buffer.

Under online immutable emission, these three requirements conflict on this
state-machine input:

1. Emit all already determined later ranks at each band close.
2. Hold an earlier rank whose membership depends on later input.
3. Emit ranks in strictly increasing order across concatenation.

If the earlier rank survives, requirement 1 violates requirement 3. Holding all
later determined ranks violates requirement 1 and requires storage proportional
to those entries, rather than the unresolved contexts. Flushing the earlier rank
speculatively is invalid for the alternate continuation.

This family is a synthetic state-machine input, not a proof that every member of
the family arises from a text's SA/LCP stream. A separate exact suffix-array and
LCP construction for `banana` plus NUL feeds the same production class and emits
ranks `3,2,0`: even sorting the early pair gives `2,3,0`. Thus rank inversions also
occur with realizable text. A stronger corpus-specific theorem would be needed
to exclude the growing-buffer family from a redesigned algorithm.

## Possible contract changes (not implemented)

Mutable per-band output could reserve/patch unresolved entries by context and
traverse bands in rank order only after resolution. That separates resolution
order from final concatenation order, but it does not emit immutable sorted
contributions at each band close. A second pass or lookahead may also settle
membership before ordered emission; neither follows from a context bound alone.
A rank-valued member 5 additionally requires an explicit format/reader change;
placing ranks into the existing position EF payload is not compatible.

## Validation status

The obstruction regression passes; the proposed concat gate still fails. No
before-fail/after-pass fix is claimed. Corpus E1 counts (8 MB / 150 MB), E2 queries,
E3 10 GB, and E4 100 GB were not run. No production `CHI_DEFER`, `CHI_PEAK`, spill,
or finish-RSS measurements are claimed. Existing format notes were clarified;
no container version or member payload changed.
