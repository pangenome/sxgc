//! Experimental bounded descriptor sort. The corpus is the only phrase-byte store.
//! Workspace here bounds descriptor count, NOT all phrase/parse metadata.
use super::{order, Min};
use std::{
    cmp::Reverse,
    collections::{BTreeMap, BinaryHeap},
};
type Pair = (u32, u32);
const ALPH: usize = 258;
#[derive(Copy, Clone)]
struct Word<'a> {
    raw: &'a [u8],
    eof: bool,
    width: usize,
}
fn tail<'a>(words: &[Word<'a>], (v, o): Pair) -> (&'a [u8], bool) {
    let p = words[v as usize];
    (&p.raw[o as usize..], p.eof)
}
fn lcp(a: &[u8], b: &[u8]) -> usize {
    a.iter().zip(b).take_while(|(x, y)| x == y).count()
}
fn cell(p: Word, o: usize) -> usize {
    let a = 2 + p.raw[o] as usize;
    let b = p
        .raw
        .get(o + 1)
        .map_or(if p.eof { 1 } else { 0 }, |&x| 2 + x as usize);
    a * ALPH + b
}
pub(super) fn run(bytes: &[u8], w: usize, s: &[usize], mut emit: impl FnMut(u64, u64)) {
    let n = bytes.len();
    let m = s.len() - 1;
    let key = |j: usize| {
        (
            &bytes[s[j]..if j + 1 == m { n } else { s[j + 1] + w }],
            j + 1 == m,
        )
    };
    let mut table = BTreeMap::new();
    for j in 0..m {
        table.insert(key(j), 0i32);
    }
    let mut words = Vec::with_capacity(table.len());
    for (&(raw, eof), id) in table.iter_mut() {
        *id = i32::try_from(words.len()).unwrap();
        words.push(Word {
            raw,
            eof,
            width: if eof { raw.len() } else { raw.len() - w },
        });
    }
    let pa: Vec<_> = (0..m).map(|j| table[&key(j)]).collect();
    drop(table);
    let k = words.len();
    let (ps, pl) = order(&pa, k as i32);
    let mut aligned = vec![0; m];
    let mut prank = vec![0u32; m];
    for (r, &p) in ps.iter().enumerate() {
        let j = p as usize;
        prank[j] = r as u32;
        if r != 0 {
            let i = ps[r - 1] as usize;
            let z = pl[r];
            aligned[r] =
                s[i + z] - s[i] + lcp(words[pa[i + z] as usize].raw, words[pa[j + z] as usize].raw);
        }
    }
    drop(pl);
    let rmq = Min::new(aligned);
    let mut loc = vec![0usize; k + 1];
    for &v in &pa {
        loc[v as usize + 1] += 1;
    }
    for v in 0..k {
        loc[v + 1] += loc[v];
    }
    let mut cur = loc.clone();
    let mut occ = vec![0u32; m];
    for j in std::iter::once(m - 1).chain(ps.iter().filter_map(|&x| (x as usize).checked_sub(1))) {
        let v = pa[j] as usize;
        occ[cur[v]] = j as u32;
        cur[v] += 1;
    }
    drop(cur);
    drop(ps);
    drop(pa);
    // Census and regeneration in physical phrase order, not random reference order.
    let mut phys: Vec<u32> = (0..k as u32).collect();
    phys.sort_unstable_by_key(|&v| words[v as usize].raw.as_ptr());
    let mut hist = vec![0usize; ALPH * ALPH];
    for &v in &phys {
        let p = words[v as usize];
        for o in 0..p.width {
            hist[cell(p, o)] += 1;
        }
    }
    let cap =
        std::env::var("XSA_OVERLAP_SORT_PAIRS").map_or(1048576, |s| s.parse::<usize>().unwrap());
    assert!(cap > 0);
    let largest = hist.iter().copied().max().unwrap();
    assert!(largest<=cap,"overlap two-byte cell {largest} exceeds hard descriptor cap {cap}; deeper partition required");
    let rank = |j: u32| {
        if j as usize + 1 == m {
            0
        } else {
            prank[j as usize + 1] + 1
        }
    };
    let mut heap = BinaryHeap::new();
    let mut rows = 0;
    let mut reps = 0;
    let mut windows = 0;
    let mut prev = None;
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
                emit((s[occ[i] as usize] + o as usize) as u64, h as u64);
                rows += 1;
                prior = Some(r);
                if i + 1 < end {
                    q.push(Reverse((rank(occ[i + 1]), i + 1, end, o)));
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
                for o in 0..p.width {
                    let c = cell(p, o);
                    if (lo..hi).contains(&c) {
                        pairs.push((v, o as u32));
                    }
                }
            }
            assert_eq!(pairs.len(), mass);
            windows += 1;
            pairs.sort_unstable_by(|&a, &b| tail(&words, a).cmp(&tail(&words, b)));
            for id in pairs {
                let (raw, eof) = tail(&words, id);
                let old = prev.map(|p| tail(&words, p));
                if old != Some((raw, eof)) {
                    flush(&mut heap, len, head);
                    head = old.map_or(0, |(a, e)| {
                        let h = lcp(a, raw);
                        assert!(h < a.len() || e, "proper prefix in overlap representatives");
                        h
                    });
                    len = raw.len() + usize::from(eof);
                    prev = Some(id);
                    reps += 1;
                }
                let v = id.0 as usize;
                let i = loc[v];
                heap.push(Reverse((rank(occ[i]), i, loc[v + 1], id.1)));
            }
        }
        lo = hi;
    }
    flush(&mut heap, len, head);
    assert_eq!(rows, n);
    eprintln!("SS_OVERLAP_BINNED rows={rows} phrases={m} distinct={k} reps={reps} windows={windows} max_cell={largest} descriptor_cap={cap}");
}
