#[path = "../src/overlap_lex_direct.rs"]
mod direct;
#[path = "../src/overlap_lexblocks.rs"]
mod restart;
use direct::LexDirect;

type Owned = (Vec<u8>, bool);
fn check(mut input: Vec<Owned>) -> (usize, usize) {
    input.sort();
    let w: Vec<_> = input.iter().map(|(w, e)| (w.as_slice(), *e)).collect();
    let a = LexDirect::fixture(&w).unwrap();
    let reference = restart::LexBlocks::fixture(&w, 16).unwrap();
    let mut max_hops = 0;
    let mut lit = 0;
    let mut payload = 0;
    for group in w.chunks(16) {
        let mut max_p = 0;
        let mut e = 0;
        for (j, (s, _)) in group.iter().enumerate() {
            let p = if j == 0 {
                0
            } else {
                (0..group[j - 1].0.len().min(s.len()))
                    .find(|&k| s[k] != group[j - 1].0[k])
                    .unwrap_or(s.len().min(group[j - 1].0.len()))
            };
            max_p = max_p.max(p);
            e += s.len() - p;
        }
        let width = |x| {
            [0xff, 0xffff, 0xffffff, 0xffffffffusize]
                .iter()
                .position(|&end| x <= end)
                .unwrap()
                + 1
        };
        lit += e;
        payload += 1 + (group.len() - 1) * width(max_p) + group.len() * width(e) + e;
    }
    assert_eq!(a.bytes(), payload + 16 + 8 * (1 + w.len().div_ceil(16)));
    assert!(lit <= reference.payload() && a.bytes() >= reference.directory());
    for (id, &(s, end)) in w.iter().enumerate() {
        let v = a.word(id).unwrap();
        let r = reference.word(id).unwrap();
        assert_eq!(v.owned(), r.owned());
        for (pos, &b) in s.iter().enumerate() {
            let (value, h) = v.traced(pos);
            assert_eq!(value, Some(1 + u16::from(b)));
            assert!(h <= id % 16);
            max_hops = max_hops.max(h);
            assert_eq!(value, r.symbol(pos));
        }
        assert_eq!(v.symbol(s.len()), end.then_some(0));
        assert_eq!(v.symbol(usize::MAX), None);
    }
    assert!(a.word(w.len()).is_err());
    (max_hops, lit)
}
#[test]
fn duplicates_end_prefixes_and_partial_blocks() {
    check(vec![(vec![0], true)]);
    let mut w = vec![(vec![255; 9], true), (vec![255; 9], false)];
    for i in 0u8..32 {
        let mut s = vec![0, 255, 1, 2, 3, 4, 5, 6, 7];
        s.extend([i, 0]);
        w.push((s, false));
    }
    assert_eq!(check(w).0, 15);
}
#[test]
fn width_boundaries_and_long_prefix() {
    for length in [254, 255, 256, 65535, 65536] {
        let s = vec![0xff; length];
        let mut w = vec![(vec![0], true), (s.clone(), false)];
        for i in 0..17 {
            let mut v = s.clone();
            v.push(i);
            w.push((v, false));
        }
        // Slot 0 is the unrelated short EOF; the long-prefix ancestor is slot 1.
        assert_eq!(check(w).0, 14);
    }
}
#[test]
fn disjoint_first_bytes() {
    let list = (0..=255)
        .map(|i| (vec![i; 9 + i as usize], i == 200))
        .collect();
    assert_eq!(check(list).0, 0);
}
#[test]
fn refusals() {
    let eof = (b"x".as_slice(), true);
    let x = (b"aaaaaaaaa".as_slice(), false);
    let long = vec![0; 1 + (1 << 20)];
    for w in [
        vec![],
        vec![x],
        vec![eof, eof],
        vec![eof, x],
        vec![x, x, eof],
        vec![(b"".as_slice(), true)],
        vec![(b"12345678".as_slice(), false), eof],
        vec![(long.as_slice(), true)],
        vec![(b"a".as_slice(), true), eof],
    ] {
        assert!(LexDirect::fixture(&w).is_err());
    }
}
