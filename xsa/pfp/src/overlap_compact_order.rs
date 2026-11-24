//! Compact finite-key admission and order for a resident encoded dictionary.
use super::overlap_range_order::{Dictionary, Load, Tail};
use std::mem::size_of;

#[repr(C)]
#[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
pub struct PackedTail {
    pub word: u32,
    pub offset: u32,
}
const _: () = assert!(size_of::<PackedTail>() == 8);
impl From<PackedTail> for Tail {
    fn from(t: PackedTail) -> Self {
        Self {
            word: t.word as usize,
            offset: t.offset as usize,
        }
    }
}

pub struct Limits {
    /// Histogram, plan, and arena payload; excludes input, other stack, consumer.
    pub workspace_bytes: usize,
    pub rows_per_range: u64,
    /// All full descriptor enumerations, including admission and distribution.
    pub passes: usize,
}
#[derive(Debug, PartialEq, Eq)]
pub enum Refusal {
    Dictionary,
    Width,
    Budget,
    Overflow,
    PassCap,
    ExactClass(Load),
    Allocation,
}
#[derive(Default, Debug)]
pub struct Counts {
    pub census_passes: usize,
    pub distributions: usize,
    pub peak: Load,
    pub total: Load,
    pub radix_digit_reads: u128,
    pub groups: usize,
}
fn add(a: Load, b: Load) -> Result<Load, Refusal> {
    Ok(Load {
        descriptors: a
            .descriptors
            .checked_add(b.descriptors)
            .ok_or(Refusal::Overflow)?,
        rows: a.rows.checked_add(b.rows).ok_or(Refusal::Overflow)?,
    })
}
fn common(a: &[u8], b: &[u8]) -> usize {
    a.iter().zip(b).take_while(|(x, y)| x == y).count()
}
fn digit(k: &[u8], depth: usize) -> u8 {
    k.get(depth).copied().unwrap_or(0)
}
// Validate geometry before using any absolute start as a slice index.
fn check_geometry(k: usize, starts: &[u64], encoded_bytes: u64) -> Result<(), Refusal> {
    u32::try_from(k).map_err(|_| Refusal::Width)?;
    if starts.len().checked_sub(1) != Some(k)
        || starts.first() != Some(&0)
        || starts.last() != Some(&encoded_bytes)
    {
        return Err(Refusal::Dictionary);
    }
    for w in starts.windows(2) {
        if w[1] <= w[0] {
            return Err(Refusal::Dictionary);
        }
        u32::try_from(w[1] - w[0]).map_err(|_| Refusal::Width)?;
    }
    Ok(())
}
fn validate(d: &Dictionary<'_>) -> Result<(), Refusal> {
    let k = d.occurrences.len();
    if d.eof >= k {
        return Err(Refusal::Dictionary);
    }
    check_geometry(k, d.starts, d.bytes.len() as u64)?;
    for (i, w) in d.starts.windows(2).enumerate() {
        let word = d.bytes[w[0] as usize..w[1] as usize]
            .strip_suffix(&[0])
            .ok_or(Refusal::Dictionary)?;
        let data = if i == d.eof {
            word.strip_suffix(&[1])
                .filter(|s| !s.is_empty() && d.occurrences[i] == 1)
                .ok_or(Refusal::Dictionary)?
        } else {
            if word.len() <= 8 {
                return Err(Refusal::Dictionary);
            }
            word
        };
        if d.occurrences[i] == 0 || data.iter().any(|&b| b < 2) {
            return Err(Refusal::Dictionary);
        }
    }
    Ok(())
}
#[derive(Default, Clone, Copy)]
struct Band {
    witness: PackedTail,
    depth: usize,
    lo: u8,
    hi: u8,
    load: Load,
    split: bool,
}
#[derive(Default, Clone, Copy)]
struct Bin {
    witness: PackedTail,
    shared: usize,
    longest: usize,
    load: Load,
}
fn visit(
    d: &Dictionary<'_>,
    b: Band,
    mut f: impl FnMut(PackedTail, &[u8], u64) -> Result<(), Refusal>,
) -> Result<(), Refusal> {
    let prefix = &d.key(b.witness.into())[..b.depth];
    for (i, &rows) in d.occurrences.iter().enumerate() {
        let w = d.key(Tail { word: i, offset: 0 });
        for offset in 0..w.len() - if i == d.eof { 1 } else { 8 } {
            let k = &w[offset..];
            if (b.lo..=b.hi).contains(&digit(k, b.depth)) && k.starts_with(prefix) {
                f(
                    PackedTail {
                        word: i as u32,
                        offset: offset as u32,
                    },
                    k,
                    rows,
                )?;
            }
        }
    }
    Ok(())
}
/// In-place binary MSD. Skip uniform bytes, split at highest varying bit.
/// Only the smaller partition recurses (at most log2(slots) nested calls).
fn radix(d: &Dictionary<'_>, mut a: &mut [PackedTail], mut depth: usize, reads: &mut u128) {
    while a.len() > 1 {
        let get = |t: PackedTail| digit(d.key(t.into()), depth);
        let base = get(a[0]);
        let varied = a[1..].iter().fold(0, |bits, &t| bits | (get(t) ^ base));
        *reads += a.len() as u128;
        if varied == 0 {
            if base == 0 {
                return;
            }
            depth += 1;
            continue;
        }
        let mask = 0x80 >> varied.leading_zeros();
        let mut mid = 0;
        for j in 0..a.len() {
            if get(a[j]) & mask == 0 {
                a.swap(mid, j);
                mid += 1
            }
        }
        *reads += a.len() as u128;
        let (l, r) = a.split_at_mut(mid);
        let (small, big) = if l.len() < r.len() { (l, r) } else { (r, l) };
        radix(d, small, depth, reads);
        a = big;
    }
}
/// Histogram plus pre-admitted plan payload. Remaining workspace buys 8-byte slots.
pub fn fixed_bytes(passes: usize) -> Option<usize> {
    passes
        .checked_mul(size_of::<Band>())?
        .checked_add(size_of::<[Bin; 256]>())
}

