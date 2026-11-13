//! xsa — chi sa.
//!
//! Suffixient-array tools over the ONE container family `xsa build` emits:
//!
//!   <name>.sxi2  SXI2 v10 - the block-compressed run stream (m1 + m12/m13),
//!                the locate anchors (m11), chi (m5), the document starts (m15)
//!                and the context trie (m20). THE only readable shape: see
//!                `qlite::refuse` for the one refusal every other file gets.
//!   <name>.sA    a chi position set (u64 LE stream), an EXPORT of m5.
//!
//! FORWARD-ONLY. There is no second reader and no degraded mode: a container in
//! an older shape is unsupported and must be rebuilt with `xsa build`. The
//! phi-era readers (.ri4, SXI1, the v2..v9 SXI2 shapes) are gone from this
//! binary, not parked behind a flag.

mod sxi;
mod sxi2;
mod qlite;
mod ctx;
mod lcpruns;
mod build;
mod chain;
mod bundle;
mod plan;
mod corpus;
mod pilemap;
mod corpusv2;
mod docstarts;

use std::fs::File;
use std::io::{BufRead, BufReader, Read, Write};
use std::process::exit;

fn die(msg: &str) -> ! {
    eprintln!("xsa: error: {}", msg);
    exit(1);
}

/// Wall-clock stage journal, enabled with XSA_TRACE=1. Build and open stages are
/// individually expensive, so every open path reports its own cost; a command
/// that spends minutes before the first answer must say where the minutes went.
pub fn trace_on() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("XSA_TRACE").is_ok_and(|v| v != "0"))
}
pub fn trace_ms(what: &str, ms: f64) {
    if trace_on() {
        eprintln!("TRACE stage={} ms={:.1}", what, ms);
    }
}
/// Record the elapsed time of a stage measured from `t0`.
pub fn trace_since(what: &str, t0: std::time::Instant) {
    if trace_on() {
        trace_ms(what, t0.elapsed().as_secs_f64() * 1e3);
    }
}

fn chi_count(path: &str) -> u64 {
    let md = std::fs::metadata(path).unwrap_or_else(|e| die(&format!("stat {}: {}", path, e)));
    if md.len() % 8 != 0 {
        die(&format!("{}: size {} not a multiple of 8 (u64 stream expected)", path, md.len()));
    }
    md.len() / 8
}

fn fmt_ratio(num: f64, den: f64) -> String {
    if den == 0.0 { "n/a".to_string() } else { format!("{:.3}", num / den) }
}

