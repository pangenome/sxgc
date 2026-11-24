# Post-close v8 space audit, not a new product run

2026-11-24 UTC. [Derivation, corrected historical verdict and conditional replacement](../../../docs/overlap_finish_credits.md). Read-only original `/mnt/nvme2n1/erikg/TAPEREADER/sandbox/ext-product-QhQZxU`, created with `fe688cc`, banked at `7bfb649`. Product SHA `6325c843c0d9aa9be00d69260aafdd794936e2e79f16174efb00c43db831f341`, original `finish.log` SHA `75a6e89636bb85e28ddf801f59cf60f0031ed6c0806c1c862819c97c56c19295`. Original `laws.rs` / finish source blobs `7be98de66e2592fefa336cc14f416606604d5bb4` / `8a04d79a0dc80cd813c3a561cb75f1910fd57777`. 1 CPU / 128 MiB, no swap / 45 s ceiling for the reduction. Read the entire 107,519,008 B frozen product once for SHA plus small directories for geometry. No raw corpus or row replay. No writes to the sandbox or timing results.

```
python3 -BE gates/overlap_finish_credits.py extract | cmp - gates/receipts/finish-credits-100m/reduction.json  # rehearsal host
python3 -BE gates/overlap_finish_credits.py review | cmp - gates/receipts/finish-credits-100m/review.json
python3 -BE gates/overlap_finish_credits_test.py
```

Reduction SHA `7e5ae1e3158af9f5cc443cd001794264187cdbcb0ab1ec25a367c84580434c18`; review SHA `f721ca2f7b6177de8d57202f2a62bbabdc8ddbfb3dc6b97c86fc91a6ad863427`. The first synthetic cross-credit assertion for 16 singleton rows predicted a stall at slice width 8/piece width 2/window 1. It **completed**: the slice precedes the next piece lookahead. The test now requires success at 8 and stall at 9; no payload rerun, credit increase or relaxed bound. 12,276 positive finite-credit schedules and 7,079 necessary-slice refusals verified.
