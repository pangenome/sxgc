//! Resident comparison from `parse.u32` and either raw text + dictionary refs
//! or the self-contained length-tagged `dictionary.packed`. No on-disk sort columns. Stdout: LE u32 (pos, byte LCP, BWT),
//! including END; an optional inherited FD gets the *same* blocks for audit.
//! Deliberately limited to the measured rung/widths, not the Pile builder.
use std::{
    cmp::Reverse,
    collections::BinaryHeap,
    fs,
    io::Write,
    os::fd::FromRawFd,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};
#[path = "../overlap_min.rs"]
mod ranges;
use ranges::BlockMin;
#[path = "../overlap_dict_index.rs"]
mod index;
use index::{local_offset, RankStarts, Suffixes};
#[link(name = "libsais_r", kind = "static")]
extern "C" {
    fn libsais_int(t: *mut i32, sa: *mut i32, n: i32, k: i32, fs: i32) -> i32;
    fn libsais_plcp_int(t: *const i32, sa: *const i32, pl: *mut i32, n: i32) -> i32;
}
const W: usize = 8;
fn stop(path: &Path) {
    if path.exists() {
        eprintln!("RESIDENT_STOP exit=75");
        std::process::exit(75);
    }
}
fn counter(s: &str, key: &str) -> u64 {
    s.lines()
        .find_map(|l| l.strip_prefix(key))
        .unwrap()
        .split_whitespace()
        .next()
        .unwrap()
        .parse()
        .unwrap()
}
fn io() -> (u64, u64) {
    let s = fs::read_to_string("/proc/self/io").unwrap();
    (counter(&s, "read_bytes:"), counter(&s, "write_bytes:"))
}
struct Phase<'a>(&'static str, Instant, (u64, u64), &'a Path);
impl<'a> Phase<'a> {
    fn begin(name: &'static str, flag: &'a Path) -> Self {
        stop(flag);
        fs::write("/proc/self/clear_refs", b"5").expect("reset own phase RSS high-water mark");
        eprintln!("RESIDENT_BEGIN phase={name}");
        Self(name, Instant::now(), io(), flag)
    }
    fn end(self) {
        let (r, w) = io();
        let h = counter(&fs::read_to_string("/proc/self/status").unwrap(), "VmHWM:") * 1024;
        eprintln!(
            "RESIDENT_PHASE phase={} seconds={:.6} peak_rss={} read_bytes={} write_bytes={}",
            self.0,
            self.1.elapsed().as_secs_f64(),
            h,
            r - self.2 .0,
            w - self.2 .1
        );
        stop(self.3);
    }
}
fn common(a: &[u8], b: &[u8]) -> usize {
    let n = a.len().min(b.len());
    let mut i = 0;
    while n - i >= 8 {
        let x = u64::from_le_bytes(a[i..i + 8].try_into().unwrap())
            ^ u64::from_le_bytes(b[i..i + 8].try_into().unwrap());
        if x != 0 {
            return i + (x.trailing_zeros() / 8) as usize;
        }
        i += 8;
    }
    while i < n && a[i] == b[i] {
        i += 1;
    }
    i
}
#[derive(Copy, Clone)]
struct Word {
    pos: u32,
    span: u32,
}
impl Word {
    fn raw(self) -> usize {
        (self.span & 0x7fffffff) as usize
    }
    fn eof(self) -> bool {
        self.span >> 31 != 0
    }
    fn width(self) -> usize {
        if self.eof() {
            self.raw()
        } else {
            self.raw().checked_sub(W).unwrap()
        }
    }
}
// In a packed input Word.pos is relative to this tape, not to the corpus.
// The grant is checked BEFORE loading the tape or allocating this index.
fn packed_words(tape: &[u8], k: usize) -> Vec<Word> {
    let mut words = Vec::with_capacity(k);
    let mut i = 0;
    while i < tape.len() {
        assert!(
            tape.len() - i >= 4 && words.len() < k,
            "invalid packed word count/header"
        );
        let w = Word {
            pos: u32::try_from(i + 4).unwrap(),
            span: le(&tape[i..i + 4]),
        };
        i += 4;
        assert!(
            w.raw() <= tape.len() - i && w.raw() > if w.eof() { 0 } else { W },
            "invalid packed word span"
        );
        if let Some(&prev) = words.last() {
            let prev: Word = prev;
            let a = prev.pos as usize;
            assert!(
                (&tape[a..a + prev.raw()], prev.eof()) < (&tape[i..i + w.raw()], w.eof()),
                "packed words not in strict lexical ID order"
            );
        }
        i += w.raw();
        words.push(w);
    }
    assert_eq!(
        words.len(),
        k,
        "packed dictionary count disagrees with parse"
    );
    words
}
fn le(a: &[u8]) -> u32 {
    u32::from_le_bytes(a.try_into().unwrap())
}
fn size(p: &Path) -> usize {
    usize::try_from(fs::metadata(p).unwrap().len()).unwrap()
}
struct Lists {
    loc: Vec<u32>,
    pair: Vec<[u32; 2]>,
    pred: Vec<u8>,
    min: BlockMin,
}
struct Dict {
    bytes: Vec<u8>,
    start: Vec<u64>,
    eof_id: usize,
    decode: [u8; 256],
}
impl Dict {
    fn key(&self, v: usize) -> &[u8] {
        &self.bytes[self.start[v] as usize..self.start[v + 1] as usize - 1]
    }
}
struct Output<'a> {
    sink: std::io::StdoutLock<'a>,
    audit: Option<fs::File>,
    bytes: Vec<u8>,
    rows: u64,
    sink_time: Duration,
    audit_time: Duration,
    flag: &'a Path,
}
impl Output<'_> {
    fn push(&mut self, p: u32, h: u32, b: u32) {
        self.bytes.extend_from_slice(&p.to_le_bytes());
        self.bytes.extend_from_slice(&h.to_le_bytes());
        self.bytes.extend_from_slice(&b.to_le_bytes());
        self.rows += 1;
        if self.bytes.len() == 65536 * 12 {
            self.send();
        }
    }
    fn send(&mut self) {
        stop(self.flag);
        let t = Instant::now();
        self.sink.write_all(&self.bytes).expect("product pipe");
        self.sink.flush().unwrap();
        self.sink_time += t.elapsed();
        if let Some(f) = &mut self.audit {
            let t = Instant::now();
            f.write_all(&self.bytes).expect("oracle pipe");
            self.audit_time += t.elapsed();
        }
        self.bytes.clear();
    }
}
// A cursor into one word's occurrences at one offset. Its first field orders
// the heap by continuation rank (not word ID, occurrence index, or text pos).
type Cursor = Reverse<(u32, u32, u32, u32, u32)>; // rank, index, end, offset, internal BWT
fn flush(q: &mut BinaryHeap<Cursor>, lists: &Lists, len: usize, head: u32, out: &mut Output) {
    let mut prior = None;
    while let Some(Reverse((mut rank, mut i, end, off, bwt))) = q.pop() {
        loop {
            let h = match prior {
                None => head,
                Some(p) => {
                    assert!(0 < p && p < rank && len > W);
                    (len - W + lists.min.between(p as usize - 1, rank as usize - 1)) as u32
                }
            };
            let p = lists.pair[i as usize][0] + off;
            out.push(
                p,
                h,
                if off != 0 {
                    bwt
                } else if p == 0 {
                    256
                } else {
                    lists.pred[i as usize] as u32
                },
            );
            prior = Some(rank);
            i += 1;
            if i == end {
                break;
            }
            rank = lists.pair[i as usize][1];
            if q.peek().is_some_and(|&Reverse((next, ..))| next < rank) {
                q.push(Reverse((rank, i, end, off, bwt)));
                break;
            }
        }
    }
}
fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    assert!(
        args.len() == 3 || args.len() == 4,
        "overlap_resident IN STOP ARRAY_GRANT [EXPECTED_INVERSE]"
    );
    let dir = PathBuf::from(&args[0]);
    let flag = Path::new(&args[1]);
    let grant: usize = args[2].to_str().unwrap().parse().unwrap();
    let phase = |name| Phase::begin(name, flag);
    let p = phase("load_ids");
    let packed = match std::env::var("XSA_RESIDENT_PACKED").as_deref() {
        Ok("1") => true,
        Ok("0") | Err(std::env::VarError::NotPresent) => false,
        _ => panic!("invalid packed input switch"),
    };
    let mb = size(&dir.join("parse.u32"));
    let m = mb / 4;
    assert!(
        m > 0 && m <= 32_000_000 && mb == 4 * m && grant >= 2 * mb,
        "invalid parse size/grant"
    );
    let mut pa: Vec<i32> = fs::read(dir.join("parse.u32"))
        .unwrap()
        .chunks_exact(4)
        .map(|r| i32::try_from(le(r)).unwrap())
        .collect();
    let k = 1 + *pa.iter().max().unwrap() as usize;
    assert!(k <= m && k <= 26_000_000);
    let data_path = dir.join(if packed {
        "dictionary.packed"
    } else {
        "corpus"
    });
    let source_bytes = size(&data_path);
    let (words, tape, n) = if packed {
        assert!(
            source_bytes <= 1_500_000_000 && grant >= source_bytes + 8 * k + mb,
            "packed tape exceeds array grant"
        );
        let bytes = fs::read(&data_path).unwrap();
        let words = packed_words(&bytes, k);
        let n: usize = pa.iter().map(|&v| words[v as usize].width()).sum();
        (words, Some(bytes), n)
    } else {
        let kb = size(&dir.join("dictionary.refs"));
        assert!(
            kb == 8 * k && grant >= 2 * kb + 2 * mb,
            "invalid refs size/grant"
        );
        let words = fs::read(dir.join("dictionary.refs"))
            .unwrap()
            .chunks_exact(8)
            .map(|r| Word {
                pos: le(&r[..4]),
                span: le(&r[4..]),
            })
            .collect();
        (words, None, source_bytes)
    };
    assert!(
        n > 0 && n <= 1_000_000_665 && m <= n,
        "outside measured corpus rung"
    );
    eprintln!("RESIDENT_INPUT packed={packed} source_bytes={source_bytes} n={n} m={m} k={k}");
    let eof = pa[m - 1] as usize;
    let mut d = 1;
    let mut descriptors = 0;
    for (i, &w) in words.iter().enumerate() {
        assert!(w.eof() == (i == eof) && w.width() > 0 && w.pos as usize + w.raw() <= source_bytes);
        d += w.raw() + 1;
        descriptors += w.width();
    }
    let wide = match std::env::var("XSA_RESIDENT_WIDE_DICT").as_deref() {
        Ok("1") => true,
        Ok("0") | Err(std::env::VarError::NotPresent) => false,
        _ => panic!("XSA_RESIDENT_WIDE_DICT must be 0 or 1"),
    };
    assert_eq!(usize::BITS, 64, "resource plan requires 64-bit addressing");
    if !wide {
        i32::try_from(d).expect("dictionary exceeds signed libsais width");
    }
    assert!(
        m + 6 * k <= i32::MAX as usize,
        "parse SA plus workspace exceeds signed width"
    );
    // Bytes of simultaneously live arrays, not file-cache residency. Reserve
    // extra SA space for recursion, and bound doubled heap capacity by 2*k:
    // a finite equal key ends at a word boundary -> at most one offset/word.
    let parse_bound = 8 * m + 8 * k + 24 * k + tape.as_ref().map_or(0, Vec::len);
    let aux_bound = source_bytes + d + 25 * (k + 1) + 32 * m;
    let retained = 12 * (k + 1) + 9 * m + 4 * m + 8 * m.div_ceil(16);
    let sa_bytes = if wide { 8 * d } else { 4 * d };
    let owner_bytes = if wide { RankStarts::bytes(d) } else { 4 * d };
    let dict_bound = d + sa_bytes + retained + (64 << 20);
    let emit_bound = d + sa_bytes + owner_bytes + retained + 40 * k + (8 << 20);
    eprintln!("RESIDENT_INDEX wide={wide} sa_bytes={sa_bytes} owner_bytes={owner_bytes} start_bytes={} native_above_i32={}",8*(k+1),d>i32::MAX as usize);
    eprintln!("RESIDENT_PLAN n={n} m={m} k={k} dict_symbols={d} descriptors={descriptors} parse_bound={parse_bound} aux_bound={aux_bound} dict_bound={dict_bound} emit_bound={emit_bound} array_grant={grant}");
    assert!(
        grant >= parse_bound.max(aux_bound).max(dict_bound).max(emit_bound),
        "resident plan exceeds array grant"
    );
    p.end();

    let p = phase("parse_sort");
    let extra = 6 * k;
    let mut ps = vec![0; m + extra];
    assert_eq!(
        unsafe {
            libsais_int(
                pa.as_mut_ptr(),
                ps.as_mut_ptr(),
                m as i32,
                k as i32,
                extra as i32,
            )
        },
        0
    );
    ps.truncate(m);
    ps.shrink_to_fit();
    p.end();
    if args.len() == 4 {
        let p = phase("check_ranks");
        assert_eq!(size(Path::new(&args[3])), 4 * m);
        let inv = fs::read(&args[3]).unwrap();
        for (r, &x) in ps.iter().enumerate() {
            let j = x as usize * 4;
            assert_eq!(le(&inv[j..j + 4]), r as u32 + 1, "external rank mismatch");
        }
        eprintln!("RESIDENT_CHECK inverse_ranks={m} virtual_end=0");
        drop(inv);
        p.end();
    }
    let p = phase("pack_dictionary");
    let source = tape.unwrap_or_else(|| fs::read(data_path).unwrap());
    let mut used = [false; 256];
    if packed {
        for w in &words {
            let i = w.pos as usize;
            for &b in &source[i..i + w.raw()] {
                used[b as usize] = true;
            }
        }
    } else {
        for &b in &source {
            used[b as usize] = true;
        }
    }
    let mut encode = [0; 256];
    let mut decode = [0; 256];
    let mut code = 2;
    for (i, b) in used.into_iter().enumerate() {
        if b {
            assert!(
                code < 256,
                "byte kernel needs two free codes (else widen/partition)"
            );
            encode[i] = code as u8;
            decode[code] = i as u8;
            code += 1;
        }
    }
    let mut dict = Dict {
        bytes: Vec::with_capacity(d),
        start: Vec::with_capacity(k + 1),
        eof_id: eof,
        decode,
    };
    let mut last_owned = Vec::with_capacity(k);
    for &w in &words {
        dict.start.push(dict.bytes.len() as u64);
        let i = w.pos as usize;
        dict.bytes
            .extend(source[i..i + w.raw()].iter().map(|&b| encode[b as usize]));
        last_owned.push(source[i + w.width() - 1]);
        if w.eof() {
            dict.bytes.push(1);
        }
        dict.bytes.push(0);
    }
    assert_eq!(dict.bytes.len(), d);
    dict.start.push(d as u64);
    drop(source);
    let last = last_owned[eof];
    p.end();

    let p = phase("aligned_and_lists");
    let mut s = Vec::with_capacity(m + 1);
    let mut pos = 0;
    let mut loc = vec![0u32; k + 1];
    for (i, &id) in pa.iter().enumerate() {
        let v = id as usize;
        assert!((i == m - 1) == words[v].eof());
        if packed && i > 0 {
            let prev = pa[i - 1] as usize;
            let at = dict.start[prev] as usize + words[prev].width();
            assert!(
                words[v].raw() >= W && dict.bytes[at..at + W] == dict.key(v)[..W],
                "broken packed parse overlap"
            );
        }
        s.push(pos);
        pos = u32::checked_add(pos, words[v].width() as u32).expect("wide corpus offset required");
        loc[v + 1] += 1;
    }
    s.push(pos);
    assert_eq!(pos as usize, n);
    drop(words);
    let mut pl = vec![0; m];
    assert_eq!(
        unsafe { libsais_plcp_int(pa.as_ptr(), ps.as_ptr(), pl.as_mut_ptr(), m as i32) },
        0
    );
    let mut aligned = vec![0; m];
    for r in 1..m {
        let (i, j) = (ps[r - 1] as usize, ps[r] as usize);
        let z = pl[j] as usize;
        let (x, y) = (pa[i + z] as usize, pa[j + z] as usize);
        assert!(x < y);
        aligned[r] = s[i + z] - s[i] + common(dict.key(x), dict.key(y)) as u32;
    }
    drop(pl);
    let min = BlockMin::new(aligned);
    for v in 0..k {
        assert!(loc[v + 1] > 0);
        loc[v + 1] += loc[v];
    }
    let mut cur = loc.clone();
    let mut pair = vec![[0; 2]; m];
    let mut pred = vec![0; m];
    let mut insert = |j: usize, next: u32| {
        let v = pa[j] as usize;
        let i = cur[v] as usize;
        cur[v] += 1;
        let pos = s[j];
        pair[i] = [pos, next];
        if j > 0 {
            pred[i] = last_owned[pa[j - 1] as usize];
        }
    };
    insert(m - 1, 0);
    for (r, &x) in ps.iter().enumerate() {
        if x > 0 {
            insert(x as usize - 1, r as u32 + 1);
        }
    }
    assert_eq!(cur[..k], loc[1..]);
    drop(cur);
    drop(pa);
    drop(ps);
    drop(s);
    drop(last_owned);
    let lists = Lists {
        pair,
        pred,
        loc,
        min,
    };
    p.end();

    let p = phase("dictionary_sort");
    // SEP < END < any raw byte. Equal finite tails are merged by continuation.
    let sa = Suffixes::sort(&dict.bytes, wide);
    p.end();
    let p = phase("owner_map");
    enum Owners {
        Dense(Vec<u32>),
        Compact(RankStarts),
    }
    let owner = if wide {
        Owners::Compact(RankStarts::new(&dict.start, d))
    } else {
        let mut a = vec![u32::MAX; d];
        for v in 0..k {
            let start = dict.start[v] as usize;
            let end = dict.start[v + 1] as usize - 1;
            a[start..end - if v == dict.eof_id { 1 } else { W }].fill(v as u32);
        }
        Owners::Dense(a)
    };
    p.end();
    let p = phase("emit");
    let audit = std::env::var("XSA_RESIDENT_AUDIT_FD").ok().map(|v| {
        let fd = v.parse().unwrap();
        assert!(fd > 2);
        unsafe { fs::File::from_raw_fd(fd) }
    });
    let mut out = Output {
        sink: std::io::stdout().lock(),
        audit,
        bytes: Vec::with_capacity(65536 * 12),
        rows: 0,
        sink_time: Duration::ZERO,
        audit_time: Duration::ZERO,
        flag,
    };
    out.push(n as u32, 0, last as u32);
    let mut q = BinaryHeap::new();
    let (mut prev, mut len, mut head) = (0, 0, 0);
    let (mut seen, mut groups, mut max_group) = (0usize, 0usize, 0usize);
    for r in 0..d {
        if r % (1 << 22) == 0 {
            stop(flag);
        }
        let i = sa.at(r);
        let v = match &owner {
            Owners::Dense(a) => {
                if a[i] == u32::MAX {
                    continue;
                }
                a[i] as usize
            }
            Owners::Compact(a) => a.word(i),
        };
        let end = dict.start[v + 1] as usize - 1;
        let size = end - i;
        if size <= if v == dict.eof_id { 1 } else { W } {
            continue;
        }
        let h = common(&dict.bytes[prev..prev + len], &dict.bytes[i..end]);
        if len == 0 || h < len.min(size) {
            flush(&mut q, &lists, len, head, &mut out);
            groups += 1;
            head = h as u32;
        } else {
            assert_eq!(len, size, "proper prefix among representatives");
        }
        let off = local_offset(i, dict.start[v]);
        let idx = lists.loc[v];
        let b = if off == 0 {
            0
        } else {
            dict.decode[dict.bytes[i - 1] as usize] as u32
        };
        q.push(Reverse((
            lists.pair[idx as usize][1],
            idx,
            lists.loc[v + 1],
            off,
            b,
        )));
        max_group = max_group.max(q.len());
        prev = i;
        len = size;
        seen += 1;
    }
    flush(&mut q, &lists, len, head, &mut out);
    out.send();
    assert_eq!(seen, descriptors);
    assert_eq!(out.rows, n as u64 + 1);
    eprintln!("RESIDENT_STREAM rows={} descriptors={seen} groups={groups} max_group={max_group} sink_write_s={:.6} audit_write_s={:.6}",out.rows,out.sink_time.as_secs_f64(),out.audit_time.as_secs_f64());
    p.end();
}
