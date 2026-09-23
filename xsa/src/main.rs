//! xsa — chi sa.
//!
//! Suffixient-array tools over the r-space artifact family:
//!   .ri4        v4 self-contained index core (rlbwt + C + run-end SA samples)
//!   <name>.sA   chi position set (u64 LE stream)
//!   .bwt.heads/.bwt.len  run-length BWT (u8 heads, u40 LE lengths)
//!
//! The index is sovereign: every subcommand runs from these artifacts alone.

use std::fs::File;
use std::io::{BufRead, BufReader, Read, Write};
use std::process::exit;

fn die(msg: &str) -> ! {
    eprintln!("xsa: error: {}", msg);
    exit(1);
}

/// Parsed .ri4 header (v4): n, k, R. Layout (little-endian):
///   u32 magic 0x52585349 ("ISXR"), u32 version, u64 n, u64 k, u64 R, ...
#[derive(Debug, Clone, Copy)]
struct Ri4Header {
    version: u32,
    n: u64,
    k: u64,
    r: u64,
}

fn read_ri4_header(path: &str) -> Ri4Header {
    let mut f = File::open(path).unwrap_or_else(|e| die(&format!("open {}: {}", path, e)));
    let mut b = [0u8; 32];
    f.read_exact(&mut b)
        .unwrap_or_else(|e| die(&format!("read {}: {}", path, e)));
    let magic = u32::from_le_bytes(b[0..4].try_into().unwrap());
    if magic != 0x52585349 {
        die(&format!("{}: bad magic {:08x} (want ISXR)", path, magic));
    }
    let version = u32::from_le_bytes(b[4..8].try_into().unwrap());
    let n = u64::from_le_bytes(b[8..16].try_into().unwrap());
    match version {
        4 => {
            let k = u64::from_le_bytes(b[16..24].try_into().unwrap());
            let r = u64::from_le_bytes(b[24..32].try_into().unwrap());
            Ri4Header { version, n, k, r }
        }
        _ => die(&format!("{}: version {} not supported yet (this build reads v4)", path, version)),
    }
}

/// chi position count from a .sA (u64 LE stream): entries = filesize / 8.
fn chi_count(path: &str) -> u64 {
    let md = std::fs::metadata(path).unwrap_or_else(|e| die(&format!("stat {}: {}", path, e)));
    if md.len() % 8 != 0 {
        die(&format!("{}: size {} not a multiple of 8 (u64 stream expected)", path, md.len()));
    }
    md.len() / 8
}

/// rlbwt pair: R = heads size; n = sum of u40 LE lengths.
fn read_rlbwt(prefix: &str) -> (u64, u64) {
    let heads = format!("{}.bwt.heads", prefix);
    let lens = format!("{}.bwt.len", prefix);
    let r = std::fs::metadata(&heads)
        .unwrap_or_else(|e| die(&format!("stat {}: {}", heads, e)))
        .len();
    let lensz = std::fs::metadata(&lens)
        .unwrap_or_else(|e| die(&format!("stat {}: {}", lens, e)))
        .len();
    if lensz != r * 5 {
        die(&format!("{}: size {} != R*5 (u40 lengths expected)", lens, lensz));
    }
    let mut f = File::open(&lens).unwrap_or_else(|e| die(&format!("open {}: {}", lens, e)));
    let mut n: u64 = 0;
    let mut buf = [0u8; 5];
    let mut nread = 0u64;
    loop {
        match f.read(&mut buf) {
            Ok(0) => break,
            Ok(_) => {
                n += u64::from(buf[0])
                    | (u64::from(buf[1]) << 8)
                    | (u64::from(buf[2]) << 16)
                    | (u64::from(buf[3]) << 24)
                    | (u64::from(buf[4]) << 32);
                nread += 1;
            }
            Err(e) => die(&format!("read {}: {}", lens, e)),
        }
    }
    if nread != r {
        die(&format!("{}: read {} records, expected {}", lens, nread, r));
    }
    (r, n)
}

fn fmt_ratio(num: f64, den: f64) -> String {
    if den == 0.0 { "n/a".to_string() } else { format!("{:.3}", num / den) }
}

