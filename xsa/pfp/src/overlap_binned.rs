//! Experimental bounded descriptor sort. The corpus is the only phrase-byte store.
//! Workspace here bounds descriptor count, NOT all phrase/parse metadata.
use super::order;
use std::{cmp::Reverse, collections::BinaryHeap};
type Pair = (u32, u32);
// The sandbox path requires n<i32::MAX. Byte LCPs therefore fit u32.
// Keep the exact leaves, but index only minima of 16 consecutive ranks.
struct BlockMin {
    leaves: Vec<u32>,
    tree: Vec<u32>,
    blocks: usize,
}
impl BlockMin {
    const B: usize = 16;
    fn new(leaves: Vec<u32>) -> Self {
        let blocks = leaves.len().div_ceil(Self::B);
        let mut tree = vec![u32::MAX; 2 * blocks];
        for (i, b) in leaves.chunks(Self::B).enumerate() {
            tree[blocks + i] = *b.iter().min().unwrap();
        }
        for i in (1..blocks).rev() {
            tree[i] = tree[i * 2].min(tree[i * 2 + 1]);
        }
        Self {
            leaves,
            tree,
            blocks,
        }
    }
    fn between(&self, x: usize, y: usize) -> usize {
        assert!(x < y && y < self.leaves.len());
        let (lo, hi) = (x + 1, y + 1);
        let (a, b) = (lo.div_ceil(Self::B), hi / Self::B);
        if a >= b {
            return *self.leaves[lo..hi].iter().min().unwrap() as usize;
        }
        let mut v = u32::MAX;
        for &t in self.leaves[lo..a * Self::B]
            .iter()
            .chain(&self.leaves[b * Self::B..hi])
        {
            v = v.min(t);
        }
        let (mut l, mut r) = (a + self.blocks, b + self.blocks);
        while l < r {
            if l % 2 == 1 {
                v = v.min(self.tree[l]);
                l += 1;
            }
            if r % 2 == 1 {
                r -= 1;
                v = v.min(self.tree[r]);
            }
            l /= 2;
            r /= 2;
        }
        v as usize
    }
}
#[cfg(test)]
mod block_tests {
    use super::*;
    #[test]
    fn every_interval() {
        for n in 1..99 {
            let t = BlockMin::new((0..n).map(|i| ((i * 37 + n) % 53) as u32).collect());
            for x in 0..n {
                for y in x + 1..n {
                    assert_eq!(
                        t.between(x, y),
                        *t.leaves[x + 1..=y].iter().min().unwrap() as usize
                    );
                }
            }
        }
    }
}
const ALPH: usize = 258;
#[derive(Copy, Clone)]
struct Word {
    pos: u32,
    span: u32,
}
impl Word {
    fn eof(self) -> bool {
        self.span >> 31 != 0
    }
    fn raw(self, bytes: &[u8]) -> &[u8] {
        let p = self.pos as usize;
        &bytes[p..p + (self.span & 0x7fff_ffff) as usize]
    }
    fn width(self, w: usize) -> usize {
        (self.span & 0x7fff_ffff) as usize - if self.eof() { 0 } else { w }
    }
}
fn tail<'a>(bytes: &'a [u8], words: &[Word], (v, o): Pair) -> (&'a [u8], bool) {
    let p = words[v as usize];
    (&p.raw(bytes)[o as usize..], p.eof())
}
fn lcp(a: &[u8], b: &[u8]) -> usize {
    a.iter().zip(b).take_while(|(x, y)| x == y).count()
}
fn refined_cell(levels: &[Vec<u32>], raw: &[u8], eof: bool, o: usize) -> usize {
    let mut c = cell(raw, eof, o);
    for (level, base) in levels.iter().enumerate() {
        c = base[c] as usize
            + if base[c + 1] - base[c] > 1 {
                raw.get(o + 2 + level)
                    .map_or(eof as usize, |&b| 2 + b as usize)
            } else {
                0
            };
    }
    c
}
fn cell(raw: &[u8], eof: bool, o: usize) -> usize {
    let a = 2 + raw[o] as usize;
    let b = raw
        .get(o + 1)
        .map_or(if eof { 1 } else { 0 }, |&x| 2 + x as usize);
    a * ALPH + b
}
pub(super) fn run(
    bytes: &[u8],
    w: usize,
    s: Vec<usize>,
    check: &impl Fn(&str),
    emit: impl FnMut(u64, u64),
) {
    match std::env::var("XSA_OVERLAP_INLINE_PREFIX").as_deref() {
        Ok("16") => run_prefixed::<16>(bytes, w, s, check, emit),
        Ok("0") | Err(_) => run_prefixed::<0>(bytes, w, s, check, emit),
        _ => panic!("supported inline prefix: 0 or 16 bytes"),
    }
}
// Zero padding preserves weak lexicographic order even for NUL bytes. A
// prefix collision (including padding collisions) always compares finite keys.
fn run_prefixed<const P: usize>(
    bytes: &[u8],
    w: usize,
    s: Vec<usize>,
    check: &impl Fn(&str),
    mut emit: impl FnMut(u64, u64),
) {
    let n = bytes.len();
    let m = s.len() - 1;
    let key = |j: usize| {
        (
            &bytes[s[j]..if j + 1 == m { n } else { s[j + 1] + w }],
            j + 1 == m,
        )
    };
    assert!(n < i32::MAX as usize);
    eprintln!("SS_OVERLAP_PHASE dict_sort");
    let mut indices: Vec<u32> = (0..u32::try_from(m).unwrap()).collect();
    let mut checks = 0u32;
    indices.sort_unstable_by(|&a, &b| {
        checks = checks.wrapping_add(1);
        if checks % 65536 == 0 {
            check("dict-sort");
        }
        key(a as usize).cmp(&key(b as usize)).then(a.cmp(&b))
    });
    eprintln!("SS_OVERLAP_PHASE dedup_scatter");
    let mut words: Vec<Word> = Vec::with_capacity(m);
    let mut pa = vec![0i32; m];
    let mut dict_adj = Vec::with_capacity(m);
    for (r, j) in indices.into_iter().enumerate() {
        if r % 32768 == 0 {
            check("dedup");
        }
        let j = j as usize;
        let (raw, eof) = key(j);
        let old = words.last().map(|&p| (p.raw(bytes), p.eof()));
        let h = old.map_or(0, |(a, _)| lcp(a, raw));
        if !old.is_some_and(|(a, e)| h == a.len() && h == raw.len() && e == eof) {
            dict_adj.push(u32::try_from(h).unwrap());
            words.push(Word {
                pos: s[j] as u32,
                span: raw.len() as u32 | ((eof as u32) << 31),
            });
        }
        pa[j] = i32::try_from(words.len() - 1).unwrap();
    }
    eprintln!("SS_OVERLAP_PHASE parse_kernel");
    check("parse-kernel");
    let k = words.len();
    let (ps, pl) = order(&pa, k as i32);
    // At the first differing phrase, lexicographic IDs are strictly ordered.
    // Its exact byte LCP is the minimum over intervening dictionary neighbors.
    let dict_min = BlockMin::new(dict_adj);
    let mut aligned = vec![0; m];
    for (r, &p) in ps.iter().enumerate() {
        let j = p as usize;
        if r != 0 {
            let i = ps[r - 1] as usize;
            let z = pl[r];
            if r % 32768 == 0 {
                check("aligned-lcp");
            }
            aligned[r] = u32::try_from(
                s[i + z] - s[i] + dict_min.between(pa[i + z] as usize, pa[j + z] as usize),
            )
            .expect("aligned LCP width");
        }
    }
    drop(dict_min);
    drop(pl);
    let rmq = BlockMin::new(aligned);
    let mut loc = vec![0u32; k + 1];
    for &v in &pa {
        loc[v as usize + 1] += 1;
    }
    for v in 0..k {
        loc[v + 1] += loc[v];
    }
    let mut cur = loc.clone();
    // Each emit-list item needs only its source position and next-rank. Build
    // both directly in next-rank order: no inverse parse SA or starts in emit.
    let mut occ = vec![(0u32, 0u32); m];
    for (j, r) in std::iter::once((m - 1, 0u32)).chain(
        ps.iter()
            .enumerate()
            .filter_map(|(r, &x)| (x as usize).checked_sub(1).map(|j| (j, (r + 1) as u32))),
    ) {
        let v = pa[j] as usize;
        occ[cur[v] as usize] = (s[j] as u32, r);
        cur[v] += 1;
    }
    drop(cur);
    drop(ps);
    drop(pa);
    drop(s);
    // Census and regeneration in physical phrase order, not random reference order.
    let mut phys: Vec<u32> = (0..k as u32).collect();
    phys.sort_unstable_by_key(|&v| words[v as usize].pos);
    eprintln!("SS_OVERLAP_PHASE pair_census");
    let mut hist = vec![0usize; ALPH * ALPH];
    for &v in &phys {
        let p = words[v as usize];
        if v % 32768 == 0 {
            check("pair-census");
        }
        for o in 0..p.width(w) {
            hist[cell(p.raw(bytes), p.eof(), o)] += 1;
        }
    }
    let cap =
        std::env::var("XSA_OVERLAP_SORT_PAIRS").map_or(1048576, |s| s.parse::<usize>().unwrap());
    assert!(cap > 0);
    let largest = hist.iter().copied().max().unwrap();
    // Refine only oversized parent cells; avoid a dense 258^3 histogram.
    let mut levels = Vec::new();
    let mut table_bytes = 0usize;
    while hist.iter().copied().max().unwrap() > cap {
        assert!(
            levels.len() < 32,
            "oversize cell requires more than 32 refinements"
        );
        table_bytes += (hist.len() + 1) * 4;
        assert!(
            table_bytes <= 8 * 1024 * 1024,
            "refinement maps exceed 8 MiB"
        );
        let mut base = Vec::with_capacity(hist.len() + 1);
        let parent = hist;
        let mut size = 0;
        for &count in &parent {
            base.push(u32::try_from(size).unwrap());
            size += if count > cap { ALPH } else { 1 };
        }
        base.push(u32::try_from(size).unwrap());
        assert!(size <= 1048576, "refined histogram exceeds slot guard");
        hist = vec![0; size];
        for c in 0..parent.len() {
            if parent[c] <= cap {
                hist[base[c] as usize] = parent[c];
            }
        }
        for &v in &phys {
            let p = words[v as usize];
            if v % 32768 == 0 {
                check("refine-census");
            }
            let raw = p.raw(bytes);
            for o in 0..p.width(w) {
                let c = refined_cell(&levels, raw, p.eof(), o);
                if parent[c] > cap {
                    hist[base[c] as usize
                        + raw
                            .get(o + 2 + levels.len())
                            .map_or(p.eof() as usize, |&b| 2 + b as usize)] += 1;
                }
            }
        }
        let refined = *hist.iter().max().unwrap();
        eprintln!("SS_OVERLAP_REFINE parent_max={largest} child_max={refined} slots={size}");
        levels.push(base);
    }

    let mut heap = BinaryHeap::new();
    let mut rows = 0;
    let mut reps = 0;
    let mut windows = 0;
    let mut prev: Option<([u8; P], Pair)> = None;
    let mut head = 0;
    let mut len = 0;
    emit(n as u64, 0);
    let mut flush =
        |q: &mut BinaryHeap<Reverse<(u32, usize, usize, u32)>>, len: usize, head: usize| {
            let mut prior = None;
            while let Some(Reverse((r, i, end, o))) = q.pop() {
                let h = if let Some(last) = prior {
                    assert!(0 < last && last < r);
                    len - w + rmq.between(last as usize - 1, r as usize - 1)
                } else {
                    head
                };
                emit(u64::from(occ[i].0) + u64::from(o), h as u64);
                rows += 1;
                prior = Some(r);
                if i + 1 < end {
                    q.push(Reverse((occ[i + 1].1, i + 1, end, o)));
                }
            }
        };
    let mut lo = 0;
    while lo < hist.len() {
        let mut mass = 0;
        let mut hi = lo;
        while hi < hist.len() && mass + hist[hi] <= cap {
            mass += hist[hi];
            hi += 1;
        }
        assert!(hi > lo);
        if mass > 0 {
            let mut pairs = Vec::with_capacity(mass);
            for &v in &phys {
                let p = words[v as usize];
                if v % 32768 == 0 {
                    check("distribute");
                }
                for o in 0..p.width(w) {
                    let raw = p.raw(bytes);
                    let c = refined_cell(&levels, raw, p.eof(), o);
                    if (lo..hi).contains(&c) {
                        let raw = &p.raw(bytes)[o..];
                        let mut key = [0u8; P];
                        let used = P.min(raw.len());
                        key[..used].copy_from_slice(&raw[..used]);
                        pairs.push((key, (v, o as u32)));
                    }
                }
            }
            assert_eq!(pairs.len(), mass);
            windows += 1;
            eprintln!("SS_OVERLAP_PHASE window_sort window={windows} pairs={mass}");
            pairs.sort_unstable_by(|(ka, a), (kb, b)| {
                checks = checks.wrapping_add(1);
                if checks % 65536 == 0 {
                    check("descriptor-sort");
                }
                ka.cmp(kb)
                    .then_with(|| tail(bytes, &words, *a).cmp(&tail(bytes, &words, *b)))
            });
            eprintln!("SS_OVERLAP_PHASE emit window={windows}");
            for (key, id) in pairs {
                let (raw, eof) = tail(bytes, &words, id);
                let old = prev.map(|(k, p)| (k, tail(bytes, &words, p)));
                if !old.is_some_and(|(k, t)| k == key && t == (raw, eof)) {
                    flush(&mut heap, len, head);
                    head = old.map_or(0, |(k, (a, e))| {
                        let h = if k != key {
                            lcp(&k, &key).min(a.len()).min(raw.len())
                        } else {
                            lcp(a, raw)
                        };
                        assert!(h < a.len() || e, "proper prefix in overlap representatives");
                        h
                    });
                    len = raw.len() + usize::from(eof);
                    prev = Some((key, id));
                    reps += 1;
                }
                let v = id.0 as usize;
                let i = loc[v] as usize;
                heap.push(Reverse((occ[i].1, i, loc[v + 1] as usize, id.1)));
            }
        }
        lo = hi;
    }
    flush(&mut heap, len, head);
    assert_eq!(rows, n);
    eprintln!("SS_OVERLAP_BINNED rows={rows} phrases={m} distinct={k} reps={reps} windows={windows} max_cell={largest} descriptor_cap={cap}");
}
