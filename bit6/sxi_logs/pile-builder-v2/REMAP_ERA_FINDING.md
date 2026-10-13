# THE 10GB RUNG FATAL: root cause and the remap-era finding (supervisor, direct execution)

Date: 2026-10-13. Author: supervisor (direct execution mode, per user directive).

## What happened

The 10GB re-rung's finish died with `FATAL pile_finish: unsupported text alphabet`
(bit6/pile_finish.cpp, historically line 811: `pf_check(v.c >= 6 && v.c < 256, ...)`,
the same check exists in the v1 path at bit6/rpfbwt_endpoints.cpp:87).

Surface cause: the REAL 10GB pile snap contains 18 occurrences of 0x00 and 23 of
0x05 (byte-histogram verified; first NUL at snap offset 1,403,706,725, immediately
before a 0x1e separator). The production remap (the involution: 0x01<->0xFF,
0x02<->0xFD, 0x03<->0xFE, 0x04<->0xFC; identity elsewhere; sha256 b4f38776...) is
IDENTITY for 0x00 and 0x05, so the remapped text still contains them and trips the
chars-0-5 reservation. The 100MB/1GB fixtures contain neither byte, which is why
every fixture gate passed and only the rung exposed it.

## The deep finding: the banked 10GB reference is from a DIFFERENT remap era

Empirical derivation (the banked v2 chunks carry per-run head SA samples, so every
record is a checkable equation `banked_text[h-1] == c` against the corpus):

- Tool: /tmp/derive.c over real10b-ref/chunks/chunk-4.crle (117,729,284 records,
  sum(len)==n exact, SUM_OK), snap bytes at the record's head position, expected
  char = production involution remap of the corpus byte.
- Result: 117,692,712 of 117,729,284 records MISMATCH the involution expectation,
  with an overwhelming uniform pattern: **banked char == involution char + 6**
  across essentially the whole alphabet (0x0a->0x10, 0x20->0x26, 0x61->0x67,
  0x65->0x6b, and for the involution-folded bytes: corpus 0x01->0x07, 0x02->0x08,
  0x03->0x09, 0x04->0x0a, 0x00(NUL)->0x06).
- Conclusion: the banked 10GB chunks were built with a **+6-shift alphabet remap**
  (the pre-involution design that vacates 0x00-0x05 as the padding range), NOT the
  current involution. The `v.c >= 6` conventions descend from that design: under
  the +6 map, text chars are always >= 6, so the check is an invariant, not a
  restriction. The byte-remap campaign's involution migrated 1-4 away but LEFT
  0x00 and 0x05 unvacated - an incomplete migration that no fixture could catch
  (all fixtures are clean of those two bytes) and only real pile text exposes.
- The current chunker on the SAME byte range (chunk-4 bounds, offset 1,250,027,909,
  n=312,534,883) emits the NUL row as c==0 with the involution - behavior
  confirmed different from the banked era.

## Consequences (careful, not yet fully resolved)

1. chi is invariant under ORDER-PRESERVING alphabet relabelings (runs and LCPs
   depend only on equality/order of bytes). A pure +6 shift is order-preserving
   for all non-wrapped bytes; the involution (flipping 0x01-0x04 up to 0xFC-0xFF)
   is NOT order-preserving. Therefore chi(+6 text) and chi(involution text) can
   DIFFER on corpora containing the affected bytes - and the 10GB corpus does
   (0x01 x1521, 0x03 x96494, 0x04 x102, 0x00 x18, 0x05 x23).
   => **chi=2,676,929,448 (the banked 10GB constant) may be UNREPRODUCIBLE by the
   current chain** - it is the chi of the +6-era text, not of the involution-era
   text. This must be resolved before any 10GB chi-expect gate: either prove the
   order differences never affect chi on this corpus, or re-bank the 10GB
   constant with the current chain (cross-verified via the v1 path).
2. The 100MB/1GB fixture constants ARE reproduced by the current chain (G1/G2/G3
   GREEN, byte-identical containers) - the involution is self-consistent there.
   The 1GB fixture's 0x01-0x04 occurrences (224 bytes) apparently do not change
   chi vs the +6 era - or the 1GB banked constant is also involution-era. Do not
   assume; verify if it ever matters.
3. The pile text CONTAINS 0x00 and 0x05, so whatever verification target is used,
   the CURRENT chain must handle them: either (a) vacate 0x00/0x05 in the remap
   (must stay bijective AND order-preserving to keep chi comparable - a +6-style
   full shift is the natural candidate, i.e. consider REVERTING to the +6-family
   remap for the pile), or (b) change the stream conventions to not reserve 0-5
   (the dollar row marker c==2 and the padding conventions must move off the
   char column). Option (a) restores the original design intent and the banked
   10GB constant's era; option (b) breaks comparability with every banked
   reference built under chars>=6 conventions.

## Applied so far (uncommitted unless noted)

- bit6/pile_finish.cpp: the alphabet check widened from `v.c >= 6` to "forbid
  exactly the vacated range {1,2,3,4}" - admits 0x00/0x05 as text. NOTE: after
  the remap-era finding this fix is UNDER REVIEW: it is correct only under
  option (b). The v1 path (rpfbwt_endpoints.cpp:87) has the same check unfixed.
- Targeted fixtures cut from the real pile around the offending bytes:
  nulA.txt (20MB, 1 NUL), nulB.txt (20MB, 1x 0x05), nul-fixture*.txt (400KB).
  The v1 path FATALs on nulA at rpfbwt_endpoints.cpp:87 (pre-fix), and the v2
  path separately hit CYCLIC_SEAM_REFUSED (work-budget exhaustion, 2.57e9 work vs
  2.56e9 budget on the 20MB window - a SECOND, independent issue in the v2 seam
  machinery on real-text windows, ~16000x v1's seam work on the same text).

## Next (the decision the operator must make, with the data above)

- Decide option (a) +6-family remap for the pile (restores banked-10GB
  comparability; the NUL/0x05 problem dissolves; BUT the fragment/1GB constants
  were involution-era - need the same invariance analysis before mixing) vs
  option (b) full-alphabet conventions (fixes the format forever, forfeits the
  10GB banked constant, requires re-banking every scale reference).
- Root-cause the v2 seam work explosion on real-text windows (independent bug).
- Then re-run the 10GB rung with the resolved alphabet policy.
