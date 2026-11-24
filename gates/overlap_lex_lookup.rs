//! Fixed small resident byte-lookup diagnostic. Not a suffix order, stream, or product.
#[path = "../xsa/pfp/src/overlap_lex_direct.rs"]
mod direct;
#[path = "../xsa/pfp/src/overlap_lexblocks.rs"]
mod restart;
use std::{
    collections::BTreeSet,
    hint::black_box,
    io::Read,
    path::Path,
    time::{Instant, SystemTime, UNIX_EPOCH},
};
const WIDTH: usize = 3072;
const WINDOWS: usize = 64;
const MIN_QUERIES: usize = 1 << 20;
const ABSENT: u16 = 257;
type Words = Vec<(Vec<u8>, bool)>;
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> usize {
        self.0 = self
            .0
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        (self.0 >> 32) as usize
    }
    fn shuffle<T>(&mut self, s: &mut [T]) {
        for i in (1..s.len()).rev() {
            s.swap(i, self.next() % (i + 1));
        }
    }
}
fn book(data: &[u8]) -> Words {
    let mut set = BTreeSet::new();
    for w in data.chunks_exact(WIDTH) {
        let mut prev = None;
        for (i, tag) in w.windows(8).enumerate().skip(1) {
            let h = tag.iter().fold(1469598103934665603u64, |h, &v| {
                (h ^ u64::from(v)).wrapping_mul(1099511628211)
            });
            if h % 31 == 0 {
                if let Some(j) = prev {
                    set.insert(w[j..i + 8].to_vec());
                }
                prev = Some(i);
            }
        }
    }
    let mut words: Words = set.into_iter().map(|s| (s, false)).collect();
    words.push((vec![0xff], true));
    words.sort();
    words
}
fn wide() -> Words {
    let mut r = Rng(0x58a3f181);
    let mut words: Words = (0..32768)
        .map(|i| {
            let n = 9 + r.next() % 97;
            ((0..n).map(|_| r.next() as u8).collect(), i == 0)
        })
        .collect();
    words.sort();
    words
}
fn deep() -> Words {
    let mut r = Rng(0x7612eddd);
    let mut words = Vec::new();
    for g in 0u16..1024 {
        let mut base = g.to_be_bytes().to_vec();
        base.extend((2..96).map(|_| r.next() as u8));
        for j in 0..16 {
            let mut s = base.clone();
            s.push(j);
            s.extend((97..120).map(|_| r.next() as u8));
            words.push((s, g == 0 && j == 0));
        }
    }
    words
}

trait View {
    fn symbol(&self, p: usize) -> Option<u16>;
}
trait Dict {
    type V<'a>: View
    where
        Self: 'a;
    fn get(&self, id: usize) -> Self::V<'_>;
}
struct Flat {
    data: Vec<u8>,
    offsets: Vec<u64>,
    eof: usize,
}
struct FlatWord<'a>(&'a [u8], bool);
impl View for FlatWord<'_> {
    fn symbol(&self, p: usize) -> Option<u16> {
        self.0
            .get(p)
            .map(|&b| 1 + u16::from(b))
            .or_else(|| (p == self.0.len() && self.1).then_some(0))
    }
}
impl Dict for Flat {
    type V<'a> = FlatWord<'a>;
    fn get(&self, id: usize) -> FlatWord<'_> {
        FlatWord(
            &self.data[self.offsets[id] as usize..self.offsets[id + 1] as usize],
            id == self.eof,
        )
    }
}
impl View for direct::Word<'_> {
    fn symbol(&self, p: usize) -> Option<u16> {
        self.symbol(p)
    }
}
impl View for restart::Word<'_> {
    fn symbol(&self, p: usize) -> Option<u16> {
        self.symbol(p)
    }
}
impl Dict for direct::LexDirect {
    type V<'a> = direct::Word<'a>;
    fn get(&self, id: usize) -> Self::V<'_> {
        self.word(id).unwrap()
    }
}
impl Dict for restart::LexBlocks {
    type V<'a> = restart::Word<'a>;
    fn get(&self, id: usize) -> Self::V<'_> {
        self.word(id).unwrap()
    }
}

