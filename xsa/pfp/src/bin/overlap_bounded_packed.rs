//! Fixture and fixed 100 MB component row43 producer; no resident/default dispatch.
#[path = "../overlap_min.rs"]
mod min;
#[path = "../overlap_range_order.rs"]
mod overlap_range_order;
use overlap_range_order as range;
#[allow(dead_code)]
#[path = "../overlap_compact_order.rs"]
mod compact;
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
    fn load(mut bytes: Vec<u8>, k: usize, eof: usize, flag: &Path) -> Self {
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
        stop(flag);
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
        lists: base + 37 * m + 8 + tree + 4 * (k + 1),
        emit: base + 21 * m + tree + 4 * (k + 1) + u128::from(arena),
    }
}
// Arithmetic only: no file arguments, cgroup operations, or producer dispatch.
fn component_plan(profile: &std::ffi::OsStr) {
    let (n, m, k, t, u, grant, arena, cap) = match profile.to_str() {
        Some("100m") => (
            100_000_503,
            3_170_531,
            2_689_467,
            127_708_112,
            95_434_516,
            320 << 20,
            64 << 20,
            8192,
        ),
        Some("1g") => (
            1_000_000_665,
            31_784_026,
            25_336_659,
            1_242_219_181,
            938_179_281,
            3 << 30,
            512 << 20,
            65536,
        ),
        _ => panic!("plan accepts banked 100m or 1g only"),
    };
    let phases = phase_bytes(m, k, t, arena);
    let parse = wide::plan(m, k, wide::Workspace::SixK, grant).unwrap();
    assert!(phases.aligned.max(phases.lists).max(phases.emit) <= u128::from(grant));
    // These profiles use the conservative two-order admission (32 passes each).
    let slots =
        (arena as usize - cap * size_of::<Cursor>() - compact::fixed_bytes(32).unwrap()) / 8;
    let min_distributions = (u as usize).div_ceil(slots);
    assert!(min_distributions < 32);
    let column_bytes = t + 4 * m;
    let scratch = column_bytes + (4 << 20); // reserved for all pipes, queues, and bounded logs
    assert!(scratch <= 2 * n);
    println!("PLAN_ONLY n={n} m={m} k={k} descriptors={u} tape={t} array_grant={grant} arena={arena} cursor_cap={cap} slots={slots} min_distributions={min_distributions} parse={} {phases:?} scratch={scratch} scratch_limit={} launch=false",parse.peak_array_bytes,2*n);
}
fn main() {
    let args: Vec<_> = std::env::args_os().collect();
    if args.get(1).is_some_and(|s| s == "--plan-component") {
        assert_eq!(args.len(), 3);
        component_plan(&args[2]);
        return;
    }
    let component = args.get(1).is_some_and(|s| s == "--component-100m");
    let compact = component || args.get(1).is_some_and(|s| s == "--fixture-compact");
    let required = if compact { 7 } else { 6 };
    if component {
        assert_eq!(args.len(), 4, "--component-100m COLUMNS STOP");
    } else {
        assert!(
        (required..=required + 1).contains(&args.len()) && (compact || args[1] == "--fixture"),
        "--fixture COLUMNS STOP ARRAY_BYTES ARENA_BYTES [EXPECTED_RANKS] or --fixture-compact COLUMNS STOP ARRAY_BYTES ARENA_BYTES CURSOR_CAP [EXPECTED_RANKS]");
    }
    let dir = Path::new(&args[2]);
    let flag = Path::new(&args[3]);
    stop(flag);
    let grant: usize = if component {
        320 << 20
    } else {
        args[4].to_str().unwrap().parse().unwrap()
    };
    let arena: usize = if component {
        64 << 20
    } else {
        args[5].to_str().unwrap().parse().unwrap()
    };
    assert!(usize::BITS == 64 && arena <= grant && (component || grant <= 200_000_000));
    let open = |name| {
        let f = File::open(dir.join(name)).unwrap();
        let len = usize::try_from(f.metadata().unwrap().len()).unwrap();
        (f, len)
    };
    let (input, mb) = open("parse.u32");
    let m = mb / 4;
    assert!(
        m > 0
            && (if component {
                m == 3_170_531
            } else {
                m <= FIXTURE
            })
            && 4 * m == mb
            && mb <= grant
    );
    let mut ids = Vec::with_capacity(m);
    {
        let mut f = BufReader::with_capacity(8192, input);
        for _ in 0..m {
            ids.push(word(&mut f));
        }
        end(&mut f);
    }
    stop(flag);
    let k = 1 + *ids.iter().max().unwrap() as usize;
    assert!(k <= m);
    let eof = ids[m - 1] as usize;
    let (mut file, t) = open("dictionary.packed");
    if component {
        assert_eq!((k, t), (2_689_467, 127_708_112));
    } else {
        assert!(t <= 16 * FIXTURE);
    }
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
    let (cap, workspace) = if compact {
        let requested: usize = if component {
            8192
        } else {
            args[6].to_str().unwrap().parse().unwrap()
        };
        assert!(requested > 0, "empty cursor cap");
        let cap = requested.min(k);
        (
            cap,
            arena
                .checked_sub(cap.checked_mul(size_of::<Cursor>()).unwrap())
                .expect("cursor budget"),
        )
    } else {
        let slots = arena.checked_sub(bins).expect("arena census")
            / (size_of::<Tail>() + size_of::<Cursor>());
        assert!(slots > 0, "empty arena");
        (slots.min(k), bins + slots * size_of::<Tail>())
    };
    let ranks =
        wide::wide_parse_ranks(&ids, k as u64, wide::Workspace::SixK, grant as u64).unwrap();
    stop(flag);
    if let Some(path) = args.get(required) {
        let mut f = BufReader::with_capacity(8192, File::open(path).unwrap());
        for &r in &ranks {
            assert_eq!(r, word(&mut f), "rank oracle");
        }
        end(&mut f);
    }
    stop(flag);
    let mut tape = vec![0; t];
    file.read_exact(&mut tape).unwrap();
    end(&mut file);
    drop(file);
    stop(flag);
    let packed = Packed::load(tape, k, eof, flag);
    stop(flag);
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
    if component {
        assert_eq!(
            (n, (0..k).map(width).sum::<usize>()),
            (100_000_503, 95_434_516)
        );
    } else {
        assert!(n <= FIXTURE as u64, "text exceeds fixture cap");
    }
    assert!(counts.iter().all(|&x| x > 0), "unused lexical ID");
    stop(flag);
    let pl = wide::ParsePlcp::build(&ids, ranks, grant as u64).unwrap();
    stop(flag);
    let mut h = vec![0; m];
    pl.fill_aligned(
        &starts,
        eof as u32,
        |a, b| common(packed.key(a as usize), packed.key(b as usize)) as u64,
        &mut h,
    )
    .unwrap();
    stop(flag);
    let order = pl.into_order();
    let rmq = min::BlockMin::new(h);
    stop(flag);
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
    assert_eq!(loc[k] as usize, m);
    let mut positions = vec![0u64; m];
    let mut next = vec![0; m];
    let mut pred = vec![0u8; m];
    for (j, r) in order.continuations() {
        let j = j as usize;
        let v = ids[j] as usize;
        let i = loc[v] as usize;
        loc[v] = loc[v].checked_add(1).unwrap();
        positions[i] = starts[j];
        next[i] = r;
        if j > 0 {
            let w = ids[j - 1] as usize;
            pred[i] = packed.decode[packed.key(w)[width(w) - 1] as usize];
        }
    }
    for v in (0..k).rev() {
        assert_eq!(loc[v], loc[v + 1]);
        loc[v] = loc[v].checked_sub(counts[v].try_into().unwrap()).unwrap();
    }
    assert_eq!(loc[0], 0);
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
    let mut out = BufWriter::with_capacity(8192, std::io::stdout().lock());
    let mut rows = 0u64;
    let mut heap = BinaryHeap::<Cursor>::with_capacity(cap);
    let mut previous_length = None;
    let mut max_ties = 0;
    // One successor per occurrence; rank m can occur, rank 0 is END.
    let rank_end = m + 1;
    let mut passes = 64;
    if compact && cap < k {
        let descriptors: usize = (0..k).map(width).sum();
        let cost = rank_end.div_ceil(cap) as u128 * 2 * (m.ilog2() + 2) as u128;
        if descriptors as u128 * cost > 100_000_000 {
            // Budget two deterministic orders *together*. Pre-count only oversized
            // complete groups; no merge, cursor copies, row buffer or output.
            passes /= 2;
            let mut probes = 0u128;
            let c = compact::order_checked(
                &d,
                compact::Limits {
                    workspace_bytes: workspace,
                    rows_per_range: n,
                    passes,
                },
                || stop(flag),
                |ties, _| {
                    if ties.len() > cap {
                        probes += ties.len() as u128 * cost;
                        assert!(probes <= 100_000_000, "rank window work cap");
                    }
                },
            )
            .unwrap();
            eprintln!("compact admission_passes={}+{} admission_digit_reads={} window_probes_bound={probes}",c.census_passes,c.distributions,c.radix_digit_reads);
        }
    }
    let mut emit = |nt: usize, at: &dyn Fn(usize) -> Tail, head: usize| {
        if rows == 0 {
            stop(flag);
            out.write_all(&row43::encode(n, 0, last as u32)).unwrap();
            rows += 1;
        }
        assert!(nt <= k);
        let len = d.key(at(0)).len();
        assert!(
            previous_length != Some(head),
            "finite tails not prefix-free"
        );
        previous_length = Some(len);
        max_ties = max_ties.max(nt);
        let mut prior = None;
        let step = if nt <= cap { rank_end } else { cap };
        for lo in (0..rank_end).step_by(step) {
            for t in 0..nt {
                let Tail { word: v, offset } = at(t);
                let mut i = loc[v];
                let mut end = loc[v + 1];
                if nt > cap {
                    let list = &next[i as usize..end as usize];
                    end = i + list.partition_point(|&r| (r as usize) < lo + step) as u32;
                    i += list.partition_point(|&r| (r as usize) < lo) as u32;
                    if i == end {
                        continue;
                    }
                }
                assert!(heap.len() < cap);
                let b = if offset == 0 {
                    0
                } else {
                    packed.decode[packed.key(v)[offset - 1] as usize]
                };
                heap.push(Reverse((
                    next[i as usize],
                    i,
                    end,
                    offset.try_into().unwrap(),
                    b,
                )));
            }
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
        }
    };
    let (total, ranges) = if compact {
        let c = compact::order_checked(
            &d,
            compact::Limits {
                workspace_bytes: workspace,
                rows_per_range: n,
                passes,
            },
            || stop(flag),
            |ties, h| emit(ties.len(), &|i| ties[i].into(), h),
        )
        .unwrap();
        eprintln!(
            "compact passes={}+{} digit_reads={} heap_cap={cap}",
            c.census_passes, c.distributions, c.radix_digit_reads
        );
        (c.total, c.distributions)
    } else {
        let c = range::order(
            &d,
            range::Limits {
                workspace_bytes: workspace,
                prefix_bits: BITS,
                rows_per_range: n,
                distributions: 64,
            },
            |ties, h| emit(ties.len(), &|i| ties[i], h),
        )
        .unwrap();
        (c.total, c.ranges)
    };

    assert_eq!((rows, total.rows), (n + 1, n));
    stop(flag);
    out.flush().unwrap();
    eprintln!("bounded rows={rows} max_ties={max_ties} ranges={}", ranges);
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
                lists: 287226677869,
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
