# 10 GB native dictionary grid: select four sort threads for the matched builds

Pinned `076980318c30e576dd2a2729be07506731253121`, `sandbox/dict-grid-10g-Yy3QxU`; raw inputs not reopened by replay. One process kept the real dictionary and parse-side state, reused one i64 SA allocation, and ran native-wide 1→4→2. Complete SHA of all **11,239,544,612** little-endian entries after each timer: `71a1bdfde1185e6f4a7d6cacc3210b4677c35e902af06956068a55c4324f3109`. No SA file or product emitted.

| Selected threads | Sort wall s | Process CPU s | Process peak RSS B | Sort attributed read/write B | Outside-clock hash s |
|---:|---:|---:|---:|---:|---:|
|1|1079.267|1079.143|110,972,248,064|0 / 0|69.949|
|4|558.868|1347.793|110,973,251,584|0 / 0|88.072|
|2|923.223|1444.941|110,974,312,448|0 / 0|85.532|

Four is the only arm within 5% of fastest with no extra sort disk bytes (**0.518×** serial wall, **1.249×** CPU). These are API-selected counts, not an observed OpenMP team-size trace. Process RSS is a high-water mark, not a delta. Across prep + all arms cgroup peak 124,423,614,464 B / 200 GiB, four build CPUs, 140 GiB arrays, zero swap/OOM, `io.stat` unavailable. The [previous stopped attempt](../native-grid-10g-stop/) has no arm digest or selection. This does not measure lane speed, establish a product default, or authorize a larger rung. `result.json` SHA-256 `4b54c25e04f4e5971b24e156ced210f7ce5cf4829127dcbcb9c0e84729c92cef`.
