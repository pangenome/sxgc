//! Small local-order crossover. No path/size/profile for a Pile input, parse or product.
#[allow(dead_code)]
#[path = "../xsa/pfp/src/overlap_compact_order.rs"]
mod compact;
#[allow(dead_code)]
#[path = "../xsa/pfp/src/overlap_range_order.rs"]
mod overlap_range_order;
use compact::{LocalOrder, PackedTail};
use overlap_range_order::Dictionary;
use std::{
    collections::BTreeMap,
    io::Read,
    path::Path,
    time::{Instant, SystemTime, UNIX_EPOCH},
};

const WINDOWS: usize = 64;
const WIDTH: usize = 3072;
const W: usize = 8;
const PASSES: usize = 32;
type Words = BTreeMap<Vec<u8>, u64>;

struct Gen(u64);
impl Gen {
    fn take(&mut self) -> u8 {
        self.0 = self
            .0
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        (self.0 >> 35) as u8
    }
}
fn between_triggers(text: &[u8], width: usize) -> Words {
    let mut words = Words::new();
    for window in text.chunks_exact(width) {
        let mut prev = None;
        for (i, w) in window.windows(W).enumerate().skip(1) {
            let h = w.iter().fold(1469598103934665603u64, |h, &v| {
                (h ^ u64::from(v)).wrapping_mul(1099511628211)
            });
            if h % 31 == 0 {
                if let Some(j) = prev {
                    *words.entry(window[j..i + W].to_vec()).or_default() += 1;
                }
                prev = Some(i);
            }
        }
    }
    words
}
fn shared_words() -> Words {
    let mut r = Gen(0xf1bb113);
    let mut words = Words::new();
    // Unique ordinary words, 128 families of long identical suffixes, different
    // predecessor bytes and unrelated occurrence masses. Not trigger-aligned text.
    let tails: Vec<Vec<u8>> = (0..128)
        .map(|i| (0..24 + i % 17).map(|_| r.take() % 32).collect())
        .collect();
    for i in 0..12288usize {
        let mut w = vec![(i / 128) as u8, (i % 128) as u8];
        let l = 12 + r.take() % 30;
        w.extend((0..l).map(|_| r.take() % 65));
        w.extend_from_slice(&tails[i % 128]);
        assert!(words.insert(w, 1 + (i % 3) as u64).is_none());
    }
    words
}
struct Deck {
    data: Vec<u8>,
    starts: Vec<u64>,
    mass: Vec<u64>,
    base: Vec<usize>,
    rows: u64,
}
impl Deck {
    fn new(words: Words) -> Self {
        assert!(!words.is_empty());
        let mut used = [false; 256];
        for w in words.keys() {
            assert!(w.len() > W && w.len() <= 8192);
            for &b in w {
                used[b as usize] = true;
            }
        }
        let mut code = [0; 256];
        for (i, b) in (0..256).filter(|&b| used[b]).enumerate() {
            code[b] = u8::try_from(i + 2).unwrap();
        }
        let mut d = Self {
            data: Vec::new(),
            starts: vec![0],
            mass: Vec::new(),
            base: vec![0],
            rows: 0,
        };
        for (w, count) in words {
            d.data.extend(w.iter().map(|&b| code[b as usize]));
            d.data.push(0);
            d.starts.push(d.data.len() as u64);
            d.mass.push(count);
            let owned = w.len() - W;
            d.base.push(d.base.last().unwrap() + owned);
            d.rows += owned as u64 * count;
        }
        // One distinct one-byte EOF word. We're verifying finite classes, not stitching windows into a parse.
        d.data.extend([2, 1, 0]);
        d.starts.push(d.data.len() as u64);
        d.mass.push(1);
        d.base.push(d.base.last().unwrap() + 1);
        d.rows += 1;
        assert!(d.data.len() < 2 << 20 && *d.base.last().unwrap() <= 1 << 20);
        d
    }
    fn dict(&self) -> Dictionary<'_> {
        Dictionary {
            bytes: &self.data,
            starts: &self.starts,
            occurrences: &self.mass,
            eof: self.mass.len() - 1,
        }
    }
}
#[repr(C)]
struct Timespec {
    i: i64,
    ns: i64,
}
unsafe extern "C" {
    fn clock_gettime(clock: i32, t: *mut Timespec) -> i32;
}
fn cpu() -> f64 {
    let mut t = Timespec { i: 0, ns: 0 };
    assert_eq!(unsafe { clock_gettime(2, &mut t) }, 0);
    t.i as f64 + t.ns as f64 / 1e9
}

