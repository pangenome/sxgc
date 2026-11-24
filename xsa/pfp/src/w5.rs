// AMENDMENT R: THE LCP CARRY (W5, transliterated): the parse-LCP
// array (Kasai over the parse-SA) + the min segment tree. The
// aligned suffixes' byte-LCP L[i] = lcp(parseSA[i-1], parseSA[i])
// in O(corpus) amortized; the emit's walk bounds every row's
// byte-work by the phrase lengths and takes the continuation LCP
// from this array. Then THE OCCURRENCE LISTS: per phrase, its parse
// positions ordered by the CONTINUATION RANK parseRank[occ+1] (the
// END first) - one list per phrase, shared by all its offsets.
use crate::col::{die, ColWriter, ColMap};

pub struct W5 {
    pub lcp: ColMap,        // u64: L[1..m-1] (L[0] unused)
    pub lcp_seg_n: u64,
    pub lcp_tree: ColMap,   // u64: the iterative min segment tree
}

// the aligned byte-LCP of parse rows i, j from byte offset `from`
// (resolves the offset into (phrase, offset) by a binary search
// over the row starts; then compares phrase bytes + follows the
// parse - the C++'s alignedLcp, transliterated)
pub fn aligned_lcp(t: &[u8], pstart: &[u64], pa: &[u32],
                   dref: &[u64], dlen: &[u32],
                   m: u64, i: u64, j: u64, from: u64) -> u64 {
    let resolve = |row: u64, target: u64| -> (u64, u64) {
        let mut lo = row;
        let mut hi = m;
        while lo + 1 < hi {
            let mid = lo + (hi - lo) / 2;
            if pstart[mid as usize] <= target { lo = mid; } else { hi = mid; }
        }
        (lo, target - pstart[lo as usize])
    };
    let (mut pi, mut off) = resolve(i, pstart[i as usize] + from);
    let (mut pj, mut oj) = resolve(j, pstart[j as usize] + from);
    let mut lcp = from;
    loop {
        if pi >= m || pj >= m { return lcp; }   // a prefix
        let v1 = pa[pi as usize] as usize;
        let v2 = pa[pj as usize] as usize;
        let r1 = dlen[v1] as u64 - off;
        let r2 = dlen[v2] as u64 - oj;
        let m3 = r1.min(r2);
        if m3 > 0 {
            let a = (dref[v1] + off) as usize;
            let b = (dref[v2] + oj) as usize;
            if t[a..a + m3 as usize] != t[b..b + m3 as usize] {
                let mut k = 0usize;
                while t[a + k] == t[b + k] { k += 1; }
                return lcp + k as u64;
            }
        }
        lcp += m3;
        if r1 < r2 { pi += 1; off = 0; oj += m3; }
        else if r2 < r1 { pj += 1; oj = 0; off += m3; }
        else { pi += 1; pj += 1; off = 0; oj = 0; }
    }
}

pub fn build(t: &[u8], pa: &[u32], pstart: &[u64], sa: &[u32],
             dref: &[u64], dlen: &[u32], m: u64,
             modeb: &str) -> W5 {
    let t0 = std::time::Instant::now();
    let lcp_p = format!("{}/parse_lcp.bin", modeb);
    {
        let mut w = ColWriter::open(&lcp_p);
        for _ in 0..m { w.put_u64(0); }
        w.close();
    }
    let mut lcp = ColMap::map_rw(&lcp_p, m, 8);
    // KASAI over the parse (text order): h drops by the leading
    // phrase per step; the extensions compare phrase bytes linearly
    let mut rank = vec![0u32; m as usize];
    for i in 0..m as usize { rank[sa[i] as usize] = i as u32; }
    let mut h: u64 = 0;
    let mut lcp_max: u64 = 0;
    for i in 0..m as usize {
        if i > 0 {
            let drop = dlen[pa[i - 1] as usize] as u64;
            h = h.saturating_sub(drop);
        }
        let r = rank[i] as usize;
        if r == 0 { continue; }
        let j = sa[r - 1] as usize;
        let got = aligned_lcp(t, pstart, pa, dref, dlen,
                              m, i as u64, j as u64, h);
        lcp.u64s_mut()[r] = got;
        if got > lcp_max { lcp_max = got; }
        h = got;
    }
    let lcp_seg_n = if m > 1 { m - 1 } else { 1 };
    let tree_p = format!("{}/lcp_tree.bin", modeb);
    {
        let mut w = ColWriter::open(&tree_p);
        for _ in 0..2 * lcp_seg_n { w.put_u64(0); }
        w.close();
    }
    let mut tree = ColMap::map_rw(&tree_p, 2 * lcp_seg_n, 8);
    for k in 0..lcp_seg_n as usize {
        tree.u64s_mut()[lcp_seg_n as usize + k] = lcp.u64s()[k + 1];
    }
    let mut k = lcp_seg_n as usize;
    while k > 1 {
        for q in k / 2..k {
            tree.u64s_mut()[q] = tree.u64s()[2 * q].min(tree.u64s()[2 * q + 1]);
        }
        k /= 2;
    }
    eprintln!("SS_PARSE_LCP rows={} lcp_max={} kasai_s={:.2} \
               rule=amendment_w5;bounds_the_emit_walk",
              m, lcp_max, crate::laws::now_s(t0));
    W5 { lcp, lcp_seg_n, lcp_tree: tree }
}