/// No callback before complete admission. A group is one finite key; its members
/// are deterministic but not ordered by continuation. `head` spans range seams.
pub fn order(
    d: &Dictionary<'_>,
    lim: Limits,
    mut group: impl FnMut(&[PackedTail], usize),
) -> Result<Counts, Refusal> {
    validate(d)?;
    let slots = lim
        .workspace_bytes
        .checked_sub(fixed_bytes(lim.passes).ok_or(Refusal::Budget)?)
        .ok_or(Refusal::Budget)?
        / 8;
    if slots == 0 || lim.rows_per_range == 0 {
        return Err(Refusal::Budget);
    }
    if lim.passes < 2 {
        return Err(Refusal::PassCap);
    }
    let fits = |x: Load| x.descriptors <= slots && x.rows <= lim.rows_per_range;
    let mut c = Counts::default();
    let mut plan = Vec::new();
    plan.try_reserve_exact(lim.passes)
        .map_err(|_| Refusal::Allocation)?;
    plan.push(Band {
        hi: 255,
        split: true,
        ..Band::default()
    });
    let mut pos = 0;
    while pos < plan.len() {
        let b = plan[pos];
        if !b.split {
            pos += 1;
            continue;
        }
        plan.remove(pos);
        c.census_passes += 1;
        let mut bins = [Bin::default(); 256];
        visit(d, b, |t, k, rows| {
            let n = &mut bins[digit(k, b.depth) as usize];
            if n.load.descriptors == 0 {
                n.witness = t;
                n.shared = k.len()
            } else {
                let begin = b.depth;
                n.shared = begin + common(&d.key(n.witness.into())[begin..n.shared], &k[begin..]);
            }
            n.longest = n.longest.max(k.len());
            n.load = add(
                n.load,
                Load {
                    descriptors: 1,
                    rows,
                },
            )?;
            Ok(())
        })?;
        // Replace the parent in-place by a lexically ordered cover. A pending
        // split needs at least one more scan, as does each finished band.
        let mut inserted = 0;
        for (byte, n) in bins
            .iter()
            .copied()
            .enumerate()
            .filter(|(_, n)| n.load.descriptors > 0)
        {
            if c.census_passes == 1 {
                c.total = add(c.total, n.load)?
            }
            let next = if fits(n.load) {
                if inserted > 0 {
                    let last = &mut plan[pos + inserted - 1];
                    let both = add(last.load, n.load)?;
                    if !last.split && fits(both) {
                        last.hi = byte as u8;
                        last.load = both;
                        continue;
                    }
                }
                Band {
                    lo: byte as u8,
                    hi: byte as u8,
                    load: n.load,
                    split: false,
                    ..b
                }
            } else {
                // The complete bin lives below this witnessed prefix. Jump
                // directly to its first divergence, including finite-key end.
                if n.shared == n.longest {
                    return Err(Refusal::ExactClass(n.load));
                }
                Band {
                    witness: n.witness,
                    depth: n.shared,
                    lo: 0,
                    hi: 255,
                    load: n.load,
                    split: true,
                }
            };
            if plan.len() >= lim.passes - c.census_passes {
                return Err(Refusal::PassCap);
            }
            plan.insert(pos + inserted, next);
            inserted += 1;
        }
    }
    for b in &plan {
        c.peak.descriptors = c.peak.descriptors.max(b.load.descriptors);
        c.peak.rows = c.peak.rows.max(b.load.rows)
    }
    let mut arena = Vec::new();
    arena
        .try_reserve_exact(c.peak.descriptors)
        .map_err(|_| Refusal::Allocation)?;
    let mut prev: Option<PackedTail> = None;
    for b in plan {
        arena.clear();
        visit(d, b, |t, _, _| {
            assert!(arena.len() < b.load.descriptors);
            arena.push(t);
            Ok(())
        })?;
        assert_eq!(arena.len(), b.load.descriptors);
        radix(d, &mut arena, b.depth, &mut c.radix_digit_reads);
        for ties in
            arena.chunk_by(|a, z| d.key((*a).into())[b.depth..] == d.key((*z).into())[b.depth..])
        {
            let k = d.key(ties[0].into());
            let h = prev.map_or(0, |p| common(k, d.key(p.into())));
            group(ties, h);
            prev = Some(ties[0]);
            c.groups += 1;
        }
        c.distributions += 1;
    }
    Ok(c)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_local_geometry_must_fit_u32() {
        let wide = u64::from(u32::MAX) + 1;
        assert_eq!(
            check_geometry(2, &[0, wide - 1, wide + 1], wide + 1),
            Ok(())
        );
        assert_eq!(check_geometry(1, &[0, wide], wide), Err(Refusal::Width));
        if let Ok(k) = usize::try_from(wide) {
            assert_eq!(check_geometry(k, &[], 0), Err(Refusal::Width));
        }
    }
}
