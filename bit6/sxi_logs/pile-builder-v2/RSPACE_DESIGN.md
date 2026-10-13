# Literal-D2 r-space audit: pipeline redesign, not column repacking
01. Audited checkout: 227d3114986a9dea81a4014be500348431afc44d; read-only source inspection, no builds or corpus passes.
02. Citation aliases: C=bit6/chunk_frontend.cpp; M=bit6/cross_lcp_merge.cpp; F=bit6/pile_finish.cpp.
03. V=bit6/sxi_logs/pile-builder-v2/PILE_VISION_V1.md; A=the same directory/AUDIT_2026-10-13_codex_astra.md.
04. E=the same directory/REMAP_ERA_FINDING.md; R=the same directory/README.md; S=bit6/rlbwt_sampler.cpp.
05. “Design” means a proposed change; “estimate” means unmeasured; source facts have citations below.
06. Supervisor coordination tool was unavailable in this session's tool catalog; no supervisor messages were sent.
07. Verdict: killing WM and .pos is necessary but does not establish literal D2 or a <=1.6n pile launch.
08. Literal D2 also prohibits the chunker's transient sa(2*n), not just persistent columns (C:158-174; V:25-30).
09. V explicitly retains doubled libsais despite its own D2; this is a real internal contradiction (V:25-30,53-55).
10. Under this task's stricter wording, replace that front end; do not silently grandfather its RAM array.
11. A correct extremely slow <=1.6n shape is constructible; a practical near-current-wall shape is still research.
12. No supported evidence permits claiming <=1.6n AND <=2x wall today; proposed budgets below are conditional.

## Q1: position-column consumers and minimum sufficient state
13. Default chunks are SXCR v3 c+varint(len); embedded h/t were removed in favor of .pos (C:181-205,226-228).
14. Header parsing accepts v2/v3; v3 without samples requires persisted order (M:1890-1904,1971-1973).
15. .pos+.ref fast loading reads header/period, segment provenance, all positions for range checks (M:2082-2109).
16. It calls validate_pos_runs; v3 h/t are taken at first/last row, then checked against those same reads (M:1830-1838,2110).
17. Minimum here: coverage/header checks plus sparse endpoint checks; v3 self-comparison is not independent proof.
18. Those checks can move to DEBUG, retaining cheap lengths/coverage/CRC and mandatory fixture gates (V:38-49).
19. Expanded side BWT comes from c/len, not per-row positions (M:1842-1846,1865-1873).
20. Its only position need is each 64th run head for corpus-symbol validation (M:1857-1863).
21. Replace expanded BWT with run rank/select; retain sparse checked heads only as optional checkpoint hints.
22. Legacy walk does NOT read .pos: it reads v2 embedded h/t and creates seeds(row,h) (M:1974-1996).
23. It builds bwt[n], lf[n], reconstructs text, then builds pos[n] and visited[n] (M:1990-2012,2034-2057).
24. Thus the suggested “loader derives seeds from .pos” conflates separate paths; the persisted path returns before walk.
25. The legacy seeds drive parallel LF propagation of all row positions; they are not needed for corpus reads (M:2037-2049).
26. Corpus-reference fallback still reconstructs text then compares it bytewise against .ref (M:2114-2143).
27. That reconstruction is redundant for production text access: SideText opens referenced segments (M:1803,2126-2133).
28. It remains an independent DEBUG provenance check; dropping it requires preserving remap/range metadata validation.
29. Legacy non-reference paths validate all positions and preceding characters; ref fallback range-checks all positions (M:2157-2178).
30. Remove fallback order/text materialization entirely from the strict path; rejecting obsolete artifacts is safer than hidden derivation.
31. Aperiodic anchor discovery uses BWT rank + corpus symbols to find terminal-prefix intervals (M:2265-2293).
32. It does not need per-row positions for discovering those intervals; store interval endpoints and anchor text coordinates.
33. Periodic discovery DOES read class positions, checks each tied row, and allocates anchors[n] (M:2227-2255).
34. Minimum periodic state: period, residue-class ordering, ascending-position tie rule; anchors[n] violates literal D2.
35. Repair reads every interval position into slice, splits sk/an, sorts and writes an overlay (M:2305-2328).
36. Minimum: exact ability to enumerate each corrected interval in global-key order; a dense slice is not mandatory.
37. Its bounded-buffer guard does not exempt slice[len] from “not transiently” (M:2317-2321; V:25-30).
38. Core initializes PosOverlay on each side's persisted column (M:2663; position consumers continue below).
39. Shard planning binary-searches pos[mid] for rankIn and wide-side merged row ranks (M:2773-2798).
40. Minimum: locate(mid) oracle plus monotone repaired order, or a different partitioning scheme without random SA reads.
41. Heap comparisons need only CURRENT front positions; next-front acquisition reads side order windows (M:2738-2742,2888-2906).
42. Minimum for a serial heap: k positions and exact successor/advance per side; replacing storage requires implementing advance.
43. Anchor-repaired cyclic side order is not obtained by naive LF stepping through rows (M:2301-2330,2730-2737).
44. Sparse emission captures emitted row,v and per-run h/t while rows stream (M:2495-2516,2903-2906).
45. Minimum emission state: pending run c,len,h,t and seam targets; whole side .pos is an implementation choice.
46. Intermediate emission re-creates L-x.pos from wm reads, dense or SXP4 (M:3563-3569,3595-3614).
47. Thus deleting only chunk-N.pos leaves a new per-row column at every tree level; replace the entire side protocol.
48. Preflight calls load_side_ext for both sides then builds hashes; it inherits all fast/fallback readers (M:3675-3686).
49. Parallel-load preflight sizing explicitly accounts for lf/pos/visited when derivation occurs (M:3785-3804).
50. Single-side repair and walk also initialize PosOverlay and anchor repair; not just the k-way route (M:3927-3940).

