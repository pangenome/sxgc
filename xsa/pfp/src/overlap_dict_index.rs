//! Resident dictionary order/ownership. All coordinates in the packed
//! dictionary can be wide without making the parse IDs or word offsets wide.
#[link(name = "libsais_r", kind = "static")]
extern "C" {
    fn libsais(t: *const u8, sa: *mut i32, n: i32, fs: i32, freq: *mut i32) -> i32;
    fn libsais64(t: *const u8, sa: *mut i64, n: i64, fs: i64, freq: *mut i64) -> i64;
}
#[cfg(dict_openmp)]
extern "C" {
    fn libsais_omp(
        t: *const u8,
        sa: *mut i32,
        n: i32,
        fs: i32,
        freq: *mut i32,
        threads: i32,
    ) -> i32;
    fn libsais64_omp(
        t: *const u8,
        sa: *mut i64,
        n: i64,
        fs: i64,
        freq: *mut i64,
        threads: i64,
    ) -> i64;
    fn omp_get_max_threads() -> i32;
    fn omp_get_thread_limit() -> i32;
}

pub struct DictSort {
    threads: i32,
    wide: bool,
}
impl DictSort {
    pub fn from_env(wide: bool) -> Self {
        let requested = match std::env::var("XSA_RESIDENT_DICT_THREADS") {
            Ok(s) => s.parse::<i32>().expect("invalid XSA_RESIDENT_DICT_THREADS"),
            Err(std::env::VarError::NotPresent) => 1,
            Err(_) => panic!("invalid XSA_RESIDENT_DICT_THREADS"),
        };
        Self::requested(wide, requested)
    }
    #[cfg(feature = "dict-grid")]
    pub fn selected_threads(&self) -> i32 {
        self.threads
    }
    pub fn requested(wide: bool, requested: i32) -> Self {
        assert!(
            (1..=8).contains(&requested),
            "XSA_RESIDENT_DICT_THREADS must be 1..=8"
        );
        let threads = if requested == 1 {
            1
        } else {
            #[cfg(not(dict_openmp))]
            panic!("parallel dictionary sort requires XSA_BUILD_DICT_OPENMP=1");
            #[cfg(dict_openmp)]
            {
                let cpus = std::thread::available_parallelism().map_or(1, |n| n.get());
                unsafe {
                    requested
                        .min(cpus.min(8) as i32)
                        .min(omp_get_max_threads())
                        .min(omp_get_thread_limit())
                }
            }
        };
        Self { threads, wide }
    }
    pub fn workspace_bytes(&self, n: usize) -> usize {
        if self.threads == 1 || n <= 1 {
            return 0;
        }
        // libsais{,64}_alloc_thread_state: three 4096-aligned allocations.
        // Wide API uses 32-bit buckets/cache as long as n fits in i32.
        let w = if n > i32::MAX as usize { 8 } else { 4 };
        self.threads as usize * (64 + (4 * 256 + 2 * 24576) * w) + 3 * (4096 + 1)
    }
    pub fn sort(&self, bytes: &[u8]) -> Suffixes {
        let n = bytes.len();
        let kernel = match (n, self.wide) {
            (0 | 1, _) => "trivial",
            (_, false) => "narrow",
            (_, true) if n <= i32::MAX as usize => "wide_via_narrow",
            _ => "native_wide",
        };
        eprintln!("RESIDENT_DICT_SORT kernel={kernel} wide={} selected_threads={} fs=0 thread_workspace_bytes={}", self.wide, self.threads, self.workspace_bytes(n));
        if self.wide {
            let n = i64::try_from(n).expect("signed 64-bit dictionary");
            let mut sa = vec![0; bytes.len()];
            let status = unsafe {
                match self.threads {
                    #[cfg(dict_openmp)]
                    t if t > 1 => libsais64_omp(
                        bytes.as_ptr(),
                        sa.as_mut_ptr(),
                        n,
                        0,
                        std::ptr::null_mut(),
                        i64::from(t),
                    ),
                    _ => libsais64(bytes.as_ptr(), sa.as_mut_ptr(), n, 0, std::ptr::null_mut()),
                }
            };
            assert_eq!(status, 0);
            Suffixes::Wide(sa)
        } else {
            let n = i32::try_from(n).expect("signed 32-bit dictionary");
            let mut sa = vec![0; bytes.len()];
            let status = unsafe {
                match self.threads {
                    #[cfg(dict_openmp)]
                    t if t > 1 => libsais_omp(
                        bytes.as_ptr(),
                        sa.as_mut_ptr(),
                        n,
                        0,
                        std::ptr::null_mut(),
                        t,
                    ),
                    _ => libsais(bytes.as_ptr(), sa.as_mut_ptr(), n, 0, std::ptr::null_mut()),
                }
            };
            assert_eq!(status, 0);
            Suffixes::Narrow(sa)
        }
    }
}
pub enum Suffixes {
    Narrow(Vec<i32>),
    Wide(Vec<i64>),
}
impl Suffixes {
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
    fn dict_sort_exact_order() {
        let mut seed = 123456789u32;
        let random = (0..131073)
            .map(|_| {
                seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
                (seed >> 24) as u8
            })
            .collect();
        for t in [
            vec![],
            vec![255],
            b"aaaabaaaa\0a\xffaaa\x01\0".to_vec(),
            vec![0; 65536],
            b"abacaba\0".repeat(12000),
            (0..=255).rev().collect(),
            random,
        ] {
            let narrow = DictSort {
                threads: 1,
                wide: false,
            }
            .sort(&t);
            // Prove permutation and adjacent suffix order independently: with
            // sentinel rank zero, each suffix is (head, rank of shorter tail).
            let mut rank = vec![0; t.len() + 1];
            for r in 0..t.len() {
                let pos = narrow.at(r);
                assert!(pos < t.len() && rank[pos] == 0);
                rank[pos] = 1 + r;
            }
            let key = |p| (t[p], rank[p + 1]);
            assert!((1..t.len()).all(|r| key(narrow.at(r - 1)) < key(narrow.at(r))));
            let wide = DictSort {
                threads: 1,
                wide: true,
            }
            .sort(&t);
            assert!((0..t.len()).all(|r| narrow.at(r) == wide.at(r)));
            #[cfg(dict_openmp)]
            for (is_wide, serial) in [(false, narrow), (true, wide)] {
                let parallel = DictSort {
                    threads: 2,
                    wide: is_wide,
                }
                .sort(&t);
                // Equality at fixed integer width implies identical native bytes.
                match (serial, parallel) {
                    (Suffixes::Narrow(a), Suffixes::Narrow(b)) => assert_eq!(a, b),
                    (Suffixes::Wide(a), Suffixes::Wide(b)) => assert_eq!(a, b),
                    _ => panic!("changed SA width"),
                }
            }
        }
    }
    #[test]
    fn dict_workspace_width_boundary() {
        let boundary = i32::MAX as usize;
        let sort = DictSort {
            threads: 2,
            wide: true,
        };
        assert_eq!(sort.workspace_bytes(1), 0);
        assert_eq!(sort.workspace_bytes(boundary), 2 * 200768 + 3 * 4097);
        assert_eq!(sort.workspace_bytes(boundary + 1), 2 * 401472 + 3 * 4097);
    }
}
