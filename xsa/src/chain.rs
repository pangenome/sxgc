//! `xsa build --input`: the consolidated external construction chain.
//!
//! One cargo-built command runs the whole verified route from pushed main:
//! snap preflight (0x1E document boundary, fail-loud on mid-document input,
//! per the demonstrated chunk_frontend refusal), libsais chunk front end,
//! externalized cross-LCP merge tree (`--emit-pf` side streams, fixed
//! concurrent pool ON by default; CROSS_NO_POOL / CROSS_ASYNC_FILL /
//! CROSS_HASH_K / CROSS_PIPELINE / CROSS_SHARDS pass through as environment
//! knobs), and the
//! external adopt finish (rpfbwt_endpoints + slim_dump + the streamed chi
//! sweep) consuming the merge's `.pftext`/`.pfck` side streams.
//!
//! Discipline carried over from the reference gates
//! (bit6/sxi_logs/external-columns/gate_10b_external_rebuild.sh): a hard
//! RLIMIT_AS cap on every phase, a disc preflight that refuses under 15%
//! free on the scratch filesystem, and one uniform journal with per-phase
//! wall / peak-RSS / disc telemetry. The production byte-remap
//! (0x01->FF, 0x02->FD, 0x03->FE, 0x04->FC; 0x1E fixed) is required for raw
//! pile bytes (literal padding-range bytes break the cyclic dollar) and is
//! embedded here exactly as the banked fragment route applied it.
//!
//! No algorithm changes: the bundled stages are the same sources, built with
//! the same gate flags, so outputs are byte-for-byte those of the chain.
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use sha2::{Digest, Sha256};

pub const USAGE: &str = "usage:
  xsa build --input <corpus> --scratch <dir> [--snap-1e] [--memory-gb N] [--threads N] [--chunks N] [--remap <256-byte-map>]

