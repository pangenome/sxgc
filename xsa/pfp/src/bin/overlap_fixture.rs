//! Streaming-SA/LCP verifier: only the verifier collects the reference arrays.
#[path = "../overlap.rs"]
mod overlap;
use std::{fs, path::Path};
fn main() {
    let a: Vec<_> = std::env::args().collect();
    let t = fs::read(&a[1]).unwrap();
    let n = t.len();
    let mut sa = Vec::new();
    let mut band = 0;
    let mut rank = vec![usize::MAX; n + 1];
    while sa.len() < n + 1 {
        let b = fs::read(Path::new(&a[2]).join(format!("band_{band}.sa"))).unwrap();
        assert_eq!(b.len() % 8, 0);
        for x in b.chunks_exact(8) {
            let pos = (u64::from_le_bytes(x.try_into().unwrap()) & 0xffffffffffff) as usize;
            assert_eq!(rank[pos], usize::MAX);
            rank[pos] = sa.len();
            sa.push(pos);
        }
        band += 1;
    }
    assert_eq!(sa[0], n);
    let mut l = vec![0; n + 1];
    let mut h = 0;
    for i in 0..n {
        let r = rank[i];
        let j = sa[r - 1];
        while i + h < n && j + h < n && t[i + h] == t[j + h] {
            h += 1
        }
        l[r] = h;
        h = h.saturating_sub(1);
    }
    drop(rank);
    let mut r = 0;
    overlap::construct(
        &t,
        |_| {},
        |p, h| {
            assert_eq!((p, h), (sa[r] as u64, l[r] as u64), "row {r}");
            r += 1;
        },
    );
    assert_eq!(r, n + 1);
    println!("PASS all {r} streamed SA and LCP rows");
}