## Q2: implementable merge routes and the unresolved shortcut
51. Design option L: run-rank/select plus sparse locate replaces .pos access, preserving current comparison semantics.
52. For an aperiodic cyclic side LF decrements SA modulo n; this is exactly the legacy propagation rule (M:2045-2049).
53. Store checkpoints at TEXT spacing K, recording(row,position), rather than every 64th run in SA order.
54. Locate(row): repeatedly LF until a checkpoint; return checkpoint.position+steps modulo n.
55. With text-grid samples and one LF cycle, worst locate <=K; every-64th-run sampling has no such bound.
56. Generate these checkpoints in one LF traversal from a known row/position; never build pos[n] or visited[n].
57. Periodic sides need an explicit residue/tie-class formula; ordinary cyclic LF is not a single covering cycle (M:2013-2031).
58. Design L cannot retain dense repair overlays: corrected intervals need lazy permutation or repeated selection.
59. Safe fallback for an affected interval: scan its rows, locate each, select the next global-key minimum after previous.
60. This uses scalar candidates and interval metadata, but costs O(m^2*K*rankcost) for an interval of m rows.
61. Therefore it is a correctness bridge, not a promised fast pile repair; periodic anchors[n] must also disappear.
62. Design option B: replace chunk+merge with online reverse-text BWT construction on one global corpus string.
63. Use one UNIQUE logical sentinel outside the byte alphabet, represented as a tagged row, never reserved text byte 0/5.
64. Keep a dynamic run sequence supporting rank(c,row), insert(row,c), replace(row,c), and global character counts.
65. Start with BWT='$', active sentinel row q=0; iterate p=n-1..0, reading remap[corpus[p]].
66. Compute q'=C[c]+rank(c,q) in the old BWT, replace old '$' at q by c, insert '$' at q'.
67. C includes the logical sentinel ordered first; update counts only after calculating the old-state insertion rank.
68. Maintain exact integer row counts and coalesce adjacent same-symbol runs after each edit; p is the only suffix coordinate.
69. This is ordinary sentinel BWT prepend construction, not yet the required cyclic output: explicitly certify off-by-one rules.
70. Runs may split/merge; use packed block leaves, aggregate per-symbol counts, bounded cache, one shared disk/RAM quota.
71. Design requirement: no SA samples on every edit; collect sparse row/position hints later by one sentinel LF traversal.
72. No k-way front locate is needed; no intermediate L-x.pos and no side-specific anchor repairs remain.
73. To preserve current finish, adapt the resulting finite-suffix order to its padded/raw dollar convention before seam repair.
74. The current final key is finite suffix plus dollars; intermediate key is cyclic with ascending-position ties (M:1420-1424).
75. A directly cyclic builder may instead feed final normalized runs, but must prove equality to RepairStream's normalized stream.
76. Sentinel BWT is NOT cyclic BWT by simply deleting '$': nonperiodic wrap-prefix order can reverse (A:29-38).
77. Keep existing seam-class semantics as the conversion contract; rows/flanks and occurrence coordinates still must be exact (F:619-763).
78. For each maximal seam interval, reconstruct positions via locate, sort/select cyclic keys, splice runs and coalesce boundaries.
79. Do not hold all occurrence coordinates or all class rows simultaneously; use bounded bands or repeated selection.
80. Design B's ordinary BWT core is concrete; fast bounded seam conversion and succinct dynamic storage remain unproven here.
81. “Insert one side into accumulated cyclic BWT using C+rank” is NOT certified by the ordinary sentinel recurrence.
82. Side extension changes wrap comparisons; fixed-length cyclic ties need special treatment (M:2227-2255,1420-1424).
83. Therefore start B as one global reverse scan, not an asserted drop-in cyclic side merge.
84. Prior art: repo sampler documents grlBWT BCR collection semantics and per-string LF walks (S:1-15).
85. Its sentinel rows are per-string, with a forward-flat mapping; that is different from one cyclic corpus (S:6-11,71-109).
86. Task-provided t10 certification is prior-art evidence, not evidence that its run/seam/container semantics match this pipeline.
87. ~/TeraTools source was not found by the restricted filename probe; no implementation-space or speed claim is made for it.

