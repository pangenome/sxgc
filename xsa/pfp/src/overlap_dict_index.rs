//! Resident dictionary order/ownership. All coordinates in the packed
//! dictionary can be wide without making the parse IDs or word offsets wide.
#[link(name = "libsais_r", kind = "static")]
extern "C" {
    fn libsais(t: *const u8, sa: *mut i32, n: i32, fs: i32, freq: *mut i32) -> i32;
    fn libsais64(t: *const u8, sa: *mut i64, n: i64, fs: i64, freq: *mut i64) -> i64;
}
pub enum Suffixes {
    Narrow(Vec<i32>),
    Wide(Vec<i64>),
}
impl Suffixes {
    pub fn sort(bytes: &[u8], wide: bool) -> Self {
        if wide {
            let n = i64::try_from(bytes.len()).expect("signed 64-bit dictionary");
            let mut sa = vec![0; bytes.len()];
            assert_eq!(
                unsafe { libsais64(bytes.as_ptr(), sa.as_mut_ptr(), n, 0, std::ptr::null_mut()) },
                0
            );
            Self::Wide(sa)
        } else {
            let n = i32::try_from(bytes.len()).expect("signed 32-bit dictionary");
            let mut sa = vec![0; bytes.len()];
            assert_eq!(
                unsafe { libsais(bytes.as_ptr(), sa.as_mut_ptr(), n, 0, std::ptr::null_mut()) },
                0
            );
            Self::Narrow(sa)
        }
    }
    #[inline]
    pub fn at(&self, rank: usize) -> usize {
        match self {
            Self::Narrow(s) => usize::try_from(s[rank]).unwrap(),
            Self::Wide(s) => usize::try_from(s[rank]).unwrap(),
        }
    }
}
// Rank of word starts at/before a position. The owner is rank-1 even when
// the word crosses many blocks. The exclusive dictionary end is NOT a start.
// One 16-byte record / 64 symbols replaces one 4-byte owner / symbol.
#[derive(Default, Copy, Clone)]
pub struct Block {
    mask: u64,
    before: u32,
}
pub struct RankStarts(Vec<Block>);
impl RankStarts {
    pub fn bytes(d: usize) -> usize {
        d.div_ceil(64) * std::mem::size_of::<Block>()
    }
    pub fn new(starts: &[u64], d: usize) -> Self {
        assert_eq!(starts.first(), Some(&0));
        assert_eq!(starts.last(), Some(&(d as u64)));
        assert!(starts.windows(2).all(|w| w[0] < w[1]));
        let mut blocks = vec![Block::default(); d.div_ceil(64)];
        for &p in &starts[..starts.len() - 1] {
            blocks[p as usize / 64].mask |= 1 << (p % 64);
        }
        let mut n = 0u32;
        for b in &mut blocks {
            b.before = n;
            n = n
                .checked_add(b.mask.count_ones())
                .expect("wide word IDs required");
        }
        assert_eq!(n as usize, starts.len() - 1);
        Self(blocks)
    }
    #[inline]
    pub fn word(&self, p: usize) -> usize {
        let b = self.0[p / 64];
        (b.before + (b.mask & (u64::MAX >> (63 - p % 64))).count_ones() - 1) as usize
    }
}
pub fn local_offset(pos: usize, start: u64) -> u32 {
    u32::try_from((pos as u64).checked_sub(start).unwrap()).expect("word offset too wide")
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn sparse_and_dense_boundaries() {
        let starts = [0, 1, 2, 63, 64, 65, 255, 802, 803];
        let idx = RankStarts::new(&starts, 803);
        assert_eq!(RankStarts::bytes(803), 13 * 16);
        for p in 0..803 {
            assert_eq!(idx.word(p), starts.partition_point(|&x| x <= p as u64) - 1);
        }
    }
    #[test]
    fn high_base_short_offset() {
        for base in [i32::MAX as u64 - 7, u32::MAX as u64 - 7, 1u64 << 40] {
            assert_eq!(local_offset((base + 200) as usize, base), 200);
        }
    }
    #[test]
    fn wide_abi_same_order() {
        let t = b"aaaabaaaa\x00a\xffaaa\x01\x00";
        let x = Suffixes::sort(t, false);
        let y = Suffixes::sort(t, true);
        for i in 0..t.len() {
            assert_eq!(x.at(i), y.at(i));
        }
    }
}
