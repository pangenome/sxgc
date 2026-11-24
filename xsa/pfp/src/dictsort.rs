// AMENDMENT R: THE DICT SORT (W1+W7 transliterated): the occurrence
// sink distributes to the equalized 2-byte-prefix cell buckets
// (never bare first-byte); per bucket: read, sort by (content bytes,
// len, pos) - the pos tie-break makes the keys UNIQUE (the sort
// kernel's instability is legal) - dedupe: the equal runs' size IS
// the census count, the run's min pos IS the first-occurrence
// reference. Buckets commit IN ORDER so the ids are BYTE ORDER.
// Then THE PA: one re-read of the sink; per occurrence, binary
// search the byte-order dictionary (the ids are the sorted
// references) - the sink is position-ordered, so the ids stream
// straight into the mapped column.
use std::fs;
use std::io::Write as _;
use std::io::{Read, Write};
use crate::col::{die, ColWriter, ColMap};
use crate::laws::Laws;
use rayon::prelude::*;

pub struct DictOut {
    pub dict_n: u64,
    pub dict_bytes_total: u64,
    pub census_total: u64,
    pub refpos: ColMap,   // u64: the first-occurrence reference
    pub len: ColMap,      // u32
    pub count: ColMap,    // u32
    pub pa: ColMap,       // u32: the id sequence, parse order
    pub m0: u64,
    pub prefix_relations: u64,
}

pub fn cell_of(t: &[u8], pos: u64, plen: u32) -> usize {
    let a = t[pos as usize] as usize;
    let b = if plen > 1 { t[pos as usize + 1] as usize } else { 0 };
    (a << 8) | b
}

