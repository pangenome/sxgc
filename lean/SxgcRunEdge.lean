import Sxgc

/-!
# SxgcRunEdge — `RunEdgeHit` outright

The semantic half of the O1 factorization (see `lean/O1_OBSTRUCTION.md`,
attack 1).  **Theorem `runEdgeHit_true`: every inclusion-maximal coverage
class contains a run-edge position** — statement-locked in `Sxgc.lean`
(`RunEdgeHit`), proven here without touching the emission rule.

## The mathematical argument

Row/position alignment (Layers 1–2 below):

* row `k` of `triplesOf T` carries `sa = (saOrder R)[k]` where
  `R = T.reverse ++ [0]`; its **position** is `rowPos T k = N - sa`
  (`N = T.length + 1`);
* the BWT char of row `k` is the char **at** position `rowPos T k` of `T`
  (when `sa ≥ 1`; `sa = 0` gives the sentinel char `0`);
* the R-suffix of row `k` is `reverse (T.take (rowPos T k - 1)) ++ [0]`.

Coverage alignment: `p = w ++ [c]` is covered at `x` iff `w` is a suffix of
`T.take (x - 1)` and `c = T[x - 1]`.  Hence the occurrences of `w` are in
bijection with the rows whose R-suffix starts with `reverse w` — the
**w-interval** — and the BWT chars of those rows are exactly the chars
following `w`'s occurrences.

