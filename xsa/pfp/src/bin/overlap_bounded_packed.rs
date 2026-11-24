//! Fixture-only row43 producer; no resident/default dispatch.
#[path = "../overlap_min.rs"]
mod min;
#[path = "../overlap_range_order.rs"]
mod range;
#[path = "../overlap_row43.rs"]
mod row43;
#[allow(dead_code)]
#[path = "../overlap_wide_parse.rs"]
mod wide;
use range::{Dictionary, Tail};
use std::{
    cmp::Reverse,
    collections::BinaryHeap,
    fs::File,
    io::{BufReader, BufWriter, Read, Write},
    mem::size_of,
    path::Path,
};
const FIXTURE: usize = 1 << 20;
const BITS: u32 = 16;
const W: usize = 8;
fn stop(flag: &Path) {
    if flag.try_exists().unwrap() {
        std::process::exit(75)
    }
}
fn word(f: &mut impl Read) -> u32 {
    let mut b = [0; 4];
    f.read_exact(&mut b).unwrap();
    u32::from_le_bytes(b)
}
fn end(f: &mut impl Read) {
    assert_eq!(f.read(&mut [0]).unwrap(), 0, "trailing bytes");
}
fn common(a: &[u8], b: &[u8]) -> usize {
    a.iter().zip(b).take_while(|(x, y)| x == y).count()
}
struct Packed {
    bytes: Vec<u8>,
    starts: Vec<u64>,
    decode: [u8; 256],
}
impl Packed {
    // Compact forward in the original allocation: a four-byte header pays for SEP and optional END.
    fn load(mut bytes: Vec<u8>, k: usize, eof: usize) -> Self {
        let mut used = [false; 256];
        let mut starts = Vec::with_capacity(k + 1);
        let mut i = 0;
        let mut prior = None;
        for v in 0..k {
            let header = word(&mut bytes.get(i..).unwrap());
            let len = (header & 0x7fffffff) as usize;
            let final_word = header >> 31 != 0;
            assert!(
                final_word == (eof == v) && len > if final_word { 0 } else { W },
                "invalid packed header"
            );
            let a = i + 4;
            i = a.checked_add(len).unwrap();
            let raw = bytes.get(a..i).expect("truncated packed word");
            if let Some((p, q, tag)) = prior {
                assert!(
                    (&bytes[p..q], tag) < (raw, final_word),
                    "packed lexical order"
                );
            }
            prior = Some((a, i, final_word));
            for &b in raw {
                used[b as usize] = true;
            }
        }
        assert_eq!(i, bytes.len(), "packed dictionary count");
        let mut encode = [0; 256];
        let mut decode = [0; 256];
        for (j, b) in (0..256).filter(|&b| used[b]).enumerate() {
            let c = u8::try_from(j + 2).expect("two free codes required");
            encode[b] = c;
            decode[c as usize] = b as u8;
        }
        i = 0;
        let mut dst = 0;
        for v in 0..k {
            starts.push(dst as u64);
            let len = (word(&mut &bytes[i..]) & 0x7fffffff) as usize;
            i += 4;
            for j in i..i + len {
                bytes[dst] = encode[bytes[j] as usize];
                dst += 1;
            }
            i += len;
            if v == eof {
                bytes[dst] = 1;
                dst += 1;
            }
            bytes[dst] = 0;
            dst += 1;
        }
        bytes.truncate(dst);
        starts.push(dst as u64);
        Self {
            bytes,
            starts,
            decode,
        }
    }
    fn key(&self, v: usize) -> &[u8] {
        &self.bytes[self.starts[v] as usize..self.starts[v + 1] as usize - 1]
    }
}
// rank, occurrence slot, exclusive end, word offset, interior predecessor
type Cursor = Reverse<(u32, u32, u32, u32, u8)>;
// Logical payloads, not RSS. Widen inputs before arithmetic, including the final arena addition.
#[derive(Debug, PartialEq)]
struct PhaseBytes {
    aligned: u128,
    lists: u128,
    emit: u128,
}
fn phase_bytes(m: u64, k: u64, t: u64, arena: u64) -> PhaseBytes {
    let (m, k) = (u128::from(m), u128::from(k));
    let base = u128::from(t) + 8 * (k + 1) + 8 * k;
    let tree = 16 * m.div_ceil(16);
    PhaseBytes {
        aligned: base + 28 * m + 8,
        lists: base + 37 * m + 8 + tree + 8 * (k + 1),
        emit: base + 21 * m + tree + 4 * (k + 1) + u128::from(arena),
    }
}
fn main() {
    let args: Vec<_> = std::env::args_os().collect();
    assert!(
        (args.len() == 6 || args.len() == 7) && args[1] == "--fixture",
        "--fixture COLUMNS STOP ARRAY_BYTES ARENA_BYTES [EXPECTED_RANKS]"
    );
    let dir = Path::new(&args[2]);
    let flag = Path::new(&args[3]);
    stop(flag);
    let grant: usize = args[4].to_str().unwrap().parse().unwrap();
    let arena: usize = args[5].to_str().unwrap().parse().unwrap();
    assert!(usize::BITS == 64 && arena <= grant && grant <= 200_000_000);
    let open = |name| {
        let f = File::open(dir.join(name)).unwrap();
        let len = usize::try_from(f.metadata().unwrap().len()).unwrap();
        (f, len)
    };
    let (input, mb) = open("parse.u32");
    let m = mb / 4;
    assert!(m > 0 && m <= FIXTURE && 4 * m == mb && mb <= grant);
    let mut ids = Vec::with_capacity(m);
    {
        let mut f = BufReader::new(input);
        for _ in 0..m {
            ids.push(word(&mut f));
        }
        end(&mut f);
    }
    let k = 1 + *ids.iter().max().unwrap() as usize;
    assert!(k <= m);
    let eof = ids[m - 1] as usize;
    let (mut file, t) = open("dictionary.packed");
    assert!(t <= 16 * FIXTURE);
    let parse = wide::plan(m as u64, k as u64, wide::Workspace::SixK, grant as u64).unwrap();
    let phases = phase_bytes(m as u64, k as u64, t as u64, arena as u64);
    eprintln!(
        "bounded arrays parse={} {phases:?} arena={arena}",
        parse.peak_array_bytes
    );
    assert!(
        grant as u128 >= phases.aligned.max(phases.lists).max(phases.emit),
        "phase grant"
    );
    let bins = (1 << BITS) * size_of::<range::Load>();
    let slots =
        arena.checked_sub(bins).expect("arena census") / (size_of::<Tail>() + size_of::<Cursor>());
    assert!(slots > 0, "empty arena");
    let ranks =
        wide::wide_parse_ranks(&ids, k as u64, wide::Workspace::SixK, grant as u64).unwrap();
    stop(flag);
    if let Some(path) = args.get(6) {
        let mut f = BufReader::new(File::open(path).unwrap());
        for &r in &ranks {
            assert_eq!(r, word(&mut f), "rank oracle");
        }
        end(&mut f);
    }
    let mut tape = vec![0; t];
    file.read_exact(&mut tape).unwrap();
    end(&mut file);
    drop(file);
    let packed = Packed::load(tape, k, eof);
    let width = |v| packed.key(v).len() - if v == eof { 1 } else { W };
    let mut counts = vec![0u64; k];
    let mut starts = Vec::with_capacity(m + 1);
    starts.push(0u64);
    for (j, &id) in ids.iter().enumerate() {
        let v = id as usize;
        assert_eq!(v == eof, j == m - 1);
        counts[v] += 1;
        if j > 0 {
            let p = ids[j - 1] as usize;
            assert!(
                packed.key(v).len() - usize::from(v == eof) >= W
                    && packed.key(p)[width(p)..] == packed.key(v)[..W],
                "broken packed overlap"
            );
        }
        starts.push(starts[j].checked_add(width(v) as u64).unwrap());
    }
    let n = starts[m];
    assert!(n <= FIXTURE as u64, "text exceeds fixture cap");
    assert!(counts.iter().all(|&x| x > 0), "unused lexical ID");
    stop(flag);
    let pl = wide::ParsePlcp::build(&ids, ranks, grant as u64).unwrap();
    let mut h = vec![0; m];
    pl.fill_aligned(
        &starts,
        eof as u32,
        |a, b| common(packed.key(a as usize), packed.key(b as usize)) as u64,
        &mut h,
    )
    .unwrap();
    let order = pl.into_order();
    let rmq = min::BlockMin::new(h);
    let mut loc = Vec::with_capacity(k + 1);
    loc.push(0u32);
    for &c in &counts {
        loc.push(
            loc.last()
                .unwrap()
                .checked_add(c.try_into().unwrap())
                .unwrap(),
        );
    }
    let mut cur = loc.clone();
    let mut positions = vec![0u64; m];
    let mut next = vec![0; m];
    let mut pred = vec![0u8; m];
    for (j, r) in order.continuations() {
        let j = j as usize;
        let v = ids[j] as usize;
        let i = cur[v] as usize;
        cur[v] += 1;
        positions[i] = starts[j];
        next[i] = r;
        if j > 0 {
            let w = ids[j - 1] as usize;
            pred[i] = packed.decode[packed.key(w)[width(w) - 1] as usize];
        }
    }
    assert_eq!(cur[..k], loc[1..]);
    drop(cur);
    drop(order);
    drop(ids);
    drop(starts);
    stop(flag);
    let d = Dictionary {
        bytes: &packed.bytes,
        starts: &packed.starts,
        occurrences: &counts,
        eof,
    };
    let last = packed.decode[packed.key(eof)[width(eof) - 1] as usize];
    let mut out = BufWriter::new(std::io::stdout().lock());
    let mut rows = 0u64;
    let mut heap = BinaryHeap::<Cursor>::with_capacity(slots.min(k));
    let mut previous_length = None;
    let mut max_ties = 0;
    let census = range::order(
        &d,
        range::Limits {
            workspace_bytes: bins + slots * size_of::<Tail>(),
            prefix_bits: BITS,
            rows_per_range: n,
            distributions: 64,
        },
        |ties, head| {
            if rows == 0 {
                stop(flag);
                out.write_all(&row43::encode(n, 0, last as u32)).unwrap();
                rows += 1;
            }
            assert!(ties.len() <= slots.min(k));
            let len = d.key(ties[0]).len();
            assert!(
                previous_length != Some(head),
                "finite tails not prefix-free"
            );
            previous_length = Some(len);
            max_ties = max_ties.max(ties.len());
            for &Tail { word: v, offset } in ties {
                let i = loc[v];
                let b = if offset == 0 {
                    0
                } else {
                    packed.decode[packed.key(v)[offset - 1] as usize]
                };
                heap.push(Reverse((
                    next[i as usize],
                    i,
                    loc[v + 1],
                    offset.try_into().unwrap(),
                    b,
                )));
            }
            let mut prior = None;
            while let Some(Reverse((r, i, end, off, b))) = heap.pop() {
                if rows % 4096 == 0 {
                    stop(flag);
                }
                let j = i as usize;
                let p = positions[j] + off as u64;
                let lcp = if let Some(prev) = prior {
                    assert!(prev > 0 && r > prev && len > W);
                    len - W + rmq.between(prev as usize - 1, r as usize - 1)
                } else {
                    head
                };
                prior = Some(r);
                let bwt = if off != 0 {
                    b as u32
                } else if p == 0 {
                    256
                } else {
                    pred[j] as u32
                };
                out.write_all(&row43::encode(p, lcp as u64, bwt)).unwrap();
                rows += 1;
                if i + 1 < end {
                    heap.push(Reverse((next[j + 1], i + 1, end, off, b)));
                }
            }
        },
    )
    .unwrap();
    assert_eq!((rows, census.total.rows), (n + 1, n));
    stop(flag);
    out.flush().unwrap();
    eprintln!(
        "bounded rows={rows} max_ties={max_ties} ranges={}",
        census.ranges
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn linear_dimensions_are_not_a_grant() {
        let (m, k, d) = (3_181_644_880, 2_344_727_230, 112_395_446_120);
        let parse = wide::plan(m, k, wide::Workspace::SixK, 384 << 30).unwrap();
        assert_eq!(
            (parse.sa_slots, parse.peak_array_bytes),
            (17_250_008_260, 176_179_804_640)
        );
        let phases = phase_bytes(m, k, d + 3 * k - 1, 128 << 30);
        assert_eq!(
            phases,
            PhaseBytes {
                aligned: 246_031_320_145,
                lists: 296_605_586_793,
                emit: 236_320_359_781 + (128 << 30)
            }
        );
        assert!(phases.emit < 384 << 30);
        assert_eq!(
            (
                size_of::<range::Load>(),
                size_of::<Tail>(),
                size_of::<Cursor>()
            ),
            (16, 16, 20)
        );
    }
}
