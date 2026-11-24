# Direct literal offsets: much cheaper than rebuilding a word, still far from flat lookups

`proc_f082`, pin `1380ca6`, `lex-lookup-small-VkWtyp`, exit 0. All **77,614,404** timed query answers (including repetitions) were checked against the original fixture bytes and explicit END/absence. Not suffix rows. The direct/restart **scatter** hypothesis passed; the flat comparison did not. At these sizes and grants this is reason to keep direct access as a small prototype, **not** to swap it into a producer.

| Dictionary / trace | Direct / restart wall (forward; reverse) | Direct / flat wall (forward; reverse) | Hops / raw answer | Current-literal hit rate (raw) |
|---|---:|---:|---:|---:|
| Book / scatter | 0.168; 0.174 | **5.640; 6.732** | 0.121 | 96.10% |
| Book / first byte | 0.396; 0.399 | 15.431; 15.351 | 7.119 | 7.32% |
| Book / scan, view reused | 0.635; 0.640 | 4.057; 3.492 | 0.508 | 88.55% |
| Wide / scatter | 0.166; 0.165 | **4.494; 4.297** | 0.009 | 99.84% |
| Wide / first byte | 0.304; 0.295 | 9.929; 9.133 | 7.178 | 6.97% |
| Wide / scan, view reused | 0.598; 0.595 | 3.231; 2.431 | 0.130 | 98.02% |
| Shared-prefix / scatter | 0.476; 0.475 | **9.168; 9.859** | 6.009 | 24.90% |
| Shared-prefix / first byte | 0.549; 0.547 | 10.984; 10.918 | **7.500** | 6.25% |
| Shared-prefix / scan, view reused | **1.805; 1.812** | 8.833; 9.085 | 6.000 | 25.00% |

One warm fixed trace per cell, flat→restart→direct then reverse, checked at least 1,048,576 symbols per timed arm. CPU ratios track wall: worst scatter direct/restart CPU ratios **0.174, 0.167, 0.476**. Absolute scatter direct costs incl. check 13.43/13.39, 17.25/17.17 and 29.08/28.90 ns/query. This does **not** measure statistical significance or include preparing the table, reading a corpus, sorting, a row verifier or a finish. Raw/prepared product clocks are not applicable.

| Dictionary | Words / distinct owned offsets | Literal B | Flat / restart / direct wire B |
|---|---:|---:|---:|
| Book | 5,928 / 192,164 | 212,155 | 287,028 / 227,527 / 232,907 |
| Wide | 32,768 / 1,608,813 | 1,833,939 | 2,133,117 / 1,927,708 / 1,948,651 |
| Shared-prefix | 16,384 / 1,835,016 | 491,520 | 2,097,176 / 532,488 / 548,888 |

Observed direct/restart wires **1.024, 1.011, 1.031×**, within 1.10. The worse direct/restart scatter ratios satisfy the predeclared 2/3 (book, wide) and 1.10 (deep) cutoffs. But even almost no backward hops did not put wide scatter within 3× flat. The shared-prefix first-byte count is exactly the predicted 122,880 hops over 16,384 lookups, maximum 15. Rebuilding one restart view per **word** rather than per byte made restart *faster* than direct on this shape's sequential scan. Directory/width/length work and correlated prefixes remain relevant; the counts don't isolate individual cycle costs, cache misses or device IO. In particular, multiplying either lookup ratio by the 100 MB bit-sort clock is not justified.

Authenticated 196,608 B sample, 64 disjoint windows, SHA `09840d8046cb448eb6d9a04d60e85bbe053cfd397b676a9f188c7a925309034c`. Other decks generated in RAM; source words, flat control, both encodings and trace/expectations coexist. Cgroup peak **46,673,920 B** of 192 MiB / one CPU / no swap; no OOM. Native process entire fixture 1.94 CPU-s, attributed R/W **0 / 8,192 B**, `io.stat` unavailable (`null`). No row/column payload scratch; memfd sample, logs and private build tools not a large-dictionary allocation result. The **10 GB** direct wire is still only bounded (≤8,718,961,516 B) from the earlier tape's complete-word counters; not built or timed here.

Replay without book or corpus access: `python3 -BE gates/overlap_lex_lookup.py archived gates/receipts/lex-lookup-small`. Score SHA `e5f606ac2c00bba4f6a2d7d047cee4c37a8e07b7bd939ab7d2ca6de363f338c6`, [theory and predeclared thresholds](../../../xsa/pfp/LEX_DIRECT.md). Full originals preserved. No promotion, rerun, or automatic dispatch.