fn cmd_stats(args: &[String]) {
    let mut ri4: Option<String> = None;
    let mut rlbwt: Option<String> = None;
    let mut chi: Option<String> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--ri4" if i + 1 < args.len() => ri4 = Some(args[i + 1].clone()),
            "--rlbwt" if i + 1 < args.len() => rlbwt = Some(args[i + 1].clone()),
            "--chi" if i + 1 < args.len() => chi = Some(args[i + 1].clone()),
            other => die(&format!("stats: unknown arg {}", other)),
        }
        i += 2;
    }
    if ri4.is_none() && rlbwt.is_none() {
        die("stats: need --ri4 and/or --rlbwt (and optionally --chi)");
    }

    let mut n = 0u64;
    let mut k = 0u64;
    let mut r = 0u64;
    if let Some(p) = &ri4 {
        let h = read_ri4_header(p);
        n = h.n;
        k = h.k;
        r = h.r;
        println!(".ri4      {}  (v{}: n={}, k={} strings, R={} runs)", p, h.version, h.n, h.k, h.r);
    }
    if let Some(p) = &rlbwt {
        let (rr, nn) = read_rlbwt(p);
        if r != 0 && (rr != r || nn != n) {
            die(&format!(
                "stats: rlbwt disagrees with .ri4 (rlbwt R={} n={} vs ri4 R={} n={})",
                rr, nn, r, n
            ));
        }
        r = rr;
        n = nn;
        println!("rlbwt     {}  (R={} runs, n={})", p, rr, nn);
    }
    let x = chi.map(|p| {
        let c = chi_count(&p);
        println!("chi       {}  (chi={} positions)", p, c);
        c
    });

    println!();
    println!("n            = {}", n);
    if k > 0 { println!("k (strings)  = {}", k); }
    println!("r (runs)     = {}", r);
    if let Some(xc) = x {
        println!("chi          = {}", xc);
        println!("n/r          = {}", fmt_ratio(n as f64, r as f64));
        println!("chi/r        = {}   (law: ~0.86)", fmt_ratio(xc as f64, r as f64));
    }
    println!("index bytes  ~ {} (rlbwt 6R + samples 8R)", r * 14);
}