## Q3: h/t sparsification and preferred zero-backlog consumption
88. Finish reads each piece's c,len,packed h,t, unpacks RawRuns, deletes the piece, advances consumed sentinel (F:395-456).
89. It repairs/pads/splices runs; cut-run boundaries use seam.before/after, then carries exact endpoints (F:819-889).
90. Per normalized run it computes topLCP=LCE(previous tail,head), interior=LCE(head,tail), singleton interior=INF (F:1714-1721).
91. Sweep uses exact endpoint positions to select witness values, not merely for validation (F:930-933,989-1009).
92. Sparse product tails are taken AFTER normalization at normalized run indices 0,1024,... (F:1670-1676).
93. Therefore h/t cannot simply be dropped from 63/64 runs without providing exact endpoint reconstruction.
94. Corpus LCE compares known positions; it cannot infer SA[row] from a row number (F:1714-1721).
95. Every-64th-run h/t checkpoints yield tiny storage, but do not bound LF distance to a sampled row.
96. Design: text-grid LF checkpoints guarantee <=K only for certified one-cycle sentinel/aperiodic sides (lines 52-57).
97. Two endpoint locates per run cost roughly 2R*d LF steps; d is measured mean steps, not “32 runs”.
98. At R=.368n and K=174, uniform-distance estimate d=87 gives ~64n LF steps (estimate, no locality guarantee).
99. Plus sequential row-front locates would cost ~87n LF steps for merge L; this can dominate existing corpus LCE.
100. Preferred finish design: producer already knows endpoints; pass exact h/t in a bounded in-process band queue.
101. Store no full piece h/t columns; compute LCE+witnesses immediately and discard each band's endpoints.
102. This is O(band) per-run transient state permitted as consumed checkpoint hints, never a per-suffix column (V:28-30).
103. If interpretation disallows even band per-run endpoint columns, pass scalar runs and dispatch only bounded LCE jobs.
104. Keep c+varint(len) replay spool only; m1 needs c/len after Huffman frequencies are known (F:1898-2007).
105. Direct queue is faster and smaller than sparse h/t plus locate, but producer/finish scheduling must prevent deadlock.
106. Specifically publish seam metadata BEFORE blocking producer on bands; current finish waits for WM first (F:1560-1611).
107. WM-kill must establish this handshake; a bounded endpoint queue otherwise deadlocks before consumption begins.

