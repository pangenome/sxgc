# The Pile, first 100 GB: built and verified (2026-11-05)

Corpus: `/mnt/nvme2n1/erikg/pile-head100.txt`, exactly 100,000,000,000 bytes of raw `pile.txt`
(no document framing, no sidecars, raw tape route, virtual END).
Container: `/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2`, 63,163,489,696 B.

```
XSA_BUILD_DONE chi=24,185,339,204 n=100,000,000,001 pipeline=v2tape
container      63,163,489,696 B   = 0.632 n
wall           12:35:10           (order-tape 12:21:46, emit-tape 12:21:42, finish 12:35:10)
waves          102              ties 32,442,986,338
peak RSS       order-tape 108.7 GB, finish 32.2 GB (finish anon 21.8 GB of 34.0 GB sampled)
peak scratch   78,880,730,679 B   = 0.789 n
```

## What this corrects
- **Scratch is not 2.2 n at scale.** 2.177-2.2 n was measured at 10 GB, where fixed overheads dominate.
  At 100 GB the concurrent band window gives 0.789 n. Pile projection: ~1.03 TB, not ~2.9 TB.
- **The wall is 1.6x Wikipedia per byte, not 3x.** 12.6 h here versus 8 h for 100 GB of enwiki.
- **The Pile is denser in distinct contexts:** chi/n = 0.242 vs enwiki 0.156 (+55%). Container 0.632 n
  vs enwiki 0.423 n. Corpus character, not size.
- chi per run = 24.18e9 / 31.38e9 = 0.771, matching enwiki's 0.78: the finish's per-run cost is stable.

## Pile projection (1.31 TB)
scratch ~1.03 TB, container ~830 GB, wall ~7 days, chi ~317 billion.

## Not yet done on this container
Sampled stream == BWT(SA), closure-0, T3 checker, and query positions against brute force ON PILE DATA.
Until those run, this is a build certificate, not a search certificate.