pub fn dictsort(t: &[u8], m0: u64, occ_path: &str, hist2: &[u64],
                modeb: &str, workspace: u64, threads: u64,
                laws: &Laws) -> DictOut {
    let t0 = std::time::Instant::now();
    // ---- the buckets (the equalized cell ranges)
    let entry_bytes = 12u64;
    let target = (workspace / 64 / entry_bytes).max(1024);
    let mut cell_bucket = [0u32; 65536];
    let mut bk = 0u32;
    {
        let mut run2 = 0u64;
        for cell in 0..65536usize {
            run2 += hist2[cell];
            cell_bucket[cell] = bk;
            if run2 >= target { run2 = 0; bk += 1; }
        }
        bk += 1;
        eprintln!("SS_KEY_BUCKETS target_per_bucket={} buckets={} \
                   rule=equalized_2byte_prefix_ranges;never_bare_first_byte",
                  target, bk);
    }
    // ---- the distribute: the sink -> the bucket files
    let buckets_dir = format!("{}/kbuckets", modeb);
    let _ = fs::create_dir_all(&buckets_dir);
    {
        let mut fin = fs::File::open(occ_path)
            .unwrap_or_else(|_| die("reopen the occurrence sink"));
        let mut bfs = Vec::with_capacity(bk as usize);
        for i in 0..bk {
            let p = format!("{}/bucket_{}.bin", buckets_dir, i);
            let f = fs::File::create(&p).unwrap_or_else(|_| die("open bucket"));
            bfs.push(std::io::BufWriter::with_capacity(1 << 18, f));
        }
        let mut rec = [0u8; 12];
        loop {
            match fin.read_exact(&mut rec) {
                Ok(()) => {}
                Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
                Err(_) => die("truncated sink"),
            }
            let pos = u64::from_le_bytes(rec[0..8].try_into().unwrap());
            let plen = u32::from_le_bytes(rec[8..12].try_into().unwrap());
            let b = cell_bucket[cell_of(t, pos, plen)] as usize;
            bfs[b].write_all(&rec).unwrap();
        }
    }
    // ---- the per-bucket sort + dedupe + the IN-ORDER commit
    let refpos_p = format!("{}/dict_refpos.bin", modeb);
    let len_p = format!("{}/dict_len.bin", modeb);
    let cnt_p = format!("{}/dict_count.bin", modeb);
    let mut dw_pos = ColWriter::open(&refpos_p);
    let mut dw_len = ColWriter::open(&len_p);
    let mut dw_cnt = ColWriter::open(&cnt_p);
    struct Buck { ded: Vec<(u64, u32)>, cnt: Vec<u32> }
    let results: Vec<Buck> = (0..bk as usize).into_par_iter()
        .map(|i| {
            laws.stop_check("dictsort");
            let p = format!("{}/bucket_{}.bin", buckets_dir, i);
            let mut f = fs::File::open(&p)
                .unwrap_or_else(|_| die("open bucket for sort"));
            let meta = f.metadata().unwrap_or_else(|_| die("stat bucket"));
            let n = meta.len() / 12;
            let mut raw = Vec::with_capacity(n as usize * 12);
            f.read_to_end(&mut raw).unwrap_or_else(|_| die("read bucket"));
            let _ = fs::remove_file(&p);
            let mut ent: Vec<(u64, u32)> = Vec::with_capacity(n as usize);
            for r in 0..n as usize {
                let pos = u64::from_le_bytes(
                    raw[r * 12..r * 12 + 8].try_into().unwrap());
                let len = u32::from_le_bytes(
                    raw[r * 12 + 8..r * 12 + 12].try_into().unwrap());
                ent.push((pos, len));
            }
            drop(raw);
            // the keys are UNIQUE by the pos tie-break
            ent.sort_unstable_by(|&(pa, la), &(pb, lb)| {
                let m = la.min(lb) as usize;
                let c = t[pa as usize..pa as usize + m]
                    .cmp(&t[pb as usize..pb as usize + m]);
                if c != std::cmp::Ordering::Equal { return c; }
                la.cmp(&lb).then(pa.cmp(&pb))
            });
            // dedupe: the equal runs; the size IS the count, the min
            // pos IS the first-occurrence reference
            let mut ded = Vec::new();
            let mut cnt = Vec::new();
            let mut o = 0usize;
            while o < ent.len() {
                let mut e2 = o + 1;
                let mut first = ent[o].0;
                while e2 < ent.len() {
                    let (a, la) = ent[o];
                    let (b, lb) = ent[e2];
                    let m = la.min(lb) as usize;
                    if t[a as usize..a as usize + m]
                        != t[b as usize..b as usize + m] { break; }
                    if la != lb { break; }
                    if b < first { first = b; }
                    e2 += 1;
                }
                ded.push((first, ent[o].1));
                cnt.push((e2 - o) as u32);
                o = e2;
            }
            Buck { ded, cnt }
        })
        .collect();
    let _ = fs::remove_dir_all(&buckets_dir);
    let mut dict_n = 0u64;
    let mut dict_bytes_total = 0u64;
    let mut census_total = 0u64;
    for b in &results {
        for z in 0..b.ded.len() {
            dw_pos.put_u64(b.ded[z].0);
            dw_len.put_u32(b.ded[z].1);
            dw_cnt.put_u32(b.cnt[z]);
            dict_bytes_total += b.ded[z].1 as u64;
            census_total += b.cnt[z] as u64;
            dict_n += 1;
        }
    }
    dw_pos.close(); dw_len.close(); dw_cnt.close();
    if census_total != m0 {
        die(&format!("CENSUS_TOTAL: {} against {}", census_total, m0));
    }
    eprintln!("SS_KEY_SORT distinct={} census={} dict_bytes={} sort_s={:.2} \
               kernel=rayon_par_sort_unique_keys_pos_tiebreak \
               rule=byte_order_ids;references_only;dedupe_byproduct;census_byproduct",
              dict_n, census_total, dict_bytes_total,
              crate::laws::now_s(t0));

    // ---- the prefix relations (the W2 detection; the ambiguity
    //      oracle's precondition, journaled)
    let refpos = ColMap::map_ro(&refpos_p, dict_n, 8);
    let len = ColMap::map_ro(&len_p, dict_n, 4);
    let count = ColMap::map_ro(&cnt_p, dict_n, 4);
    let mut prefix_relations = 0u64;
    for i in 1..dict_n as usize {
        let (a, la) = (refpos.u64s()[i - 1], len.u32s()[i - 1] as usize);
        let (b, lb) = (refpos.u64s()[i], len.u32s()[i] as usize);
        if la < lb && t[a as usize..a as usize + la]
            == t[b as usize..b as usize + la] { prefix_relations += 1; }
    }
    eprintln!("SS_GPARSE_DICT distinct={} dict_bytes={} prefix_relations={} \
               max_prefix_run=0 rule=byte_order_ids_from_the_key_sort",
              dict_n, dict_bytes_total, prefix_relations);

    // ---- THE PA: the sink re-read + the binary-search annotation
    let tp0 = std::time::Instant::now();
    let pa_p = format!("{}/pa.bin", modeb);
    {
        let mut pa = ColMap::map_rw(&pa_p, m0, 4);
        let rp = refpos.u64s();
        let ln = len.u32s();
        let mut fin = fs::File::open(occ_path)
            .unwrap_or_else(|_| die("reopen sink for PA"));
        let mut rec = [0u8; 12];
        let mut ann = 0u64;
        let mut prev_pos = 0u64;
        while ann < m0 {
            fin.read_exact(&mut rec)
                .unwrap_or_else(|_| die("truncated sink (PA)"));
            let pos = u64::from_le_bytes(rec[0..8].try_into().unwrap());
            let l = u32::from_le_bytes(rec[8..12].try_into().unwrap()) as usize;
            if ann > 0 && pos <= prev_pos {
                die("ID_ORDER: the sink is not position-ordered");
            }
            prev_pos = pos;
            // the id: binary search the byte-order ids
            let key_cmp = |id: usize| -> std::cmp::Ordering {
                let dl = ln[id] as usize;
                let m = l.min(dl);
                let c = t[pos as usize..pos as usize + m]
                    .cmp(&t[rp[id] as usize..rp[id] as usize + m]);
                if c != std::cmp::Ordering::Equal { return c; }
                l.cmp(&dl)
            };
            let mut lo = 0usize;
            let mut hi = dict_n as usize;
            while lo < hi {
                let mid = lo + (hi - lo) / 2;
                if key_cmp(mid) == std::cmp::Ordering::Less { hi = mid; }
                else { lo = mid + 1; }
            }
            if lo == 0 || key_cmp(lo - 1) != std::cmp::Ordering::Equal {
                die(&format!("ID_LOOKUP: the occurrence at pos {} len {} has \
                              no dictionary entry - the key sort lost a phrase",
                             pos, l));
            }
            pa.u32s_mut()[ann as usize] = (lo - 1) as u32;
            ann += 1;
        }
        let _ = fs::remove_file(occ_path);
        if ann != m0 {
            die(&format!("PA_ANNOTATE: {} against {} rows", ann, m0));
        }
    }
    let pa = ColMap::map_ro(&pa_p, m0, 4);
    eprintln!("SS_PARSE_IDS phrases={} ids_s={:.2} \
               rule=byte_order_id_annotation;positions_are_prefix_sums",
              m0, crate::laws::now_s(tp0));
    DictOut { dict_n, dict_bytes_total, census_total, refpos, len, count,
              pa, m0, prefix_relations }
}
