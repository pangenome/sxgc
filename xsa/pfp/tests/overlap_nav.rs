#[path = "../src/overlap_nav.rs"]
mod nav;
use nav::*;
fn lcp<T: Eq>(a: &[T], b: &[T]) -> usize {
    a.iter().zip(b).take_while(|(x, y)| x == y).count()
}
#[test]
fn columns() {
    for n in [0, 1, 2, 63, 64, 65, 129, 217] {
        for mode in 0..5 {
            let a: Vec<_> = (0..n)
                .map(|i| match mode {
                    0 => 0,
                    1 => u64::MAX,
                    2 => u64::MAX / (n.max(2) as u64 - 1) * i as u64,
                    3 => {
                        if i % 64 == 0 && i == 0 {
                            0
                        } else {
                            u64::MAX
                        }
                    }
                    _ => i as u64 * 139,
                })
                .collect();
            let c = Monotone::build(n, a.last().copied().unwrap_or(0), a.iter().copied()).unwrap();
            for (i, &x) in a.iter().enumerate() {
                assert_eq!(c.get(i), Some(x));
            }
            assert_eq!(c.get(n), None);
        }
    }
    for (n, max, v) in [
        (2, 4, vec![3, 2]),
        (2, 0, vec![0, 1]),
        (3, 0, vec![0]),
        (0, 1, vec![0]),
    ] {
        assert!(Monotone::build(n, max, v).is_err());
    }
}
// Dense fixture uses word-LCP plus owned bytes; independent oracle sorts raw suffix slices.
fn fixture(t: &[u8], w: usize, trigger: impl Fn(&[u8]) -> bool) -> bool {
    let n = t.len();
    let mut s = vec![0];
    s.extend(
        t.windows(w)
            .enumerate()
            .skip(1)
            .filter(|(_, b)| trigger(b))
            .map(|(p, _)| p),
    );
    let m = s.len();
    s.push(n);
    let mut raw = t.to_vec();
    raw.push(0); // data bytes strictly positive; END sorts first
    let words: Vec<_> = (0..m)
        .map(|j| &raw[s[j]..if j + 1 == m { n + 1 } else { s[j + 1] + w }])
        .collect();
    let mut dict = words.clone();
    dict.sort();
    dict.dedup();
    let ids: Vec<_> = words
        .iter()
        .map(|x| dict.binary_search(x).unwrap() as u32)
        .collect();
    let mut sa: Vec<_> = (0..m as u32).collect();
    sa.sort_by_key(|&j| &ids[j as usize..]);
    let mut brute = sa.clone();
    brute.sort_by_key(|&j| &t[s[j as usize]..]);
    assert_eq!(sa, brute);
    let mut pl = vec![0; m];
    for pair in sa.windows(2) {
        let (a, b) = (pair[0] as usize, pair[1] as usize);
        let z = lcp(&ids[a..], &ids[b..]);
        pl[b] = (s[b + z] - s[b] + lcp(words[a + z], words[b + z])) as u64;
        assert_eq!(pl[b] as usize, lcp(&t[s[a]..], &t[s[b]..]));
    }
    for step in [None, Some(2), Some(4), Some(8)] {
        let nav = Successors::build(&ids, &sa, step).unwrap();
        let c = Aligned::build(
            &nav,
            n as u64,
            w as u64,
            s[..m].iter().map(|&v| v as u64),
            pl.iter().copied(),
        )
        .unwrap();
        let rm = BlockMin::build(&c, 7).unwrap();
        assert_eq!(nav.position(0), Some(m as u32));
        assert_eq!(c.start(m), Some(n as u64));
        for (i, &j) in sa.iter().enumerate() {
            let r = i as u32 + 1;
            assert_eq!(nav.position(r), Some(j));
            let next = nav.successor(r).unwrap();
            assert_eq!(nav.position(next), Some(j + 1));
            assert_eq!(c.leaf(r), Some(pl[j as usize]));
            for x in 0..r {
                let expected = if x == 0 {
                    0
                } else {
                    lcp(&t[s[sa[x as usize - 1] as usize]..], &t[s[j as usize]..])
                };
                assert_eq!(rm.between(x, r), Some(expected as u64));
            }
        }
        // Materialize tiny finite-key classes only in oracle fixture. Recover row position,
        // preceding byte and within-tie LCP using only list rank and dictionary offset.
        let pred: Vec<_> = sa
            .iter()
            .map(|&j| {
                if j == 0 {
                    256u16
                } else {
                    let i = j as usize - 1;
                    words[i][s[i + 1] - s[i] - 1] as u16
                }
            })
            .collect();
        let mut rows = vec![(n as u64, 0, t.last().map(|&b| b as u16).unwrap_or(256))];
        let mut groups = std::collections::BTreeMap::new();
        for (i, &j) in sa.iter().enumerate() {
            let j = j as usize;
            for off in 0..s[j + 1] - s[j] {
                groups
                    .entry(&words[j][off..])
                    .or_insert_with(Vec::new)
                    .push((nav.successor(i as u32 + 1).unwrap(), i as u32 + 1, off));
            }
        }
        let mut last_key: &[u8] = &[];
        for (key, mut list) in groups {
            list.sort();
            let mut prior = None;
            for (next, r, off) in list {
                let j = nav.position(next).unwrap() - 1;
                assert_eq!(Some(j), nav.position(r));
                let pos = c.start(j as usize).unwrap() + off as u64;
                let h = match prior {
                    Some(x) => (key.len() - w) as u64 + rm.between(x, next).unwrap(),
                    None => lcp(last_key, key) as u64,
                };
                rows.push((
                    pos,
                    h,
                    if off == 0 {
                        pred[(r - 1) as usize]
                    } else {
                        dict[ids[j as usize] as usize][off - 1] as u16
                    },
                ));
                prior = Some(next);
            }
            last_key = key;
        }
        let mut order: Vec<_> = (0..=n).collect();
        order.sort_by_key(|&p| &t[p..]);
        for (i, &p) in order.iter().enumerate() {
            assert_eq!(
                rows[i],
                (
                    p as u64,
                    if i == 0 {
                        0
                    } else {
                        lcp(&t[order[i - 1]..], &t[p..]) as u64
                    },
                    p.checked_sub(1).map(|x| t[x] as u16).unwrap_or(256)
                )
            );
        }
    }
    (1..m).any(|j| pl[j] + (s[j] as u64) < pl[j - 1] + s[j - 1] as u64)
}
#[test]
fn raw_oracles() {
    fixture(&[], 8, |_| true);
    fixture(&vec![1; 77], 8, |_| true);
    let mut seed = 23u64;
    let mut needs_slack = false;
    for n in 1..70 {
        let t: Vec<_> = (0..n)
            .map(|_| {
                seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
                1 + (seed >> 63) as u8
            })
            .collect();
        needs_slack |= fixture(&t, 3, |w| w[0] == w[2]);
    }
    assert!(needs_slack);
}
#[test]
fn refusals() {
    assert!(Successors::build(&[0, 0], &[0, 1], Some(2)).is_err());
    assert!(Successors::build(&[0, 1], &[1, 1], None).is_err());
    let nav = Successors::build(&[1, 0], &[1, 0], Some(2)).unwrap();
    for (n, w, s, p) in [
        (8, 2, [1, 2], [0, 0]),
        (8, 2, [0, 7], [0, 0]),
        (8, 2, [0, 2], [0, 1]),
        (8, 2, [0, 2], [7, 0]),
        (u64::MAX, 2, [0, 2], [0, 0]),
    ] {
        assert!(Aligned::build(&nav, n, w, s, p).is_err());
    }
}

#[test]
fn checked_wide_positions() {
    let big = u32::MAX as u64 + 13;
    let nav = Successors::build(&[0, 1], &[0, 1], Some(4)).unwrap();
    let c = Aligned::build(&nav, big * 3, 8, [0, big], [0, big + 1]).unwrap();
    assert_eq!(c.start(1), Some(big));
    assert_eq!(c.leaf(2), Some(big + 1));
    assert_eq!(c.leaf(3), None);
    assert_eq!(c.leaf(0), Some(0));
    let rm = BlockMin::build(&c, 16).unwrap();
    assert_eq!(rm.between(1, 2), Some(big + 1));
    assert_eq!(rm.between(2, 3), None);
}
