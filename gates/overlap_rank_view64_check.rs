//! Independent raw reference for the native-front -> u64 ranks -> aligned/list handoff.
#[path = "../xsa/pfp/src/overlap_rank_view64.rs"]
mod view;
use std::path::Path;
fn read(p: &Path, max: u64) -> Vec<u8> {
    assert!(p.is_file() && p.metadata().unwrap().len() <= max);
    std::fs::read(p).unwrap()
}
fn column(p: &Path) -> Vec<u64> {
    let b = read(p, 8_000_000);
    assert_eq!(b.len() % 8, 0);
    b.chunks_exact(8)
        .map(|c| u64::from_le_bytes(c.try_into().unwrap()))
        .collect()
}
fn main() {
    let dir = std::env::args_os().nth(1).unwrap();
    let dir = Path::new(&dir);
    let raw = read(&dir.join("raw"), 100_000);
    let tape = read(&dir.join("columns/dictionary.lex64"), 200_000);
    let keys = column(&dir.join("columns/keys.u64"));
    let inv = column(&dir.join("inverse.u64"));
    let mut starts = vec![0];
    for p in 1..=raw.len().saturating_sub(8) {
        let h = raw[p..p + 8].iter().fold(1469598103934665603u64, |h, b| {
            (h ^ *b as u64).wrapping_mul(1099511628211)
        });
        if h % 31 == 0 {
            starts.push(p);
        }
    }
    let m = starts.len();
    assert_eq!(m, keys.len());
    let mut ord: Vec<usize> = (0..m).collect();
    ord.sort_unstable_by_key(|&j| &raw[starts[j]..]);
    let mut expected = vec![0; m];
    for (r, &j) in ord.iter().enumerate() {
        expected[j] = 1 + r as u64;
    }
    assert_eq!(inv, expected);
    for workers in [1, 2, 4] {
        let bound = view::array_bound(tape.len() as u64, m as u64, workers).unwrap();
        assert!(view::RankView::build(
            &tape,
            &keys,
            &inv,
            raw.len() as u64,
            workers,
            bound - 1,
            &|| false
        )
        .is_err());
        let v = view::RankView::build(
            &tape,
            &keys,
            &inv,
            raw.len() as u64,
            workers,
            bound,
            &|| false,
        )
        .unwrap();
        let mut prev = &[][..];
        let mut seen = 0;
        for (r, &j) in ord.iter().enumerate() {
            let p = starts[j];
            let suffix = &raw[p..];
            let lcp = prev.iter().zip(suffix).take_while(|(a, b)| a == b).count();
            let row = v.get(r).unwrap();
            assert_eq!(
                row,
                view::Occurrence {
                    ordinal: j as u64,
                    key: keys[j],
                    start: p as u64,
                    successor: expected.get(j + 1).copied().unwrap_or(0),
                    aligned_lcp: lcp as u64,
                    predecessor: p.checked_sub(1).map(|i| raw[i] as u16).unwrap_or(256)
                }
            );
            prev = suffix;
            if seen == r {
                let span = v.slots(row.key);
                assert_eq!(span.start, r);
                assert!(!span.is_empty());
                let mut last = None;
                for t in span.clone() {
                    let e = v.get(t).unwrap();
                    assert_eq!(e.key, row.key);
                    assert!(last.is_none_or(|x| x < e.successor));
                    last = Some(e.successor);
                }
                seen = span.end;
            }
        }
        assert_eq!(seen, m);
        assert_eq!(v.lce(0, m as u64), Some(0));
        assert_eq!(v.lce(0, u64::MAX), None);
        for r in 0..m {
            for t in [0, r, (r * 687 + 77) % m, m - 1] {
                let a = starts[ord[r]];
                let b = starts[ord[t]];
                let h = if a == b {
                    raw.len() - a
                } else {
                    raw[a..]
                        .iter()
                        .zip(&raw[b..])
                        .take_while(|(x, y)| x == y)
                        .count()
                };
                assert_eq!(v.lce(r as u64 + 1, t as u64 + 1), Some(h as u64));
            }
        }
        assert!(v.slots(u64::MAX).is_empty() && v.get(m).is_none());
    }
    let build = |k: &[u64], r: &[u64], t: &[u8], n: u64| {
        view::RankView::build(t, k, r, n, 4, u64::MAX, &|| false).is_ok()
    };
    let n = raw.len() as u64;
    assert!(!build(&keys, &inv, &tape, n + 1));
    assert!(!build(&keys, &inv, &tape[..tape.len() - 1], n));
    let mut bad = keys.clone();
    bad[0] = u64::MAX;
    assert!(!build(&bad, &inv, &tape, n));
    let mut bad = inv.clone();
    bad[0] = 0;
    assert!(!build(&keys, &bad, &tape, n));
    if m > 1 {
        bad = inv.clone();
        bad.swap(0, m - 1);
        assert!(!build(&keys, &bad, &tape, n));
        bad[0] = bad[1];
        assert!(!build(&keys, &bad, &tape, n));
    }
    assert!(view::RankView::build(&tape, &keys, &inv, n, 1, u64::MAX, &|| true).is_err());
    for m in [u32::MAX as u64, u32::MAX as u64 + 1, u32::MAX as u64 + 7] {
        assert_eq!(
            view::array_bound(111, m, 4).unwrap(),
            111 + 40 * m + 8 + 16 * m.div_ceil(64) + (12 << 20)
        );
    }
    assert!(view::array_bound(8, u64::MAX, 4).is_err());
    println!("RAW_CHECK aligned_and_lists={m} workers=1,2,4");
}