The consolidated external construction chain (chunk -> merge tree -> adopt
finish -> chi sweep), one cargo-built command with bundled provenance.

  --input PATH       corpus file: ONE cyclic 0x1E-terminated byte string
                     (s1 0x1E s2 0x1E ... sk 0x1E); mid-document input fails
                     loud before any phase runs (the demonstrated refusal).
  --scratch DIR      all intermediate and final artifacts are written here:
                     chunks/ merged/ mwork/ finish/ and xsa-build.log.
                     The scratch filesystem must hold >= 15% free (df gate).
  --snap-1e          additionally verify and journal the input snap
                     (SNAP_OK n=<size> sha256=<...>); the boundary check
                     itself always runs. Snapping is never done silently:
                     a raw prefix must be extended forward to the next 0x1E
                     in the source corpus before it is passed here.
  --memory-gb N      hard RLIMIT_AS cap (GiB) applied to every phase
                     (default 24; a lower inherited hard limit always wins).
  --threads N        merge/finish thread count (default 48, gate discipline).
  --chunks N         chunk count (default 32, the 10 GB gate value; outputs
                     are chunk-count invariant - the exact merge certifies
                     trees of 5 and 16 chunks byte-identical to the bank).
  --remap PATH       256-byte bijective byte map applied by the chunk front
                     end. Default: the embedded production map (the banked
                     fragment route's frag.remap, sha256 b4f38776...).

Pool knobs pass through to the merge unchanged (default: fixed concurrent
pool ON): CROSS_NO_POOL=1, CROSS_ASYNC_FILL=1, CROSS_HASH_K=K, CROSS_PIPELINE=1,
CROSS_SHARDS=S (parallel emission: the order/run-boundary shard count for the
merge walks; default --threads, 1 = the serial walk).

Outputs (byte-identical to the reference chain on the same corpus):
  merged/frag.rlebwt .rlebwt.meta .ssa .ssa_t .pftext .pfck
  finish/frag.ri4 .head_sa .agg .sA
The uniform journal (xsa-build.log) reports chi and per-phase wall / peak
RSS / disc telemetry.

  xsa build --selftest [CASES] [--threads N] [--scratch DIR]
                     runs the merge selftest for the six banked sweep seeds
                     (11, 99, 20261002, 7, 5, 13; default 2000 cases each).";

/// The production byte-remap: an involution that swaps the padding-range
/// bytes 0x01->0xFF, 0x02->0xFD, 0x03->0xFE, 0x04->0xFC (and back),
/// keeping the 0x1E document separator fixed. Literal padding-range bytes
/// in raw pile text collide with the cyclic padding dollar and the merge
/// refuses loudly; the banked fragment route applied exactly this map
/// before the chunk front end.
const PRODUCTION_REMAP: [u8; 256] = {
    let mut map = [0u8; 256];
    let mut i = 0;
    while i < 256 {
        map[i] = i as u8;
        i += 1;
    }
    map[0x01] = 0xFF;
    map[0x02] = 0xFD;
    map[0x03] = 0xFE;
    map[0x04] = 0xFC;
    map[0xFC] = 0x04;
    map[0xFD] = 0x02;
    map[0xFE] = 0x03;
    map[0xFF] = 0x01;
    map
};
/// sha256 of the banked fragment route's frag.remap (256 bytes).
const PRODUCTION_REMAP_SHA256: &str =
    "b4f387767707f3095a0981148a1753f8da5885c30c19753b3b93887153e611be";
/// The six banked selftest sweep seeds (bit6/sxi_logs/pool-verification).
const SELFTEST_SEEDS: [u64; 6] = [11, 99, 20261002, 7, 5, 13];
/// Disc preflight: refuse when the scratch filesystem is more than this
/// percent used (the gates' >= 15% free discipline).
const DISC_MAX_USED_PERCENT: u32 = 85;
/// ulimit wrapper: cap phase address space, then exec the real command.
/// A lower inherited hard limit always wins and is journaled, never raised.
const PHASE_WRAPPER: &str = "ulimit -v \"$1\" 2>/dev/null || ulimit -v \"$(ulimit -H -v)\" 2>/dev/null || true; shift; printf 'PHASE_CAP_KB=%s\\n' \"$(ulimit -v)\" >&2; exec \"$@\"";

#[derive(Debug)]
struct Options {
    input: PathBuf,
    scratch: PathBuf,
    snap_1e: bool,
    memory_gb: u64,
    threads: u32,
    chunks: u32,
    remap: Option<PathBuf>,
}

#[derive(Debug)]
struct SelftestOptions {
    cases: u64,
    threads: u32,
    scratch: Option<PathBuf>,
}

fn parse(args: &[String]) -> Result<SelftestOptions, String> {
    let mut cases = 2000u64;
    let mut threads = 48u32;
    let mut scratch = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--selftest" => {
                if let Some(value) = args.get(i + 1).filter(|s| s.chars().all(|c| c.is_ascii_digit()) && !s.is_empty()) {
                    cases = value.parse().map_err(|_| "--selftest: integer expected".to_string())?;
                    i += 1;
                }
            }
            "--threads" => {
                i += 1;
                threads = parse_u32(args.get(i), "--threads")?;
            }
            "--scratch" => {
                i += 1;
                scratch = Some(PathBuf::from(value(args.get(i), "--scratch")?));
            }
            _ => return Err(format!("unknown option {}", args[i])),
        }
        i += 1;
    }
    if cases == 0 {
        return Err("--selftest needs a positive case count".into());
    }
    Ok(SelftestOptions { cases, threads, scratch })
}

fn value<'a>(arg: Option<&'a String>, flag: &str) -> Result<&'a String, String> {
    arg.filter(|s| !s.is_empty()).ok_or_else(|| format!("{flag} requires a value"))
}

fn parse_u32(arg: Option<&String>, flag: &str) -> Result<u32, String> {
    value(arg, flag)?.parse().map_err(|_| format!("{flag}: integer expected"))
}