fn cmd_tags(args: &[String]) {
    let mut chi: Option<String> = None;
    let mut sidecar: Option<String> = None;
    let mut revlines = false;
    let mut out: Option<String> = None;
    let mut i = 0;
    while i < args.len() {
        let step;
        match args[i].as_str() {
            "--chi" if i + 1 < args.len() => { chi = Some(args[i + 1].clone()); step = 2; }
            "--sidecar" if i + 1 < args.len() => { sidecar = Some(args[i + 1].clone()); step = 2; }
            "--output" if i + 1 < args.len() => { out = Some(args[i + 1].clone()); step = 2; }
            "--revlines" => { revlines = true; step = 1; }
            other => die(&format!("tags: unknown arg {}", other)),
        }
        i += step;
    }
    let (chi_path, sc_path) = (
        chi.unwrap_or_else(|| die("tags: need --chi <f.sA>")),
        sidecar.unwrap_or_else(|| die("tags: need --sidecar <names.tsv>")),
    );

    // sidecar: name \t fstart \t len (names may contain spaces; tab-split only)
    struct Str { name: String, fs: u64, len: u64, fstart_stream: u64 }
    let mut strs: Vec<Str> = Vec::new();
    let sf = File::open(&sc_path).unwrap_or_else(|e| die(&format!("open {}: {}", sc_path, e)));
    for (ln, line) in BufReader::new(sf).lines().enumerate() {
        let line = line.unwrap_or_else(|e| die(&format!("read {}: {}", sc_path, e)));
        if line.is_empty() { continue; }
        let c: Vec<&str> = line.split('\t').collect();
        if c.len() < 3 {
            die(&format!("{}:{}: expected 3 tab-separated fields", sc_path, ln + 1));
        }
        let (fs, len): (u64, u64) = (
            c[1].trim().parse().unwrap_or_else(|_| die(&format!("{}:{}: bad fstart", sc_path, ln + 1))),
            c[2].trim().parse().unwrap_or_else(|_| die(&format!("{}:{}: bad len", sc_path, ln + 1))),
        );
        // stream fstart derived from lens (+ sentinels), NEVER from the
        // sidecar's forward offset (sub-collection sidecars carry offsets
        // into a larger original text; only len is authoritative here)
        let fstart_stream: u64 = if ln == 0 { 0 } else {
            strs.last().map(|s: &Str| s.fstart_stream + s.len + 1).unwrap()
        };
        strs.push(Str { name: c[0].to_string(), fs, len, fstart_stream });
    }
    if strs.is_empty() { die("tags: empty sidecar"); }
    let n_streams: u64 = strs.last().unwrap().fstart_stream + strs.last().unwrap().len + 1;
    let chi_n = chi_count(&chi_path);   // sanity: count vs sidecar n below

    // chi positions: u64 LE stream
    let cf = File::open(&chi_path).unwrap_or_else(|e| die(&format!("open {}: {}", chi_path, e)));
    let mut cnt: Vec<u64> = vec![0; strs.len()];
    let mut tags: Vec<(u64, u64)> = Vec::new();   // (chi position, forward position)
    let mut buf = [0u8; 8];
    let mut nchi = 0u64;
    let mut virtual_end = 0u64;
    let mut first_err: Option<String> = None;
    {
        let mut r = cf;
        loop {
            match r.read_exact(&mut buf) {
                Ok(()) => {}
                Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
                Err(e) => die(&format!("read {}: {}", chi_path, e)),
            }
            let p = u64::from_le_bytes(buf);
            nchi += 1;
            if p == n_streams {
                // virtual end witness (S subseteq [n]): the theory includes
                // position n; attribute to no string, emit fwd = -1
                virtual_end += 1;
                tags.push((p, u64::MAX));
                continue;
            }
            // owning string: fstart_stream <= p <= fstart_stream + len
            // (endmarker positions are legitimate chi members: the walk starts there)
            let ii = strs.partition_point(|s| s.fstart_stream <= p);
            let i = if ii == 0 { usize::MAX } else { ii - 1 };
            if i == usize::MAX || p > strs[i].fstart_stream + strs[i].len {
                if first_err.is_none() {
                    first_err = Some(format!("position {} outside any string", p));
                }
                break;
            }
            let s = &strs[i];
            let j = p - s.fstart_stream;               // 0-based within-string stream offset
            let fwd = if revlines {
                s.fs + (s.len - j)                     // reversed stream -> forward coords
            } else {
                s.fs + j
            };
            cnt[i] += 1;
            tags.push((p, fwd));
        }
    }
    if let Some(e) = first_err { die(&format!("tags: {}", e)); }
    if nchi == 0 { die("tags: empty chi set"); }

    if let Some(op) = &out {
        let mut f = File::create(op).unwrap_or_else(|e| die(&format!("create {}: {}", op, e)));
        for (p, fwd) in &tags {
            if *fwd == u64::MAX {
                writeln!(f, "{}\t-1", p).unwrap();   // virtual end witness
            } else {
                writeln!(f, "{}\t{}", p, fwd).unwrap();
            }
        }
    }

    println!("chi positions  = {} over {} strings (n = {})", nchi, strs.len(), n_streams);
    if virtual_end > 0 {
        println!("virtual end    = {} (position n; S subseteq [n] convention)", virtual_end);
    }
    let _ = chi_n;
    println!("per-string first-appearance counts (top 15):");
    let mut idx: Vec<usize> = (0..strs.len()).collect();
    idx.sort_by(|&a, &b| cnt[b].cmp(&cnt[a]).then(strs[a].name.cmp(&strs[b].name)));
    for &i in idx.iter().take(15) {
        if cnt[i] == 0 { break; }
        println!("  {:>12}  {}", cnt[i], strs[i].name);
    }
    let tot: u64 = cnt.iter().sum::<u64>() + virtual_end;
    println!("sum check      = {} {}", tot, if tot == nchi { "OK" } else { "MISMATCH" });
    if tot != nchi { exit(1); }
}

fn usage() -> ! {
    eprintln!("xsa — chi sa. suffixient-array tools over r-space artifacts");
    eprintln!();
    eprintln!("usage:");
    eprintln!("  xsa stats --ri4 <f.ri4> [--chi <f.sA>]   header + law check");
    eprintln!("  xsa stats --rlbwt <prefix> [--chi ...]   from rlbwt pair");
    eprintln!("  xsa tags --chi <f.sA> --sidecar <names.tsv> [--revlines]");
    eprintln!("           [--output tags.tsv]                  first-appearance attribution");
    eprintln!();
    eprintln!("artifacts: .ri4 (v4: rlbwt + C + run-end SA samples), .sA (chi, u64 LE)");
    exit(2);
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(|s| s.as_str()) {
        Some("stats") => cmd_stats(&args[1..]),
        Some("tags") => cmd_tags(&args[1..]),
        Some("-h") | Some("--help") | None => usage(),
        Some(other) => die(&format!("unknown subcommand '{}' (try --help)", other)),
    }
}
