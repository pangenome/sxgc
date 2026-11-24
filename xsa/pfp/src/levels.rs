// AMENDMENT R: THE LEVELS (built from the frozen L spec, not from the
// C++): levels are data, not modes. The census arithmetic decides BY
// NUMBER: M <= XSA_SA_CEILING (the default 2^31-1, the law) -> the
// 32-bit kernel at this level; M > the ceiling AND the next level
// condenses -> ONE MORE PARSE LEVEL + the induction upward; no
// condensation -> wide integer kernel (the residual - unintegrated
// so far: the census dies loudly with the numbers, never a silent
// fallback).
// Level k+1's corpus is level k's ID sequence stored BIG-ENDIAN, so
// the byte machinery's memcmp IS the ID-lexicographic order. The
// one-sided trigger parses every level uniformly (the same rule over
// the ID alphabet); the phrasebook is REFERENCES ONLY.
use crate::col::{die, ColWriter, ColMap};
use crate::laws::{env_u64, Laws};
use std::fs;
use std::io::Write as _;

// the deepest level's SA via the FFI kernel (32-bit; the sequential
// libsais_int - the certified END-FIRST semantics verified
// empirically at W2)
#[allow(dead_code)]
fn sa_kernel_int(t: &mut [i32], sa: &mut [i32], k: i32) -> i32 {
    let n = t.len() as i32;
    if n == 0 { return 0; }
    unsafe {
        libsais_int(t.as_mut_ptr(), sa.as_mut_ptr(), n, k, 0)
    }
}
extern "C" {
    fn libsais_int(t: *mut i32, sa: *mut i32, n: i32, k: i32,
                   fs: i32) -> i32;
}

pub struct Level2 {
    pub m2: u64,
    pub dict_n2: u64,
    pub dict_ref2: ColMap,  // u64: the firstocc x 4 (BE bytes)
    pub dict_len2: ColMap,  // u32: the phrase length x 4 (BE bytes)
    pub pa2: ColMap,        // u32: the level-2 id sequence
    pub pstart2: ColMap,    // u64: the ROW's own start x 4 (the tiling
                            // sum - NOT the firstocc; the repeats move
                            // rows away from their first occurrence)
    pub parse_sa2: ColMap,  // u32: the deepest level's SA
    pub be: ColMap,         // the BE text (u8 view)
    pub be_bytes: u64,
    pub prefix_rel2: u64,
}