fn crossover(name: &str, deck: Deck, guard: &impl Fn()) {
    let d = deck.dict();
    let u = *deck.base.last().unwrap();
    // Match the archived emitting order's descriptor slot/input ratio for bits.
    // Give bytes exactly the same payload grant; it must buy its own frames.
    let fixed = compact::fixed_bytes(PASSES).unwrap();
    let slots100 = ((64 << 20) - 8192 * 20 - fixed) / 8;
    let slots = (u as u64 * slots100 as u64).div_ceil(95_434_516) as usize;
    let budget = compact::workspace_for(PASSES, slots, LocalOrder::Bits).unwrap();
    let mut shape = None;
    for (rep, algs) in [
        (1, [LocalOrder::Bits, LocalOrder::Bytes]),
        (2, [LocalOrder::Bytes, LocalOrder::Bits]),
    ] {
        for how in algs {
            guard();
            let mut seen = vec![0u64; u.div_ceil(64)];
            let mut previous: Option<PackedTail> = None;
            let mut checked = 0;
            let mut rows = 0;
            let mut largest = 0;
            let start = Instant::now();
            let cpu0 = cpu();
            let c = compact::order_checked_with(
                &d,
                compact::Limits {
                    workspace_bytes: budget,
                    rows_per_range: deck.rows,
                    passes: PASSES,
                },
                how,
                guard,
                |g, h| {
                    let key = d.key(g[0].into());
                    if let Some(p) = previous {
                        let last = d.key(p.into());
                        assert!(last < key);
                        assert_eq!(h, last.iter().zip(key).take_while(|(x, y)| x == y).count());
                    } else {
                        assert_eq!(h, 0);
                    }
                    for &t in g {
                        let v = t.word as usize;
                        let i = deck.base[v] + t.offset as usize;
                        assert!(i < deck.base[v + 1]);
                        assert_eq!(key, d.key(t.into()));
                        let bit = 1u64 << (i % 64);
                        assert_eq!(seen[i / 64] & bit, 0);
                        seen[i / 64] |= bit;
                        rows += deck.mass[v];
                        checked += 1;
                    }
                    largest = largest.max(g.len());
                    previous = Some(g[0]);
                },
            )
            .unwrap();
            assert_eq!(
                (checked, c.total.descriptors, rows, c.total.rows),
                (u, u, deck.rows, deck.rows)
            );
            assert_eq!(
                seen.iter().map(|n| n.count_ones() as usize).sum::<usize>(),
                u
            );
            let wall = start.elapsed().as_secs_f64();
            let cpu_s = cpu() - cpu0;
            let s = (c.groups, largest);
            if let Some(old) = shape {
                assert_eq!(s, old);
            }
            shape = Some(s);
            println!("CHECKED shape={name} rep={rep} order={how:?} dictionary_bytes={} words={} descriptors={u} rows={rows} groups={} largest={largest} workspace={budget} peak_slots={} scans={} distributions={} digit_reads={} byte_partitions={} frames_peak={} frame_reserve={} leaf_comparisons={} shifted_slots={} wall_s={wall:.9} cpu_s={cpu_s:.9}",d.bytes.len(),d.occurrences.len(),c.groups,c.peak.descriptors,c.census_passes,c.distributions,c.radix_digit_reads,c.byte_partitions,c.byte_frames_peak,c.byte_stack_reserved,c.leaf_comparisons,c.leaf_shifted_slots);
        }
    }
}
fn main() {
    let args: Vec<_> = std::env::args().collect();
    assert!(
        cfg!(all(target_os = "linux", target_pointer_width = "64"))
            && args.len() == 4
            && args[1] == "--fixed-small-windows"
    );
    let end = args[3].parse::<u64>().unwrap();
    assert!(end <= 1795531169);
    let guard = || {
        if Path::new(&args[2]).try_exists().unwrap()
            || SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_secs()
                >= end
        {
            std::process::exit(75);
        }
    };
    guard();
    let mut input = vec![0; WINDOWS * WIDTH];
    let mut f = std::io::stdin().lock();
    f.read_exact(&mut input).unwrap();
    assert_eq!(f.read(&mut [0]).unwrap(), 0);
    crossover("book", Deck::new(between_triggers(&input, WIDTH)), &guard);
    let mut r = Gen(4910553);
    let random: Vec<_> = (0..1 << 20).map(|_| r.take() % 243).collect();
    crossover("wide", Deck::new(between_triggers(&random, 16384)), &guard);
    crossover("families", Deck::new(shared_words()), &guard);
    guard();
}
