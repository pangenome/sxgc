#[path = "../src/overlap_lexblocks.rs"]
mod lex;
use lex::LexBlocks;
use std::cmp::Ordering;
#[allow(dead_code)]
#[path = "../src/overlap_nav.rs"]
mod nav;
// A real local trigger rule (all complete windows, except forced origin).
// Only the independent verifier sees raw suffix slices. Production classes
// need an orderer, not the tiny BTreeMap oracle used here for finite keys.
fn integrated(t: &[u8], all: bool) -> (usize, usize) {
    let n = t.len();
    let mut s = vec![0];
    s.extend(
        t.windows(8)
            .enumerate()
            .skip(1)
            .filter(|(_, a)| {
                all || a.iter().fold(1469598103934665603u64, |h, &b| {
                    (h ^ u64::from(b)).wrapping_mul(1099511628211)
                }) % 31
                    == 0
            })
            .map(|(i, _)| i),
    );
    let m = s.len();
    s.push(n);
    let words: Vec<_> = (0..m)
        .map(|j| {
            (
                &t[s[j]..if j + 1 == m { n } else { s[j + 1] + 8 }],
                j + 1 == m,
            )
        })
        .collect();
    let mut dict = words.clone();
    dict.sort();
    dict.dedup();
    let ids: Vec<_> = words
        .iter()
        .map(|w| dict.binary_search(w).unwrap() as u32)
        .collect();
    let mut order: Vec<_> = (0..m as u32).collect();
    order.sort_by_key(|&j| &ids[j as usize..]);
    let psi = nav::Successors::build(&ids, &order, Some(8)).unwrap();
    let heads =
        nav::Heads::from_sorted(dict.len(), m, order.iter().map(|&j| ids[j as usize])).unwrap();
    drop(order);
    let compact = LexBlocks::fixture(&dict, 8).unwrap();
    let common = |a: u32, b: u32| {
        let u = compact.word(a as usize)?;
        let v = compact.word(b as usize)?;
        let mut p = 0;
        while matches!((u.symbol(p),v.symbol(p)),(Some(x),Some(y)) if x!=0 && x==y) {
            p += 1;
        }
        Ok(p as u64)
    };
    let aligned = nav::Aligned::from_parse(
        &psi,
        &heads,
        &ids,
        *ids.last().unwrap(),
        n as u64,
        8,
        |id| Ok(compact.word(id as usize)?.owned() as u64),
        common,
    )
    .unwrap();
    let pred = psi
        .predecessor_bytes(&heads, &ids, |id| {
            let w = compact.word(id as usize)?;
            Ok((w.symbol(w.owned() - 1).unwrap() - 1) as u8)
        })
        .unwrap();
    for j in 0..=m {
        assert_eq!(aligned.start(j), Some(s[j] as u64));
    }
    drop(ids);
    let rm = nav::BlockMin::build(&aligned, 16).unwrap();
    let mut groups = std::collections::BTreeMap::<Vec<u16>, Vec<(u32, usize, usize, usize)>>::new();
    for id in 0..dict.len() {
        let w = compact.word(id).unwrap();
        for off in 0..w.owned() {
            let key: Vec<_> = (off..).map_while(|j| w.symbol(j)).collect();
            let g = groups.entry(key).or_default();
            for i in heads.slots(id).unwrap() {
                g.push((psi.successor(i as u32 + 1).unwrap(), i, id, off));
            }
        }
    }
    let nkeys = groups.len();
    let mut rows = vec![(n, 0, t[n - 1] as u16)];
    let mut prev = Vec::<u16>::new();
    for (key, mut ties) in groups {
        ties.sort();
        let mut prior = None;
        for (r, i, id, off) in ties {
            let j = psi.position(i as u32 + 1).unwrap() as usize;
            let pos = aligned.start(j).unwrap() as usize + off;
            let h = if let Some(pr) = prior {
                key.len() - 8 + rm.between(pr, r).unwrap() as usize
            } else {
                prev.iter().zip(&key).take_while(|(a, b)| a == b).count()
            };
            let byte = if pos == 0 {
                256
            } else if off == 0 {
                pred[i] as u16
            } else {
                compact.word(id).unwrap().symbol(off - 1).unwrap() - 1
            };
            rows.push((pos, h, byte));
            prior = Some(r);
        }
        prev = key;
    }
    let mut brute: Vec<_> = (0..=n).collect();
    brute.sort_by_key(|&p| &t[p..]);
    assert_eq!(rows.len(), brute.len());
    let mut prev = &[][..];
    for (r, p) in brute.into_iter().enumerate() {
        let lcp = prev.iter().zip(&t[p..]).take_while(|(a, b)| a == b).count();
        assert_eq!(
            rows[r],
            (p, lcp, p.checked_sub(1).map(|i| t[i] as u16).unwrap_or(256))
        );
        prev = &t[p..];
    }
    (m, nkeys)
}
#[test]
fn sampled_text_order_kasai_and_heads() {
    integrated(&[0], false);
    integrated(&[255; 400], true);
    let mut h = 55u64;
    let text: Vec<_> = (0..12000)
        .map(|i| {
            h = h.wrapping_mul(6364136223846793005).wrapping_add(1);
            if i % 97 < 65 {
                i as u8 % 7
            } else {
                (h >> 48) as u8
            }
        })
        .collect();
    let (m, g) = integrated(&text, false);
    assert!(m > 100 && g > m);
}