// THE OCCURRENCE LISTS: per phrase, its parse positions ordered by
// the continuation rank (the END first)
pub fn occ_lists(pa: &[u32], rank: &[u32], m: u64, dict_n: u64,
                 modeb: &str) -> (ColMap, ColMap) {
    let t0 = std::time::Instant::now();
    let ostart_p = format!("{}/occ_start.bin", modeb);
    let olist_p = format!("{}/occ_list.bin", modeb);
    {
        let mut freq = vec![0u32; dict_n as usize];
        for i in 0..m as usize { freq[pa[i] as usize] += 1; }
        let mut w = ColWriter::open(&ostart_p);
        let mut tot = 0u64;
        for v in 0..dict_n as usize {
            w.put_u32(tot as u32);
            tot += freq[v] as u64;
        }
        w.put_u32(tot as u32);
        w.close();
        let ostart: Vec<u32> = {
            let mut acc = 0u32;
            let mut v2 = Vec::with_capacity(dict_n as usize + 1);
            for v in 0..dict_n as usize {
                v2.push(acc);
                acc += freq[v];
            }
            v2.push(acc);
            v2
        };
        let mut olist = vec![0u32; tot as usize];
        let mut cur = ostart.clone();
        for i in 0..m as usize {
            let v = pa[i] as usize;
            olist[cur[v] as usize] = i as u32;
            cur[v] += 1;
        }
        // sort each phrase's list by the continuation rank
        // (disjoint ranges: the per-phrase slices are independent)
        use rayon::prelude::*;
        use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};
        let next = AtomicUsize::new(0);
        let ol: &[AtomicU32] = unsafe {
            std::slice::from_raw_parts(
                olist.as_ptr() as *const AtomicU32, olist.len())
        };
        // (the per-phrase ranges are disjoint; the atomic view is only
        //  for the borrow checker - the writes never overlap)
        (0..dict_n as usize).into_par_iter().for_each(|_v| {
            let v = next.fetch_add(1, Ordering::Relaxed);
            let a = ostart[v] as usize;
            let b = ostart[v + 1] as usize;
            if b - a > 1 {
                let mut ids: Vec<u32> =
                    (0..b - a).map(|q| unsafe {
                        ol[a + q].load(Ordering::Relaxed)
                    }).collect();
                ids.sort_unstable_by_key(|&i2| {
                    if (i2 as u64) + 1 < m {
                        rank[i2 as usize + 1] + 1
                    } else { 0 }
                });
                for (q, id) in ids.iter().enumerate() {
                    unsafe {
                        ol[a + q].store(*id, Ordering::Relaxed);
                    }
                }
            }
        });
        let mut w = ColWriter::open(&olist_p);
        for v in 0..olist.len() { w.put_u32(olist[v]); }
        w.close();
    }
    let ostart = ColMap::map_ro(&ostart_p, dict_n + 1, 4);
    let olist = ColMap::map_ro(&olist_p, m, 4);
    eprintln!("SS_OCC_LISTS phrases={} occurrences={} lists_s={:.2} \
               rule=keyed_by_the_continuation_rank;one_list_per_phrase",
              dict_n, m, crate::laws::now_s(t0));
    (ostart, olist)
}
