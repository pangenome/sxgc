// AMENDMENT R: THE PARSE (STROKE P, transliterated exactly from the
// C++): the streaming one-sided trigger walk. A start s is a trigger
// iff km(s) (the FNV-1a hash of the k bytes) is STRICTLY smaller than
// the w k-mers preceding it at distance k..k+w-1 (the window FULLY
// INSIDE the phrase), suppressed for the first k+w positions of a
// phrase; phrases end with their trigger k-mer; the 65535 cap forces
// splits; the final phrase runs to the corpus end. The phrasebook is
// REFERENCES ONLY ((firstPos, len)); the occurrence sink is a disc
// stream; the 2-byte cell histogram rides the walk.
pub struct ParseOut {
    pub m0: u64,                    // the parse rows
    pub occ_path: String,           // the (pos:u64, len:u32) sink, parse order
    pub hist2: Vec<u64>,            // the 65536 cell histogram
    pub forced_splits: u64,
    pub parse_bytes: u64,
}

const K: u64 = 8;    // XSA_PFP_K
const W: u64 = 8;    // XSA_PFP_W
const SCAN_CHUNK: u64 = 64 << 20;

fn km_at(buf: &[u8], o: usize) -> u64 {
    let mut h: u64 = 1469598103934665603;
    for i in 0..K as usize {
        h ^= buf[o + i] as u64;
        h = h.wrapping_mul(1099511628211);
    }
    h
}

use std::io::Write as _;

pub fn parse(corpus: &std::fs::File, file_n: u64, modeb: &str,
             laws: &super::laws::Laws) -> ParseOut {
    let t0 = std::time::Instant::now();
    let occ_path = format!("{}/occurrences.bin", modeb);
    let mut sink = std::io::BufWriter::with_capacity(
        1 << 20, std::fs::File::create(&occ_path)
            .unwrap_or_else(|_| super::col::die("open the occurrence sink")));
    let mut hist2 = vec![0u64; 65536];
    let mut m0: u64 = 0;
    let mut forced = 0u64;

    let _ = corpus;
    let mmap = unsafe { memmap2::Mmap::map(corpus)
        .unwrap_or_else(|_| super::col::die("mmap corpus")) };
    let t: &[u8] = &mmap[..];

    let mut prev_start: u64 = 0;
    let mut parse_bytes: u64 = 0;
    // (the closure borrows are painful; do it inline instead)
    let ov = W + K;
    let mut base: u64 = 0;
    while base < file_n {
        laws.stop_check("parse");
        let chunk = SCAN_CHUNK.min(file_n - base);
        let from = base.saturating_sub(ov);
        let want = (chunk + ov * 2).min(file_n - from);
        parse_bytes += want;
        let km = |p: u64| -> u64 { km_at(t, p as usize) };
        let hi = base + chunk;
        let mut p = base;
        while p < hi {
            // THE TRIGGER (one-sided; the window FULLY INSIDE the phrase)
            let mut trig = false;
            if p + K <= file_n && p >= prev_start + K + W {
                let hp = km(p);
                let mut ok = true;
                let mut j = K;
                while j < K + W && ok {
                    if p >= prev_start + j && km(p - j) <= hp { ok = false; }
                    j += 1;
                }
                trig = ok;
            }
            let is_forced = p > prev_start && p - prev_start >= 65535;
            if trig || is_forced {
                let end_pos = if trig { p + K } else { p };
                if end_pos > prev_start {
                    if is_forced && !trig { forced += 1; }
                    // close_phrase inline
                    let plen = (end_pos - prev_start) as u32;
                    let (q0, q1): (u8, u8) =
                        (t[prev_start as usize],
                         if plen > 1 { t[prev_start as usize + 1] } else { 0 });
                    sink.write_all(&prev_start.to_le_bytes())
                        .unwrap_or_else(|_| super::col::die("sink write"));
                    sink.write_all(&plen.to_le_bytes())
                        .unwrap_or_else(|_| super::col::die("sink write"));
                    hist2[(((q0 as usize) << 8)
                        | (if plen > 1 { q1 as usize } else { 0 }))] += 1;
                    m0 += 1;
                    prev_start = end_pos;
                }
            }
            p += 1;
        }
        base += chunk;
    }
    // the final phrase
    if file_n > prev_start {
        let plen = (file_n - prev_start) as u32;
        let (q0, q1): (u8, u8) =
            (t[prev_start as usize],
             if plen > 1 { t[prev_start as usize + 1] } else { 0 });
        sink.write_all(&prev_start.to_le_bytes()).unwrap();
        sink.write_all(&plen.to_le_bytes()).unwrap();
        hist2[(((q0 as usize) << 8) | (if plen > 1 { q1 as usize } else { 0 }))] += 1;
        m0 += 1;
    }
    drop(sink);
    eprintln!("SS_GPARSE_PASS mode=R k={} w={} threads=0 phrases={} \
               avg_len={:.2} forced_splits={} parse_bytes={} parse_s={:.2} \
               rule=streaming_one_sided_trigger;references_only;no_text_to_disc",
              K, W, m0,
              file_n as f64 / m0.max(1) as f64, forced, parse_bytes,
              super::laws::now_s(t0));
    ParseOut { m0, occ_path, hist2, forced_splits: forced, parse_bytes }
}