## Q4: finish state inventory and production/debug separation
108. Production band state: RawRuns, normalized NRun buffer, gidx, topLcp/interior, previous tail (F:1665-1667,1693,1700-1738).
109. Replace duplicate packed hp/tp+RawRun residency with queue decode directly into bounded normalized jobs (F:424-447).
110. Production scalar state: pending source/coalesced repaired runs, row counters, seam cursor, per-char totals/run frequencies (F:806-889).
111. Production sweep: rr_len[256],rr_pos[256],rr_act[256],previous_interior,m,p,p_sa_last (F:919-925).
112. Witnesses require exact endpoints+LCPs and that scalar sweep; neither dense SA nor dense run h/t is intrinsic (F:930-1009).
113. Production seam state currently includes classes,occ,runs and temporary sa/chainSet/sortedSet (F:619-753).
114. Those are potentially large; budget the largest class, not merely “small seam spool”; nested intervals are filtered (F:707-716).
115. Replace class-wide coordinate copies by bounded reconstruction/selection; keep maximal interval descriptors and exact flanks.
116. Witness state: bounded bucket buffers, spill-run descriptors/cursors/heaps, delta-varint spill files (F:1030-1088,1124-1185).
117. m5 requires sorted strictly increasing witnesses, chi, EF low/high writers; it does not require endpoint columns (F:1833-1864).
118. m11 requires only normalized sparse tails; existing spool stores u64 every 1024th run (F:1675-1676,1878-1895).
119. m1 requires char-run frequencies, total counts, deterministic Huffman tie/code order, gamma lengths (F:1898-1969).
120. m4 is empty; m7 remap table; m10 derived marker; preserve member ordering/types exactly (F:1870-1876).
121a. Production PFCK fingerprint hints: tau=ceil(8n/R), 8 bytes/checkpoint, about .368n at stated R (F:269-280,1532-1545).
121b. Keep PFCK only during seam/LCE sweep; delete immediately after last LCE, before allocating the output/encoding regions.
121c. Sweep peak U .736+W .280+PFCK .368+Q .003+B .040=1.427n; serialization peak after deleting PFCK=1.527n.
121d. Sparse-locate indexes compete with PFCK during sweep; exact queue avoids that added term; scalar byte LCE needs no PFCK.
121. BitsOut/member CRC/container table state is encoding state, not row state; file-backed spools still count as scratch (F:1227-1366).
122. DEBUG-only: bridge c/len/dense tails/top/interior/emission-order witnesses (F:1677-1688,1727-1731).
123. DEBUG-only: banked sA sampled verification input; keep bounded samples in fixture gates (F:1815-1829).
124. DEBUG-only: legacy .ssa/.ssa_t comparability emitted outside v2 (M:3253-3258,3309-3310).
125. DEBUG-only: check_pieces; Python version retired, C is already streaming; production need not run either (R:627-634; V:44-46).
126. Required m1 replay is NOT debug decode verification: Huffman frequencies require a later c/len pass (F:1898-2007).
127. Replace raw padded replay with exact NORMALIZED compact replay; prove counts and resulting member bytes at gates.