// THE ONE-SIDED TRIGGER over the ID alphabet (uniformly: a k-mer is a
// trigger iff its key is STRICTLY smaller than the w k-mers preceding
// it at distance k..k+w-1, the window FULLY INSIDE the phrase,
// suppressed for the first k+w positions; phrases end with their
// trigger; the prefix-freeness proof carries verbatim)
pub fn parse_ids(pa: &[u32], k2: u64, w2: u64)
                 -> (Vec<(u64, u32)>, Vec<u32>, Vec<u64>) {
    // returns (dict: (firstocc, len) per distinct phrase, pa2, pstart2
    // as id-space starts)
    let m = pa.len() as u64;
    let mut phrases: Vec<(u64, u32)> = Vec::new();   // (firstocc, len) by id
    let mut dict_of: std::collections::HashMap<u64, u32> = std::collections::HashMap::new();
    let mut pa2: Vec<u32> = Vec::new();
    let mut pstart2: Vec<u64> = Vec::new();
    let mut phrase_start: u64 = 0;
    let key_at = |p: u64| -> u64 {
        (pa[p as usize] as u64) << 32 | pa[p as usize + 1] as u64
    };
    let mut fire = |end: u64,
                   phrase_start: &mut u64,
                   phrases: &mut Vec<(u64, u32)>,
                   dict_of: &mut std::collections::HashMap<u64, u32>,
                   pa2: &mut Vec<u32>,
                   pstart2: &mut Vec<u64>| {
        if end < *phrase_start { return; }
        let len = (end - *phrase_start + 1) as u32;
        // the FNV-1a content hash over the IDs (the dedupe key)
        let mut hh: u64 = 1469598103934665603;
        for b in *phrase_start..=end {
            hh ^= (pa[b as usize] as u64) << ((b & 3) * 8);
            hh = hh.wrapping_mul(1099511628211);
        }
        let id = match dict_of.get(&hh) {
            Some(&id) => {
                // verify on collision (length + content)
                if phrases[id as usize].1 != len {
                    die("LEVEL2_HASH: phrase length collision");
                }
                let a = phrases[id as usize].0;
                for q in 0..len as u64 {
                    if pa[(a + q) as usize] != pa[(*phrase_start + q) as usize] {
                        die("LEVEL2_HASH: phrase content collision");
                    }
                }
                id
            }
            None => {
                let id = phrases.len() as u32;
                dict_of.insert(hh, id);
                phrases.push((*phrase_start, len));
                id
            }
        };
        pa2.push(id);
        pstart2.push(*phrase_start);
        *phrase_start = end + 1;
    };
    let mut p = 0u64;
    while p + k2 <= m {
        if p >= phrase_start + w2 {
            let key = key_at(p);
            let mut trig = true;
            let back = w2.min(p - phrase_start);
            for j in 1..=back {
                if !(key < key_at(p - j)) { trig = false; break; }
            }
            if trig {
                fire(p + k2 - 1, &mut phrase_start, &mut phrases,
                     &mut dict_of, &mut pa2, &mut pstart2);
            }
        }
        p += 1;
    }
    if phrase_start < m {
        fire(m - 1, &mut phrase_start, &mut phrases, &mut dict_of,
             &mut pa2, &mut pstart2);
    }
    (phrases, pa2, pstart2)
}

// THE LEVEL-2 DICTIONARY SORT: the ID-ORDER LAW - the ids are
// assigned in ID-SEQUENCE lexicographic order (prefix sorts first);
// the level-2 SA is then the aligned order.
pub fn sort_level2_dict(pa: &[u32], phrases: &mut Vec<(u64, u32)>)
                        -> (Vec<u32>, u64) {
    let d2 = phrases.len() as u64;
    let mut order: Vec<u32> = (0..d2 as u32).collect();
    order.sort_unstable_by(|&a, &b| {
        let (fa, la) = phrases[a as usize];
        let (fb, lb) = phrases[b as usize];
        let c = la.min(lb) as usize;
        for q in 0..c {
            let va = pa[(fa + q as u64) as usize];
            let vb = pa[(fb + q as u64) as usize];
            if va != vb { return va.cmp(&vb); }
        }
        la.cmp(&lb)
    });
    let mut new_id = vec![0u32; d2 as usize];
    let mut fo_n = vec![0u64; d2 as usize];
    let mut ln_n = vec![0u32; d2 as usize];
    let mut rel2 = 0u64;
    for r in 0..d2 as usize {
        let old = order[r] as usize;
        new_id[old] = r as u32;
        fo_n[r] = phrases[old].0;
        ln_n[r] = phrases[old].1;
        if r + 1 < d2 as usize {
            let o2 = order[r + 1] as usize;
            let c = phrases[old].1.min(phrases[o2].1) as usize;
            let mut q = 0usize;
            while q < c
                && pa[(phrases[old].0 + q as u64) as usize]
                    == pa[(phrases[o2].0 + q as u64) as usize] { q += 1; }
            if q == c { rel2 += 1; }
        }
    }
    for i in 0..d2 as usize { phrases[i] = (fo_n[i], ln_n[i]); }
    (new_id, rel2)
}

