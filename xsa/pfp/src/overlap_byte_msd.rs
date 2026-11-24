//! Local order of one admitted descriptor band, no change to band or group semantics.
use super::{digit, Counts, Dictionary, PackedTail};
use std::mem::size_of;

// Small buckets: binary insertion by borrowed finite suffix, no per-tail key or sort allocation.
const LEAF: usize = 32;
type Histogram = [usize; 256];
const FRAME: usize = 2 * size_of::<Histogram>() + 8 * size_of::<usize>();

// Only recurse on a non-largest bucket that is larger than LEAF. It is at most
// half the caller's length. Iterate the largest; the virtual end needs no sort.
fn frames(slots: usize) -> usize {
    if slots <= LEAF {
        0
    } else {
        1 + (slots / (LEAF + 1)).ilog2() as usize
    }
}
pub fn stack_payload(slots: usize) -> usize {
    frames(slots) * FRAME
}

fn leaf(d: &Dictionary<'_>, a: &mut [PackedTail], depth: usize, c: &mut Counts) {
    let mut comparisons = 0u64;
    let mut shifts = 0u64;
    for i in 1..a.len() {
        let item = a[i];
        let key = &d.key(item.into())[depth..];
        comparisons += 1;
        if &d.key(a[i - 1].into())[depth..] <= key {
            continue;
        }
        let j = a[..i - 1].partition_point(|&t| {
            comparisons += 1;
            &d.key(t.into())[depth..] <= key
        });
        a.copy_within(j..i, j + 1);
        a[j] = item;
        shifts += (i - j) as u64;
    }
    c.leaf_comparisons += u128::from(comparisons);
    c.leaf_shifted_slots += u128::from(shifts);
}

fn large(
    d: &Dictionary<'_>,
    mut a: &mut [PackedTail],
    mut depth: usize,
    level: usize,
    limit: usize,
    c: &mut Counts,
) {
    assert!(level <= limit);
    c.byte_frames_peak = c.byte_frames_peak.max(level);
    loop {
        debug_assert!(a.len() > LEAF);
        let mut end: Histogram = [0; 256];
        for &t in a.iter() {
            end[digit(d.key(t.into()), depth) as usize] += 1;
        }
        c.radix_digit_reads += a.len() as u128;
        let mut big = 0;
        for b in 1..256 {
            if end[b] > end[big] {
                big = b;
            }
        }
        if end[big] == a.len() {
            if big == 0 {
                return;
            }
            depth += 1;
            continue;
        }
        let mut next: Histogram = [0; 256];
        let mut sum = 0;
        for (v, p) in end.iter_mut().zip(&mut next) {
            *p = sum;
            sum += *v;
            *v = sum;
        }
        debug_assert_eq!(sum, a.len());
        // [begin[b],next[b]) is already correctly placed. Every iteration
        // places one more descriptor into its *actual* bucket; no cloning.
        for b in 0..256 {
            while next[b] < end[b] {
                let i = next[b];
                let dst = digit(d.key(a[i].into()), depth) as usize;
                debug_assert!(next[dst] < end[dst]);
                if b != dst {
                    a.swap(i, next[dst]);
                }
                next[dst] += 1;
            }
        }
        c.radix_digit_reads += a.len() as u128;
        c.byte_partitions += 1;
        for b in 1..256 {
            if b != big {
                let child = &mut a[end[b - 1]..end[b]];
                if child.len() > LEAF {
                    large(d, child, depth + 1, level + 1, limit, c);
                } else if child.len() > 1 {
                    leaf(d, child, depth + 1, c);
                }
            }
        }
        if big == 0 {
            return;
        }
        a = &mut a[end[big - 1]..end[big]];
        depth += 1;
        if a.len() <= LEAF {
            leaf(d, a, depth, c);
            return;
        }
    }
}

pub fn sort(d: &Dictionary<'_>, a: &mut [PackedTail], depth: usize, c: &mut Counts) {
    if a.len() <= LEAF {
        leaf(d, a, depth, c);
    } else {
        large(d, a, depth, 1, frames(a.len()), c);
    }
}
