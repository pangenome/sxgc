//! Bounded order of *finite owned tails* over an explicitly resident encoded dictionary.
use std::mem::size_of;

pub struct Dictionary<'a> {
    /// Per word: remapped data (>=2), optional final END=1, then SEP=0.
    pub bytes: &'a [u8],
    pub starts: &'a [u64], // includes exclusive dictionary end
    pub occurrences: &'a [u64],
    pub eof: usize,
}
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Tail {
    pub word: usize,
    pub offset: usize,
}
impl Dictionary<'_> {
    fn word(&self, i: usize) -> &[u8] {
        &self.bytes[self.starts[i] as usize..self.starts[i + 1] as usize - 1]
    }
    pub fn key(&self, t: Tail) -> &[u8] {
        &self.word(t.word)[t.offset..]
    }
    fn visit(
        &self,
        bits: u32,
        mut f: impl FnMut(usize, Tail) -> Result<(), Refusal>,
    ) -> Result<(), Refusal> {
        for word in 0..self.occurrences.len() {
            let w = self.word(word);
            for offset in 0..w.len() - if word == self.eof { 1 } else { 8 } {
                let bucket = u16::from_be_bytes([w[offset], w[offset + 1]]) as usize >> (16 - bits);
                f(bucket, Tail { word, offset })?;
            }
        }
        Ok(())
    }
    fn validate(&self) -> Result<(), Refusal> {
        if self.eof >= self.occurrences.len()
            || self.starts.len().checked_sub(1) != Some(self.occurrences.len())
            || self.starts.first() != Some(&0)
            || self.starts.last() != Some(&(self.bytes.len() as u64))
            || !self.starts.windows(2).all(|w| w[0] < w[1])
        {
            return Err(Refusal::Dictionary);
        }
        for (i, &n) in self.occurrences.iter().enumerate() {
            let w = self.word(i);
            let data = if i == self.eof {
                w.strip_suffix(&[1]).ok_or(Refusal::Dictionary)?
            } else {
                if w.len() <= 8 {
                    return Err(Refusal::Dictionary);
                }
                w
            };
            if self.bytes[self.starts[i + 1] as usize - 1] != 0
                || n == 0
                || (i == self.eof && (n != 1 || data.is_empty()))
                || data.iter().any(|&b| b < 2)
            {
                return Err(Refusal::Dictionary);
            }
        }
        Ok(())
    }
}
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Load {
    pub descriptors: usize,
    pub rows: u64,
}
impl Load {
    fn plus(self, b: Self) -> Result<Self, Refusal> {
        Ok(Self {
            descriptors: self
                .descriptors
                .checked_add(b.descriptors)
                .ok_or(Refusal::Overflow)?,
            rows: self.rows.checked_add(b.rows).ok_or(Refusal::Overflow)?,
        })
    }
}
#[derive(Debug, PartialEq, Eq)]
pub enum Refusal {
    Dictionary,
    Budget,
    Overflow,
    Allocation,
    Bucket { index: usize, load: Load },
    Distributions { needed: usize },
}
pub struct Limits {
    pub workspace_bytes: usize,
    pub prefix_bits: u32,
    pub rows_per_range: u64,
    pub distributions: usize,
}
#[derive(Default, Debug)]
pub struct Census {
    pub ranges: usize,
    pub total: Load,
    pub peak: Load,
}

// Greedy by both dimensions. The census guarantees every individual bin fits.
fn ranges(
    bins: &[Load],
    slots: usize,
    rows: u64,
    mut f: impl FnMut(std::ops::Range<usize>, Load),
) -> Result<(), Refusal> {
    let (mut lo, mut current) = (0, Load::default());
    for (i, &b) in bins.iter().enumerate() {
        if b.descriptors > slots || b.rows > rows {
            return Err(Refusal::Bucket { index: i, load: b });
        }
        if b.descriptors > slots - current.descriptors || b.rows > rows - current.rows {
            f(lo..i, current);
            lo = i;
            current = Load::default();
        }
        current = current.plus(b)?;
    }
    if current.descriptors != 0 {
        f(lo..bins.len(), current);
    }
    Ok(())
}
/// Callback receives a complete equivalence class (unordered within class), and
/// finite LCP with the preceding class, including across distribution boundaries.
/// Caller owns END and the continuation-rank merge, not one output row per Tail.
pub fn order(
    d: &Dictionary<'_>,
    lim: Limits,
    mut group: impl FnMut(&[Tail], usize),
) -> Result<Census, Refusal> {
    if !(1..=16).contains(&lim.prefix_bits) {
        return Err(Refusal::Budget);
    }
    let n = 1usize << lim.prefix_bits;
    let slots = lim
        .workspace_bytes
        .checked_sub(n * size_of::<Load>())
        .ok_or(Refusal::Budget)?
        / size_of::<Tail>();
    d.validate()?;
    let mut bins = Vec::new();
    bins.try_reserve_exact(n).map_err(|_| Refusal::Allocation)?;
    bins.resize(n, Load::default());
    d.visit(lim.prefix_bits, |i, t| {
        bins[i] = bins[i].plus(Load {
            descriptors: 1,
            rows: d.occurrences[t.word],
        })?;
        Ok(())
    })?;
    let mut census = Census::default();
    for &b in &bins {
        census.total = census.total.plus(b)?;
    }
    ranges(&bins, slots, lim.rows_per_range, |_, load| {
        census.ranges += 1;
        census.peak.descriptors = census.peak.descriptors.max(load.descriptors);
        census.peak.rows = census.peak.rows.max(load.rows);
    })?;
    if census.ranges > lim.distributions {
        return Err(Refusal::Distributions {
            needed: census.ranges,
        });
    }
    let mut arena = Vec::new();
    arena
        .try_reserve_exact(census.peak.descriptors)
        .map_err(|_| Refusal::Allocation)?;
    let mut prev = None;
    ranges(&bins, slots, lim.rows_per_range, |r, load| {
        arena.clear();
        d.visit(lim.prefix_bits, |i, t| {
            if r.contains(&i) {
                arena.push(t);
            }
            Ok(())
        })
        .expect("infallible distribution");
        assert_eq!(arena.len(), load.descriptors);
        arena.sort_unstable_by(|&a, &b| d.key(a).cmp(d.key(b)));
        for ties in arena.chunk_by(|&a, &b| d.key(a) == d.key(b)) {
            let key = d.key(ties[0]);
            let lcp = prev.map_or(0, |t| {
                key.iter().zip(d.key(t)).take_while(|(a, b)| a == b).count()
            });
            group(ties, lcp);
            prev = Some(ties[0]);
        }
    })?;
    Ok(census)
}
