# Native wide handoff into aligned byte LCP and occurrence lists (generated text)

No timed Pile study. `proc_9bde` exited 0 after checking the new native front and its separate legacy-column verifier. `proc_7fd5` compiled `front64.cpp`, `parse64.cpp` and the Rust independent raw checker, but the Python fixture runner exited **1** on case 3: the front refused 8,046 column bytes under its unchanged 7,938-byte maximum. Keep this traceback. It was a mistaken test expectation, not a wrapped rank or an incorrect suffix. `proc_a3d5` exited 0 with the **same native binaries**, checking the exact predicted refusal and all other cases. `proc_b219` rebuilt the consumer with the block minima and additional raw LCE oracle checks and exited 0. No 10 GB consumer result or `>2^32`-row trial.

Reproduce fixtures from checked-in sources:

```
d=$(mktemp -d)
g++ -std=c++20 -O3 -Wall -Wextra -Werror -fopenmp xsa/pfp/tools/front64.cpp -o "$d/front"
g++ -std=c++20 -O3 -Wall -Wextra -Werror -fopenmp xsa/pfp/tools/parse64.cpp -o "$d/parse64"
rustc --edition=2021 -O -Dwarnings gates/overlap_rank_view64_check.rs -o "$d/check"
python3 -BE gates/overlap_rank_view64_test.py "$d/front" "$d/parse64" "$d/check"
```

Every successful case crosses actual native file boundaries from raw → `dictionary.lex64` + `keys.u64` → `inverse.u64`. The oracle independently scans raw triggers, sorts raw suffixes at their starts, and checks rank identity, every adjacent byte LCP, lists, positions, predecessors, successors, and additional LCE pairs (including both orderings, END and self). [Algorithm, API and memory bound](../../../xsa/pfp/RANK_VIEW64.md). Do not confuse the [real prepared-column rank pair](../parse64-10g/) with either these fixtures or the [raw-front prelaunch refusal](../raw64-p1-prelaunch/).
