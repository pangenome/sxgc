//! Prepared P1 consumer for LE64 lexical record addresses and one-based u64 ranks.
//! No byte-level SA, dense lexical IDs, signed PLCP call, occurrence sort or rank spool.
use std::ops::Range;
type R<T> = Result<T, &'static str>;
const W: usize = 8;
const BLOCK: usize = 64;

pub fn array_bound(dict: u64, m: u64, workers: usize) -> R<u64> {
    if m == 0 || !(1..=64).contains(&workers) {
        return Err("invalid dimensions");
    }
    // immutable tape, keys + inverse; ordinal at rank, text starts (+ terminal), byte PLCP
    let bytes = dict as u128
        + 40 * m as u128
        + 8
        + 16 * m.div_ceil(BLOCK as u64) as u128
        + (4 << 20)
        + (workers << 21) as u128;
    if bytes > isize::MAX as u128 {
        return Err("address extent");
    }
    Ok(bytes as u64)
}
fn vec64(n: usize, value: u64) -> R<Vec<u64>> {
    let mut v = Vec::new();
    v.try_reserve_exact(n).map_err(|_| "allocation refused")?;
    v.resize(n, value);
    Ok(v)
}
fn word(tape: &[u8], key: u64) -> R<(&[u8], bool)> {
    let p = usize::try_from(key).map_err(|_| "word address")?;
    let record = tape.get(p..).ok_or("word outside tape")?;
    let tag = u64::from_le_bytes(record.get(..8).ok_or("short header")?.try_into().unwrap());
    let len = usize::try_from(tag >> 1).map_err(|_| "word length")?;
    let eof = tag & 1 != 0;
    if len == 0 || !eof && len <= W {
        return Err("empty owned stride");
    }
    Ok((record[8..].get(..len).ok_or("truncated record")?, eof))
}
#[derive(Debug, PartialEq, Eq)]
pub struct Occurrence {
    pub ordinal: u64,
    pub key: u64,
    pub start: u64,
    pub successor: u64,
    pub aligned_lcp: u64,
    pub predecessor: u16,
}
pub struct RankView<'a> {
    tape: &'a [u8],
    keys: &'a [u64],
    inv: &'a [u64],
    ord: Vec<u64>,
    starts: Vec<u64>,
    plcp_bytes: Vec<u64>,
    minima: Vec<u64>,
}
impl<'a> RankView<'a> {
    pub fn build(
        tape: &'a [u8],
        keys: &'a [u64],
        inv: &'a [u64],
        n: u64,
        workers: usize,
        grant: u64,
        stop: &(impl Fn() -> bool + Sync),
    ) -> R<Self> {
        let m = keys.len();
        if inv.len() != m || array_bound(tape.len() as u64, m as u64, workers)? > grant || stop() {
            return Err("rank view grant, stop or extent");
        }
        let mut ord = vec64(m, u64::MAX)?;
        for (j, &r) in inv.iter().enumerate() {
            if j % 65536 == 0 && stop() {
                return Err("stop");
            }
            if r == 0 || r > m as u64 {
                return Err("rank outside permutation");
            }
            let slot = &mut ord[(r - 1) as usize];
            if *slot != u64::MAX {
                return Err("duplicate rank");
            }
            *slot = j as u64;
        }
        // Consecutive distinct heads in the verified order must cover *exactly* the tape.
        // This also refuses mid-word addresses that happen to look like valid headers.
        let mut end = 0u64;
        let mut previous = None;
        let mut prev_word = None;
        let mut eof = None;
        for (r, &j) in ord.iter().enumerate() {
            if r % 65536 == 0 && stop() {
                return Err("stop");
            }
            let j = j as usize;
            let key = keys[j];
            let pair = (key, inv.get(j + 1).copied().unwrap_or(0));
            if previous.is_some_and(|p| p >= pair) {
                return Err("bad suffix rank recurrence");
            }
            if previous.map(|p: (u64, u64)| p.0) != Some(key) {
                if key != end {
                    return Err("noncanonical address");
                }
                let rec = word(tape, key)?;
                if prev_word.is_some_and(|p| p >= rec) {
                    return Err("word order");
                }
                end += 8 + rec.0.len() as u64;
                prev_word = Some(rec);
                if rec.1 && eof.replace(key).is_some() {
                    return Err("multiple terminal records");
                }
            }
            previous = Some(pair);
        }
        if end != tape.len() as u64 || eof.is_none() {
            return Err("uncovered dictionary or no EOF");
        }
        let mut starts = vec64(m + 1, 0)?;
        let mut overlap = None;
        for (j, &key) in keys.iter().enumerate() {
            if j % 65536 == 0 && stop() {
                return Err("stop");
            }
            let (raw, terminal) = word(tape, key)?;
            if terminal != (j + 1 == m) || overlap.is_some_and(|s| raw.get(..W) != Some(s)) {
                return Err("text-order EOF/overlap");
            }
            let stride = raw.len() - if terminal { 0 } else { W };
            starts[j + 1] = starts[j].checked_add(stride as u64).ok_or("text extent")?;
            overlap = (!terminal).then(|| &raw[stride..]);
        }
        if starts[m] != n {
            return Err("wrong text extent");
        }
        let mut plcp_bytes = vec64(m, 0)?;
        // Generalized Kasai on the tokens. Each worker independently seeds h=0 at its
        // first text ordinal. No carry across worker seams and no concurrent scatter.
        std::thread::scope(|scope| -> R<()> {
            let block = m.div_ceil(workers);
            let mut joins = Vec::with_capacity(workers);
            for (lane, part) in plcp_bytes.chunks_mut(block).enumerate() {
                let sa = &ord;
                let s = &starts;
                joins.push(scope.spawn(move || -> R<()> {
                    let mut h = 0;
                    for (q, bytes) in part.iter_mut().enumerate() {
                        if q % 16384 == 0 && stop() {
                            return Err("stop");
                        }
                        let j = lane * block + q;
                        let r = inv[j] as usize;
                        if r == 1 {
                            h = 0;
                            continue;
                        }
                        let p = sa[r - 2] as usize;
                        while h < m - j && h < m - p && keys[j + h] == keys[p + h] {
                            h += 1;
                            if h % 65536 == 0 && stop() {
                                return Err("stop");
                            }
                        }
                        if h >= m - j || h >= m - p || keys[p + h] >= keys[j + h] {
                            return Err("non-increasing mismatch or nonunique EOF");
                        }
                        let left = word(tape, keys[p + h])?.0;
                        let right = word(tape, keys[j + h])?.0;
                        let same =
                            left.iter().zip(right).take_while(|(a, b)| a == b).count() as u64;
                        *bytes = (s[j + h] - s[j])
                            .checked_add(same)
                            .ok_or("byte LCP overflow")?;
                        if *bytes > n - s[p].max(s[j]) {
                            return Err("byte LCP outside suffix");
                        }
                        h = h.saturating_sub(1);
                    }
                    Ok(())
                }));
            }
            for j in joins {
                j.join().map_err(|_| "PLCP worker panicked")??;
            }
            Ok(())
        })?;
        if stop() {
            return Err("stop");
        }
        let blocks = m.div_ceil(BLOCK);
        let mut minima = vec64(2 * blocks, u64::MAX)?;
        for (b, rows) in ord.chunks(BLOCK).enumerate() {
            if b % 1024 == 0 && stop() {
                return Err("stop");
            }
            minima[blocks + b] = rows.iter().map(|&j| plcp_bytes[j as usize]).min().unwrap();
        }
        for b in (1..blocks).rev() {
            minima[b] = minima[2 * b].min(minima[2 * b + 1]);
        }
        Ok(Self {
            minima,
            tape,
            keys,
            inv,
            ord,
            starts,
            plcp_bytes,
        })
    }
    /// Half-open, zero-based list slots; rank = slot+1. All ties in successor order.
    pub fn slots(&self, key: u64) -> Range<usize> {
        self.ord.partition_point(|&j| self.keys[j as usize] < key)
            ..self.ord.partition_point(|&j| self.keys[j as usize] <= key)
    }
    /// Aligned byte LCE of two one-based parse ranks, zero denoting the virtual END.
    pub fn lce(&self, a: u64, b: u64) -> Option<u64> {
        if a.max(b) > self.ord.len() as u64 {
            return None;
        }
        if a.min(b) == 0 {
            return Some(0);
        }
        if a == b {
            return Some(self.starts.last()? - self.starts[self.ord[(a - 1) as usize] as usize]);
        }
        // Adjacent LCP slots [min(rank),max(rank)); up to two partial blocks.
        let (mut l, mut r) = (a.min(b) as usize, a.max(b) as usize);
        let mut h = u64::MAX;
        while l < r && l % BLOCK != 0 {
            h = h.min(self.plcp_bytes[self.ord[l] as usize]);
            l += 1;
        }
        while l < r && r % BLOCK != 0 {
            r -= 1;
            h = h.min(self.plcp_bytes[self.ord[r] as usize]);
        }
        let blocks = self.minima.len() / 2;
        l = l / BLOCK + blocks;
        r = r / BLOCK + blocks;
        while l < r {
            if l % 2 == 1 {
                h = h.min(self.minima[l]);
                l += 1;
            }
            if r % 2 == 1 {
                r -= 1;
                h = h.min(self.minima[r]);
            }
            l /= 2;
            r /= 2;
        }
        Some(h)
    }
    pub fn get(&self, slot: usize) -> Option<Occurrence> {
        let &ordinal = self.ord.get(slot)?;
        let j = ordinal as usize;
        let predecessor = if j == 0 {
            256
        } else {
            let (raw, eof) = word(self.tape, self.keys[j - 1]).ok()?;
            if eof {
                return None;
            }
            raw[raw.len() - W - 1] as u16
        };
        Some(Occurrence {
            ordinal,
            key: self.keys[j],
            start: self.starts[j],
            successor: self.inv.get(j + 1).copied().unwrap_or(0),
            aligned_lcp: self.plcp_bytes[j],
            predecessor,
        })
    }
}
