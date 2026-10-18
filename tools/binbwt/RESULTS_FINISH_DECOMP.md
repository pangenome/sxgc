# finish-decomposed: STEP 0 anatomy (the state-vs-verification split)

Base `641e00e` (pile-week-v1), branch `finish-decomposed`. Inherited from
`binbwt-v3-finish-fast` (`a123e98`, cherry-picked as `e2abe36`): the measure-first
instrument (`PILE_FINISH_PROF` ITIMER_PROF sampler + ELF .symtab resolution,
`PILE_FINISH_REGION` wall/CPU/fault/device-read deltas, `PILE_FINISH_ACCESS`
corpus-read/page-hop counters, and the `pf_check(bool,const char*)` overload).

Nothing about the fold's work changed: the 1 GB gate below is byte-identical
(`cmp=0`) and reproduces the charter's counters exactly.

## The measurement (1 GB fixture, 96 threads, our job alone on the box)

```
PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609     finish wall 236.0 s
cmp=0  (bank: extcols-scratch/pilev2-g2-1b/frag.sxi2)
SLIM_FP pile-finish-corpus queries=473695414 verified_symbols=12820424563
SLIM_FP pile-finish-seam   verified_symbols=29271872
```

Phase anatomy of the 236.0 s:

| phase | wall s | serial? | unit rate |
|---|---:|---|---|
| readiness (PFCK) | 2.21 | serial | 1 pass over corpus |
| **sweep** | **189.37** | mixed | below |
| m5-ef | 25.88 | mixed | 283,296,933 witnesses |
| m1-runs | 18.43 | serial | 368,036,609 runs |
| publish | 0.10 | serial | |

The sweep, decomposed by `PILE_FINISH_REGION` (these regions are per band, so
they neither overlap nor double count; `repair` wall = piecewait + readband +
feed):

```
PILE_FINISH_REGION piece-wait     wall=24.185  cpu=0.654
PILE_FINISH_REGION readband       wall= 5.486  cpu= 5.577   raw_runs_ns=14.91
PILE_FINISH_REGION repair-feed    wall=51.966  cpu=51.746   raw_runs_ns=141.20
PILE_FINISH_REGION lce            wall=71.400  cpu=6585.518 runs_ns=194.00
PILE_FINISH_REGION advance-witness wall=32.035 cpu=32.152   runs_ns=87.04
PILE_FINISH_SWEEP_ANATOMY piecewait=24.185 readband=5.486 feed=51.966 repair_total=83.647
PILE_FINISH_CORPUS_ACCESS phase=sweep calls=57875150134 page_hops=29366413735 hop_rate=0.5074
```

## The split, and what it says

`lce` is 71.400 s **wall** against 6585.5 s of **thread CPU** = **92.2x parallel**.
The verification work the charter describes (12,820,424,563 verified symbols +
473,695,414 queries, i.e. the `SlimFingerprint::lce` direct-verify loops) **is
already fanned out per band over the 96-thread pool** (`processBuffer`'s
`pool.run`, `pile_finish.cpp:2385`); the main thread is one of its workers (it
spends 52.7% of its own 236 s of CPU inside that region). So the "verification
goes parallel" half of the operator's decomposition is already landed, and
cannot pay again *as fan-out*.

What is left is a **schedule** problem, not a work problem. Per band the three
stages are strictly serialised into each other:

```
read_band -> feed_raw (SERIAL, 141 ns/raw run) -> LCE (parallel, 92x) -> sweep.advance (SERIAL, 87 ns/run)
```

so the finish's wall is the *sum* of a parallel stage and two serial stages
instead of the max. The state-maintenance/verification split, measured:

| | 1 GB | share of finish |
|---|---:|---:|
| serial state maintenance (feed 52.0 + advance 32.0 + readband 5.5) | 89.5 s | 37.9% |
| verification (LCE, parallel, 6585 s CPU) | 71.7 s | 30.4% |
| producer backpressure (piece-wait) | 24.2 s | 10.3% |
| witness/EF + m1 encode (post-sweep, serial-ish) | 44.3 s | 18.8% |
| readiness | 2.2 s | 0.9% |

The serial fold is 89.5 s of the 236.0 s; the verification is 71.7 s of which
**the main thread's own participation is the only reason it is on the critical
path at all** (it is 1 worker of 92). Sliding the verification of band *k* under
the fold of band *k+1* alone should take the sweep term from 189 s to ~90 s
with no change in work. That is the decomposition to build.

## Provenance / load labelling

Producer rebuilt from this tree: sha256 `5fa842fc1907...` = the attested
`binbwt` v2 binary, so the producer side of every gate below is the gated one.
Walls above were taken with **our own producer+finish the only jobs on the box**
(loadavg 16.8-21.0 before the run, 55.6 during it - the load *is* this run).
100 MB pyramid gate: `producer=0 finish=0 cmp=0`, chi=29716349, sweep 15.56 s
(repair 7.50 = wait 1.82 + read 0.68 + feed 4.90, lce 4.70 at 91x, advance 3.13).