fn parse_chain(args: &[String]) -> Result<Options, String> {
    let mut input = None;
    let mut scratch = None;
    let mut snap_1e = false;
    let mut memory_gb = 24u64;
    let mut threads = 48u32;
    let mut chunks = 32u32;
    let mut remap = None;
    let mut i = 0;
    while i < args.len() {
        let flag = &args[i];
        match flag.as_str() {
            "--input" => {
                i += 1;
                let path = PathBuf::from(value(args.get(i), "--input")?);
                if input.replace(path).is_some() {
                    return Err("specify --input only once".into());
                }
            }
            "--scratch" => {
                i += 1;
                let path = PathBuf::from(value(args.get(i), "--scratch")?);
                if scratch.replace(path).is_some() {
                    return Err("specify --scratch only once".into());
                }
            }
            "--snap-1e" => snap_1e = true,
            "--memory-gb" => {
                i += 1;
                memory_gb = value(args.get(i), "--memory-gb")?
                    .parse().map_err(|_| "--memory-gb: integer expected".to_string())?;
                if memory_gb == 0 {
                    return Err("--memory-gb must be positive".into());
                }
            }
            "--threads" => {
                i += 1;
                threads = parse_u32(args.get(i), "--threads")?;
            }
            "--chunks" => {
                i += 1;
                chunks = parse_u32(args.get(i), "--chunks")?;
                if chunks == 0 {
                    return Err("--chunks must be positive".into());
                }
            }
            "--remap" => {
                i += 1;
                let path = PathBuf::from(value(args.get(i), "--remap")?);
                if remap.replace(path).is_some() {
                    return Err("specify --remap only once".into());
                }
            }
            _ => return Err(format!("unknown option {flag}")),
        }
        i += 1;
    }
    let input = input.ok_or("need --input <corpus>")?;
    let scratch = scratch.ok_or("need --scratch <dir>")?;
    Ok(Options { input, scratch, snap_1e, memory_gb, threads, chunks, remap })
}

/// Last byte of a file (the snap-boundary probe).
fn last_byte(path: &Path) -> Result<u8, String> {
    let mut file = File::open(path).map_err(|e| format!("open {}: {e}", path.display()))?;
    let size = file.metadata().map_err(|e| format!("stat {}: {e}", path.display()))?.len();
    if size == 0 {
        return Err(format!("{}: empty input", path.display()));
    }
    use std::io::Seek;
    let mut byte = [0u8; 1];
    file.seek(std::io::SeekFrom::Start(size - 1)).and_then(|_| file.read_exact(&mut byte))
        .map_err(|e| format!("read tail {}: {e}", path.display()))?;
    Ok(byte[0])
}