/// `xsa stats`: the header + law check of THE one container shape.
///
/// There is no `.ri4` arm, no rlbwt pair arm and no format sniffing: the file is
/// opened by the one reader (`sxi::Container::open`, which refuses anything that
/// is not SXI2 v10 with m15 and m20) and the numbers printed are its own header.
/// `--ri4` stays accepted as a spelling of `--sxi` because the flag names the
/// historic artifact, not a format: both mean "this container file".
fn cmd_stats(args: &[String]) {
    let mut container: Option<String> = None;
    let mut chi: Option<String> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--ri4" | "--sxi" if i + 1 < args.len() => container = Some(args[i + 1].clone()),
            "--chi" if i + 1 < args.len() => chi = Some(args[i + 1].clone()),
            other => die(&format!("stats: unknown arg {}", other)),
        }
        i += 2;
    }
    let p = container.unwrap_or_else(|| die("stats: need --sxi FILE (and optionally --chi FILE)"));
    let c = sxi::Container::open(&p).unwrap_or_else(|| die(&format!("stats: {p}: not a container")));
    let embedded_chi = c.complete.then_some(c.member(5).count);
    let index_bytes = std::fs::metadata(&p).map(|m| m.len()).ok();
    println!(
        "container {}  (SXI2 v{}: n={}, k={}, R={} runs)",
        p, c.version, c.n, c.k, c.r
    );
    c.print_members();
    let n = c.n;
    let k = c.k;
    let r = c.r;
    let x = chi.map(|p| {
        let c = chi_count(&p);
        println!("chi       {}  (chi={} positions)", p, c);
        c
    }).or(embedded_chi);

    println!();
    println!("n            = {}", n);
    if k > 0 { println!("k (records)  = {}", k); }
    println!("r (runs)     = {}", r);
    if let Some(xc) = x {
        println!("chi          = {}", xc);
        println!("n/r          = {}", fmt_ratio(n as f64, r as f64));
        println!("chi/r        = {}   (law: ~0.86)", fmt_ratio(xc as f64, r as f64));
    }
    if let Some(bytes) = index_bytes {
        println!("index bytes  = {}", bytes);
    }
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
    eprintln!("  xsa build --tape --input <file> --scratch <dir> [--threads N] [--memory-gb N]");
    eprintln!("             [--docs-map X]  X is a DIR holding seps.bin: embed m15, the document");
    eprintln!("              starts (SXI2 v10, boundaries as METADATA); X is a PREFIX holding");
    eprintln!("              the CORPUS V2 sidecar pair: validate it and journal CONTAINER_DOCS_V2");
    eprintln!("  xsa docstarts embed|verify|probe|info - document starts as container member m15");
    eprintln!("           (SXI2 v10): `embed` adds it from a map, `verify` is the rank/select parity");
    eprintln!("           gate against seps.bin, `probe` answers rank/select for a positions file");
    eprintln!("  xsa mem <container> <query> [-k N] [--min-len N] [--docs DIR] [--limit N] [--jsonl] [--all-mems]");
    eprintln!("           maximal exact matches of <query>: interval widths + bounded positions;");
    eprintln!("           default is the single longest MEM, --all-mems reports every one;");
    eprintln!("           xsa mem <container> --serve -j N is the framed (u32 len + bytes) mode;");
    eprintln!("  xsa seeds <container> <text-file|-> [--target-mult T] [--max-len 64] [--min-len 12] [--max-seeds K] [--stride S] [--skip-markup] [--docs DIR] [--limit N]");
    eprintln!("           [--jsonl] [--nearest] [--anchors] [--windows-out FILE --window-len L]");
    eprintln!("           adaptive exact-match seeding: per window the seed LENGTH is solved so");
    eprintln!("           multiplicity lands on the target fan-out; --max-mult M [--min-mult m] is the");
    eprintln!("           alternative multiplicity-band mode; --anchors emits the chain-ready anchor table");
    eprintln!("  xsa seeds <container> --serve -j N [--rss-gb G] [--cache-gb G]   warm/batch path:");
    eprintln!("           u32-length-prefixed window frames on stdin (mem --serve's encoding), the index");
    eprintln!("           opened ONCE, one fixed-width anchor frame per window on stdout;");
    eprintln!("           xsa seeds <container> --windows FILE --corpus FILE  declared window set");
    eprintln!("           (XSWIN001/JSONL: doc, base, start, len) -> the anchor table as TSV");
    eprintln!("  xsa fanout <container> <text-file|-> [--max-seeds 256] [--min-len 12] [--max-len 64] [--region-gap 2000] [--top 15] [--docs DIR] [--jsonl]");
    eprintln!("           rarity-weighted fan-out: weight(seed)=ln(D/mult), score(doc)=sum over the");
    eprintln!("           DISTINCT seeds shared; hits are also clustered into regions and scored");
    eprintln!("  xsa sxi-info <container> [--chi-out sorted.sA]   members + the header law");
    eprintln!("  --sxi is an alias for --ri4: both name the container file, not a format");
    eprintln!("  xsa stats <container> [--chi <f.sA>]     header + law check");
    eprintln!("  xsa tags --chi <f.sA> --sidecar <names.tsv> [--revlines]");
    eprintln!("           [--output tags.tsv]                  first-appearance attribution");
    eprintln!("  xsa branches <container> (--contexts FILE [--jsonl] | --serve [-j N]) [-k N] [--docs DIR | --corpus FILE]");
    eprintln!("  xsa stateless-bench CONTAINER CORPUS THREADS POSITIONS [LEFT]");
    eprintln!("  xsa consult --container <sxi2> --windows <jsonl|bin> --output <file> [--corpus FILE] [--rss-gb 8]");
    eprintln!("           [-j N] [--resume] [--left 1024] [--chunk-rows N] [--bin|--jsonl] [--no-verify-prefix]");
    eprintln!("           BOUNDED-MEMORY streaming extraction (cache-build): one query per position, the");
    eprintln!("           next-byte branch histogram after the last <=1024 bytes, appended to an fsync'd,");
    eprintln!("           journaled, resumable row file. Windows are (doc, base, start, len, left); see");
    eprintln!("           CONSULT.md for the row layout, the journal and the memory law.");
    eprintln!("  xsa query <container> <pattern> [<pattern> ...] [-j N] [--output tsv|jsonl] [--all | --count | --limit N] [--docs DIR] [--context N]");
    eprintln!("           --patterns: the corpus contract, rec<0x1e>rec<0x1e>...rec<0x1e>, arbitrary");
    eprintln!("           bytes, no escaping, id = 0-based record index; --patterns-fasta reads FASTA;");
    eprintln!("           one orientation only: the container has no alphabet, --mode is refused;");
    eprintln!("           tsv (default): document rows when sidecars match; otherwise pattern_id<TAB>offset");
    eprintln!("           --patterns FILE (0x1E records), --patterns-fasta FILE; --docs DIR overrides discovery\n           TSV pattern_id, doc_id (0-based), offset_in_doc, title, global_offset");
    eprintln!("           sidecars and any MANIFEST must match; title newlines/backslashes are escaped");
    eprintln!("           --count: interval widths only, pattern_id<TAB>pattern<TAB>occurrences; no locate walks");
    eprintln!("           --limit N: at most N hits per pattern in suffix-array rank order (0 allowed)");
    eprintln!("           deterministic, no RNG/seed; dispersed document examples, not all occurrences");
    eprintln!("           rank order is lexicographic, not a statistically unbiased document sample");
    eprintln!("           --count and --limit are exclusive; default/--all emits every hit");
    eprintln!("           -j defaults to all cores; the container identifies its own format");
    eprintln!("  xsa corpus --wikipedia <dump.xml.bz2|-> --out DIR [--separator 0x1e|none]");
    eprintln!("           one streaming pass -> corpus.bin (\"<title>\\n<body>\\x1E\" per doc), seps.bin");
    eprintln!("           (u32 start deltas + every-64th u64 anchor), titles.bin, MANIFEST.json");
    eprintln!("  xsa lcp <container> [--run I | --runs A:B | --sample N --seed S | --all] [--audit]");
    eprintln!("           member m14: the LCP over runs. One value per BWT run, in run order,");
    eprintln!("           lcpRuns[i] = lce(SA[r_i-1], SA[r_i]) at run i's HEAD row; random access is one");
    eprintln!("           16-byte directory entry + one bit slice + one escape-list binary search.");
    eprintln!("           --audit: tile the directory over the payload and check every escape list.");
    eprintln!("  xsa lcp-verify <container> --corpus FILE [--sample N] [--deep K] [--seed S]");
    eprintln!("           THE CORRECTNESS GATE: every sampled run's STORED LCP against a brute-force");
    eprintln!("           lce over the corpus (positions from locate: m11 anchor + LF walk).");
    eprintln!("  xsa corpus --pile-map --out DIR [--pile FILE] [--shards DIR] [--limit-bytes N]");
    eprintln!("           document map for pile.txt: seps.bin (XSEP v1), titles.bin (the");
    eprintln!("           pile_set_name column) and meta.bin (offset, length, shard, record, set");
    eprintln!("           name, internal 0x1E); every document is checked against the pile bytes");
    eprintln!("  xsa corpus --pile-verify --map DIR [--samples N]");
    eprintln!("           re-extract sampled documents from their shard records and compare");
    eprintln!();
    eprintln!("THE ONE CONTAINER FORMAT (forward-only):");
    eprintln!("  {}", qlite::SUPPORTED_SHAPE);
    eprintln!("  Every command refuses any other file BY NAME, with the rebuild instruction:");
    eprintln!("  an artifact in an older shape is UNSUPPORTED and must be rebuilt with");
    eprintln!("  `xsa build` - this build has no second reader and no --legacy-* escape hatch.");
    eprintln!("  .sA is a chi export (u64 LE), not an index.");
    exit(2);
}

