#[path = "../src/overlap_wide_parse.rs"]
mod wide;
use wide::{plan, wide_parse_ranks, Workspace::*};
extern "C" {
    fn libsais_int(t: *mut i32, sa: *mut i32, n: i32, k: i32, fs: i32) -> i32;
}
fn compare(ids: &[u32], k: u64) {
    let mut naive: Vec<_> = (0..ids.len()).collect();
    naive.sort_by(|&a, &b| ids[a..].cmp(&ids[b..]));
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
    let mut expected = vec![0; ids.len()];
    for (r, &j) in naive.iter().enumerate() {
        assert_eq!(sa[r] as usize, j);
        expected[j] = (r + 1) as u32;
    }
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
