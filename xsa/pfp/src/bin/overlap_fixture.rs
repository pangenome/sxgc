//! In-memory fixture constructor, not a resource-bounded production path.
//! Corpus is used to find/deduplicate phrases. Emit uses only dictionary and parse data.
use std::{collections::BTreeMap, fs, path::Path};
#[link(name = "libsais_r", kind = "static")]
extern "C" {
    fn libsais_int(t: *mut i32, sa: *mut i32, n: i32, k: i32, fs: i32) -> i32;
}
fn order(t: &[i32], k: i32) -> (Vec<i32>, Vec<usize>) {
    let mut tmp = t.to_vec();
    let mut sa = vec![0; t.len()];
    let n = i32::try_from(t.len()).unwrap();
    assert_eq!(
        unsafe { libsais_int(tmp.as_mut_ptr(), sa.as_mut_ptr(), n, k, 0) },
        0
    );
    let mut rank = vec![0; t.len()];
    for (r, &i) in sa.iter().enumerate() {
        rank[i as usize] = r;
    }
    let mut l = vec![0; t.len()];
    let mut h = 0;
    for i in 0..t.len() {
        let r = rank[i];
        if r == 0 {
            h = 0;
            continue;
        }
        let j = sa[r - 1] as usize;
        while i + h < t.len() && j + h < t.len() && t[i + h] == t[j + h] {
            h += 1
        }
        l[r] = h;
        h = h.saturating_sub(1);
    }
    (sa, l)
}
struct Min(Vec<usize>, usize);
impl Min {
    fn new(x: Vec<usize>) -> Self {
        let n = x.len();
        let mut a = vec![0; 2 * n];
        a[n..].copy_from_slice(&x);
        for i in (1..n).rev() {
            a[i] = a[2 * i].min(a[2 * i + 1]);
        }
        Self(a, n)
    }
    fn between(&self, x: usize, y: usize) -> usize {
        assert!(x < y);
        let (mut l, mut r) = (self.1 + x + 1, self.1 + y + 1);
        let mut v = usize::MAX;
        while l < r {
            if l % 2 == 1 {
                v = v.min(self.0[l]);
                l += 1
            }
            if r % 2 == 1 {
                r -= 1;
                v = v.min(self.0[r]);
            }
            l /= 2;
            r /= 2;
        }
        v
    }
}
fn construct(bytes: &[u8]) -> (Vec<u32>, Vec<u32>) {
    let w = 8;
    let n = bytes.len();
    let mut s = vec![0usize];
    for (i, b) in bytes.windows(w).enumerate().skip(1) {
        let mut h = 1469598103934665603u64;
        for &c in b {
            h = (h ^ c as u64).wrapping_mul(1099511628211);
        }
        if h % 31 == 0 {
            s.push(i)
        }
    }
    let m = s.len();
    s.push(n);
    let mut t: Vec<_> = bytes.iter().map(|&c| i32::from(c) + 2).collect();
    t.push(1);
    let phrase = |i: usize| &t[s[i]..if i + 1 == m { n + 1 } else { s[i + 1] + w }];
    let mut words = BTreeMap::new();
    for i in 0..m {
        words.insert(phrase(i), 0i32);
    }
    for (i, id) in words.values_mut().enumerate() {
        *id = i as i32;
    }
    let pa: Vec<_> = (0..m).map(|i| words[phrase(i)]).collect();
    let k = words.len();
    let mut freq = vec![0usize; k];
    for &v in &pa {
        freq[v as usize] += 1;
    }
    let mut dict = vec![];
    let mut d = vec![];
    let mut widths = vec![];
    let mut owner = vec![];
    for (&word, &id) in &words {
        d.push(dict.len());
        let width = word.len() - if word.last() == Some(&1) { 1 } else { w };
        widths.push(width);
        dict.extend_from_slice(word);
        dict.push(0);
        owner.resize(dict.len(), id);
    }
    d.push(dict.len());
    drop(words);
    let (ps, pl) = order(&pa, k as i32);
    let rmq = Min::new(pl);
    let mut prank = vec![0; m];
    for (r, &p) in ps.iter().enumerate() {
        prank[p as usize] = r;
    }
    let (ds, dl) = order(&dict, 258);
    let mut groups = vec![u32::MAX; dict.len()];
    let mut counts = vec![];
    let mut head_lcp = vec![];
    let mut lens = vec![];
    let mut last_len = 0;
    let mut gap = usize::MAX;
    for (r, &p) in ds.iter().enumerate() {
        gap = gap.min(dl[r]);
        let p = p as usize;
        let v = owner[p] as usize;
        if p - d[v] >= widths[v] {
            continue;
        }
        let len = d[v + 1] - 1 - p;
        if last_len == 0 || gap < len.min(last_len) {
            head_lcp.push(if last_len == 0 { 0 } else { gap });
            counts.push(0);
            lens.push(len);
        } else {
            assert_eq!(len, last_len, "proper prefix among representatives");
        }
        let g = counts.len() - 1;
        groups[p] = g as u32;
        counts[g] += freq[v];
        last_len = len;
        gap = usize::MAX;
    }
    drop(ds);
    drop(dl);
    drop(owner);
    assert_eq!(counts.iter().sum::<usize>(), n);
    let mut base = vec![];
    let mut at = 1;
    for &c in &counts {
        base.push(at);
        at += c;
    }
    let mut next = base.clone();
    let mut prior = vec![usize::MAX; counts.len()];
    let mut out = vec![0u32; n + 1];
    out[0] = n as u32;
    let mut lcp = vec![0u32; n + 1];
    // Visit occurrences by *next* parse-suffix rank. Stable integer bucket placement.
    for j in std::iter::once(m - 1).chain(ps.iter().filter_map(|&x| (x as usize).checked_sub(1))) {
        let v = pa[j] as usize;
        for off in 0..widths[v] {
            let g = groups[d[v] + off] as usize;
            let idx = next[g];
            next[g] += 1;
            out[idx] = (s[j] + off) as u32;
            let h = if idx == base[g] {
                head_lcp[g]
            } else {
                let (i, b) = (prior[g], j + 1);
                assert!(i < m && b < m);
                let z = rmq.between(prank[i], prank[b]);
                let (x, y) = (pa[i + z] as usize, pa[b + z] as usize);
                let prefix = dict[d[x]..d[x + 1] - 1]
                    .iter()
                    .zip(&dict[d[y]..d[y + 1] - 1])
                    .take_while(|(a, b)| a == b)
                    .count();
                lens[g] - w + s[i + z] - s[i] + prefix
            };
            lcp[idx] = h as u32;
            prior[g] = j + 1;
        }
    }
    for g in 0..counts.len() {
        assert_eq!(next[g], base[g] + counts[g]);
    }
    println!("bytes={n} phrases={m} distinct={k} dict_symbols={} representatives={} tied_rows={} max_tile={}",dict.len(),counts.len(),n-counts.len(),widths.iter().max().unwrap());
    (out, lcp)
}
fn main() {
    let a: Vec<_> = std::env::args().collect();
    let t = fs::read(&a[1]).unwrap();
    assert!(t.len() < i32::MAX as usize);
    let (sa, lcp) = construct(&t);
    // Verification only: banked bands are not supplied to construct(). Independently
    // derive the LCP oracle from the certified SA + corpus by the text-order Kasai walk.
    let mut rank = vec![usize::MAX; t.len() + 1];
    let mut row = 0;
    let mut band = 0;
    while row < sa.len() {
        let b = fs::read(Path::new(&a[2]).join(format!("band_{band}.sa"))).unwrap();
        assert_eq!(b.len() % 8, 0);
        for x in b.chunks_exact(8) {
            let p = (u64::from_le_bytes(x.try_into().unwrap()) & 0xffffffffffff) as usize;
            assert_eq!(p, sa[row] as usize, "SA row {row}");
            assert_eq!(rank[p], usize::MAX);
            rank[p] = row;
            row += 1;
        }
        band += 1;
    }
    let mut h = 0;
    for i in 0..t.len() {
        let r = rank[i];
        let j = sa[r - 1] as usize;
        while i + h < t.len() && j + h < t.len() && t[i + h] == t[j + h] {
            h += 1;
        }
        assert_eq!(lcp[r] as usize, h, "LCP row {r}");
        h = h.saturating_sub(1);
    }
    assert_eq!(lcp[0], 0);
    println!(
        "PASS all {row} SA positions and adjacent LCP values, including END; no landing search"
    );
}
