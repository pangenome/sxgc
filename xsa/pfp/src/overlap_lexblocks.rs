//! Fixture-only random-access dictionary, block-restart predecessor front coding.
//! Never linked into a producer. IDs follow sorted (raw,EOF); virtual END < every raw byte.
const W: usize = 8;
const MAX_WORD: usize = 1 << 20;
const PACKED_FIXTURE_MAX: usize = 10_000_000;
type Result<T> = std::result::Result<T, &'static str>;

fn uvlen(mut x: usize) -> usize {
    let mut len = 1;
    while x >= 128 {
        x >>= 7;
        len += 1;
    }
    len
}
fn put(mut x: usize, data: &mut Vec<u8>) {
    while x >= 128 {
        data.push((x as u8 & 127) | 128);
        x >>= 7;
    }
    data.push(x as u8);
}
fn get(data: &[u8], at: &mut usize) -> Result<usize> {
    // suffix length and prefix <=1 MiB (tagged <=2^21+1): four bytes suffice.
    let mut x = 0;
    for j in 0..4 {
        let b = *data.get(*at).ok_or("truncated length")?;
        *at += 1;
        x |= usize::from(b & 127) << (7 * j);
        if b < 128 {
            return if j != 0 && b == 0 {
                Err("noncanonical length")
            } else {
                Ok(x)
            };
        }
    }
    Err("overlong length")
}
fn reserve<T>(n: usize) -> Result<Vec<T>> {
    let mut v = Vec::new();
    v.try_reserve_exact(n).map_err(|_| "fixture allocation")?;
    Ok(v)
}

pub struct LexBlocks {
    span: usize,
    k: usize,
    offsets: Vec<u64>,
    data: Vec<u8>,
}
impl LexBlocks {
    /// Measure the parameterized codec before allocating. Source fixture and the
    /// final encoded table coexist here: not a streaming-scale construction proof.
    pub fn fixture(words: &[(&[u8], bool)], span: usize) -> Result<Self> {
        if !matches!(span, 4 | 8 | 16) || words.is_empty() || words.len() > PACKED_FIXTURE_MAX / 5 {
            return Err("fixture dimensions");
        }
        let (mut size, mut packed, mut eofs) = (0, 0, 0);
        let prefix = |i: usize| {
            if i % span == 0 {
                0
            } else {
                words[i - 1]
                    .0
                    .iter()
                    .zip(words[i].0)
                    .take_while(|(a, b)| a == b)
                    .count()
            }
        };
        for (i, &(s, e)) in words.iter().enumerate() {
            if s.len() <= if e { 0 } else { W }
                || s.len() > MAX_WORD
                || (i > 0 && words[i - 1] >= (s, e))
            {
                return Err("invalid lexical word");
            }
            packed += 4 + s.len();
            if packed > PACKED_FIXTURE_MAX {
                return Err("fixture cap");
            }
            eofs += usize::from(e);
            let p = prefix(i);
            let lit = s.len() - p;
            size +=
                lit + uvlen(2 * lit + usize::from(e)) + if i % span == 0 { 0 } else { uvlen(p) };
        }
        if eofs != 1 {
            return Err("EOF count");
        }
        let mut data = reserve(size)?;
        let mut offsets = reserve(words.len().div_ceil(span) + 1)?;
        for (i, &(s, e)) in words.iter().enumerate() {
            let p = prefix(i);
            if i % span == 0 {
                offsets.push(data.len() as u64)
            } else {
                put(p, &mut data)
            }
            let lit = s.len() - p;
            put(2 * lit + usize::from(e), &mut data);
            data.extend_from_slice(&s[p..]);
        }
        if data.len() != size {
            return Err("codec ledger");
        }
        offsets.push(size as u64);
        Ok(Self {
            span,
            k: words.len(),
            data,
            offsets,
        })
    }
    pub fn payload(&self) -> usize {
        self.data.len()
    }
    pub fn directory(&self) -> usize {
        self.offsets.len() * 8
    }
    /// Parse at most span records, skipping literal bodies by their lengths.
    /// Each shared prefix truncates previous spans; the new literal adds at most one.
    pub fn word(&self, id: usize) -> Result<Word<'_>> {
        if id >= self.k {
            return Err("word id");
        }
        let block = id / self.span;
        let data = &self.data[self.offsets[block] as usize..self.offsets[block + 1] as usize];
        let mut v = Word {
            data,
            pieces: [Piece::default(); 16],
            used: 0,
            len: 0,
            eof: false,
        };
        let mut at = 0;
        for i in 0..=id % self.span {
            let p = if i == 0 { 0 } else { get(data, &mut at)? };
            let tag = get(data, &mut at)?;
            let lit = tag >> 1;
            if p > v.len || p + lit > MAX_WORD || lit > data.len() - at {
                return Err("invalid block extent");
            }
            while v.used > 0
                && (v.used == 1 && p == 0 || v.used > 1 && v.pieces[v.used - 2].end >= p)
            {
                v.used -= 1;
            }
            if v.used > 0 {
                v.pieces[v.used - 1].end = p;
            }
            if lit > 0 {
                v.pieces[v.used] = Piece {
                    end: p + lit,
                    body: at,
                };
                v.used += 1;
            }
            at += lit;
            v.len = p + lit;
            v.eof = tag & 1 != 0;
            if v.len <= if v.eof { 0 } else { W } {
                return Err("empty owned stride");
            }
        }
        Ok(v)
    }
}
#[derive(Default, Copy, Clone)]
struct Piece {
    end: usize,
    body: usize,
}
pub struct Word<'a> {
    data: &'a [u8],
    pieces: [Piece; 16],
    used: usize,
    len: usize,
    eof: bool,
}
impl Word<'_> {
    pub fn owned(&self) -> usize {
        self.len - if self.eof { 0 } else { W }
    }
    /// 1..=256 raw byte+1; one explicit END=0 only in the EOF word. No SEP in a finite key.
    pub fn symbol(&self, pos: usize) -> Option<u16> {
        if pos >= self.len {
            return (pos == self.len && self.eof).then_some(0);
        }
        let parts = &self.pieces[..self.used];
        let i = parts.partition_point(|x| x.end <= pos);
        let lo = if i == 0 { 0 } else { parts[i - 1].end };
        Some(self.data[parts[i].body + pos - lo] as u16 + 1)
    }
}
