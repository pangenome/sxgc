#[path = "../src/overlap_range_order.rs"]
mod overlap_range_order;
use overlap_range_order::*;
use std::{collections::BTreeMap, mem::size_of};

fn limits(bits: u32, slots: usize, rows: u64, scans: usize) -> Limits {
    Limits {
        prefix_bits: bits,
        workspace_bytes: (1 << bits) * size_of::<Load>() + slots * size_of::<Tail>(),
        rows_per_range: rows,
        distributions: scans,
    }
}

// Oracle materializes suffix strings from raw words, not from dictionary starts.
fn oracle(raw: &[Vec<u8>], eof: usize, mass: &[u64], lim: Limits) -> Census {
    let mut starts = vec![0];
    let mut bytes = vec![];
    let mut expected = BTreeMap::<Vec<u8>, Vec<(usize, usize)>>::new();
    let mut rows = 0u64;
    for (i, w) in raw.iter().enumerate() {
        bytes.extend(w);
        if i == eof {
            bytes.push(1)
        }
        bytes.push(0);
        starts.push(bytes.len() as u64);
        for j in 0..if i == eof { w.len() } else { w.len() - 8 } {
            let mut key = w[j..].to_vec();
            if i == eof {
                key.push(1)
            }
            expected.entry(key).or_default().push((i, j));
            rows += mass[i];
        }
    }
    let count = expected.values().map(Vec::len).sum();
    let mut expected = expected.into_iter();
    let mut prev = vec![];
    let d = Dictionary {
        bytes: &bytes,
        starts: &starts,
        occurrences: mass,
        eof,
    };
    let c = order(&d, lim, |group, h| {
        let (key, ids) = expected.next().unwrap();
        let mut got: Vec<_> = group
            .iter()
            .map(|&t| {
                assert_eq!(d.key(t), key);
                (t.word, t.offset)
            })
            .collect();
        got.sort();
        assert_eq!(got, ids);
        assert_eq!(
            h,
            (0..prev.len().min(key.len()))
                .find(|&i| prev[i] != key[i])
                .unwrap_or(prev.len().min(key.len()))
        );
        prev = key;
    })
    .unwrap();
    assert!(expected.next().is_none());
    assert_eq!(
        c.total,
        Load {
            descriptors: count,
            rows
        }
    );
    c
}
#[test]
fn tied_owned_tails_eof_and_nonzero_seam() {
    // Prefix 42,43 and 42,44 must land on opposite sides of a cut; the
    // duplicated 42,43 tail contributes two occurrence lists, not one row.
    let raw = [
        vec![90, 42, 43, 7, 7, 7, 7, 7, 7, 7],
        vec![42, 44],
        vec![91, 42, 43, 7, 7, 7, 7, 7, 7, 7],
    ];
    let c = oracle(&raw, 1, &[3, 1, 5], limits(16, 2, 8, 10));
    assert_eq!(
        c.peak,
        Load {
            descriptors: 2,
            rows: 8
        }
    );
    assert_eq!(c.ranges, 3);
}
#[test]
fn distributed_small_oracle() {
    let mut s = 23u32;
    let mut byte = || {
        s = s.wrapping_mul(1664525).wrapping_add(1013904223);
        2 + ((s >> 24) % 251) as u8
    };
    let raw: Vec<Vec<_>> = (0..53)
        .map(|i| (0..9 + i % 19).map(|_| byte()).collect())
        .collect();
    let mut mass: Vec<_> = (0..53).map(|i| 1 + i % 7).collect();
    mass[30] = 1;
    assert!(oracle(&raw, 30, &mass, limits(12, 22, 90, 100)).ranges > 20);
}
#[test]
fn refuses_before_emission() {
    let bytes = b"zzzzzzzzzz\0z\x01\0";
    let starts = [0, 11, 14];
    let mass = [12, 1];
    let d = Dictionary {
        bytes,
        starts: &starts,
        occurrences: &mass,
        eof: 1,
    };
    let run = |lim| order(&d, lim, |_, _| panic!("output before complete admission")).unwrap_err();
    let skew = Refusal::Bucket {
        index: 0x7a7a,
        load: Load {
            descriptors: 2,
            rows: 24,
        },
    };
    assert_eq!(run(limits(16, 1, 25, 3)), skew);
    assert_eq!(run(limits(16, 8, 23, 3)), skew);
    assert_eq!(
        run(limits(16, 2, 24, 1)),
        Refusal::Distributions { needed: 2 }
    );
    assert_eq!(
        run(Limits {
            workspace_bytes: 0,
            ..limits(16, 8, 24, 3)
        }),
        Refusal::Budget
    );
    assert_eq!(
        order(
            &Dictionary {
                occurrences: &[u64::MAX, 1],
                ..d
            },
            limits(8, 10, u64::MAX, 10),
            |_, _| panic!()
        )
        .unwrap_err(),
        Refusal::Overflow
    );
    assert_eq!(
        order(
            &Dictionary {
                starts: &[0, u64::MAX, 14],
                ..d
            },
            limits(8, 8, 24, 3),
            |_, _| panic!()
        )
        .unwrap_err(),
        Refusal::Dictionary
    );
}