fn check(words: &[(Vec<u8>, bool)], keys: bool) {
    let slices: Vec<_> = words.iter().map(|(v, b)| (v.as_slice(), *b)).collect();
    for span in [4, 8, 16] {
        let b = LexBlocks::fixture(&slices, span).unwrap();
        let mut expected = 0;
        let mut desc = Vec::new();
        for (i, (s, e)) in words.iter().enumerate() {
            let w = b.word(i).unwrap();
            let p = if i % span == 0 {
                0
            } else {
                s.iter()
                    .zip(&words[i - 1].0)
                    .take_while(|(a, b)| a == b)
                    .count()
            };
            let lit = s.len() - p;
            let sz = |v: usize| (usize::BITS - v.leading_zeros()).max(1).div_ceil(7) as usize;
            expected += lit + sz(2 * lit + usize::from(*e)) + if i % span == 0 { 0 } else { sz(p) };
            for (j, &c) in s.iter().enumerate() {
                assert_eq!(w.symbol(j), Some(c as u16 + 1));
            }
            assert_eq!(w.symbol(s.len()), e.then_some(0));
            assert_eq!(w.symbol(usize::MAX), None);
            assert_eq!(w.owned(), s.len() - if *e { 0 } else { 8 });
            if keys {
                desc.extend((0..w.owned()).map(|j| (i, j)));
            }
        }
        assert_eq!(b.payload(), expected);
        assert_eq!(b.directory(), 8 * (words.len().div_ceil(span) + 1));
        assert!(b.word(words.len()).is_err());
        // Brute all finite tails. Preserve descriptor ties without trying to order occurrences.
        let full: Vec<Vec<u16>> = words
            .iter()
            .map(|(v, e)| {
                v.iter()
                    .map(|&x| x as u16 + 1)
                    .chain(e.then_some(0))
                    .collect()
            })
            .collect();
        let mut brute = desc.clone();
        brute.sort_by(|&(a, x), &(c, y)| full[a][x..].cmp(&full[c][y..]));
        desc.sort_by(|&(a, x), &(c, y)| {
            let wa = b.word(a).unwrap();
            let wc = b.word(c).unwrap();
            for z in 0.. {
                let l = wa.symbol(x + z);
                let r = wc.symbol(y + z);
                if l != r {
                    return l.cmp(&r);
                }
                if l.is_none() {
                    break;
                }
            }
            Ordering::Equal
        });
        assert_eq!(brute, desc);
    }
}
#[test]
fn keys_and_seams() {
    let mut w = vec![
        (b"abcxabcdx".to_vec(), false),
        (b"abcxabcdx".to_vec(), true),
        (vec![0xff; 9], false),
        (vec![0; 10], false),
    ];
    for i in 0..79u8 {
        let mut v = vec![i / 9, 0, 0, 0, i % 9];
        v.extend_from_slice(b"same finite suffix");
        w.push((v, false));
    }
    w.sort();
    check(&w, true);
    check(&[(vec![0xff], true)], true);
}
#[test]
fn long_and_decreasing_prefixes() {
    let mut words = vec![(vec![0; 16384], false), (vec![0; 16384], true)];
    for i in 1..30 {
        let mut v = vec![0; 16384 - i];
        v.push(255);
        words.push((v, false));
    }
    words.sort();
    check(&words, false);
}
#[test]
fn refusals() {
    let normal = &[5u8; 9][..];
    let bad = [(normal, false), (normal, false), (normal, true)];
    for span in [0, 1, 17, usize::MAX] {
        assert!(LexBlocks::fixture(&[(normal, true)], span).is_err());
    }
    assert!(LexBlocks::fixture(&bad, 4).is_err());
    assert!(LexBlocks::fixture(&[(normal, false)], 4).is_err());
    assert!(LexBlocks::fixture(&[(&[0; 8], false), (normal, true)], 4).is_err());
    assert!(LexBlocks::fixture(&[(normal, true), (normal, false)], 4).is_err());
}
