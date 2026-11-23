//! In-memory fixture constructor, not a resource-bounded production path.
//! Corpus is used to find/deduplicate phrases. Emit uses only dictionary and parse data.
use std::{
    cmp::Reverse,
    collections::{BTreeMap, BinaryHeap},
};
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
#[path = "overlap_binned.rs"]
mod binned;
pub fn construct(bytes: &[u8], check: impl Fn(&str), mut emit: impl FnMut(u64, u64)) {
    let w = 8;
    let n = bytes.len();
    let mut s = vec![0usize];
    for (i, b) in bytes.windows(w).enumerate().skip(1) {
        if i % 1048576 == 0 {
            check("anchors");
        }
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
    if std::env::var_os("XSA_TEST_OVERLAP_BINNED").is_some() {
        return binned::run(bytes, w, s, &check, emit);
    }
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
    drop(t);
    let mut aligned = vec![0; m];
    for r in 1..m {
        let (i, j, z) = (ps[r - 1] as usize, ps[r] as usize, pl[r]);
        let (x, y) = (pa[i + z] as usize, pa[j + z] as usize);
        let prefix = dict[d[x]..d[x + 1] - 1]
            .iter()
            .zip(&dict[d[y]..d[y + 1] - 1])
            .take_while(|(a, b)| a == b)
            .count();
        aligned[r] = s[i + z] - s[i] + prefix;
    }
    drop(pl);
    let rmq = Min::new(aligned);
    let mut prank = vec![0; m];
    for (r, &p) in ps.iter().enumerate() {
        prank[p as usize] = r;
    }
    let (ds, dl) = order(&dict, 258);
    let symbols = dict.len();
    drop(dict);
    let mut loc = vec![0; k + 1];
    for v in 0..k {
        loc[v + 1] = loc[v] + freq[v];
    }
    let mut cur = loc.clone();
    let mut occ = vec![0; m];
    for j in std::iter::once(m - 1).chain(ps.iter().filter_map(|&x| (x as usize).checked_sub(1))) {
        let v = pa[j] as usize;
        occ[cur[v]] = j;
        cur[v] += 1;
    }
    drop(cur);
    drop(freq);
    drop(ps);
    drop(pa);
    let rank = |j: usize| if j + 1 == m { 0 } else { prank[j + 1] + 1 };
    emit(n as u64, 0);
    let mut rows = 0;
    let mut reps = 0;
    let mut head = 0;
    let mut heap = BinaryHeap::new();
    let mut flush =
        |queue: &mut BinaryHeap<Reverse<(usize, usize, usize, usize)>>, len: usize, head: usize| {
            let mut prior = None;
            while let Some(Reverse((r, idx, end, off))) = queue.pop() {
                let l = if let Some(prev) = prior {
                    assert!(prev > 0 && prev < r);
                    len - w + rmq.between(prev - 1, r - 1)
                } else {
                    head
                };
                emit((s[occ[idx]] + off) as u64, l as u64);
                rows += 1;
                prior = Some(r);
                if idx + 1 < end {
                    queue.push(Reverse((rank(occ[idx + 1]), idx + 1, end, off)));
                }
            }
        };
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
            flush(&mut heap, last_len, head);
            head = if last_len == 0 { 0 } else { gap };
            reps += 1;
        } else {
            assert_eq!(len, last_len, "proper prefix among representatives");
        }
        let idx = loc[v];
        heap.push(Reverse((rank(occ[idx]), idx, loc[v + 1], p - d[v])));
        last_len = len;
        gap = usize::MAX;
    }
    flush(&mut heap, last_len, head);
    assert_eq!(rows, n);
    eprintln!("SS_OVERLAP_STREAM bytes={n} phrases={m} distinct={k} dict_symbols={symbols} representatives={reps} tied_rows={} max_tile={}",n-reps,widths.iter().max().unwrap());
}