/// The demonstrated refusal: a raw cut that ends mid-document must never be
/// quietly trimmed or extended; the chain refuses before any phase runs.
fn check_snap_boundary(path: &Path) -> Result<u64, String> {
    let size = path.metadata().map_err(|e| format!("stat {}: {e}", path.display()))?.len();
    let tail = last_byte(path)?;
    if tail != 0x1e {
        return Err(format!(
            "SNAP_REFUSED mid-document input {}: last byte is 0x{tail:02x}, want 0x1e \
             (document boundary); snap the prefix forward to the next 0x1e in the \
             source corpus and retry - the chain never trims or extends silently",
            path.display()
        ));
    }
    Ok(size)
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let mut file = File::open(path).map_err(|e| format!("open {}: {e}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut block = [0u8; 1 << 20];
    loop {
        let n = file.read(&mut block).map_err(|e| format!("read {}: {e}", path.display()))?;
        if n == 0 {
            break;
        }
        hasher.update(&block[..n]);
    }
    Ok(format!("{:x}", hasher.finalize()))
}

/// `df --output=pcent` percent-used value: `  Use%\n 89%` -> Some(89).
fn parse_used_percent(output: &str) -> Option<u32> {
    output.lines().rev().find_map(|line| {
        let trimmed = line.trim().trim_end_matches('%').trim();
        trimmed.parse::<u32>().ok().filter(|_| trimmed.chars().all(|c| c.is_ascii_digit()))
    })
}

fn disc_allows_used_percent(used: u32) -> bool {
    used <= DISC_MAX_USED_PERCENT
}

/// Percent used on the filesystem holding `path` (df gate probe).
fn disc_used_percent(path: &Path) -> Result<u32, String> {
    let out = Command::new("df").arg("--output=pcent").arg(path)
        .output().map_err(|e| format!("run df: {e}"))?;
    if !out.status.success() {
        return Err("df failed".into());
    }
    let text = String::from_utf8_lossy(&out.stdout);
    parse_used_percent(&text).ok_or_else(|| format!("unparsable df output: {text:?}"))
}

fn disc_preflight(path: &Path) -> Result<(), String> {
    let used = disc_used_percent(path)?;
    if !disc_allows_used_percent(used) {
        return Err(format!(
            "DF_GATE_FAIL {} {}% used (>= {}% free required on the scratch filesystem)",
            path.display(), used, 100 - DISC_MAX_USED_PERCENT
        ));
    }
    Ok(())
}

/// Bytes used on the filesystem holding `path` (peak-disc telemetry).
fn disc_used_bytes(path: &Path) -> Result<u64, String> {
    let out = Command::new("df").arg("-B1").arg("--output=used").arg(path)
        .output().map_err(|e| format!("run df: {e}"))?;
    if !out.status.success() {
        return Err("df failed".into());
    }
    let text = String::from_utf8_lossy(&out.stdout);
    text.lines().rev().find_map(|line| line.trim().parse::<u64>().ok())
        .ok_or_else(|| format!("unparsable df output: {text:?}"))
}

/// Parse `/usr/bin/time -v` output for wall clock and peak RSS.
fn parse_time_report(text: &str) -> (Option<String>, Option<u64>) {
    let mut wall = None;
    let mut rss = None;
    for line in text.lines() {
        let line = line.trim_start();
        if line.starts_with("Elapsed (wall clock) time") {
            wall = line.split("): ").last().map(|s| s.trim().to_string());
        } else if let Some(rest) = line.strip_prefix("Maximum resident set size (kbytes):") {
            rss = rest.trim().parse().ok();
        }
    }
    (wall, rss)
}

/// One uniform journal: every line is appended to the log and echoed.
struct Journal {
    file: File,
}

impl Journal {
    fn open(path: &Path) -> Result<Journal, String> {
        let file = File::options().create_new(true).append(true).open(path)
            .map_err(|e| format!("create {}: {e}", path.display()))?;
        Ok(Journal { file })
    }
    fn line(&mut self, text: &str) {
        let _ = writeln!(self.file, "{text}");
        println!("{text}");
    }
}

/// Run one phase under /usr/bin/time -v with the hard RLIMIT_AS cap, exactly
/// as the reference gates do: stdout/stderr land in `log_path`, wall/peak-RSS
/// telemetry in `time_path`, both under the scratch tree and journaled.
fn run_phase(
    journal: &mut Journal,
    scratch: &Path,
    tag: &str,
    memory_kb: u64,
    program: &str,
    args: &[String],
) -> Result<(), String> {
    let log_path = scratch.join(format!("{tag}.log"));
    let time_path = scratch.join(format!("{tag}.time"));
    let _ = fs::remove_file(&log_path);
    let _ = fs::remove_file(&time_path);
    let command_line = format!("{program} {}", args.join(" "));
    journal.line(&format!("PHASE {tag} START {command_line}"));
    let log = File::options().create(true).append(true).open(&log_path)
        .map_err(|e| format!("create {}: {e}", log_path.display()))?;
    let log_err = log.try_clone().map_err(|e| format!("clone {tag} log handle: {e}"))?;
    let status = Command::new("bash")
        .arg("-c").arg(PHASE_WRAPPER)
        .arg("xsa-phase") // $0
        .arg(memory_kb.to_string()) // $1: RLIMIT_AS in KiB
        .arg("/usr/bin/time").arg("-v").arg("-o").arg(&time_path)
        .arg(program).args(args)
        .current_dir(scratch)
        .stdout(Stdio::from(log))
        .stderr(Stdio::from(log_err))
        .status()
        .map_err(|e| format!("launch {tag}: {e}"))?;
    let (wall, peak_rss_kb) = fs::read_to_string(&time_path)
        .map(|text| parse_time_report(&text))
        .unwrap_or((None, None));
    let used_after = disc_used_bytes(scratch)?;
    journal.line(&format!(
        "PHASE {tag} END rc={} wall={} peak_rss_kb={} disc_used_bytes={}",
        status.code().unwrap_or(-1),
        wall.as_deref().unwrap_or("?"),
        peak_rss_kb.map(|v| v.to_string()).unwrap_or_else(|| "?".into()),
        used_after,
    ));
    if !status.success() {
        let tail = fs::read_to_string(&log_path).unwrap_or_default();
        let tail: Vec<&str> = tail.lines().rev().take(5).collect::<Vec<_>>().into_iter().rev().collect();
        return Err(format!("PHASE {tag} FAILED (rc {}); log tail:\n{}", status.code().unwrap_or(-1), tail.join("\n")));
    }
    Ok(())
}

fn require_log_contains(scratch: &Path, tag: &str, needle: &str) -> Result<String, String> {
    let text = fs::read_to_string(scratch.join(format!("{tag}.log")))
        .map_err(|e| format!("read {tag} log: {e}"))?;
    let line = text.lines().rev().find(|l| l.contains(needle))
        .ok_or_else(|| format!("{tag} log missing {needle}"))?;
    Ok(line.to_string())
}

fn count_log_contains(scratch: &Path, tag: &str, needle: &str) -> usize {
    fs::read_to_string(scratch.join(format!("{tag}.log")))
        .map(|t| t.lines().filter(|l| l.contains(needle)).count())
        .unwrap_or(0)
}

/// The bundled native stages (verified by hash on every use).
fn bundle_tools() -> Result<PathBuf, String> {
    let root = super::bundle::resolve()?;
    for name in ["chunk_frontend", "cross_lcp_merge", "rpfbwt_endpoints", "slim_dump"] {
        let path = root.join(name);
        let ok = fs::metadata(&path).map(|m| m.is_file()).unwrap_or(false);
        if !ok {
            return Err(format!("bundled stage missing: {name} (rebuild this xsa package)"));
        }
    }
    Ok(root)
}

fn write_remap(scratch: &Path, override_path: Option<&Path>) -> Result<PathBuf, String> {
    let path = scratch.join("remap-256.bin");
    match override_path {
        Some(source) => {
            let bytes = fs::read(source).map_err(|e| format!("read remap {}: {e}", source.display()))?;
            if bytes.len() != 256 {
                return Err(format!("remap {} must have exactly 256 bytes", source.display()));
            }
            fs::write(&path, bytes).map_err(|e| format!("write {}: {e}", path.display()))?;
        }
        None => {
            // The embedded production map must equal the banked fragment
            // route's frag.remap byte for byte; refuse any drift.
            let digest = format!("{:x}", Sha256::digest(PRODUCTION_REMAP));
            if digest != PRODUCTION_REMAP_SHA256 {
                return Err("internal production remap drift: sha256 mismatch".into());
            }
            fs::write(&path, PRODUCTION_REMAP).map_err(|e| format!("write {}: {e}", path.display()))?;
        }
    }
    Ok(path)
}

fn run_selftest(options: SelftestOptions) -> Result<(), String> {
    let tools = bundle_tools()?;
    let merge = tools.join("cross_lcp_merge");
    let (hold, _guard) = match options.scratch {
        Some(dir) => {
            fs::create_dir_all(&dir).map_err(|e| format!("create {}: {e}", dir.display()))?;
            (dir, None)
        }
        None => {
            let dir = tempfile::tempdir().map_err(|e| format!("tempdir: {e}"))?;
            (dir.path().to_path_buf(), Some(dir))
        }
    };
    let mut journal = Journal::open(&hold.join("xsa-build-selftest.log"))?;
    journal.line(&format!("SELFTEST_SWEEP START cases={} seeds={} threads={}",
        options.cases, SELFTEST_SEEDS.len(), options.threads));
    for seed in SELFTEST_SEEDS {
        let tag = format!("selftest-seed-{seed}");
        let args = [
            "--selftest".to_string(), options.cases.to_string(),
            "--seed".to_string(), seed.to_string(),
            "--threads".to_string(), options.threads.to_string(),
        ];
        run_phase(&mut journal, &hold, &tag, SELFTEST_MEMORY_KB, merge.to_str().unwrap(), &args)?;
        let pass = require_log_contains(&hold, &tag, "SELFTEST_PASS")?;
        journal.line(&format!("SEED {seed} {pass}"));
    }
    journal.line("SELFTEST_SWEEP_PASS");
    Ok(())
}

/// Hard RLIMIT_AS for selftest phases (KiB): the merge selftest is small but
/// is capped like every other run.
const SELFTEST_MEMORY_KB: u64 = 24 * 1048576;

fn run_chain(options: Options) -> Result<(), String> {
    let tools = bundle_tools()?;
    // Absolute paths everywhere: phases run with cwd inside the scratch tree.
    let input = fs::canonicalize(&options.input)
        .map_err(|e| format!("resolve {}: {e}", options.input.display()))?;
    if !input.is_file() {
        return Err(format!("{} is not a regular file", input.display()));
    }
    let scratch = &options.scratch;
    fs::create_dir_all(scratch).map_err(|e| format!("create {}: {e}", scratch.display()))?;
    let scratch = fs::canonicalize(scratch).map_err(|e| format!("resolve {}: {e}", options.scratch.display()))?;
    let journal_path = scratch.join("xsa-build.log");
    if journal_path.exists() {
        return Err(format!(
            "scratch already used: {} exists; move the previous run away (never overwrite)",
            journal_path.display()
        ));
    }
    let mut journal = Journal::open(&journal_path)?;
    for knob in ["CROSS_NO_POOL", "CROSS_ASYNC_FILL", "CROSS_HASH_K", "CROSS_PIPELINE", "CROSS_SHARDS"] {
        if let Some(value) = std::env::var_os(knob) {
            journal.line(&format!("KNOB {knob}={}", value.to_string_lossy()));
        }
    }
    // Preflight: disc gate before anything is written.
    disc_preflight(&scratch)?;
    journal.line(&format!(
        "XSA_BUILD START input={} scratch={} memory_gb={} threads={} chunks={} remap={}",
        input.display(), scratch.display(), options.memory_gb, options.threads,
        options.chunks, if options.remap.is_some() { "provided" } else { "production" }
    ));
    // Snap preflight: the demonstrated refusal fires before any phase runs.
    let n = check_snap_boundary(&input)?;
    if options.snap_1e {
        let digest = sha256_file(&input)?;
        journal.line(&format!("SNAP_OK n={n} sha256={digest}"));
    } else {
        journal.line(&format!("SNAP_BOUNDARY_OK n={n} (0x1e; pass --snap-1e to also journal the full sha256)"));
    }
    let remap_path = write_remap(&scratch, options.remap.as_deref())?;
    let remap_sha = sha256_file(&remap_path)?;
    journal.line(&format!("REMAP {} sha256={remap_sha}", remap_path.display()));
    for dir in ["chunks", "merged", "mwork", "finish"] {
        fs::create_dir_all(scratch.join(dir)).map_err(|e| format!("create {dir}: {e}"))?;
    }
    let memory_kb = options.memory_gb * 1048576;
    let merged = scratch.join("merged");
    let finish = scratch.join("finish");
    let prefix = merged.join("frag");
    let frag = prefix.to_str().ok_or("scratch path is not UTF-8")?.to_string();
    let s = |path: &str| path.to_string();

    // 1. chunk front end (libsais, document-aligned cyclic BWT chunks).
    run_phase(&mut journal, &scratch, "chunk", memory_kb, tools.join("chunk_frontend").to_str().unwrap(), &[
        s(&input.to_string_lossy()), options.chunks.to_string(),
        s(&scratch.join("chunks").to_string_lossy()), s(&remap_path.to_string_lossy()),
    ])?;
    journal.line(&require_log_contains(&scratch, "chunk", "CHUNKS_PASS")?);

    // 2. externalized merge tree (delete-as-consumed scratch, --emit-pf).
    run_phase(&mut journal, &scratch, "merge", memory_kb, tools.join("cross_lcp_merge").to_str().unwrap(), &[
        "--tree".into(), s(&scratch.join("chunks").to_string_lossy()), options.chunks.to_string(),
        n.to_string(), frag.clone(),
        "--threads".into(), options.threads.to_string(),
        "--work".into(), s(&scratch.join("mwork").to_string_lossy()),
        "--emit-pf".into(),
    ])?;
    let pairs = count_log_contains(&scratch, "merge", "CROSS_PAIR");
    journal.line(&format!("MERGE cross_pairs={pairs}"));
    journal.line(&require_log_contains(&scratch, "merge", "CROSS_PF_EMIT")?);

    // 3. external adopt finish: endpoints + slim consume the pf side streams.
    run_phase(&mut journal, &scratch, "endpoints", memory_kb, tools.join("rpfbwt_endpoints").to_str().unwrap(), &[
        frag.clone(), s(&finish.join("frag.ri4").to_string_lossy()),
        s(&finish.join("frag.head_sa").to_string_lossy()),
        "--pf-text".into(), format!("{frag}.pftext"),
        "--pf-checkpoints".into(), format!("{frag}.pfck"),
    ])?;
    journal.line(&require_log_contains(&scratch, "endpoints", "ENDPOINT_PASS")?);
    run_phase(&mut journal, &scratch, "slim", memory_kb, tools.join("slim_dump").to_str().unwrap(), &[
        "--slim".into(), "--resolve-ri4".into(),
        "--ri4".into(), s(&finish.join("frag.ri4").to_string_lossy()),
        "--head-sa".into(), s(&finish.join("frag.head_sa").to_string_lossy()),
        "-t".into(), options.threads.to_string(),
        "--pf-text".into(), format!("{frag}.pftext"),
        "--pf-checkpoints".into(), format!("{frag}.pfck"),
        "-o".into(), s(&finish.join("frag.agg").to_string_lossy()),
    ])?;

    // 4. streamed chi sweep (this same xsa binary).
    let xsa = std::env::current_exe().map_err(|e| format!("current exe: {e}"))?;
    run_phase(&mut journal, &scratch, "sweep", memory_kb, xsa.to_str().unwrap(), &[
        "chi-rspace".into(), "--stream-agg".into(),
        "--ri4".into(), s(&finish.join("frag.ri4").to_string_lossy()),
        "--agg".into(), s(&finish.join("frag.agg").to_string_lossy()),
        "-o".into(), s(&finish.join("frag.sA").to_string_lossy()),
    ])?;
    let chi = require_log_contains(&scratch, "sweep", "chi = ")?;
    let peak_disc = disc_used_bytes(&scratch)?;
    journal.line(&format!("PEAK_DISC_USED_BYTES {peak_disc} (scratch filesystem, after finish)"));
    journal.line(&format!("XSA_BUILD_DONE {chi}"));
    Ok(())
}

pub fn command(args: &[String]) {
    if args.iter().any(|a| matches!(a.as_str(), "--help" | "-h")) {
        println!("{USAGE}");
        return;
    }
    let result = if args.iter().any(|a| a == "--selftest") {
        parse(args).and_then(run_selftest)
    } else {
        parse_chain(args).and_then(run_chain)
    };
    if let Err(e) = result {
        super::die(&format!("build: {e}"));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmpfile(bytes: &[u8]) -> PathBuf {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("corpus");
        std::fs::write(&path, bytes).unwrap();
        // The tempdir is removed on drop; copy the name semantics we need by
        // keeping the file alive for the duration of the test via leak.
        std::mem::forget(dir);
        path
    }

    #[test]
    fn snap_refusal_mid_document_input_fails_loud() {
        let unaligned = tmpfile(b"mid-document cut ends without a separator");
        let err = check_snap_boundary(&unaligned).unwrap_err();
        assert!(err.contains("SNAP_REFUSED"), "refusal must be loud: {err}");
        assert!(err.contains("0x1e"), "refusal must name the boundary byte: {err}");
        let empty = tmpfile(b"");
        assert!(check_snap_boundary(&empty).unwrap_err().contains("empty input"));
    }

    #[test]
    fn snap_aligned_input_passes() {
        let aligned = tmpfile(b"s1\x1es2\x1e");
        assert_eq!(check_snap_boundary(&aligned).unwrap(), 6);
    }

    #[test]
    fn disc_preflight_refuses_under_15_percent_free() {
        assert!(disc_allows_used_percent(85));
        assert!(!disc_allows_used_percent(86));
        assert_eq!(parse_used_percent("  Use%\n 85%\n"), Some(85));
        assert_eq!(parse_used_percent("  Use%\n 89%\n"), Some(89));
        assert_eq!(parse_used_percent("  Use%\n 0%\n"), Some(0));
        assert_eq!(parse_used_percent("garbage"), None);
    }

    #[test]
    fn production_remap_matches_banked_framemap() {
        let digest = format!("{:x}", Sha256::digest(PRODUCTION_REMAP));
        assert_eq!(digest, PRODUCTION_REMAP_SHA256, "embedded remap drifted from frag.remap");
        assert_eq!(PRODUCTION_REMAP[0x01], 0xFF);
        assert_eq!(PRODUCTION_REMAP[0x02], 0xFD);
        assert_eq!(PRODUCTION_REMAP[0x03], 0xFE);
        assert_eq!(PRODUCTION_REMAP[0x04], 0xFC);
        assert_eq!(PRODUCTION_REMAP[0x1e], 0x1e, "document separator must stay fixed");
        let mut sorted = PRODUCTION_REMAP;
        sorted.sort();
        assert_eq!(sorted, core::array::from_fn(|i| i as u8), "remap must be bijective");
    }

    #[test]
    fn time_report_parse() {
        let text = "\tElapsed (wall clock) time (h:mm:ss or m:ss): 1:07.98\n\
                    \tMaximum resident set size (kbytes): 6048\n";
        let (wall, rss) = parse_time_report(text);
        assert_eq!(wall.as_deref(), Some("1:07.98"));
        assert_eq!(rss, Some(6048));
        let days = "\tElapsed (wall clock) time (h:mm:ss or m:ss): 1-02:03:04\n";
        assert_eq!(parse_time_report(days).0.as_deref(), Some("1-02:03:04"));
    }
}