## Q5: term-by-term pile budget and wall frontier
128. All arithmetic counts temporary output container as scratch and excludes only the original corpus (V:12-15).
129. Budget variables: final compact normalized replay U, witness spill W, sparse tails Q, final container P, bounded reserve B.
130. Task observations: W~.28n; P~.468n from V:12-13; R/n=.368 is task-provided, not a universal run bound.
131. c+varint(len) replay U~2R=.736n only if varint lengths mostly fit one byte (C:199-202); measure actual U.
132. Q=8R/1024=.002875n for existing u64 tail spool (F:1675-1676); store it once, delete after m11.
133. Design static finish cap: U .736 + W .280 + P .468 + Q .003 + B .040 = 1.527n.
134. Remaining .073n is margin, not a hidden allowance for another run set or whole member bit spool.
135. No whole WM, .pos, expanded BWT, side text, dense h/t, dense overlays, dense chunk SA in this shape.
136. Witness merging duplicates only the current value bucket; cap its encoded bytes within B, repartition before overflow.
137. Current merge duplicates one entire bucket until unlink; RAM fan-in 64 alone does not bound its disk bytes (F:1142-1180).
138. Avoid whole EF/head/len encoding duplication: reserve exact member regions in the output, pwrite bit streams there.
139. Alternatively schedule/unlink spools so live encoded bytes fit .073n+B; full output+full bit spools does not fit (F:1839-1864,1943).
140. Construction budget for candidate B: dynamic runs/index/allocator garbage <=1.50n, caches and metadata <=.10n; no finish yet.
141. This is a hard engineering allocation, NOT an observed achievable compressed dynamic-BWT bound.
142. A straightforward pointer tree is untenable: 16 B/run alone is 5.89n at R=.368n (arithmetic; compare packed proposal line 70).
143. Build frozen compact replay in destructive block drain; release run/index blocks before allocating finish witnesses/output.
144. If destructive drain still needs LF index for endpoint resolution, reconstruction state must fit the reserve or be recomputed.
145. A conservative correct alternative avoids any run index: repeatedly select the next cyclic rotation directly from corpus.
146. At each rank scan p=0..n-1, choose minimum key greater than last; compare at most n bytes, tie ascending p (M:1420-1424).
147. Keep last,best,p scalar positions; emit run c=T[p-1],h/t while streaming normalized order; no position columns anywhere.
148. Coalesce globally, perform exactly the same normalized LCE+sweep/m11; replay c/len only; budget is lines 129-139.
149. This constructive fallback proves storage sufficiency conditional on U/W limits; it is not usable at the pile.
150. Selection costs ~n^2 comparisons and worst O(n^3) inspected symbols: vastly >2x current wall, not a five-day contender.
151. Current measured anatomy is 737s contaminated ceiling: chunk123,walk170.6,count26,emit325; finish overlaps (R:599-609).
152. Finish sweep294.6=LCE154.8+advance64.7+repair74.3; m5 30.8,m1 57.3; do not sum overlapping walls (R:605-609).
153. Existing clean projection400-650 is explicitly extrapolated, not measured; A targets300-620 likewise (R:620-625; A:95-108).
154. Candidate B phase cost: scan n/bandwidth + n*(replace+insert+rank) + storage compaction; typically serial q dependency.
155. Dynamic update wall model T_B=n*u, u measured seconds/edit INCLUDING cache misses/compaction; no speed factor attested.
156. Gate <=2x737 needs u well below 1.474us/byte, after subtracting finish and conversion; 330s/GB needs far less.
157. Conversion cost sum(class_work)+endpoint_locates; repeated selection is quadratic in class sizes, an explicit red flag.
158. Finish direct-queue LCE/advance retains observed query/sweep work plus backpressure; retain PFCK until sweep completes, then unlink (F:1532-1545).
159. Sparse reconstruction adds 2R*d*t_LF: K174,d87 gives 64e9*t_LF seconds/GB (estimate).
160. At t_LF=20ns this alone is1280s; at100ns it is6400s: respectively1.74x/8.68x737 BEFORE other phases (estimate).
161. These latency points are sensitivity examples, not measured NVMe/RAM LF costs; random external accesses can be worse.
162. Frontier: text-grid K gives pair checkpoints about16n/K bytes and endpoint work aboutR*K*t_LF seconds.
163. K64: checkpoints .250n, endpoint23.55n LF; K174: .092n,64.03n; K1024: .0156n,376.83n (estimates).
164. Checkpoint row+pos values are conservatively u64 pairs; packed41-bit pairs save bytes, still only sparse checkpoints.
165. With static finish1.527n, persistent K174 hints exceed1.6n; fuse/drop them before output or reserve less elsewhere.
166. Exact h/t every64th run uses2*41*R/(8*64)=.0589n at pile, but gives NO d bound; plot its measured d, not K=64.
167. Dense h/t at pile uses2*41*R/8=3.772n, before c/len; dense SXP4 positions use41/8=5.125n (arithmetic).
168. At1GB, stated27-bit h/t+c/u32len is11.75R=4.324n; window overlap may lower peak (M:3078-3114).
169. Frontier is therefore conditional: bounded exact endpoint queue best; sparse-locate trades storage for potentially >2x wall.
170. Cannot extrapolate 1GB width27 or R ratio to a guaranteed pile ceiling; byte quota must stop BEFORE overrunning.

