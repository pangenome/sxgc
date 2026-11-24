#[path = "../src/overlap_wide_parse.rs"]
mod wide;
use wide::{plan, plcp_array_bytes, wide_parse_ranks, ParsePlcp, Workspace::*};
extern "C" {
    fn libsais_plcp_int(t: *const i32, sa: *const i32, pl: *mut i32, n: i32) -> i32;
    fn libsais_int(t: *mut i32, sa: *mut i32, n: i32, k: i32, fs: i32) -> i32;
}
fn narrow(ids: &[u32], k: u64) -> (Vec<i32>, Vec<i32>) {
    let mut t: Vec<_> = ids.iter().map(|&x| i32::try_from(x).unwrap()).collect();
    let fs = 6 * i32::try_from(k).unwrap();
    let mut sa = vec![0; t.len() + fs as usize];
    assert_eq!(
        unsafe {
            libsais_int(
                t.as_mut_ptr(),
                sa.as_mut_ptr(),
                t.len() as i32,
                k as i32,
                fs,
            )
        },
        0
    );
    let mut pl = vec![-1; t.len()];
    assert_eq!(
        unsafe { libsais_plcp_int(t.as_ptr(), sa.as_ptr(), pl.as_mut_ptr(), t.len() as i32) },
        0
    );
    sa.truncate(t.len());
    (sa, pl)
}
fn lcp<T: Eq>(a: &[T], b: &[T]) -> usize {
    a.iter().zip(b).take_while(|(x, y)| x == y).count()
}
fn compare(ids: &[u32], k: u64) {
    let mut naive: Vec<_> = (0..ids.len()).collect();
    naive.sort_by(|&a, &b| ids[a..].cmp(&ids[b..]));
    let (sa, narrow_pl) = narrow(ids, k);
    let mut expected = vec![0; ids.len()];
    for (r, &j) in naive.iter().enumerate() {
        assert_eq!(sa[r] as usize, j);
        expected[j] = (r + 1) as u32;
    }
    let pl = ParsePlcp::build(ids, expected.clone(), 12 * ids.len() as u64).unwrap();
    let mut naive_pl = vec![0; ids.len()];
    for w in naive.windows(2) {
        naive_pl[w[1]] = lcp(&ids[w[0]..], &ids[w[1]..]) as u32;
    }
    assert_eq!(pl.text_plcp(), naive_pl);
    assert!(pl.text_plcp().iter().map(|&z| z as i32).eq(narrow_pl));
    for ws in [SixK, FourK] {
        let grant = plan(ids.len() as u64, k, ws, u64::MAX)
            .unwrap()
            .peak_array_bytes;
        assert_eq!(wide_parse_ranks(ids, k, ws, grant).unwrap(), expected);
        assert!(wide_parse_ranks(ids, k, ws, grant - 1).is_err());
    }
}
#[test]
fn suffix_oracles() {
    compare(&[0], 1);
    compare(&[0; 201], 1);
    compare(&(0..1024).map(|x| x % 3).collect::<Vec<_>>(), 3);
    compare(&(0..700).rev().collect::<Vec<_>>(), 700);
    for n in 3..=7 {
        for mut code in 0..3usize.pow(n) {
            let ids: Vec<_> = (0..n)
                .map(|_| {
                    let x = code % 3;
                    code /= 3;
                    x as u32
                })
                .collect();
            compare(&ids, 3);
        }
    }
}
#[test]
fn arithmetic_only_widths() {
    let i = i32::MAX as u64;
    let u = u32::MAX as u64;
    for m in [i - 7, i - 6, i - 5, i, i + 1, u] {
        let p = plan(m, 1, SixK, u64::MAX).unwrap();
        assert_eq!(p.sa_slots as u64, m + 6);
    }
    // Same signed-representable n and alphabet: 4k fits the old extent, 6k does not.
    let m = 400_000_000;
    assert!(plan(m, m, FourK, u64::MAX).unwrap().sa_slots as u64 <= i);
    assert!(plan(m, m, SixK, u64::MAX).unwrap().sa_slots as u64 > i);
    let smaller = plan(m, m, FourK, u64::MAX).unwrap().peak_array_bytes;
    assert!(plan(m, m, SixK, smaller).is_err());
    assert!(plan(m, m, FourK, smaller).is_ok());
    let p = plan(u, u, FourK, u64::MAX).unwrap();
    assert_eq!(p.fs as u64, 4 * u);
    assert_eq!(p.peak_array_bytes, 52 * u);
    for (m, k) in [(0, 0), (1, 0), (1, 2), (u + 1, 1), (u64::MAX, u64::MAX)] {
        assert!(plan(m, k, SixK, u64::MAX).is_err());
    }
    assert!(wide_parse_ranks(&[0, u32::MAX], 2, SixK, u64::MAX).is_err());
}

