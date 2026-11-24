//! Experimental bounded descriptor sort. The corpus is the only phrase-byte store.
//! Workspace here bounds descriptor count, NOT all phrase/parse metadata.
use super::order;
use std::{cmp::Reverse, collections::BinaryHeap};
type Pair = (u32, u32);
#[path = "overlap_min.rs"]
mod ranges;
use ranges::BlockMin;
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
const LEAF: u32 = 1 << 31;
fn leaf_node(tree: &[u32], raw: &[u8], eof: bool, o: usize) -> usize {
    let mut i = cell(raw, eof, o);
    let mut d = o + 2;
    while tree[i] & LEAF == 0 {
        i = tree[i] as usize + raw.get(d).map_or(eof as usize, |&b| 2 + b as usize);
        d += 1;
    }
    i
}
fn number(tree: &mut [u32], counts: &[usize], id: usize, hist: &mut Vec<usize>) {
    let base = tree[id];
    if base & LEAF != 0 {
        tree[id] = LEAF | hist.len() as u32;
        hist.push(counts[id]);
    } else {
        for c in 0..ALPH {
            number(tree, counts, base as usize + c, hist);
        }
    }
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
    emit: impl FnMut(u64, u64, u8),
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
    mut emit: impl FnMut(u64, u64, u8),
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
    // Lexical IDs have served their purpose: ps and the aligned byte minima
    // are final. Renumber by first occurrence while visiting the parse once.
    // Occurrence lists built below then share the physical dictionary layout.
    let mut remap = vec![u32::MAX; k];
    let mut physical = Vec::with_capacity(k);
    for (j, v) in pa.iter_mut().enumerate() {
        let old = *v as usize;
        let id = &mut remap[old];
        if *id == u32::MAX {
            *id = physical.len() as u32;
            assert_eq!(words[old].pos as usize, s[j]);
            physical.push(words[old]);
        }
        *v = *id as i32;
    }
    assert_eq!(physical.len(), k);
    drop(remap);
    words = physical;
    let phys = 0..k as u32;
    let mut loc = vec![0u32; k + 1];
    for &v in &pa {
        loc[v as usize + 1] += 1;
    }
    for v in 0..k {
        loc[v + 1] += loc[v];
    }
    let carry = std::env::var("XSA_OVERLAP_CARRY_PRED").as_deref() == Ok("1");
    let mask = if carry {
        assert!(
            k < 1 << 24 && m < 1 << 24,
            "packed predecessor sandbox width"
        );
        0xffffff
    } else {
        u32::MAX
    };
    let starts: Vec<u8> = if carry {
        s[..m]
            .iter()
            .map(|&p| if p == 0 { 0 } else { bytes[p - 1] })
            .collect()
    } else {
        Vec::new()
    };
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
        occ[cur[v] as usize] = (
            s[j] as u32,
            r | if carry { u32::from(starts[j]) << 24 } else { 0 },
        );
        cur[v] += 1;
    }
    drop(starts);
    drop(cur);
    drop(ps);
    drop(pa);
    drop(s);
    // Census and regeneration in physical phrase order, not random reference order.
    eprintln!("SS_OVERLAP_PHASE pair_census");
    let mut hist = vec![0usize; ALPH * ALPH];
    for v in phys.clone() {
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
    let mut tree = Vec::new();
    let mut depth = 0;
    while hist.iter().copied().max().unwrap() > cap {
        assert!(
            depth < 32,
            "oversize cell requires more than 32 refinements"
        );
        depth += 1;
        let old = hist.len();
        if tree.is_empty() {
            tree.resize(old, LEAF);
        }
        for i in 0..old {
            if hist[i] > cap {
                let size = tree.len() + ALPH;
                assert!(size <= 1048576, "refinement node budget");
                tree[i] = tree.len() as u32;
                tree.resize(size, LEAF);
                hist[i] = 0;
            }
        }
        hist.resize(tree.len(), 0);
        for v in phys.clone() {
            let p = words[v as usize];
            if v % 32768 == 0 {
                check("refine-census");
            }
            let raw = p.raw(bytes);
            for o in 0..p.width(w) {
                let c = leaf_node(&tree, raw, p.eof(), o);
                if c >= old {
                    hist[c] += 1;
                }
            }
        }
        let refined = *hist.iter().max().unwrap();
        eprintln!(
            "SS_OVERLAP_REFINE depth={depth} parent_max={largest} child_max={refined} nodes={}",
            tree.len()
        );
    }
    if !tree.is_empty() {
        let counts = hist;
        hist = Vec::with_capacity(tree.len());
        for root in 0..ALPH * ALPH {
            number(&mut tree, &counts, root, &mut hist);
        }
        assert_eq!(counts.iter().sum::<usize>(), hist.iter().sum::<usize>());
    }

    let mut heap = BinaryHeap::new();
    let mut rows = 0;
    let mut reps = 0;
    let mut windows = 0;
    let mut prev: Option<([u8; P], Pair)> = None;
    let mut head = 0;
    let mut len = 0;
    emit(n as u64, 0, bytes.last().copied().unwrap_or(0));
    let mut flush =
        |q: &mut BinaryHeap<Reverse<(u32, usize, usize, u32, u8)>>, len: usize, head: usize| {
            let mut prior = None;
            while let Some(Reverse((r, i, end, o, pred))) = q.pop() {
                let h = if let Some(last) = prior {
                    assert!(0 < last && last < r);
                    len - w + rmq.between(last as usize - 1, r as usize - 1)
                } else {
                    head
                };
                let pos = occ[i].0 + o;
                let raw = if carry {
                    if o == 0 {
                        (occ[i].1 >> 24) as u8
                    } else {
                        pred
                    }
                } else if pos == 0 {
                    0
                } else {
                    bytes[pos as usize - 1]
                };
                emit(u64::from(pos), h as u64, raw);
                rows += 1;
                prior = Some(r);
                if i + 1 < end {
                    q.push(Reverse((occ[i + 1].1 & mask, i + 1, end, o, pred)));
                }
            }
        };
    let rounds = if P == 16 {
        std::env::var("XSA_OVERLAP_RESTAGE").map_or(0, |s| s.parse::<usize>().unwrap())
    } else {
        0
    };
    assert!(rounds <= 16);
    let restage = rounds != 0;
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
            check("@distribute");
            let mut pairs = Vec::with_capacity(mass);
            for v in phys.clone() {
                let p = words[v as usize];
                if v % 32768 == 0 {
                    check("distribute");
                }
                for o in 0..p.width(w) {
                    let raw = p.raw(bytes);
                    let c = if tree.is_empty() {
                        cell(raw, p.eof(), o)
                    } else {
                        (tree[leaf_node(&tree, raw, p.eof(), o)] ^ LEAF) as usize
                    };
                    if (lo..hi).contains(&c) {
                        let raw = &p.raw(bytes)[o..];
                        let mut key = [0u8; P];
                        let used = P.min(raw.len());
                        key[..used].copy_from_slice(&raw[..used]);
                        let tag = if carry && o != 0 {
                            u32::from(p.raw(bytes)[o - 1]) << 24
                        } else {
                            0
                        };
                        pairs.push((key, (v | tag, o as u32)));
                    }
                }
            }
            assert_eq!(pairs.len(), mass);
            windows += 1;
            eprintln!("SS_OVERLAP_PHASE window_sort window={windows} pairs={mass}");
            check("@prefix-sort");
            let mut depth = P;
            for round in 0..rounds {
                // Inductively compress the padded prefix into a monotone class.
                // Low byte is its padded LCP with the immediately previous class.
                assert!(mass < 1 << 24);
                pairs.sort_unstable_by(|a, b| a.0.cmp(&b.0));
                let mut last = None;
                let mut class = 0u32;
                let mut code = 0u32;
                let mut active = 0;
                let mut chunk_bytes = 0usize;
                for i in 0..pairs.len() {
                    // A singleton is already fully ordered in this window. Its
                    // next bytes cannot affect class order or any adjacent LCP.
                    let same = last == Some(pairs[i].0)
                        || pairs.get(i + 1).is_some_and(|q| q.0 == pairs[i].0);
                    let (key, id) = &mut pairs[i];
                    if last != Some(*key) {
                        let h = last.map_or(0, |k: [u8; P]| {
                            if round == 0 {
                                lcp(&k, key)
                            } else if k[..4] != key[..4] {
                                key[3] as usize
                            } else {
                                depth - 12 + lcp(&k[4..], &key[4..])
                            }
                        });
                        code = (class << 8) | h as u32;
                        class += 1;
                        last = Some(*key);
                    }
                    key.fill(0);
                    key[..4].copy_from_slice(&code.to_be_bytes());
                    if same && tail(bytes, &words, (id.0 & mask, id.1)).0.len() > depth {
                        pairs.swap(i, active);
                        active += 1;
                    }
                }
                // Partitioning only swaps into already-encoded positions.
                // Completed short keys correctly retain a zero-padded chunk.
                pairs[..active].sort_unstable_by_key(|(_, id)| (id.0 & mask, id.1));
                check("@refill");
                for (i, (key, id)) in pairs[..active].iter_mut().enumerate() {
                    if i % 32768 == 0 {
                        check("restage");
                    }
                    let (a, _) = tail(bytes, &words, (id.0 & mask, id.1));
                    if a.len() > depth {
                        let used = 12.min(a.len() - depth);
                        key[4..4 + used].copy_from_slice(&a[depth..depth + used]);
                        chunk_bytes += used;
                    }
                }
                check("@prefix-sort");
                depth += 12;
                eprintln!("SS_OVERLAP_REFILL window={windows} depth={depth} active={active} bytes={chunk_bytes}");
                if active == 0 {
                    break;
                }
            }
            check("@exact-sort");
            pairs.sort_unstable_by(|(ka, a), (kb, b)| {
                checks = checks.wrapping_add(1);
                if checks % 65536 == 0 {
                    check("descriptor-sort");
                }
                ka.cmp(kb).then_with(|| {
                    let (x, xe) = tail(bytes, &words, (a.0 & mask, a.1));
                    let (y, ye) = tail(bytes, &words, (b.0 & mask, b.1));
                    if x.len().min(y.len()) <= depth {
                        (x.len(), xe).cmp(&(y.len(), ye))
                    } else {
                        (&x[depth..], xe).cmp(&(&y[depth..], ye))
                    }
                })
            });
            eprintln!("SS_OVERLAP_PHASE emit window={windows}");
            check("@emit");
            for (index, (key, tagged)) in pairs.into_iter().enumerate() {
                let pred = (tagged.0 >> 24) as u8;
                let id = (tagged.0 & mask, tagged.1);
                let (raw, eof) = tail(bytes, &words, id);
                let old = prev.map(|(k, p)| (k, tail(bytes, &words, p)));
                if (restage && index == 0)
                    || !old.is_some_and(|(k, (a, e))| {
                        k == key
                            && e == eof
                            && a.len() == raw.len()
                            && a.get(depth..) == raw.get(depth..)
                    })
                {
                    flush(&mut heap, len, head);
                    head = old.map_or(0, |(k, (a, e))| {
                        let h = if restage && index == 0 {
                            lcp(a, raw)
                        } else if restage && k != key {
                            let h = if k[..4] != key[..4] {
                                key[3] as usize
                            } else {
                                depth - 12 + lcp(&k[4..], &key[4..])
                            };
                            h.min(a.len()).min(raw.len())
                        } else if k != key {
                            lcp(&k, &key).min(a.len()).min(raw.len())
                        } else {
                            let skip = depth.min(a.len()).min(raw.len());
                            skip + lcp(&a[skip..], &raw[skip..])
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
                heap.push(Reverse((
                    occ[i].1 & mask,
                    i,
                    loc[v + 1] as usize,
                    id.1,
                    pred,
                )));
            }
        }
        lo = hi;
    }
    flush(&mut heap, len, head);
    assert_eq!(rows, n);
    eprintln!("SS_OVERLAP_BINNED rows={rows} phrases={m} distinct={k} reps={reps} windows={windows} max_cell={largest} descriptor_cap={cap}");
}
