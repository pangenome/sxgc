#[path = "../src/overlap_compact_order.rs"]
mod compact;
#[path = "../src/overlap_range_order.rs"]
mod overlap_range_order;
use overlap_range_order as reference;
use std::collections::BTreeMap;

struct Input {
    words: Vec<Vec<u8>>,
    starts: Vec<u64>,
    bytes: Vec<u8>,
    mass: Vec<u64>,
}
impl Input {
    fn new(mut words: Vec<Vec<u8>>) -> Self {
        words.last_mut().unwrap().push(1);
        let mut bytes = vec![];
        let mut starts = vec![0];
        for w in &words {
            bytes.extend(w);
            bytes.push(0);
            starts.push(bytes.len() as u64)
        }
        let mut mass: Vec<_> = (0..words.len()).map(|i| 1 + (i % 5) as u64).collect();
        *mass.last_mut().unwrap() = 1;
        Self {
            words,
            starts,
            bytes,
            mass,
        }
    }
    fn dict(&self) -> reference::Dictionary<'_> {
        reference::Dictionary {
            starts: &self.starts,
            bytes: &self.bytes,
            occurrences: &self.mass,
            eof: self.mass.len() - 1,
        }
    }
    fn verify(&self, slots: usize, passes: usize, rows: u64) -> compact::Counts {
        let d = self.dict();
        let mut brute = BTreeMap::<Vec<u8>, Vec<_>>::new();
        let mut total = reference::Load::default();
        for (i, w) in self.words.iter().enumerate() {
            for j in 0..w.len() - if i == d.eof { 1 } else { 8 } {
                brute.entry(w[j..].to_vec()).or_default().push((i, j));
                total.rows += self.mass[i];
                total.descriptors += 1
            }
        }
        let s = compact::shortest_census(&d, 9 * (self.words.len() - 1), || {}).unwrap();
        assert_eq!(s.entries, self.words.len() - 1);
        assert_eq!(s.payload_bytes, 9 * s.entries);
        assert_eq!(s.largest_class, brute.values().map(Vec::len).max().unwrap());
        let mut prev = vec![];
        let expected: Vec<_> = brute
            .into_iter()
            .map(|(key, ids)| {
                let h = prev.iter().zip(&key).take_while(|(a, b)| a == b).count();
                prev = key;
                (h, ids)
            })
            .collect();
        let budget = compact::fixed_bytes(passes).unwrap() + 8 * slots;
        let mut it = expected.iter();
        let c = compact::order(
            &d,
            compact::Limits {
                workspace_bytes: budget,
                passes,
                rows_per_range: rows,
            },
            |g, h| {
                let mut ids: Vec<_> = g
                    .iter()
                    .map(|t| (t.word as usize, t.offset as usize))
                    .collect();
                ids.sort_unstable();
                assert_eq!(&(h, ids), it.next().unwrap());
            },
        )
        .unwrap();
        assert!(it.next().is_none());
        assert_eq!(c.total, total);
        assert!(c.census_passes + c.distributions <= passes);
        assert!(c.peak.rows <= rows && c.peak.descriptors <= slots);
        let run_reference = |bytes, rows| {
            let mut it = expected.iter();
            reference::order(
                &d,
                reference::Limits {
                    workspace_bytes: bytes,
                    prefix_bits: 8,
                    distributions: passes - 1,
                    rows_per_range: rows,
                },
                |g, h| {
                    let mut ids: Vec<_> = g.iter().map(|t| (t.word, t.offset)).collect();
                    ids.sort_unstable();
                    assert_eq!(&(h, ids), it.next().unwrap());
                },
            )
            .map(|r| {
                assert!(it.next().is_none());
                assert_eq!(r.total, c.total);
                r.ranges + 1
            })
        };
        let matched = run_reference(budget, rows);
        if matched.is_err() {
            assert_eq!(
                run_reference(4096 + 16 * total.descriptors, u64::MAX),
                Ok(2)
            )
        }
        eprintln!("budget={budget} compact={c:?} matched_reference_passes={matched:?}");
        c
    }
}
#[test]
fn adaptive_skew_ties_and_long_prefix() {
    // Families with a 96-byte shared prefix, exact duplicates, and proper-prefix tails.
    let mut words = vec![];
    for i in 0..6 {
        for _ in 0..2 {
            let mut w = vec![64; 96];
            w.push(50 + i);
            w.extend([64; 8]);
            words.push(w)
        }
    }
    words.push(vec![64; 32]);
    let c = Input::new(words).verify(50, 256, 180);
    assert!(c.census_passes > 1 && c.distributions > 1 && c.groups < c.total.descriptors);
}
#[test]
fn shortest_counts_the_ninth_byte_not_trigger_or_occurrence_mass() {
    let mut input = Input::new(vec![
        [vec![22; 3], vec![40; 9]].concat(),
        [vec![33; 5], vec![40; 9]].concat(),
        [vec![33; 3], vec![40; 8]].concat(),
        vec![40; 9], // EOF word ends in END, not another member of the ordinary tie
    ]);
    input.mass = vec![9_000_000_000, 3, 2, 1];
    let d = input.dict();
    assert_eq!(
        compact::shortest_census(&d, 27, || {})
            .unwrap()
            .largest_class,
        2
    );
    assert_eq!(
        compact::shortest_census(&d, 26, || {}),
        Err(compact::Refusal::Budget)
    );
    let only = Input::new(vec![vec![255]]);
    assert_eq!(
        compact::shortest_census(&only.dict(), 0, || {})
            .unwrap()
            .largest_class,
        1
    );
    let mut bad = input.bytes.clone();
    bad[2] = 1; // EOF marker in ordinary data would invalidate the singleton argument
    assert_eq!(
        compact::shortest_census(&reference::Dictionary { bytes: &bad, ..d }, 27, || {}),
        Err(compact::Refusal::Dictionary)
    );
}
#[test]
fn refusals_are_atomic() {
    let input = Input::new(vec![vec![9; 9], vec![9; 9], vec![9, 9]]);
    let d = input.dict();
    let run = |passes, slots: usize, rows| {
        compact::order(
            &d,
            compact::Limits {
                passes,
                workspace_bytes: compact::fixed_bytes(passes).unwrap() + slots * 8,
                rows_per_range: rows,
            },
            |_, _| panic!(),
        )
        .unwrap_err()
    };
    assert_eq!(
        run(20, 1, 9),
        compact::Refusal::ExactClass(reference::Load {
            descriptors: 2,
            rows: 3
        })
    );
    assert_eq!(run(20, 3, 2), run(20, 1, 9));
    assert_eq!(run(2, 3, 5), compact::Refusal::PassCap);
    assert_eq!(run(8, 0, 9), compact::Refusal::Budget);
    let check = |d| {
        compact::order(
            &d,
            compact::Limits {
                passes: 10,
                workspace_bytes: 65536,
                rows_per_range: u64::MAX,
            },
            |_, _| panic!(),
        )
        .unwrap_err()
    };
    assert_eq!(
        check(reference::Dictionary {
            starts: &[0, 9, u64::MAX, 1],
            ..d
        }),
        compact::Refusal::Dictionary
    );
    assert_eq!(
        check(reference::Dictionary {
            occurrences: &[u64::MAX, 2, 1],
            ..d
        }),
        compact::Refusal::Overflow
    );
}
#[test]
fn seeded_varied_shapes() {
    let mut s = 123u32;
    let mut byte = || {
        s = s.wrapping_mul(1664525).wrapping_add(1013904223);
        (s >> 24) as u8
    };
    for alphabet in [2, 4, 15, 253] {
        let words = (0..80)
            .map(|_| {
                (0..10 + byte() % 48)
                    .map(|_| 2 + byte() % alphabet)
                    .collect()
            })
            .collect();
        Input::new(words).verify(100, 256, 500);
    }
}

/// Reads 256 evenly spaced 64-byte windows, not the whole corpus. Words are
/// stand-ins for prepared dictionary words, not trigger-aligned PFP columns.
#[test]
#[ignore]
fn distributed_windows() {
    use std::io::{Read, Seek, SeekFrom};
    let mut file = std::fs::File::open(std::env::var("COMPACT_SAMPLE").unwrap()).unwrap();
    let size = file.metadata().unwrap().len();
    let words = (0..256)
        .map(|i| {
            let mut buf = [0; 64];
            file.seek(SeekFrom::Start(i * (size - 64) / 255)).unwrap();
            file.read_exact(&mut buf).unwrap();
            buf.into_iter().map(|b| 2 + b % 254).collect()
        })
        .collect();
    Input::new(words).verify(4096, 32, 16000);
}