#[test]
fn aligned_bytes_and_list_order() {
    // Each nonfinal occurrence owns three bytes and stores another eight. Final
    // occurrence owns its complete eleven bytes and contains a unique final byte.
    for m in [1, 2, 41, 235] {
        let mut text: Vec<_> = (0..3 * m + 8)
            .map(|i| {
                if i < 3 * m / 2 {
                    b'a'
                } else {
                    b'a' + (i % 7) as u8
                }
            })
            .collect();
        *text.last_mut().unwrap() = b'~';
        let starts: Vec<_> = (0..m).map(|i| i * 3).chain([text.len()]).collect();
        let words: Vec<_> = starts[..m].iter().map(|&p| &text[p..p + 11]).collect();
        let mut dict = words.clone();
        dict.sort();
        dict.dedup();
        let ids: Vec<_> = words
            .iter()
            .map(|w| dict.binary_search(w).unwrap() as u32)
            .collect();
        let k = dict.len() as u64;
        let ranks = wide_parse_ranks(
            &ids,
            k,
            FourK,
            plan(m as u64, k, FourK, u64::MAX).unwrap().peak_array_bytes,
        )
        .unwrap();
        let pl = ParsePlcp::build(&ids, ranks, 12 * m as u64).unwrap();
        let mut aligned = vec![u64::MAX; m];
        let word_lcp = |x: u32, y: u32| lcp(dict[x as usize], dict[y as usize]);
        pl.fill_aligned(
            &starts.iter().map(|&x| x as u64).collect::<Vec<_>>(),
            ids[m - 1],
            |x, y| word_lcp(x, y) as u64,
            &mut aligned,
        )
        .unwrap();
        let (sa, npl) = narrow(&ids, k);
        let mut raw: Vec<_> = (0..m).collect();
        raw.sort_by_key(|&j| &text[starts[j]..]);
        assert_eq!(aligned[0], 0);
        for (r, ((i, j, z), w)) in pl.predecessor_pairs().zip(raw.windows(2)).enumerate() {
            assert_eq!([i as usize, j as usize], [w[0], w[1]]);
            let (a, b) = (sa[r] as usize, sa[r + 1] as usize);
            let t = npl[b] as usize;
            assert_eq!(z as usize, t);
            assert_eq!(
                aligned[r + 1] as usize,
                starts[a + t] - starts[a] + word_lcp(ids[a + t], ids[b + t])
            );
            assert_eq!(
                aligned[r + 1] as usize,
                lcp(&text[starts[w[0]]..], &text[starts[w[1]]..])
            );
        }
        let order = pl.into_order();
        let mut naive: Vec<_> = (0..m).collect();
        naive.sort_by_key(|&j| &text[starts[j + 1]..]);
        assert!(order.continuations().map(|(j, _)| j as usize).eq(naive));
        assert!(order
            .continuations()
            .eq(std::iter::once((m as u32 - 1, 0)).chain(
                sa.iter()
                    .enumerate()
                    .filter(|(_, p)| **p > 0)
                    .map(|(r, &p)| (p as u32 - 1, r as u32 + 1))
            )));
    }
}

#[test]
fn plcp_refusals_and_wide_aligned_sum() {
    let u = u64::from(u32::MAX);
    assert_eq!(plcp_array_bytes(u - 1), Ok(12 * (u - 1)));
    assert!(plcp_array_bytes(u).is_err());
    for ranks in [vec![0, 1], vec![1, 1], vec![2, 1], vec![1], vec![1, 3]] {
        assert!(ParsePlcp::build(&[0, 1], ranks, 24).is_err());
    }
    assert!(ParsePlcp::build(&[0, 1], vec![1, 2], 23).is_err());
    let dup = ParsePlcp::build(&[1, 1], vec![2, 1], 24).unwrap();
    assert!(dup
        .fill_aligned(&[0, 1, 2], 1, |_, _| 0, &mut [0; 2])
        .is_err());
    // Equal token run with legal 31-bit per-word strides, accumulated >u32.
    let stride = 1u64 << 30;
    let ids = [0, 0, 0, 0, 0, 1];
    let pl = ParsePlcp::build(&ids, vec![1, 2, 3, 4, 5, 6], 72).unwrap();
    let mut out = [0; 6];
    let starts = (0..=6).map(|p| p * stride).collect::<Vec<_>>();
    pl.fill_aligned(&starts, 1, |_, _| 1, &mut out).unwrap();
    assert_eq!(out[1], 4 * stride + 1);
    assert!(pl
        .fill_aligned(&starts, 1, |_, _| u64::MAX, &mut out)
        .is_err());
}