pub fn build_level2(pa: &[u32], m0: u64, modeb: &str, threads: u64,
                    laws: &Laws) -> Option<Level2> {
    let ceiling = env_u64("XSA_SA_CEILING", 2147483647);
    if m0 <= ceiling { return None; }
    let t0 = std::time::Instant::now();
    let l2dir = format!("{}/L2", modeb);
    let _ = fs::create_dir_all(&l2dir);
    // (1) THE BIG-ENDIAN ID COLUMN: memcmp over it IS the ID order
    let be_path = format!("{}/pa_be.bin", modeb);
    let be_bytes = m0 * 4;
    {
        let mut w = ColWriter::open(&be_path);
        for i in 0..m0 as usize {
            w.put_u32(pa[i].swap_bytes());
        }
        w.close();
        eprintln!("SS_LEVEL_BE bytes={} rule=big_endian_id_column",
                  be_bytes);
    }
    let be = ColMap::map_ro(&be_path, be_bytes, 1);
    // (2) THE LEVEL-2 PARSE (the one-sided trigger, uniformly)
    let k2 = env_u64("XSA_LEVEL_K", 2).max(2);
    let w2 = env_u64("XSA_LEVEL_W", 8).max(1);
    let (mut phrases, mut pa2v, pstart2v) = parse_ids(pa, k2, w2);
    let m2 = pa2v.len() as u64;
    eprintln!("SS_LEVEL2_PARSE m1={} m2={} condensation={:.2}x \
               distinct2={} k={} w={} rule=the_one_sided_trigger_over_the_id_alphabet;references_only_no_expansion",
              m0, m2, m0 as f64 / m2.max(1) as f64, phrases.len(), k2, w2);
    laws.stop_check("levels");
    // THE CENSUS ARITHMETIC (by number): if the next level STILL
    // exceeds the ceiling, it must condense; if it does not
    // condense, the wide integer residual is required - NOT integrated:
    // die with the numbers, never a silent fallback
    if m2 > ceiling {
        if m2 * 2 > m0 {
            die(&format!("LEVELS_RESIDUAL: m2={} does not condense \
                          from m1={} against the ceiling {} - the \
                          phrase-incompressible residual requires \
                          an uncertified 64-bit integer kernel; the wide \
                          byte dictionary path does not handle this residual",
                         m2, m0, ceiling));
        }
        die(&format!("LEVELS_RECURSE: m2={} still exceeds the ceiling \
                     {} - a THIRD level is required; the Rust machine \
                     implements the two-level chain first (the census \
                     receipts decide)", m2, ceiling));
    }
    // (3) THE ID-ORDER LAW (the dict sort)
    let (new_id, rel2) = sort_level2_dict(pa, &mut phrases);
    for id in pa2v.iter_mut() { *id = new_id[*id as usize]; }
    eprintln!("SS_LEVEL2_DICT distinct={} prefix_relations={} \
               rule=id_order_law;prefix_sorts_first",
              phrases.len(), rel2);
    // (4) THE COLUMNS: the dict as BE-byte references; the ROW
    //     starts = the tiling sum x 4 (the row's OWN start)
    let ref2_p = format!("{}/dict_ref2.bin", l2dir);
    let len2_p = format!("{}/dict_len2.bin", l2dir);
    let pa2_p = format!("{}/pa2.bin", l2dir);
    let ps2_p = format!("{}/pstart2.bin", l2dir);
    {
        let mut w = ColWriter::open(&ref2_p);
        for v in 0..phrases.len() {
            w.put_u64(phrases[v].0 * 4);
        }
        w.close();
    }
    {
        let mut w = ColWriter::open(&len2_p);
        for v in 0..phrases.len() {
            w.put_u32(phrases[v].1 * 4);
        }
        w.close();
    }
    {
        let mut w = ColWriter::open(&pa2_p);
        for j in 0..m2 as usize { w.put_u32(pa2v[j]); }
        w.close();
    }
    {
        // the row starts: the cumulative tiling (the row's OWN start)
        let mut w = ColWriter::open(&ps2_p);
        let mut acc: u64 = 0;
        for j in 0..m2 as usize {
            w.put_u64(pstart2v[j] * 4);
            acc += phrases[pa2v[j] as usize].1 as u64;
        }
        w.close();
        if acc != m0 {
            die(&format!("LEVEL2_TILING: the phrases tile {} IDs \
                          against M={}", acc, m0));
        }
    }
    let dict_ref2 = ColMap::map_ro(&ref2_p, phrases.len() as u64, 8);
    let dict_len2 = ColMap::map_ro(&len2_p, phrases.len() as u64, 4);
    let pa2 = ColMap::map_ro(&pa2_p, m2, 4);
    let pstart2 = ColMap::map_ro(&ps2_p, m2, 8);
    let dict_n2 = phrases.len() as u64;
    // (5) THE DEEPEST LEVEL'S SA (the 32-bit FFI kernel; under a
    //     prefix-relation class the certified direct-walk fallback -
    //     the ground truth is the level-1 sequence itself)
    let sa2_p = format!("{}/parse_sa2.bin", l2dir);
    let mut parse_sa2 = ColMap::map_rw(&sa2_p, m2, 4);
    if rel2 == 0 {
        let mut ti: Vec<i32> = Vec::with_capacity(m2 as usize);
        for j in 0..m2 as usize { ti.push(pa2.u32s()[j] as i32); }
        // (libsais writes i32s; the column is u32s - reinterpret)
        let sa_slice = unsafe {
            std::slice::from_raw_parts_mut(
                parse_sa2.ptr as *mut i32, m2 as usize)
        };
        let rc = sa_kernel_int(&mut ti, sa_slice,
                               dict_n2.min(2147483647) as i32);
        if rc != 0 { die(&format!("LIBSAIS_INT_L2 rc={}", rc)); }
        eprintln!("SS_LEVEL2_SA rows={} kernel=libsais_int_ffi \
                   rule=the_deepest_level_kernel;the_census_arithmetic",
                  m2);
    } else {
        // the direct walk: the level-1 ID suffix compare (the ground
        // truth; the same circle-break as the byte slide)
        let ps = pstart2.u64s();
        let mut sa: Vec<u32> = (0..m2 as u32).collect();
        sa.sort_unstable_by(|&x, &y| {
            let mut a = ps[x as usize] >> 2;
            let mut b = ps[y as usize] >> 2;
            loop {
                if a >= m0 { return (b < m0).cmp(&false); }
                if b >= m0 { return false.cmp(&true); }
                let va = pa[a as usize];
                let vb = pa[b as usize];
                if va != vb { return va.cmp(&vb); }
                a += 1; b += 1;
            }
        });
        parse_sa2.u32s_mut().copy_from_slice(&sa);
        eprintln!("SS_LEVEL2_SA_FALLBACK rows={} prefix_relations={} \
                   rule=certified_direct_walk_over_the_level1_sequence",
                  m2, rel2);
    }
    // the permutation belt
    {
        let mut seen = vec![false; m2 as usize];
        for i in 0..m2 as usize {
            let v = parse_sa2.u32s()[i] as usize;
            if v >= m2 as usize || seen[v] {
                die(&format!("LIBSAIS_L2_PERM: row {} at rank {}",
                             v, i));
            }
            seen[v] = true;
        }
    }
    eprintln!("SS_LEVELS plan=2 m1={} m2={} ceiling={} levels_s={:.2} \
               rule=amendment_l_levels_are_data;the_induction_upward_through_the_phrasebook_chain",
              m0, m2, ceiling, crate::laws::now_s(t0));
    Some(Level2 { m2, dict_n2, dict_ref2, dict_len2, pa2, pstart2,
                  parse_sa2, be, be_bytes, prefix_rel2: rel2 })
}

// the raw FFI entry (the caller owns the buffers)
pub fn sa_kernel_int_raw(t: *mut i32, sa: &mut [i32], k: i32) -> i32 {
    let n = sa.len() as i32;
    if n == 0 { return 0; }
    unsafe { libsais_int(t, sa.as_mut_ptr(), n, k, 0) }
}