fn main() {
    let raw: Vec<_> = std::env::args_os().skip(1).collect();
    if raw.first().is_some_and(|a| a == "branches") {
        qlite::branches_command(&raw[1..]);
        return;
    }
    if raw.first().is_some_and(|a| a == "stateless-bench") {
        qlite::stateless_bench_command(&raw[1..]);
        return;
    }
    if raw.first().is_some_and(|a| a == "consult") {
        qlite::consult_command(&raw[1..]);
        return;
    }
    if raw.first().is_some_and(|a| a == "search") {
        qlite::search_command(&raw[1..]);
        return;
    }
    if raw.first().is_some_and(|a| a == "mem") {
        qlite::mem_command(&raw[1..]);
        return;
    }
    if raw.first().is_some_and(|a| a == "seeds") {
        qlite::seeds_command(&raw[1..]);
        return;
    }
    if raw.first().is_some_and(|a| a == "fanout") {
        qlite::fanout_command(&raw[1..]);
        return;
    }
    if raw.first().is_some_and(|a| a == "query") {
        qlite::command(&raw[1..], false);
        return;
    }
    if raw.first().is_some_and(|a| a == "lcp") {
        // `xsa lcp`: the m14 accessor - "what is the LCP of run i" (SXI2 v8).
        lcpruns::command(&raw[1..].iter().map(|a| a.to_string_lossy().into_owned()).collect::<Vec<_>>());
        return;
    }
    if raw.first().is_some_and(|a| a == "lcp-verify") {
        // `xsa lcp-verify`: the correctness gate - every sampled run's STORED LCP
        // against a brute-force LCE over the corpus.
        lcpruns::verify_command(&raw[1..].iter().map(|a| a.to_string_lossy().into_owned()).collect::<Vec<_>>());
        return;
    }
    if raw.first().is_some_and(|a| a == "verify") {
        // `xsa verify`: the byte certificate over query positions - every located
        // offset is read back out of the corpus and compared with the pattern.
        qlite::command(&raw[1..], true);
        return;
    }
    let args: Vec<String> = raw.into_iter().map(|a| a.into_string().unwrap_or_else(|_| die("arguments must be UTF-8"))).collect();
    match args.first().map(|s| s.as_str()) {
        Some("build") => build::command(&args[1..]),
        Some("sxi-info") => sxi::command(&args[1..]),
        Some("docstarts") => docstarts::command(&args[1..]),
        Some("ctx-build") | Some("branch-of") | Some("trie-stat") | Some("trie-info")
        | Some("ctx-verify") | Some("ctx-selftest") | Some("ctx-curve") | Some("relax") | Some("relax-stat") | Some("trie-bench")
        | Some("ctx-tap") | Some("trie-member") | Some("ctx-tap-selftest") => ctx::command(&args[0], &args[1..]),
        Some("stats") => cmd_stats(&args[1..]),
        Some("tags") => cmd_tags(&args[1..]),

        // FORWARD-ONLY: `query-legacy`, `mems`, `serve`, `witness-build`,
        // `forks`, `build-anchors` and `chi-rspace` are GONE with the phi-era
        // readers they called. They were the .ri4/SXI1/v2..v9 arms, and after
        // this change the only container a reader accepts is SXI2 v10, which
        // none of them could parse. Naming the shape in the refusal (instead of
        // "unknown subcommand") keeps a script's failure legible.
        Some(retired @ ("query-legacy" | "mems" | "serve" | "witness-build" | "forks"
                       | "build-anchors" | "chi-rspace")) => die(&format!(
            "xsa {retired}: this subcommand was REMOVED with the phi-era readers (.ri4, SXI1,\n  \
             SXI2 v2..v9). This build serves exactly ONE container format: {}\n  \
             rebuild with `xsa build`; `xsa query` / `search` / `consult` / `seeds` / `mem`\n  \
             / `branches` / `lcp` read it.",
            qlite::SUPPORTED_SHAPE
        )),
        Some("corpus") => corpus::command(&args[1..]),
        Some("-h") | Some("--help") | None => usage(),
        Some(other) => die(&format!("unknown subcommand '{}' (try --help)", other)),
    }
}
