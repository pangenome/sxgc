//! Small resident prototype only: one literal per lexical word, direct symbol access.
//! No Pile loader, external wire decoder, producer, or per-word pointer table.
const SPAN: usize = 16;
const W: usize = 8;
const CAP: usize = 10_000_000;
const MAX_WORD: usize = 1 << 20;
type Result<T> = std::result::Result<T, &'static str>;
type Record<'a> = (&'a [u8], bool);

fn width(x: usize) -> usize {
    (usize::BITS - x.leading_zeros()).div_ceil(8).max(1) as usize
}
fn field(bytes: &[u8], width: usize, i: usize) -> usize {
    let j = i * width;
    match width {
        1 => bytes[j] as usize,
        2 => u16::from_le_bytes(bytes[j..j + 2].try_into().unwrap()) as usize,
        3 => u32::from_le_bytes([bytes[j], bytes[j + 1], bytes[j + 2], 0]) as usize,
        4 => u32::from_le_bytes(bytes[j..j + 4].try_into().unwrap()) as usize,
        _ => unreachable!(),
    }
}
struct Plan {
    p: [usize; SPAN],
    e: [usize; SPAN],
    pw: usize,
    ew: usize,
    bytes: usize,
}
fn block(words: &[Record<'_>]) -> Plan {
    let (mut p, mut e) = ([0; SPAN], [0; SPAN]);
    let mut total = 0;
    for (i, &(s, _)) in words.iter().enumerate() {
        if i != 0 {
            p[i] = words[i - 1]
                .0
                .iter()
                .zip(s)
                .take_while(|(a, b)| a == b)
                .count();
        }
        total += s.len() - p[i];
        e[i] = total;
    }
    let pw = width(*p.iter().max().unwrap());
    let ew = width(total);
    let n = words.len();
    debug_assert!(pw <= ew && ew <= 4);
    Plan {
        p,
        e,
        pw,
        ew,
        bytes: 1 + (n - 1) * pw + n * ew + total,
    }
}
fn alloc<T>(n: usize) -> Result<Vec<T>> {
    let mut v = Vec::new();
    v.try_reserve_exact(n).map_err(|_| "fixture allocation")?;
    Ok(v)
}
/// `k,eof` (16 bytes on wire), one u64 offset per block and final offset, payload.
/// Each block: width byte; (n-1) common-prefix lengths; n cumulative literal
/// ends; concatenated literals. n=min(16,k-first). No sentinel in the raw data.
pub struct LexDirect {
    k: usize,
    eof: usize,
    offsets: Vec<u64>,
    data: Vec<u8>,
}
impl LexDirect {
    pub fn fixture(words: &[Record<'_>]) -> Result<Self> {
        if words.is_empty() || words.len() > CAP / 5 {
            return Err("fixture dimensions");
        }
        let mut eof = None;
        let mut packed = 0;
        for (i, &(s, end)) in words.iter().enumerate() {
            if s.len() <= if end { 0 } else { W }
                || s.len() > MAX_WORD
                || (i > 0 && words[i - 1] >= (s, end))
            {
                return Err("invalid lexical word");
            }
            packed += 4 + s.len();
            if packed > CAP {
                return Err("fixture cap");
            }
            if end && eof.replace(i).is_some() {
                return Err("multiple EOF");
            }
        }
        let eof = eof.ok_or("no EOF")?;
        let size = words.chunks(SPAN).map(|w| block(w).bytes).sum::<usize>();
        let mut data = alloc(size)?;
        let mut offsets = alloc(words.len().div_ceil(SPAN) + 1)?;
        for w in words.chunks(SPAN) {
            let b = block(w);
            let start = data.len();
            offsets.push(start as u64);
            data.push(((b.ew - 1) * 4 + b.pw - 1) as u8);
            for &x in &b.p[1..w.len()] {
                data.extend_from_slice(&(x as u32).to_le_bytes()[..b.pw]);
            }
            for &x in &b.e[..w.len()] {
                data.extend_from_slice(&(x as u32).to_le_bytes()[..b.ew]);
            }
            for (i, &(s, _)) in w.iter().enumerate() {
                data.extend_from_slice(&s[b.p[i]..]);
            }
            if data.len() - start != b.bytes {
                return Err("block ledger");
            }
        }
        if data.len() != size {
            return Err("payload ledger");
        }
        offsets.push(size as u64);
        Ok(Self {
            k: words.len(),
            eof,
            offsets,
            data,
        })
    }
    pub fn bytes(&self) -> usize {
        16 + 8 * self.offsets.len() + self.data.len()
    }
    pub fn word(&self, id: usize) -> Result<Word<'_>> {
        if id >= self.k {
            return Err("word id");
        }
        let b = id / SPAN;
        let i = id % SPAN;
        let n = (self.k - b * SPAN).min(SPAN);
        let data = &self.data[self.offsets[b] as usize..self.offsets[b + 1] as usize];
        let mode = data[0];
        let pw = usize::from(mode & 3) + 1;
        let ew = usize::from(mode >> 2) + 1;
        let (p, data) = data[1..].split_at((n - 1) * pw);
        let (e, literals) = data.split_at(n * ew);
        let prefix = if i == 0 { 0 } else { field(p, pw, i - 1) };
        let base = if i == 0 { 0 } else { field(e, ew, i - 1) };
        let len = prefix + (field(e, ew, i) - base);
        Ok(Word {
            p,
            e,
            literals,
            pw,
            ew,
            i,
            prefix,
            base,
            len,
            eof: id == self.eof,
        })
    }
}
/// Ephemeral; don't charge one per descriptor or cursor. The initial literal is
/// immediately accessible. At most 15 preceding prefix fields to inspect on a miss.
pub struct Word<'a> {
    p: &'a [u8],
    e: &'a [u8],
    literals: &'a [u8],
    pw: usize,
    ew: usize,
    i: usize,
    prefix: usize,
    base: usize,
    len: usize,
    eof: bool,
}
impl Word<'_> {
    pub fn owned(&self) -> usize {
        self.len - if self.eof { 0 } else { W }
    }
    /// Comparison domain is raw+1, explicit 0 only at the tagged EOF, then absence.
    pub fn symbol(&self, pos: usize) -> Option<u16> {
        self.traced(pos).0
    }
    /// Hops are predecessor links, *not* cache misses or physical loads.
    pub fn traced(&self, pos: usize) -> (Option<u16>, usize) {
        if pos >= self.len {
            return ((self.eof && pos == self.len).then_some(0), 0);
        }
        let mut j = self.i;
        let mut p = self.prefix;
        while pos < p {
            j -= 1;
            p = if j == 0 {
                0
            } else {
                field(self.p, self.pw, j - 1)
            };
        }
        let base = if j == self.i {
            self.base
        } else if j == 0 {
            0
        } else {
            field(self.e, self.ew, j - 1)
        };
        (
            Some(1 + u16::from(self.literals[base + (pos - p)])),
            self.i - j,
        )
    }
}