#[test]
fn continuation_consumer_matches_raw_suffix_rows() {
    // Exact 8-byte overlap. The 9ABCDEFGH finite tails agree across word IDs;
    // successor suffixes interleave occurrences of both words.
    let trigger = b"ABCDEFGH";
    let raw = [
        [&trigger[..], b"a9", trigger].concat(),
        [&trigger[..], b"b9", trigger].concat(),
        [&trigger[..], b"x"].concat(),
    ];
    let parse = [0, 1, 0, 0, 1, 2];
    let mut text = vec![];
    let mut at = vec![];
    let mut mass = [0; 3];
    for word in parse {
        at.push(text.len());
        mass[word] += 1;
        let w = &raw[word];
        text.extend_from_slice(&w[..if word == 2 { w.len() } else { w.len() - 8 }]);
    }
    at.push(text.len());
    let end = text.len();
    text.push(1);
    let mut bytes = vec![];
    let mut starts = vec![0];
    for (i, w) in raw.iter().enumerate() {
        bytes.extend(w);
        if i == 2 {
            bytes.push(1)
        }
        bytes.push(0);
        starts.push(bytes.len() as u64);
    }
    let d = Dictionary {
        bytes: &bytes,
        starts: &starts,
        occurrences: &mass,
        eof: 2,
    };
    let common = |a: usize, b: usize| {
        text[a..]
            .iter()
            .zip(&text[b..])
            .take_while(|(l, r)| l == r)
            .count()
    };
    let mut rows = vec![(end, 0)];
    let c = order(&d, limits(16, 6, 16, 16), |group, head| {
        let len = d.key(group[0]).len();
        let mut tied = vec![];
        for t in group {
            for (i, &w) in parse.iter().enumerate() {
                if w == t.word {
                    tied.push((at[i + 1], at[i] + t.offset));
                }
            }
        }
        tied.sort_by(|a, b| text[a.0..].cmp(&text[b.0..]));
        for (i, &(next, pos)) in tied.iter().enumerate() {
            rows.push((
                pos,
                if i == 0 {
                    head
                } else {
                    len - 8 + common(tied[i - 1].0, next)
                },
            ));
        }
    })
    .unwrap();
    assert!(c.ranges > 1);
    let mut sa: Vec<_> = (0..=end).collect();
    sa.sort_by(|&a, &b| text[a..].cmp(&text[b..]));
    assert_eq!(
        rows,
        sa.iter()
            .enumerate()
            .map(|(i, &pos)| (pos, if i == 0 { 0 } else { common(sa[i - 1], pos) }))
            .collect::<Vec<_>>()
    );
}
