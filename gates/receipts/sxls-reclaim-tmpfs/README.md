# Private SXLS credit retirement — decision receipt (base 5a1bb56)

**Recommendation: try a separately labelled, matched 10 GB A/B, not a new default.** The small test identifies directory enumeration as a real scaling mechanism in both processes. It does not attribute all of the reported 5,570 harness CPU seconds, establish an NVMe saving, or predict the full run's wall time.

## Contract and proof

Set `XSA_PRIVATE_SXLS_RECLAIM=1` **on the resident sink only**. This asserts a fresh private bands directory, one sequential producer starting at zero, one frozen non-spooling finish and nobody else depending on marker history. Observational footprint sampling that tolerates ENOENT is fine. No restart, replay, forensic history, or partial-state recovery contract. A sink cannot discover arbitrary readers or inspect a separately launched finish's env; establishing exclusivity and non-spooling is the launcher's responsibility. The implementation additionally checks resident mode, positive window, absence of enabled spool in the sink environment, and at slice zero a real empty directory. No path is swept. Defaults and harness unchanged.

* `main.rs` resident `publish` uses `u=0,1,...`, never reuses a name and passes a fixed window. Thus each old credit belongs to the fresh stream and has exactly one producer-side waiter, `u=old+W`.
* `bit6/xsa_finish.cpp` `openNextSlice` computes `path` from `nextSlice`, returns from `waitSlice(path+".done")`, opens/validates the payload and advances `nextSlice`. Both the empty and nonempty branches close/unlink the payload **before** exclusive creation of `.consumed`. Nonempty data needed later is already copied. The finish only closes the credit FD afterwards; unlinking the directory entry need not wait for close. Neither branch waits on that name again. This is not true of a replaying or second reader.
* In `emit_lcp_slice_mode` only after the original wait for exactly `old=u-W` succeeds, require the payload absent and **both** entries empty, regular, single-link (no following symlinks). Only unlink those two exact marker paths, before admitting `u`. Metadata validation relies on the private namespace, not protection against hostile name replacement. Bad state fails closed before the first unlink; an unlink failure or crash between unlinks is fatal/nonresumable. No payload, fsync, rename, polling, scratch accounting or sampler change.
* Inductively there are at most `min(slices,W)` published namesets and at most twice that many markers (including the final undrained-by-producer tail). At production W=64: at most 128, not 65,670. No final sweep. The existing W-entry queue, slice byte buffers, file traffic and window credits are unchanged. New userspace space is constant-many short paths/stats, no per-history table. Marker file lengths are zero but dentries, inodes and directory storage cost kernel memory: O(W) *live names*, not a promise about slab reclamation timing. Keep the full tmpfs reserve, control margin and RAM/scratch gates.

## Matched small mechanism study

Prediction: with `S` slices, scanning after each publish costs O(S²) metadata visits without retirement, O(SW) with it. At a fixed progress point, independent samples cost O(S) vs O(W). With W near or above S, or when payload IO dominates, do not expect the same speedup.

Reproduce on Linux with `/dev/shm` tmpfs:

```sh
cargo test --release --offline --manifest-path xsa/pfp/Cargo.toml --bin pfp_sort_r -j2 --no-run
# Use the exact Executable printed above as TEST.
(ulimit -v 262144; timeout 90 taskset -c 24,25 python3 gates/sxls_reclaim_study.py "$TEST")
```

No raw corpus, prepared columns, construction, piece or SXI2 timing here. Synthetic producer commands, real release Rust sink writer and real `scratch_gate` per publication, separate Python consumer copying/verifying each byte before unlink/credit. Actual harness `footprint` at the same 32 progress checkpoints per arm (not a comparison at different wall-time cadences); sink and sampler quiescent for reproducibility. ABBA, 1,024 unique slices x 2 rows, one escape/slice, W=8, scalar writes both arms, only difference absent flag vs `1`. 256 MiB address-space limit per process, two allowed CPUs shared, no private cgroup RAM/swap reservation. Exactly 67,584 payload bytes written/fsynced/read per arm, same ordered SHA-256 in `study.txt`. Sink samples match the entire expected scratch trajectory, peaking at 528 bytes; harness alloc peak 28,672 bytes for seven tmpfs payload pages, not including kernel inode memory. Sink HWM 2.69–2.74 MiB. `/proc/PID/io` in receipt is task attribution including test logs/stdio; no physical payload IO estimate from tmpfs.

| measurement | absent (two arms) | enabled (two arms) |
|---|---:|---:|
| final markers | 2,048 | 16 |
| Python footprint CPU / wall (s) | 2.03–2.06 / 2.03–2.06 | .033–.035 / .033–.035 |
| Rust scratch gate wall (s) | 2.30–2.42 | .088–.088 |
| Rust total CPU (s) | 2.37–2.48 | .168–.169 |
| fixture wall, excludes cleanup (s) | 4.54–4.71 | .329–.338 |

The study also withholds slice 0 credit but supplies slice 1 credit: no admission of slice 8; stop exits 75 in the original wait with earlier markers intact. Isolated refusal cases: nonresident, spool, zero window, preexisting marker, active payload, nonempty credit, symlink, hardlink, missing done. Normal completion checks exact payload contents and tail count. No frozen binary was run or changed. This stresses hot tmpfs metadata, not cache/workset equivalence to the 10 GB product.

For any next arm require pinned baseline binary/inputs and matched sink writer, grants, scratch, FS placement, sampling, finish, verifier, product profile and phase boundaries. Record marker high water, sink and harness CPU/wall by phase, memory and attributed/physical traffic separately, verify identical complete container against anchor. The existing harness deliberately strips ambient XSA flags: opt in only through a separately approved sink-launch change; do not inject into a live run. No rung or default promoted here.