Main proof (by contradiction on "no run-edge row of the w-interval has
char `c`"):

1. **Block argument.**  The w-interval is convex in SA order (rows are
   sorted by their R-suffixes, and "starts with `reverse w`" is
   lex-convex).  If some w-row has char `≠ c`, then along the interval the
   is-`c` status changes at an adjacent pair `(k, k+1)` of w-rows, and the
   `c`-side of that pair is a run edge (its char differs from its
   neighbour's).  So: no char-`c` w-row is an edge ⟹ **every** w-row has
   char `c`.

2. **The sentinel row kills the suffix case.**  If `w` is a suffix of `T`
   (including `w = []`), the `sa = 0` row is a w-row with char `0 ≠ c`
   (positive texts), contradicting (1).

3. **Right-maximality kills the rest.**  Otherwise every occurrence of `w`
   is followed by `c`, so `rightExts w T = [c]` has length 1, and `w` is
   not a suffix of `T` — so `w` is not right-maximal, contradicting
   `(w, c) ∈ requirements`.

So some run-edge row `k` of the w-interval has char `c`; its position `v`
ends an occurrence of `p = w ++ [c]`, and since every requirement covered
at `m` is a suffix of the maxW witness `p` (which ends at `v`), all of
`covSet T m` is covered at `v`: `ScopeLe T m v`, and `v` is a run-edge
position.  -/

namespace Sxgc

/-! Row geometry is assembled in `Sxgc` for the covering proof. -/

/-- **RunEdgeHit outright** — every inclusion-maximal coverage class contains
a run-edge position. -/
theorem runEdgeHit_true (T : Text) (hT : positive T = true) : RunEdgeHit T := by
  intro m hmP hmax
  -- position bounds for m
  have hm1 : 1 ≤ m ∧ m ≤ T.length := by
    rcases List.mem_map.mp hmP with ⟨i, hi, rfl⟩
    rw [List.mem_range] at hi
    omega
  obtain ⟨hm1, hmT⟩ := hm1
  -- the maxW witness
  obtain ⟨p, hpCov, hpW⟩ := exists_maxW (T := T) (x := m) (hne := hmax.1)
  obtain ⟨hpReq, hpCover⟩ := (mem_covSet T m p).mp hpCov
  obtain ⟨hwSub, hwMax, hcExt⟩ := (mem_requirements p.1 p.2 T).mp hpReq
  obtain ⟨w, c⟩ := p
  -- the cover decomposition
  obtain ⟨u, hu⟩ := (coversAt_iff_suffix (w ++ [c]) m T).mp hpCover
  rw [pref] at hu
  have hsplit : T.take m = T.take (m - 1) ++ [T[m - 1]!] := take_succ_last T hm1 hmT
  have hlast : c = T[m - 1]! ∧ u ++ w = T.take (m - 1) := by
    have hassoc : (u ++ w) ++ [c] = T.take (m - 1) ++ [T[m - 1]!] := by
      rw [show (u ++ w) ++ [c] = u ++ (w ++ [c]) from List.append_assoc u w [c],
        ← hu, hsplit]
    exact append_last_inj _ _ _ _ hassoc
  -- positivity
  have hwpos : ∀ x ∈ w, 1 ≤ x := by
    intro x hx
    have hxw : x ∈ T.take (m - 1) := by
      rw [← hlast.2]
      exact List.mem_append_right _ hx
    have hxT : x ∈ T := List.mem_of_mem_take hxw
    exact (positive_of_mem T hT hxT).1
  have hcpos : 1 ≤ c := by
    have hcT : c ∈ T := occurs_append_last w c T ((mem_rightExts w c T).mp hcExt)
    exact (positive_of_mem T hT hcT).1
  -- the row of m
  obtain ⟨k₀, hk₀, hsa₀⟩ := saRow_surj T (T.length + 1 - m) (by omega)
  have hrp₀ : rowPos T k₀ = m := by
    unfold rowPos; rw [hsa₀]; omega
  have hchar₀ : charRow T k₀ = c := by
    rw [charRow_at_pos T k₀ hk₀ (by rw [hsa₀]; omega), hrp₀]
    exact hlast.1.symm
  have hW₀ : wRow T w k₀ := by
    refine (wRow_iff_suffix T w k₀ hk₀ hwpos).mpr ⟨u, ?_⟩
    rw [hrp₀]
    exact hlast.2.symm
  -- KEY: a char-c run-edge w-row exists
  have key : ∃ k, k < (triplesOf T).length ∧ wRow T w k ∧ charRow T k = c ∧
      isRunEdge (triplesOf T) k = true := by
    classical
    by_cases hB : ∃ k₁, k₁ < (triplesOf T).length ∧ wRow T w k₁ ∧ charRow T k₁ ≠ c
    · obtain ⟨k₁, hk₁, hW₁, hc₁⟩ := hB
      exact exists_edge_wRow T w c hk₀ hk₁ hW₀ hchar₀ hW₁ hc₁
    · exfalso
      have hall : ∀ k, k < (triplesOf T).length → wRow T w k → charRow T k = c := by
        intro k hk hW
        by_cases hne : charRow T k = c
        · exact hne
        · exact absurd ⟨k, hk, hW, hne⟩ hB
      by_cases hSuf : SuffixOf w T
      · -- the sentinel row is a w-row of char 0 ≠ c
        obtain ⟨k₀', hk₀', hsa₀'⟩ := saRow_surj T 0 (by omega)
        obtain ⟨q₀, hq₀⟩ := hSuf
        have hW₀' : wRow T w k₀' := by
          refine (wRow_iff_suffix T w k₀' hk₀' hwpos).mpr ⟨q₀, ?_⟩
          have hrp : rowPos T k₀' - 1 = T.length := by
            unfold rowPos; rw [hsa₀']; omega
          rw [hrp, take_full T T.length (by omega)]
          exact hq₀
        have hc₀' : charRow T k₀' = 0 := by
          refine charRow_zero T k₀' hk₀' ?_
          rw [← saRow_eq T k₀' hk₀', hsa₀']
        have hcc := hall k₀' hk₀' hW₀'
        omega
      · -- every occurrence of w is followed by c
        have hfoll : ∀ e, e ≤ T.length - 1 → SuffixOf w (T.take e) → T[e]! = c := by
          intro e he hsuf
          obtain ⟨q, hq⟩ := hsuf
          obtain ⟨k, hk, hsak⟩ :=
            saRow_surj T (T.length + 1 - (e + 1)) (by omega)
          have hsa1 : 1 ≤ saRow T k := by rw [hsak]; omega
          have hrp : rowPos T k = e + 1 := by
            unfold rowPos; rw [hsak]; omega
          have hWk : wRow T w k := by
            refine (wRow_iff_suffix T w k hk hwpos).mpr ⟨q, ?_⟩
            rw [hrp, show e + 1 - 1 = e from by omega]
            exact hq
          have hck := hall k hk hWk
          rw [charRow_at_pos T k hk hsa1, hrp,
            show e + 1 - 1 = e from by omega] at hck
          exact hck
        -- every right extension equals c
        have hocc : ∀ x, occurs (w ++ [x]) T = true → x = c := by
          intro x hoccx
          obtain ⟨i, hi, htake⟩ := (occurs_eq_true (w ++ [x]) T (by simp)).mp hoccx
          have hlen : (w ++ [x]).length = w.length + 1 := by simp
          have hf : i + w.length + 1 ≤ T.length := by omega
          have hfull : T.take (i + w.length + 1) = T.take i ++ w ++ [x] := by
            rw [show i + w.length + 1 = i + (w ++ [x]).length from by
                  rw [hlen]; omega,
              List.take_add, htake, List.append_assoc]
          have hsucc : T.take (i + w.length + 1)
              = T.take (i + w.length) ++ [T[i + w.length]!] :=
            take_succ_last T (by omega) (by omega)
          rw [hsucc] at hfull
          obtain ⟨hx, hqw⟩ := append_last_inj _ _ _ _ hfull
          have he : i + w.length ≤ T.length - 1 := by omega
          have hfolli := hfoll (i + w.length) he ⟨T.take i, hqw⟩
          omega
        have hext : ∀ x ∈ rightExts w T, x = c :=
          fun x hx => hocc x ((mem_rightExts w x T).mp hx)
        have hlen1 : (rightExts w T).length = 1 := by
          have hle : (rightExts w T).length ≤ 1 := by
            rw [rightExts]
            refine dedup_all_eq_le_one c _ ?_
            intro x hx
            obtain ⟨_, hxocc⟩ := List.mem_filter.mp hx
            exact hocc x hxocc
          have hge : 1 ≤ (rightExts w T).length := List.length_pos_of_mem hcExt
          omega
        -- w nonempty would contradict ¬SuffixOf
        cases w with
        | nil => exact absurd (show SuffixOf [] T from ⟨T, by simp⟩) hSuf
        | cons a as =>
          simp only [rightMaximal] at hwMax
          rw [Bool.and_eq_true, Bool.or_eq_true] at hwMax
          obtain ⟨_, hisf | hlen2⟩ := hwMax
          · exact absurd (suffix_of_isSuffix _ _ hisf) hSuf
          · exact absurd (of_decide_eq_true hlen2) (by omega)
  obtain ⟨k, hk, hW, hck, hedge⟩ := key
  -- sa of the edge row is positive
  have hsa1 : 1 ≤ saRow T k := by
    by_cases h0 : 1 ≤ saRow T k
    · exact h0
    · exfalso
      have hz : saRow T k = 0 := by omega
      have hcz : charRow T k = 0 := charRow_zero T k hk (by
        rw [← saRow_eq T k hk, hz])
      omega
  have hv1 : 1 ≤ rowPos T k := by
    have hlt := saRow_lt T k hk
    unfold rowPos; omega
  have hv2 : rowPos T k ≤ T.length := by
    have hlt := saRow_lt T k hk
    unfold rowPos; omega
  -- the witness word ends at v := rowPos T k
  obtain ⟨q, hq⟩ := (wRow_iff_suffix T w k hk hwpos).mp hW
  have hvchar : T[rowPos T k - 1]! = c := by
    rw [← charRow_at_pos T k hk hsa1]
    exact hck
  have hpsuf : ∃ qq, T.take (rowPos T k) = qq ++ (w ++ [c]) := by
    refine ⟨q, ?_⟩
    rw [take_succ_last T hv1 hv2, hvchar, hq, List.append_assoc q w [c]]
  -- ScopeLe
  have hscope : ScopeLe T m (rowPos T k) := by
    intro q hq'
    obtain ⟨hqReq, hqCov⟩ := (mem_covSet T m q).mp hq'
    have hle : (q.1 ++ [q.2]).length ≤ (w ++ [c]).length := by
      have h1 : wlen q ≤ maxW T m := wlen_le_maxW T m q hq'
      have h2 : maxW T m ≤ wlen (w, c) := hpW
      have h3 : wlen (w, c) = (w ++ [c]).length := wlen_congr (w, c)
      have h4 : wlen q = (q.1 ++ [q.2]).length := wlen_congr q
      omega
    have hdrop : (w ++ [c]).drop ((w ++ [c]).length - (q.1 ++ [q.2]).length)
        = q.1 ++ [q.2] := coversAt_suffix_of_coversAt hqCov hpCover hle
    obtain ⟨qq, hqq⟩ := hpsuf
    have hpv : coversAt (w ++ [c]) (rowPos T k) T = true :=
      (coversAt_iff_suffix _ _ T).mpr ⟨qq, hqq⟩
    have hqv : coversAt (q.1 ++ [q.2]) (rowPos T k) T = true :=
      coversAt_of_suffix hpv hdrop
    exact (mem_covSet T (rowPos T k) q).mpr ⟨hqReq, hqv⟩
  -- membership in runEdgePositions
  have hmem : rowPos T k ∈ runEdgePositions T := by
    unfold runEdgePositions
    refine List.mem_filter.mpr ⟨?_, decide_eq_true ⟨hv1, hv2⟩⟩
    refine List.mem_map.mpr ⟨k, ?_, ?_⟩
    · refine List.mem_filter.mpr ⟨by rw [List.mem_range]; exact hk, hedge⟩
    · rw [saRow_getD T k hk]
      rfl
  exact ⟨rowPos T k, hmem, hscope⟩

/-- Every run-edge position is dominated by an emitted position, unbounded. -/
theorem runEdgeDominate_true (T : Text) (hT : positive T = true) :
    RunEdgeDominate T := by
  intro x hx
  have hr : 1 ≤ x ∧ x ≤ T.length := of_decide_eq_true (List.mem_filter.mp hx).2
  exact scan_domination T hT x (mem_positionsT hr.1 hr.2)

end Sxgc
