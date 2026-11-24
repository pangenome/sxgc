//! Fixture-only post-parse navigation. END has rank zero; parse ranks are one-based.
type R<T> = Result<T, &'static str>;
fn reserve<T>(n: usize) -> R<Vec<T>> {
    let mut v = Vec::new();
    v.try_reserve_exact(n).map_err(|_| "allocation extent")?;
    Ok(v)
}
fn filled<T: Clone>(n: usize, x: T) -> R<Vec<T>> {
    let mut v = reserve(n)?;
    v.resize(n, x);
    Ok(v)
}
struct Block {
    base: u64,
    offset: usize, // word-aligned low bits
    high: [u64; 3],
    width: u32,
}
pub struct Monotone {
    blocks: Vec<Block>,
    low: Vec<u64>,
    len: usize,
}
impl Monotone {
    /// Inclusive universe ceiling. See OVERLAP_NAV.md for the capacity proof.
    pub fn build(len: usize, max: u64, values: impl IntoIterator<Item = u64>) -> R<Self> {
        Self::generate(len, max, values.into_iter().map(Ok))
    }
    fn generate(len: usize, max: u64, mut it: impl Iterator<Item = R<u64>>) -> R<Self> {
        let mut blocks = reserve(len.div_ceil(64))?;
        let ceil_log = 128 - u128::from(max).div_ceil(len.max(1) as u128).leading_zeros();
        let cap = usize::try_from((len as u128 * ceil_log as u128).div_ceil(64))
            .map_err(|_| "low extent")?;
        let mut low: Vec<u64> = reserve(cap)?;
        let mut prev = 0;
        for start in (0..len).step_by(64) {
            let t = (len - start).min(64);
            let mut buf = [0; 64];
            for x in &mut buf[..t] {
                *x = it.next().ok_or("short column")??;
                if *x < prev || *x > max {
                    return Err("not monotone or outside universe");
                }
                prev = *x;
            }
            let base = buf[0];
            let width = ((prev - base) / t as u64).checked_ilog2().unwrap_or(0);
            let offset = low.len();
            let end = offset
                .checked_add((t * width as usize).div_ceil(64))
                .ok_or("low extent")?;
            if end > cap {
                return Err("low capacity bound");
            }
            low.resize(end, 0);
            let mut high = [0; 3];
            for (i, &x) in buf[..t].iter().enumerate() {
                let delta = x - base;
                let p = ((delta >> width) as usize) + i; // quotient < 2t
                high[p / 64] |= 1 << (p % 64);
                if width != 0 {
                    let bit = i * width as usize;
                    let a = offset + bit / 64;
                    let sh = bit % 64;
                    let v = delta & ((1 << width) - 1);
                    low[a] |= v << sh;
                    if sh + width as usize > 64 {
                        low[a + 1] |= v >> (64 - sh);
                    }
                }
            }
            blocks.push(Block {
                base,
                offset,
                high,
                width,
            });
        }
        if it.next().is_some() {
            return Err("long column");
        }
        Ok(Self { blocks, low, len })
    }
    pub fn get(&self, index: usize) -> Option<u64> {
        if index >= self.len {
            return None;
        }
        let b = &self.blocks[index / 64];
        let i = index % 64;
        let mut k = i as u32;
        for (word, &x) in b.high.iter().enumerate() {
            if k >= x.count_ones() {
                k -= x.count_ones();
                continue;
            }
            let mut x = x;
            for _ in 0..k {
                x &= x - 1;
            }
            let hi = word * 64 + x.trailing_zeros() as usize - i;
            let w = b.width as usize;
            let mut lo = 0;
            if w != 0 {
                let bit = i * w;
                let a = b.offset + bit / 64;
                let sh = bit % 64;
                lo = self.low[a] >> sh;
                if sh + w > 64 {
                    lo |= self.low[a + 1] << (64 - sh);
                }
                lo &= (1 << w) - 1;
            }
            return b.base.checked_add(((hi as u64) << w) | lo);
        }
        None
    }
}
struct Samples {
    step: u32,
    bits: Vec<u64>,
    before: Vec<u32>,
    positions: Vec<u32>,
}
pub struct Successors {
    psi: Vec<u32>,
    samples: Option<Samples>,
}
impl Successors {
    /// Lexical word IDs and zero-based parse SA. Check permutation and recurrence before use.
    pub fn build(ids: &[u32], sa: &[u32], step: Option<u32>) -> R<Self> {
        let m = ids.len();
        if m == 0
            || m >= u32::MAX as usize
            || m != sa.len()
            || step.is_some_and(|b| !matches!(b, 2 | 4 | 8))
        {
            return Err("parse extent or sample step");
        }
        let mut inverse = filled(m, 0u32)?;
        for (i, &j) in sa.iter().enumerate() {
            let slot = inverse.get_mut(j as usize).ok_or("invalid occurrence")?;
            if *slot != 0 {
                return Err("duplicate occurrence");
            }
            *slot = i as u32 + 1;
        }
        let mut psi = reserve(m)?;
        let mut last = None;
        let mut samples = match step {
            Some(b) => Some(Samples {
                step: b,
                bits: filled(m.div_ceil(64), 0)?,
                before: reserve(m.div_ceil(64))?,
                positions: reserve(m.div_ceil(b as usize))?,
            }),
            None => None,
        };
        for (i, &j) in sa.iter().enumerate() {
            let r = inverse.get(j as usize + 1).copied().unwrap_or(0);
            let key = (ids[j as usize], r);
            if last.is_some_and(|x| x >= key) {
                return Err("invalid parse SA");
            }
            last = Some(key);
            psi.push(r);
            if let Some(s) = samples.as_mut() {
                if j % s.step == 0 {
                    s.bits[i / 64] |= 1 << (i % 64);
                    s.positions.push(j);
                }
            }
        }
        if let Some(s) = samples.as_mut() {
            let mut n = 0;
            for x in &s.bits {
                s.before.push(n);
                n += x.count_ones();
            }
        }
        Ok(Self { psi, samples })
    }
    pub fn successor(&self, rank: u32) -> Option<u32> {
        self.psi.get(rank.checked_sub(1)? as usize).copied()
    }
    /// Unsampled mode: caller can instead index its retained parse SA at rank-1.
    pub fn position(&self, mut rank: u32) -> Option<u32> {
        if rank as usize > self.psi.len() {
            return None;
        }
        let mut steps = 0;
        loop {
            if rank == 0 {
                return (self.psi.len() as u32).checked_sub(steps);
            }
            let i = (rank - 1) as usize;
            if let Some(s) = &self.samples {
                let mask = 1u64 << (i % 64);
                let x = s.bits[i / 64];
                if x & mask != 0 {
                    return s.positions
                        [(s.before[i / 64] + (x & (mask - 1)).count_ones()) as usize]
                        .checked_sub(steps);
                }
            }
            // Final occurrence is an implicit sample; no hop to virtual END needed.
            rank = self.psi[i];
            if rank == 0 {
                return (self.psi.len() as u32 - 1).checked_sub(steps);
            }
            steps += 1;
            if self.samples.as_ref().is_some_and(|s| steps >= s.step) {
                return None;
            }
        }
    }
}
pub struct Aligned<'a> {
    nav: &'a Successors,
    starts: Monotone,
    adjusted: Monotone,
    slack: u64,
    n: u64,
}
impl<'a> Aligned<'a> {
    /// Starts and raw-byte predecessor PLCP in occurrence order (exclude virtual END).
    pub fn build(
        nav: &'a Successors,
        n: u64,
        w: u64,
        starts: impl IntoIterator<Item = u64>,
        plcp: impl IntoIterator<Item = u64>,
    ) -> R<Self> {
        let m = nav.psi.len();
        let slack = w.checked_sub(1).ok_or("empty trigger")?;
        let ceiling = n
            .checked_add(
                slack
                    .checked_mul(m as u64 - 1)
                    .ok_or("adjustment overflow")?,
            )
            .ok_or("adjustment overflow")?;
        let starts = Monotone::build(m, n, starts)?;
        let first = nav.position(1).ok_or("invalid first rank")? as usize;
        let mut previous = (0u64, 0u64);
        let adjusted = Monotone::generate(
            m,
            ceiling,
            plcp.into_iter().enumerate().map(|(j, p)| {
                let s = starts.get(j).ok_or("bad start")?;
                if (j == 0 && s != 0)
                    || (j > 0 && (starts.get(j - 1) == Some(s) || w > n - s))
                    || (j == first && p != 0)
                    || p > n - s
                {
                    return Err("invalid start/PLCP");
                }
                if j > 0 {
                    let stride = s - previous.0;
                    let residual = previous.1.saturating_sub(stride);
                    if residual > slack && p < residual {
                        return Err("lost shared next trigger");
                    }
                }
                previous = (s, p);
                Ok(s + p + slack * j as u64)
            }),
        )?;
        let out = Self {
            nav,
            starts,
            adjusted,
            slack,
            n,
        };
        let mut limit = 0;
        for r in 1..=m as u32 {
            let j = nav.position(r).ok_or("invalid position")? as usize;
            if out.plcp(j).ok_or("invalid PLCP")? > limit {
                return Err("PLCP past predecessor END");
            }
            limit = n - out.starts.get(j).ok_or("invalid start")?;
        }
        Ok(out)
    }
    pub fn start(&self, j: usize) -> Option<u64> {
        if j == self.starts.len {
            Some(self.n)
        } else {
            self.starts.get(j)
        }
    }
    pub fn plcp(&self, j: usize) -> Option<u64> {
        Some(self.adjusted.get(j)? - self.starts.get(j)? - self.slack * j as u64)
    }
    pub fn leaf(&self, r: u32) -> Option<u64> {
        if r == 0 {
            Some(0)
        } else {
            self.plcp(self.nav.position(r)? as usize)
        }
    }
}
/// Iterative min tree over complete/partial rank blocks (not over leaves).
pub struct BlockMin<'a> {
    col: &'a Aligned<'a>,
    b: usize,
    tree: Vec<u64>,
}
impl<'a> BlockMin<'a> {
    pub fn build(col: &'a Aligned<'a>, b: usize) -> R<Self> {
        let m = col.nav.psi.len();
        if b == 0 {
            return Err("bad minima extent");
        }
        let q = m.div_ceil(b);
        let mut tree = filled(q.checked_mul(2).ok_or("tree extent")?, u64::MAX)?;
        for i in 0..m {
            tree[q + i / b] = tree[q + i / b].min(col.leaf(i as u32 + 1).ok_or("invalid leaf")?);
        }
        for i in (1..q).rev() {
            tree[i] = tree[2 * i].min(tree[2 * i + 1]);
        }
        Ok(Self { col, b, tree })
    }
    /// LCP between distinct aligned ranks, including END.
    pub fn between(&self, x: u32, y: u32) -> Option<u64> {
        if x >= y || y as usize > self.col.nav.psi.len() {
            return None;
        }
        if x == 0 {
            return Some(0);
        }
        let (mut l, mut r) = (x as usize, y as usize);
        let mut h = u64::MAX;
        while l < r && l % self.b != 0 {
            h = h.min(self.col.leaf(l as u32 + 1)?);
            l += 1;
        }
        while l < r && r % self.b != 0 {
            h = h.min(self.col.leaf(r as u32)?);
            r -= 1;
        }
        let q = self.tree.len() / 2;
        l = q + l / self.b;
        r = q + r / self.b;
        while l < r {
            if l % 2 == 1 {
                h = h.min(self.tree[l]);
                l += 1;
            }
            if r % 2 == 1 {
                r -= 1;
                h = h.min(self.tree[r]);
            }
            l /= 2;
            r /= 2;
        }
        Some(h)
    }
}