## Q6: safest sequence and adversarial gates
171. First let sol-4b land WM-kill; assume removal for budgets, but this checkout still has WM finish wait/build (F:1560-1611).
172. Independent preparatory work: turn debug bridges/comparability off, instrument live bytes, bound witness-bucket duplication.
173. Independent concept, adjacent code: direct-to-container bit writers; gate member CRCs/padding at100MB/1GB (F:1839-1864,1943-2018).
174. Next coordinate producer/finish bounded queue with sol-4b; conflicts in walk callbacks, piece publication, seam handshake (M:2903,3035-3114).
175. Preserve exact h/t transiently first: strongest low-risk disk saving; gate normalized runs,chi,witnesses,full container.
176. Replace m1 raw replay by normalized compact replay; this changes scratch protocol but preserves encoding contract (F:1964-2007).
177. Prototype ordinary online BWT with tagged sentinel on tiny synthetic data; no libsais/per-row arrays in proposed production path.
178. Implement succinct disk dynamic runs only AFTER update semantics pass; gate raw finite order against banked merge output.
179. Implement cyclic seam adapter/periodic ties; strongest oracle compares EVERY normalized(run c,len,h,t) on tiny inputs.
180. Include 0x00/0x05, remap-active bytes, [0x1e,0x0a,0x1e], periodic strings, nested intervals (V:38-49; A:29-38).
181. Then100MB/1GB full container cmp,exact chi,sorted witness equality; gates authorized for implementer, not run by this auditor.
182. Measure quiet-box wall and shared RAM/disk peak INCLUDING allocator dead blocks,open-unlinked files,member spools/output.
183. Remove chunk SA,.pos,repair slices,all fallback derivation only when replacement gates pass; never claim D2 before then.
184. Riskiest step: conflating sentinel/collection BCR order with required cyclic normalized order and periodic tie coordinates.
184a. Tiny scalar-selection reference needs exact byte LCE, so its worst-case finish cost also grows; its storage proof is independent of fingerprint density.
185. Oracle catches it through exact endpoint comparisons before LCE,then chi/witness differences,m11 tails,full container cmp.
186. Equal run chars/lengths alone miss wrong periodic h/t; container m11 plus tiny full endpoint oracle are both required.
187. Second risk: succinct dynamic storage or seam-class transient exceeds1.6n; enforce quota plus maximum-class measurements.
188. Remap-era history forbids treating old10GB chi as the same oracle automatically; use current100MB/1GB banks (E:20-58; V:47-49).
189. Decision result: approve bounded endpoint consumption first; treat practical literal-D2 construction as a separately gated algorithm project.
190. Residual: no practical <=1.6n near-current-wall builder is attested; the fully constructive fallback exceeds2x by enormous factors.