#[derive(Clone, Copy)]
struct Q {
    id: u32,
    pos: u32,
    want: u16,
}
enum Trace {
    Fresh(Vec<Q>),
    Scan { len: Vec<usize>, want: Vec<u16> },
}
impl Trace {
    fn n(&self) -> usize {
        match self {
            Self::Fresh(q) => q.len(),
            Self::Scan { want, .. } => want.len(),
        }
    }
}
fn trace(words: &Words, how: &str) -> Trace {
    // The expected answer comes from the original complete raw lexical record, not any tested adapter.
    let mut rng = Rng(0xd5406fb);
    let mut q = Vec::new();
    let mut want = Vec::new();
    let mut len = Vec::new();
    for (id, (s, eof)) in words.iter().enumerate() {
        let owned = s.len() - if *eof { 0 } else { 8 };
        let oracle = |p: usize| match s.get(p) {
            Some(&v) => 1 + u16::from(v),
            None if *eof && p == s.len() => 0,
            None => ABSENT,
        };
        match how {
            "scan" => {
                len.push(s.len() + 2);
                want.extend((0..s.len() + 2).map(oracle));
            }
            "head" => q.push(Q {
                id: id as u32,
                pos: 0,
                want: oracle(0),
            }),
            "scatter" => {
                for off in 0..owned {
                    let pos = off + rng.next() % 17;
                    q.push(Q {
                        id: id as u32,
                        pos: pos as u32,
                        want: oracle(pos),
                    });
                }
            }
            _ => unreachable!(),
        }
    }
    if how == "scan" {
        Trace::Scan { len, want }
    } else {
        rng.shuffle(&mut q);
        Trace::Fresh(q)
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
#[inline(never)]
fn checked<D: Dict>(d: &D, t: &Trace) {
    match t {
        Trace::Fresh(q) => {
            for x in black_box(q.as_slice()) {
                assert_eq!(
                    d.get(x.id as usize)
                        .symbol(x.pos as usize)
                        .unwrap_or(ABSENT),
                    x.want
                );
            }
        }
        Trace::Scan { len, want } => {
            let mut at = 0;
            for (id, &n) in len.iter().enumerate() {
                let v = d.get(id);
                for (p, &w) in black_box(&want[at..at + n]).iter().enumerate() {
                    assert_eq!(v.symbol(p).unwrap_or(ABSENT), w);
                }
                at += n;
            }
            assert_eq!(at, want.len());
        }
    }
}
fn timed<D: Dict>(d: &D, t: &Trace, cycles: usize, guard: &impl Fn()) -> (f64, f64) {
    let start = Instant::now();
    let c = cpu();
    for _ in 0..cycles {
        guard();
        checked(black_box(d), black_box(t));
    }
    (start.elapsed().as_secs_f64(), cpu() - c)
}
fn study(name: &str, w: Words, guard: &impl Fn()) {
    assert!(w.windows(2).all(|x| x[0] < x[1]) && w.iter().filter(|x| x.1).count() == 1);
    let refs: Vec<_> = w.iter().map(|(s, e)| (s.as_slice(), *e)).collect();
    let d = direct::LexDirect::fixture(&refs).unwrap();
    let b = restart::LexBlocks::fixture(&refs, 16).unwrap();
    let eof = w.iter().position(|x| x.1).unwrap();
    let mut f = Flat {
        data: Vec::with_capacity(w.iter().map(|x| x.0.len()).sum()),
        offsets: vec![0],
        eof,
    };
    let mut owned = 0;
    let mut lit = 0;
    for (i, (s, e)) in w.iter().enumerate() {
        let n = s.len() - if *e { 0 } else { 8 };
        assert_eq!(d.get(i).owned(), n);
        assert_eq!(b.get(i).owned(), n);
        owned += n;
        f.data.extend_from_slice(s);
        f.offsets.push(f.data.len() as u64);
        let p = if i % 16 == 0 {
            0
        } else {
            w[i - 1].0.iter().zip(s).take_while(|(x, y)| x == y).count()
        };
        lit += s.len() - p;
    }
    println!(
        "DECK shape={name} k={} raw={} owned={owned} literals={lit} flat={} restart={} direct={}",
        w.len(),
        f.data.len(),
        16 + f.data.len() + 8 * f.offsets.len(),
        b.payload() + b.directory(),
        d.bytes()
    );
    for how in ["scatter", "head", "scan"] {
        guard();
        let t = trace(&w, how);
        let n = t.n();
        let cycles = MIN_QUERIES.div_ceil(n);
        let (mut hist, mut absent, mut end, mut headers, mut views) =
            ([0usize; 16], 0usize, 0usize, 0usize, 0usize);
        let mut count = |v: (Option<u16>, usize), want| {
            assert_eq!(v.0.unwrap_or(ABSENT), want);
            match v.0 {
                Some(0) => end += 1,
                None => absent += 1,
                Some(_) => hist[v.1] += 1,
            }
        };
        match &t {
            Trace::Fresh(q) => {
                for x in q {
                    let id = x.id as usize;
                    count(d.get(id).traced(x.pos as usize), x.want);
                    headers += 1 + id % 16;
                    views += 1;
                }
            }
            Trace::Scan { len, want } => {
                let mut at = 0;
                for (id, &n) in len.iter().enumerate() {
                    let v = d.get(id);
                    for (p, &a) in want[at..at + n].iter().enumerate() {
                        count(v.traced(p), a);
                    }
                    at += n;
                    headers += 1 + id % 16;
                    views += 1;
                }
            }
        }
        let hops = hist.iter().enumerate().map(|(i, &n)| i * n).sum::<usize>();
        let raw = hist.iter().sum::<usize>();
        assert_eq!(raw + end + absent, n);
        println!("TRACE shape={name} schedule={how} queries={n} cycles={cycles} views={views} restart_headers={headers} raw={raw} end={end} absent={absent} hops={hops} max_hops={} literal_hits={}",hist.iter().rposition(|&x|x!=0).unwrap(),hist[0]);
        for (rep, modes) in [["flat", "restart", "direct"], ["direct", "restart", "flat"]]
            .iter()
            .enumerate()
        {
            for &mode in modes {
                guard();
                let (wall, c) = match mode {
                    "flat" => timed(&f, &t, cycles, guard),
                    "restart" => timed(&b, &t, cycles, guard),
                    "direct" => timed(&d, &t, cycles, guard),
                    _ => unreachable!(),
                };
                println!("LOOKUP shape={name} schedule={how} rep={rep} table={mode} verified={} wall_s={wall:.9} cpu_s={c:.9}",cycles*n);
            }
        }
    }
}
fn main() {
    let args: Vec<_> = std::env::args().collect();
    assert!(
        cfg!(all(target_os = "linux", target_pointer_width = "64"))
            && args.len() == 4
            && args[1] == "--fixed-lookup-windows"
    );
    let until = args[3].parse::<u64>().unwrap();
    assert!(until <= 1795531169);
    let guard = || {
        if Path::new(&args[2]).try_exists().unwrap()
            || SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_secs()
                >= until
        {
            std::process::exit(75);
        }
    };
    guard();
    let mut sample = vec![0; WIDTH * WINDOWS];
    let mut input = std::io::stdin().lock();
    input.read_exact(&mut sample).unwrap();
    assert_eq!(input.read(&mut [0]).unwrap(), 0);
    study("book", book(&sample), &guard);
    study("wide", wide(), &guard);
    study("deep", deep(), &guard);
    guard();
}
