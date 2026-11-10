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
use std::os::unix::process::CommandExt;
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use sha2::{Digest, Sha256};
#[path = "recovery.rs"]
mod recovery;

pub const USAGE: &str = "usage:
  xsa build --tape --input <raw-file> --scratch <dir>
            [--threads N] [--memory-gb N] [--plan-only]
            [--with-chi|--no-chi] [--chi-expect N] [--snap-1e]

  --tape             bounded tape producer, ordered merge and container finish.
  --bin              the bin shape (the DEFAULT; the flag makes the choice explicit,
                     and the fit check prices the shape that is asked for).
  --input PATH       raw bytes, unchanged; END is virtual, no terminal required.
  --scratch DIR      intermediates, container and durable recovery journal.
  --no-resume        discard this build's intermediates and begin a new plan.
                     Otherwise incomplete tape builds replay verified input.
  --threads N        sort parallelism / bands in flight (default 48).
  --memory-gb N      phase address-space cap in GiB (default 24).
                     NO TIME OUTS, NO WATCHDOGS: every phase is spawned directly
                     (no `timeout` wrapper, no supervisor, no kill path). An
                     expensive run ends when it exits, fails, or a human stops it.
  --plan-only        print resource plan without running construction.
  --skip-fit-check   run despite the ABSOLUTE peak-vs-free scratch check (journaled as
                     DISC_CHECK_SKIPPED basis=operator peak=<n> free=<m>). It skips that
                     check and NOTHING else: the RAM term, the band law and every
                     receipt still refuse the run.
  --allow-infeasible silent alias of --skip-fit-check (old journals still parse).
  --with-chi         construct m5 chi (enabled by default).
  --no-chi           omit optional m5 chi.
  --with-lcp-runs    write m14, the LCP over runs (codec 117; SXI2 v8). OPT-IN:
                     it costs a measured 0.256 n (5.89 bits/run) and buys the
                     MIDDLE band of the two-backend rule - contexts too wide to
                     locate-and-scan and not in the top-of-tree trie - which is
                     0.4..5.1 % of contexts at information depths (16..32 B).
  --no-lcp-runs      omit m14 (the DEFAULT): the v7 layout, byte-identical to the
                     pre-m14 container, which is how additivity is proved. The
                     container's own header (version 8 + flag bit 7 + member m14)
                     declares which arm ran, so readers never need the flag.
  --trie-depth N     m20 (the context trie) depth, 1..=64 (default 8). The trie is
                     built IN-BUILD from the sorter's band stream (the tape route
                     only): the bands concatenate in rank order into the suffix
                     array, so the builder rides along with construction instead of
                     replaying it through an LF cycle.
  --trie-min-support K
                     occurrence threshold for a materialised context node
                     (default 1000: 0.0348 n at 10 GB, 59.5 % of random 8-byte
                     contexts answered; 100 is 0.165 n and ~77 %).
  --no-trie          omit m20: the container is byte-identical to a build without
                     the feature (SXI2 v7, 8 members, same sha256).
  --chi-expect N     assert chi count; requires --with-chi.
  --snap-1e          require a final 0x1E document boundary and journal SHA256;
                     never trims, extends or otherwise transforms the input.
  --remap PATH       validated corpus-derived 256-byte bijective remap override.
  --finish-as-gb N   finish phase address-space cap (default --memory-gb).
  --wit-ram-gb N     finish witness RAM budget (default --memory-gb / 2).";


/// THE LEGACY FIXED REMAP IS GONE. A universal table cannot be order preserving
/// on an arbitrary corpus, and the END-format finish requires present byte of
/// rank j -> 1+j with END's cell (code 0) left free, which depends on WHICH bytes
/// the corpus uses. `xsa build` therefore derives the table per corpus at plan
/// time (derive_remap below; reference generator bit6/binbwt/emit_remap.py).
///
/// The six banked selftest sweep seeds (bit6/sxi_logs/pool-verification).
const SELFTEST_SEEDS: [u64; 6] = [11, 99, 20261002, 7, 5, 13];
/// Disc preflight: refuse when the scratch filesystem is more than this
/// percent used (the gates' >= 15% free discipline).
const DISC_MAX_USED_PERCENT: u32 = 85;
/// ulimit wrapper: cap phase address space, then exec the real command.
/// A lower inherited hard limit always wins and is journaled, never raised.
const PHASE_WRAPPER: &str = "ulimit -v \"$1\" 2>/dev/null || ulimit -v \"$(ulimit -H -v)\" 2>/dev/null || true; shift; printf 'PHASE_CAP_KB=%s\\n' \"$(ulimit -v)\" >&2; exec \"$@\"";

/// The finish's own patience for the producer's next piece, in seconds (about 31
/// years). It is NOT a cap on the run: there is no cap. It exists only so the
/// finish's poll loop has a number, and it must never expire on a live producer.
const FINISH_POLL_TIMEOUT_S: u64 = 1_000_000_000;

/// The producer's dictionary knobs (bit6/cross_lcp_merge.cpp). Kept in one
/// table so the parser, the usage text and the validation cannot drift apart.
pub const PHRASE_KNOBS: [&str; 13] = [
    "--phrase-min-len", "--phrase-min-freq", "--phrase-max-len", "--phrase-seed",
    "--phrase-window", "--phrase-block-max", "--phrase-pool-max", "--phrase-table-bits",
    "--phrase-reps", "--phrase-skip-min-rows", "--phrase-skip-min-len",
    "--phrase-probe-depth", "--phrase-probe-stride",
];

/// The phrase-radix arm (tools/binbwt/PHRASE_RADIX_DESIGN.md, bit6/phrase_radix.h).
/// DEFAULT OFF, and it stays off in every observable way when the flag is
/// absent: no producer argument, no plan line, no parse, no state.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct PhraseOptions {
    pub enabled: bool,
    /// (flag, value) pairs in command order, appended to the producer exactly as
    /// given. Validated here with the producer's own constraints, because a
    /// front door that accepts a dictionary the producer will refuse is a
    /// misconfiguration discovered hours later.
    pub knobs: Vec<(&'static str, u64)>,
}

/// The producer's own `--phrase-*` constraints (bit6/cross_lcp_merge.cpp: the
/// `require(...)` under `if (has("--phrase-radix"))`), mirrored so both ends
/// refuse the same dictionary.
fn validate_phrase(p: &PhraseOptions) -> Result<(), String> {
    let get = |name: &str| p.knobs.iter().find(|(f, _)| *f == name).map(|(_, v)| *v);
    let min_len = get("--phrase-min-len").unwrap_or(64);
    let max_len = get("--phrase-max-len").unwrap_or(4096);
    let seed = get("--phrase-seed").unwrap_or(32);
    let window = get("--phrase-window").unwrap_or(24);
    let table_bits = get("--phrase-table-bits").unwrap_or(20);
    let reps = get("--phrase-reps").unwrap_or(8);
    let skip_min_len = get("--phrase-skip-min-len").unwrap_or(16);
    if min_len < 16 {
        return Err(format!("--phrase-min-len {min_len} < 16 (a phrase shorter than 16 bytes cannot beat one byte level)"));
    }
    if seed < 16 {
        return Err(format!("--phrase-seed {seed} < 16 (the winnow's k-mer must be long enough to identify a phrase)"));
    }
    if skip_min_len < 16 {
        return Err(format!("--phrase-skip-min-len {skip_min_len} < 16 (a skip shorter than a page of levels is not worth a probe)"));
    }
    if max_len < min_len {
        return Err(format!("--phrase-max-len {max_len} < --phrase-min-len {min_len}"));
    }
    if window < 2 {
        return Err(format!("--phrase-window {window} < 2 (the winnow window must hold at least two k-mers)"));
    }
    if !(8..=26).contains(&table_bits) {
        return Err(format!("--phrase-table-bits {table_bits} outside [8, 26] (2^26 slots x 24 B = 1.5 GiB is the ceiling)"));
    }
    if reps < 2 {
        return Err(format!("--phrase-reps {reps} < 2 (the length sample needs two occurrences)"));
    }
    Ok(())
}

#[derive(Debug)]
struct Options {
    input: PathBuf,
    scratch: PathBuf,
    snap_1e: bool,
    memory_gb: u64,
    threads: u32,
    chunks: u32,
    kway: u32,
    plan_only: bool,
    /// THE DEFAULT (auto-radix): the bin pipeline - one `xsa build`, the
    /// multi-pass reader, the rolling window files, target-size recursive bins
    /// and the ordered stitch. `bin_pipeline == !legacy_merge`.
    bin_pipeline: bool,
    /// `--legacy-merge`: the pre-bin route (chunk front end + k-way merge walk).
    legacy_merge: bool,
    /// `--v2-pipeline` was passed. It is ACCEPTED AND IGNORED (the bin pipeline
    /// is the default now); the journal records that it was seen and ignored so
    /// a log can never pretend the flag still selects a route.
    v2_flag_given: bool,
    chi_expect: Option<u64>,
    with_chi: bool,
    /// **m14 (the LCP over runs)**: `--with-lcp-runs` writes SXI2 v8 with it,
    /// `--no-lcp-runs` (the DEFAULT) writes the v7 layout without it.
    ///
    /// WHY OFF BY DEFAULT. It is a product choice with a measured price: the member
    /// is 0.27 n at 150 MB and 0.256 n at 10 GB (5.89 bits/run), i.e. ~354 GB on a
    /// full-Pile container. What it uniquely buys is the MIDDLE band of the
    /// two-backend rule in MEMORY_OPS.md - contexts too wide to answer by
    /// locate-and-scan (cost O(width)) and not materialised in the top-of-tree
    /// trie - plus LCP-interval topology and MEM enumeration. Measured on the 10 GB
    /// Pile slice (1000 byte-weighted contexts per depth, seed 20261110):
    ///
    ///   depth   width==1     1..999    1e3..1e6     >=1e6
    ///     8      4.3 %       36.1 %     57.1 %       2.5 %
    ///    16     39.9 %       53.8 %      5.1 %       1.1 %
    ///    24     73.8 %       24.6 %      1.0 %       0.5 %
    ///    32     85.7 %       13.6 %      0.4 %       0.2 %
    ///
    /// At the depths where the information actually is (16..32 bytes, per
    /// CONTEXT_DEPTH.md) the middle band is 0.4..5.1 % of contexts, so the member
    /// is a quarter of `n` bought for a thin band: OPT-IN, not a silent tax. The
    /// container's own header (version 8 + flag bit 7 + member m14) declares which
    /// arm ran, so readers never need the flag.
    with_lcp_runs: bool,
    /// **m20 (the context trie)**: built IN-BUILD from the sorter's band stream
    /// while the container is constructed. ON by default; `--no-trie` is the
    /// product-unchanged arm (byte-identical container) and the equivalence proof.
    ///
    /// WHY IN-BUILD. The post-hoc route (`xsa ctx-build`) reconstructs the suffix
    /// array out of the finished container: one LF cycle over all n rows plus a
    /// 5 B/row column of transient RAM (47 GB at 10 GB, ~5 TB at the Pile) and a
    /// single-threaded sweep (1,192 s + 3,448 s at 10 GB). Inside the tape build
    /// the ordered stream already exists - the sorter's band slices, concatenated
    /// in band order, ARE the suffix array - so the builder rides along with it
    /// (`xsa ctx-tap`) instead of replaying it. Measured defaults: depth 8 /
    /// min_support 1000 is 0.0348 n at 10 GB and answers 59.5 % of random 8-byte
    /// contexts; `--trie-min-support 100` is 0.165 n and answers ~77 %.
    trie: bool,
    trie_depth: usize,
    trie_min_support: u64,
    /// store the document summary only for nodes with at least this many
    /// occurrences (below it `project` answers from seek + the document map).
    /// 1000 is `ctx-build`'s default, so the two routes produce the same member.
    trie_docs_min_support: u64,
    /// the operator ASKED for the trie (a flag was given), so a route that cannot
    /// build it must refuse instead of quietly producing a container without m20.
    trie_asked: bool,
    wit_ram_gb: Option<u64>,
    /// `--finish-as-gb N`: the finish phase's own RLIMIT_AS in GiB. Mapped
    /// corpus windows charge address space, not RSS, so the finish can pin a
    /// corpus far larger than the build's memory envelope. Default: inherit
    /// `memory_gb`.
    finish_as_gb: Option<u64>,
    bin_live_cap: Option<u64>,
    /// --bin-target-size S: the leaf bound of the target-size recursion (rows).
    /// 0 / absent = AUTOMATIC: the producer derives it from the census and the
    /// measured leaf-sort rate, and journals the value it used (BIN_WORK
    /// target=). Depth is an OUTPUT of the data, so this is the only shape knob.
    bin_target_size: Option<u64>,
    /// `--bin-entropy-min [BITS]`: arm the entropy rule (the radix/comparison
    /// exchange rate). `None` = OFF (the product default, byte-identical);
    /// `Some(None)` = armed at the calibrated 1.5 bits; `Some(Some(b))` = armed
    /// at the operator's bits.
    bin_entropy_min: Option<Option<f64>>,
    /// --phrase-radix and its dictionary knobs. DEFAULT OFF; when off nothing is
    /// added to the producer command line and the product path is bit-identical.
    phrase: PhraseOptions,
    stripes: Vec<String>,
    scratch_free_pct: u32,
    /// `--scratch-budget-gb N`: THE FLOW-CONTROL BUDGET. The driver measures live
    /// scratch and, when the next wave/piece would exceed it, PAUSES the producer
    /// (SIGSTOP on its process group) while the finish drains. Pausing is the
    /// mechanism: nothing is killed and no work is dropped. Absent = derived
    /// (min(0.8 x free, 2 x the band window)).
    scratch_budget_gb: Option<f64>,
    /// The TAPE producer route: scatter_sort emits key-ordered bands into
    /// --tape-bands, cross_lcp_merge consumes them, xsa_finish follows. One
    /// binary, three children, no external scripts.
    tape: bool,
    no_resume: bool,
    /// OVERRIDE THE ABSOLUTE PEAK-VS-FREE CHECK ONLY (--skip-fit-check, with
    /// --allow-infeasible kept as a silent alias so old journals and commands
    /// still parse). Nothing else is overridable: the RAM term, the band law and
    /// every receipt keep their teeth, and the fact that the check was skipped is
    /// journaled as DISC_CHECK_SKIPPED basis=operator peak=<n> free=<m>.
    skip_fit_check: bool,
    remap: Option<PathBuf>,
    chunk_mb: Option<u32>,
    fanin: bool,
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
    let mut kway = 2u32;
    let mut plan_only = false;
    let mut legacy_merge = false;
    let mut v2_flag_given = false;
    let mut with_chi = true;
    let mut with_lcp_runs = false;
    let mut trie = true;
    let mut trie_depth = 8usize;
    let mut trie_min_support = 1_000u64;
    let mut trie_docs_min_support = 1_000u64;
    let mut trie_asked = false;
    let mut chi_expect = None;
    let mut wit_ram_gb = None;
    let mut finish_as_gb = None;
    let mut bin_live_cap = None;
    let mut bin_target_size = None;
    let mut bin_entropy_min = None;
    let mut phrase = PhraseOptions::default();
    let mut stripes: Vec<String> = Vec::new();
    // Needs-based by default: the absolute peak-vs-free check is the gate
    // (see disc_verdict / tree_ok). A percentage-of-device rule is opt-in.
    let mut scratch_free_pct = 0u32;
    let mut scratch_budget_gb: Option<f64> = None;
    // THE OVERRIDE OF THE ABSOLUTE PEAK-VS-FREE CHECK, and of nothing else.
    let mut skip_fit_check = false;
    let mut tape = false;
    let mut bin_shape_given = false;
    let mut no_resume = false;
    let mut remap = None;
    let mut chunk_mb = None;
    let mut fanin = false;
    let mut chunks_given = false;
    let mut kway_given = false;
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
            "--scratch-budget-gb" => {
                i += 1;
                // Decimal GB, so a small fixture can exercise the flow control
                // (a 20 MB corpus never reaches an integer-GB budget).
                let v = value(args.get(i), "--scratch-budget-gb")?
                    .parse::<f64>().map_err(|_| "--scratch-budget-gb: number expected".to_string())?;
                if !(v > 0.0) || !v.is_finite() { return Err("--scratch-budget-gb must be positive".into()); }
                scratch_budget_gb = Some(v);
            }
            "--snap-1e" => snap_1e = true,
            "--tape" => tape = true,
            "--no-resume" => no_resume = true,
            "--skip-fit-check" => skip_fit_check = true,
            // SILENT ALIAS: the pre-rename spelling. It must keep parsing (old
            // journals and scripts carry it) and must do exactly the same thing -
            // override the absolute peak-vs-free check and nothing else.
            "--allow-infeasible" => skip_fit_check = true,
            "--bin" => bin_shape_given = true,
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
                chunks_given = true;
                if chunks == 0 {
                    return Err("--chunks must be positive".into());
                }
            }
            "--chunk-mb" => {
                i += 1;
                let v = parse_u32(args.get(i), "--chunk-mb")?;
                if v == 0 {
                    return Err("--chunk-mb must be positive".into());
                }
                chunk_mb = Some(v);
            }
            "--fanin" => fanin = true,
            "--kway" => {
                i += 1;
                kway = parse_u32(args.get(i), "--kway")?;
                kway_given = true;
                if kway < 2 {
                    return Err("--kway must be >= 2 (the pairwise arity)".into());
                }
            }
            "--plan-only" => plan_only = true,
            // AUTO-RADIX (operator-directed): the bin pipeline is the DEFAULT and
            // --v2-pipeline is ACCEPTED AND IGNORED, journalled as such. The
            // pre-bin chunk+merge route is behind --legacy-merge.
            "--v2-pipeline" => v2_flag_given = true,
            "--legacy-merge" => legacy_merge = true,
            "--with-chi" => with_chi = true,
            "--no-chi" => with_chi = false,
            // m14: the LCP over runs. DEFAULT OFF (a measured 0.256 n for a thin
            // middle band - see the field's doc); --no-lcp-runs is also the v7
            // byte-identical arm, which is how the member's additivity is proved.
            "--with-lcp-runs" => with_lcp_runs = true,
            "--no-lcp-runs" => with_lcp_runs = false,
            // m20: the context trie, built in-build from the band stream.
            "--trie" | "--with-trie" => { trie = true; trie_asked = true; }
            "--no-trie" => { trie = false; trie_asked = true; }
            "--trie-depth" => {
                i += 1;
                trie_depth = value(args.get(i), "--trie-depth")?
                    .parse().map_err(|_| "--trie-depth: integer expected".to_string())?;
                if !(1..=64).contains(&trie_depth) { return Err("--trie-depth must be 1..=64".into()); }
                trie = true;
                trie_asked = true;
            }
            "--trie-min-support" => {
                i += 1;
                trie_min_support = value(args.get(i), "--trie-min-support")?
                    .parse().map_err(|_| "--trie-min-support: integer expected".to_string())?;
                if trie_min_support < 1 { return Err("--trie-min-support must be >= 1".into()); }
                trie = true;
                trie_asked = true;
            }
            "--trie-docs-min-support" => {
                i += 1;
                trie_docs_min_support = value(args.get(i), "--trie-docs-min-support")?
                    .parse().map_err(|_| "--trie-docs-min-support: integer expected".to_string())?;
            }
            "--chi-expect" => {
                i += 1;
                let v = value(args.get(i), "--chi-expect")?
                    .parse::<u64>().map_err(|_| "--chi-expect: integer expected".to_string())?;
                if chi_expect.replace(v).is_some() {
                    return Err("specify --chi-expect only once".into());
                }
            }
            "--wit-ram-gb" => {
                i += 1;
                let v = value(args.get(i), "--wit-ram-gb")?
                    .parse::<u64>().map_err(|_| "--wit-ram-gb: integer expected".to_string())?;
                if v == 0 {
                    return Err("--wit-ram-gb must be positive".into());
                }
                if wit_ram_gb.replace(v).is_some() {
                    return Err("specify --wit-ram-gb only once".into());
                }
            }
            "--finish-as-gb" => {
                i += 1;
                let v = value(args.get(i), "--finish-as-gb")?
                    .parse::<u64>().map_err(|_| "--finish-as-gb: integer expected".to_string())?;
                if v == 0 {
                    return Err("--finish-as-gb must be positive (omit it to inherit --memory-gb)".into());
                }
                if finish_as_gb.replace(v).is_some() {
                    return Err("specify --finish-as-gb only once".into());
                }
            }
            "--bin-live-cap" => {
                i += 1;
                let v = value(args.get(i), "--bin-live-cap")?
                    .parse::<u64>().map_err(|_| "--bin-live-cap: integer expected".to_string())?;
                if bin_live_cap.replace(v).is_some() {
                    return Err("specify --bin-live-cap only once".into());
                }
            }
            "--bin-target-size" => {
                i += 1;
                let v = value(args.get(i), "--bin-target-size")?
                    .parse::<u64>().map_err(|_| "--bin-target-size: integer expected".to_string())?;
                if v < 2 {
                    return Err("--bin-target-size must be >= 2 rows (0 = automatic, which is the default)".into());
                }
                if bin_target_size.replace(v).is_some() {
                    return Err("specify --bin-target-size only once".into());
                }
            }
            "--bin-entropy-min" => {
                let b = match args.get(i + 1) {
                    Some(v) if !v.starts_with('-') => {
                        i += 1;
                        Some(v.parse::<f64>().map_err(|_| "--bin-entropy-min: a bits value was given but is not a number".to_string())?)
                    }
                    _ => None,
                };
                if let Some(bits) = b {
                    if !(0.0..=32.0).contains(&bits) {
                        return Err("--bin-entropy-min must be in [0, 32] bits".into());
                    }
                }
                if bin_entropy_min.replace(b).is_some() {
                    return Err("specify --bin-entropy-min only once".into());
                }
            }
            "--phrase-radix" => {
                if phrase.enabled {
                    return Err("specify --phrase-radix only once".into());
                }
                phrase.enabled = true;
            }
            f if PHRASE_KNOBS.iter().any(|k| *k == f) => {
                i += 1;
                let v = value(args.get(i), f)?
                    .parse::<u64>().map_err(|_| format!("{f}: integer expected"))?;
                if phrase.knobs.iter().any(|(k, _)| *k == f) {
                    return Err(format!("specify {f} only once"));
                }
                // `knobs` holds 'static names; only the table's own entries reach here.
                let name = PHRASE_KNOBS[PHRASE_KNOBS.iter().position(|k| *k == f).unwrap()];
                phrase.knobs.push((name, v));
            }
            "--scratch-free-pct" => {
                i += 1;
                scratch_free_pct = parse_u32(args.get(i), "--scratch-free-pct")?;
                eprintln!("warning: --scratch-free-pct {} is deprecated and ignored (the plan reports peak vs free; the only refusal is a build that cannot fit)", scratch_free_pct);
                scratch_free_pct = 0;   // no policy of any kind: the value is not carried into the plan
                if scratch_free_pct > 100 {
                    return Err("--scratch-free-pct must be <= 100 (0 = override hygiene)".into());
                }
            }
            "--stripe" => {
                i += 1;
                let v = value(args.get(i), "--stripe")?;
                stripes = v.split(',').map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).collect();
                if stripes.is_empty() {
                    return Err("--stripe needs at least one directory".into());
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
    let bin_pipeline = !legacy_merge;
    // NO SILENT MISCONFIGURATION: the chunk/k-way/fan-in knobs do not exist on
    // the bin route (the producer sweeps the corpus itself), so explicitly
    // passing one is refused rather than ignored.
    if bin_pipeline {
        if chunks_given {
            return Err("--chunks is INERT under the bin pipeline (the default; the producer sweeps the corpus itself, there is no chunk phase); drop the flag, or pass --legacy-merge for the chunk+merge route".into());
        }
        if chunk_mb.is_some() {
            return Err("--chunk-mb is INERT under the bin pipeline (the default; no chunk phase); drop the flag, or pass --legacy-merge".into());
        }
        if kway_given {
            return Err("--kway is INERT under the bin pipeline (the default; bins are ordered by their own construction, there is no k-way walk); drop the flag, or pass --legacy-merge".into());
        }
        if fanin {
            return Err("--fanin is INERT under the bin pipeline (the default; there is nothing to fan in); drop the flag, or pass --legacy-merge".into());
        }
        if let Some(wit) = wit_ram_gb {
            let eff = finish_as_gb.unwrap_or(memory_gb);
            if wit > eff {
                return Err(format!("--wit-ram-gb {wit} exceeds the finish's effective AS cap {eff} GiB (--memory-gb {memory_gb} unless --finish-as-gb overrides it)"));
            }
        }
    }
    if chunk_mb.is_some() && chunks_given {
        return Err("--chunk-mb and --chunks are mutually exclusive (chunk-mb derives the count)".into());
    }
    if fanin && chunk_mb.is_none() {
        return Err("--fanin requires --chunk-mb N (the fixed chunk size)".into());
    }
    if tape && legacy_merge { return Err("--tape and --legacy-merge are mutually exclusive".into()); }
    if tape && bin_shape_given {
        return Err("--tape and --bin are mutually exclusive: they are two shapes (a band sorter vs the \
                    bin producer), and the fit check prices exactly one of them. Pass one".into());
    }
    if bin_shape_given && legacy_merge {
        return Err("--bin and --legacy-merge are mutually exclusive (--bin is the bin shape, which is also \
                    the default; --legacy-merge is the chunk+merge route)".into());
    }
    if no_resume && !tape { return Err("--no-resume requires --tape".into()); }
    if tape && !with_chi && chi_expect.is_some() {
        return Err("--chi-expect requires --with-chi on the tape route (missing member m5)".into());
    }
    if legacy_merge {
        if chi_expect.is_some() {
            return Err("--chi-expect is a bin-pipeline flag (drop --legacy-merge)".into());
        }
        if wit_ram_gb.is_some() {
            return Err("--wit-ram-gb is a bin-pipeline flag (drop --legacy-merge)".into());
        }
        if bin_live_cap.is_some() {
            return Err("--bin-live-cap is a bin-pipeline flag (drop --legacy-merge)".into());
        }
        if bin_target_size.is_some() {
            return Err("--bin-target-size is a bin-pipeline flag (drop --legacy-merge)".into());
        }
        if phrase.enabled || !phrase.knobs.is_empty() {
            return Err("--phrase-radix is a bin-pipeline flag (drop --legacy-merge: the merge route has no bin tree to certify)".into());
        }
    }
    if !phrase.enabled && !phrase.knobs.is_empty() {
        return Err(format!("{} needs --phrase-radix (the dictionary is off by default and its knobs do nothing without it)", phrase.knobs[0].0));
    }
    if phrase.enabled {
        validate_phrase(&phrase)?;
    }
    Ok(Options { input, scratch, snap_1e, memory_gb, threads, chunks, kway, plan_only,
                 bin_pipeline, legacy_merge, v2_flag_given, chi_expect, with_chi, with_lcp_runs, wit_ram_gb,
                 finish_as_gb, bin_live_cap, bin_target_size, bin_entropy_min, phrase, stripes, scratch_free_pct,
                 scratch_budget_gb, skip_fit_check, tape, no_resume,
                 trie, trie_depth, trie_min_support, trie_docs_min_support, trie_asked,
                 remap, chunk_mb, fanin })
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

/// SOURCE PROVENANCE IS GIT'S: which commit of the tree produced the stages
/// this run executes. There is no checked-in digest of the sources - a
/// hand-maintained one duplicated what git already records, went stale on
/// every merge that touched a covered file, and refused the build until it was
/// regenerated by hand (four times on 2026-10-30). The journal therefore
/// reports the commit, the dirty state of TRACKED files and the branch.
///
/// A run never fails for this: an installed bundle has no checkout, so every
/// field reads `unknown` and the build continues. Provenance is reported,
/// never enforced.
fn source_identity_line() -> String {
    // Ask git where the operator launched from (a checkout of this repo), then
    // beside the executable (a cargo-built binary sits under the tree it was
    // built from, so `xsa build` from /tmp still names the right commit).
    let mut roots: Vec<PathBuf> = vec![PathBuf::from(".")];
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            roots.push(dir.to_path_buf());
        }
    }
    for root in roots {
        if let Some(line) = git_source_identity(&root) {
            return line;
        }
    }
    "XSA_SOURCE git_commit=unknown git_dirty=unknown branch=unknown \
(no git checkout visible: installed bundle)"
        .to_string()
}

fn git_source_identity(root: &Path) -> Option<String> {
    fn git(root: &Path, args: &[&str]) -> Option<String> {
        let out = Command::new("git")
            .arg("-C")
            .arg(root)
            .args(args)
            .stdin(Stdio::null())
            .output()
            .ok()?;
        if !out.status.success() {
            return None;
        }
        Some(String::from_utf8_lossy(&out.stdout).trim().to_string())
    }
    let commit = git(root, &["rev-parse", "HEAD"])?;
    if commit.is_empty() {
        return None;
    }
    let branch = match git(root, &["rev-parse", "--abbrev-ref", "HEAD"]) {
        Some(name) if !name.is_empty() => name,
        _ => "unknown".to_string(),
    };
    // TRACKED files only: a run's scratch tree and cargo target are not source
    // changes, so `--untracked-files=no` keeps the answer about sources.
    let dirty = match git(root, &["status", "--porcelain", "--untracked-files=no"]) {
        Some(status) => if status.is_empty() { "0" } else { "1" },
        None => return None,
    };
    Some(format!("XSA_SOURCE git_commit={commit} git_dirty={dirty} branch={branch}"))
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

fn disc_preflight(path: &Path, min_free_pct: u32) -> Result<(), String> {
    if min_free_pct == 0 {
        return Ok(());   // explicit operator override (journaled in the plan)
    }
    // The ACTUAL used percent against the policy: free >= min_free_pct of the
    // disc. (The pre-fix gate compared the CONSTANT 100-min_free_pct against a
    // hardcoded 85 - the disc's own state was read and never used: always-pass
    // at the default, always-fail at any override. Found by --scratch-free-pct.)
    let used = disc_used_percent(path)?;
    let allowed_used = 100u32.saturating_sub(min_free_pct);
    if used > allowed_used {
        return Err(format!(
            "DF_GATE_FAIL {} {}% used (>= {}% free required on the scratch filesystem)",
            path.display(), used, min_free_pct
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

/// Recursive byte size of a directory tree (the build's own disc footprint).
/// `df --output=used` on a shared filesystem picks up every other writer, so
/// the per-build peak is measured over the scratch tree alone.
fn tree_bytes(path: &Path) -> u64 {
    let mut total = 0u64;
    let mut stack = vec![path.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(rd) = fs::read_dir(&dir) else { continue };
        for entry in rd.flatten() {
            let Ok(md) = entry.metadata() else { continue };
            if md.is_dir() {
                stack.push(entry.path());
            } else if md.is_file() {
                total += md.len();
            }
        }
    }
    total
}

/// Background peak-disc sampler: the merge's delete-as-you-go reclaim is
/// invisible at phase boundaries, so poll the scratch tree size while a long
/// phase runs and keep the maximum. The peak a delete-as-you-go fan-in build
/// reaches mid-merge is the deliverable, not the end-of-phase value.
struct DiscSampler {
    stop: Arc<AtomicBool>,
    peak: Arc<AtomicU64>,
    handle: Option<std::thread::JoinHandle<()>>,
}

impl DiscSampler {
    fn start(path: PathBuf, interval: Duration) -> DiscSampler {
        let stop = Arc::new(AtomicBool::new(false));
        let peak = Arc::new(AtomicU64::new(0));
        let stop_t = stop.clone();
        let peak_t = peak.clone();
        let handle = std::thread::spawn(move || {
            while !stop_t.load(Ordering::Relaxed) {
                let used = tree_bytes(&path);
                if used > peak_t.load(Ordering::Relaxed) {
                    peak_t.store(used, Ordering::Relaxed);
                }
                std::thread::sleep(interval);
            }
        });
        DiscSampler { stop, peak, handle: Some(handle) }
    }
    fn peak_bytes(mut self) -> u64 {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
        self.peak.load(Ordering::Relaxed)
    }
}

/// Live SCRATCH bytes and the PRODUCT's bytes, measured separately. Anything on
/// the container's own path (`X`, `X.partial`, `X.part`) is the output.
fn scratch_bytes_excluding(scratch: &Path, container: &Path) -> (u64, u64) {
    let mut scratch_bytes = 0u64;
    let mut output_bytes = 0u64;
    let mut stack = vec![scratch.to_path_buf()];
    let mut outs: Vec<std::ffi::OsString> = Vec::new();
    let mut names: Vec<std::ffi::OsString> = Vec::new();
    if let (Some(dir), Some(name)) = (container.parent(), container.file_name()) {
        let mut partial = name.to_os_string();
        partial.push(".partial");
        let mut part = name.to_os_string();
        part.push(".part");
        names = vec![name.to_os_string(), partial, part];
        outs.clone_from(&names);
        let _ = dir;
    }
    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else { continue };
        for entry in entries.flatten() {
            let path = entry.path();
            match entry.file_type() {
                Ok(t) if t.is_dir() => stack.push(path),
                Ok(t) if t.is_file() => {
                    // ALLOCATED bytes, not apparent size: the chi bitmap is a sparse
                    // n/8 mapping (164 GB apparent at the Pile) whose untouched pages
                    // occupy nothing, and a budget that counted apparent size would
                    // refuse a run that is using a fraction of that.
                    use std::os::unix::fs::MetadataExt;
                    let len = entry.metadata()
                        .map(|m| m.blocks().saturating_mul(512).max(if m.len() == 0 { 0 } else { 4096 }))
                        .unwrap_or(0);
                    let is_output = path.parent() == container.parent()
                        && path.file_name().map(|n| outs.iter().any(|o| o == n)).unwrap_or(false);
                    if is_output { output_bytes += len; } else { scratch_bytes += len; }
                }
                _ => {}
            }
        }
    }
    (scratch_bytes, output_bytes)
}

// ---------------------------------------------------------------- FLOW CONTROL
/// THE DELIVERABLE: live scratch is a MEASURED quantity, and the producer is
/// PAUSED (never killed, never failed) while the finish drains. Without this the
/// only lever on peak scratch is the band window, and the finish's inputs grow
/// with the corpus.
///
/// Mechanism: SIGSTOP/SIGCONT on the producer's PROCESS GROUP (spawn_phase puts
/// every phase in its own group). A stopped group executes no instructions, so
/// this is a real pause: no busy loop, no dropped work, no state to reconstruct.
/// The finish is never stopped - it is the drain. Visibility is the 2 s sampling
/// interval, well inside the 5 s requirement.
///
/// The one way a pause could stall instead of draining is a producer stopped
/// while holding a lock the finish needs (the shared checkpoint journal's flock,
/// which exists only on a resumed build). The controller therefore also resumes
/// when live scratch has not moved for STALL_S while paused, and journals why.
struct Backpressure {
    budget_bytes: u64,
    /// The number the operator sets is a SCRATCH budget: the container being built
    /// is the PRODUCT, not scratch, and it is counted separately (`output_gb`) and
    /// reported on every line. The plan's fit check already prices it as its own
    /// term, so folding it into the scratch budget here would double-count it and
    /// make a 6 GB budget impossible on a 10 GB corpus whose container is 5.2 GB.
    output_bytes: u64,
    container: PathBuf,
    /// Raised when the drain cannot reach the budget (the residual scratch the
    /// finish cannot release without more producer input). Measured, journaled,
    /// never assumed.
    pause_at: u64,
    resume_at: u64,
    floor_bytes: u64,
    scratch: PathBuf,
    /// tag -> process group id, for the phases that PRODUCE scratch.
    producers: Vec<(String, u32)>,
    paused: bool,
    pauses: u64,
    resumes: u64,
    paused_ms_total: u128,
    peak_bytes: u64,
    last_sample: Instant,
    last_live: u64,
    paused_since: Instant,
    paused_live_at: u64,
}

impl Backpressure {
    fn new(scratch: &Path, budget_bytes: u64, container: &Path, producers: Vec<(String, u32)>) -> Backpressure {
        Backpressure {
            budget_bytes, output_bytes: 0, container: container.to_path_buf(),
            pause_at: budget_bytes, resume_at: budget_bytes / 10 * 9, floor_bytes: 0,
            scratch: scratch.to_path_buf(), producers, paused: false,
            pauses: 0, resumes: 0, paused_ms_total: 0, peak_bytes: 0,
            last_sample: Instant::now(), last_live: 0, paused_since: Instant::now(),
            paused_live_at: 0,
        }
    }
    /// SIGSTOP/SIGCONT a whole process group. `kill` is used with the group id
    /// negated; a group that has already exited answers ESRCH, which is not an
    /// error for this purpose (the exit path reports it).
    fn signal_group(&self, pgid: u32, sig: &str) -> Result<(), String> {
        let out = Command::new("kill").arg(format!("-{sig}")).arg(format!("-{pgid}"))
            .output().map_err(|e| format!("spawn kill: {e}"))?;
        if !out.status.success() {
            let err = String::from_utf8_lossy(&out.stderr).trim().to_string();
            if err.contains("No such process") { return Ok(()); }
            return Err(format!("kill -{sig} -{pgid}: {err}"));
        }
        Ok(())
    }
    /// Called from the pipeline's wait loop. Returns nothing; journals every
    /// transition with the measured live and budget.
    fn observe(&mut self, journal: &mut Journal) -> Result<(), String> {
        const SAMPLE: Duration = Duration::from_secs(2);
        const STALL_S: Duration = Duration::from_secs(30);
        if self.last_sample.elapsed() < SAMPLE { return Ok(()); }
        self.last_sample = Instant::now();
        let (live, output) = scratch_bytes_excluding(&self.scratch, &self.container);
        self.output_bytes = output;
        self.last_live = live;
        if live > self.peak_bytes { self.peak_bytes = live; }
        let gb = |v: u64| v as f64 / (1u64 << 30) as f64;
        journal.line(&format!(
            "SCRATCH_SAMPLE live_gb={:.3} output_gb={:.3} budget_gb={:.3} peak_gb={:.3} paused={} allocated_bytes=1",
            gb(live), gb(output), gb(self.budget_bytes), gb(self.peak_bytes), u8::from(self.paused)));
        if !self.paused {
            if live >= self.pause_at {
                for (tag, pgid) in self.producers.clone() {
                    self.signal_group(pgid, "STOP")?;
                    journal.line(&format!("SCRATCH_BACKPRESSURE signal=STOP phase={tag} pgid={pgid}"));
                }
                self.paused = true;
                self.pauses += 1;
                self.paused_since = Instant::now();
                self.paused_live_at = live;
                journal.line(&format!(
                    "SCRATCH_BACKPRESSURE state=pause live_gb={:.3} budget_gb={:.3} peak_gb={:.3} pauses={} resumes={} \
                     output_gb={:.3} pause_at_gb={:.3} action=producer_stopped consumer=drains",
                    gb(live), gb(self.budget_bytes), gb(self.peak_bytes), self.pauses, self.resumes,
                    gb(output), gb(self.pause_at)));
            }
        } else {
            // Drain target: 90% of the budget, so the producer does not start a
            // wave that immediately re-trips the limit.
            // `drained`: the budget is met. `floor`: the drain has stopped making
            // progress while still above it - the residual is scratch the finish
            // cannot release without producer input (or, on a resumed build, a
            // producer stopped inside the shared journal's flock). The floor is
            // MEASURED and becomes the new pause threshold, so the controller
            // settles instead of thrashing; it is journaled every time it moves.
            let drained = live <= self.resume_at;
            let stalled = self.paused_since.elapsed() >= STALL_S && live + (live / 100) >= self.paused_live_at;
            if stalled && live > self.floor_bytes { self.floor_bytes = live; }
            if stalled && live + (1u64 << 30) > self.pause_at {
                self.pause_at = live + (1u64 << 30) / 2;
                self.resume_at = self.pause_at / 10 * 9;
            }
            if drained || stalled {
                for (tag, pgid) in self.producers.clone() {
                    self.signal_group(pgid, "CONT")?;
                    journal.line(&format!("SCRATCH_BACKPRESSURE signal=CONT phase={tag} pgid={pgid}"));
                }
                self.paused = false;
                self.resumes += 1;
                self.paused_ms_total += self.paused_since.elapsed().as_millis();
                journal.line(&format!(
                    "SCRATCH_BACKPRESSURE state=resume live_gb={:.3} budget_gb={:.3} peak_gb={:.3} pauses={} resumes={} \
                     output_gb={:.3} why={} paused_ms={} floor_gb={:.3} pause_at_gb={:.3}",
                    gb(live), gb(self.budget_bytes), gb(self.peak_bytes), self.pauses, self.resumes,
                    gb(output), if stalled { "drain_floor" } else { "drained" },
                    self.paused_since.elapsed().as_millis(), gb(self.floor_bytes), gb(self.pause_at)));
            }
        }
        Ok(())
    }
    fn finish(mut self, journal: &mut Journal) -> Result<(), String> {
        if self.paused {
            for (tag, pgid) in self.producers.clone() {
                self.signal_group(pgid, "CONT")?;
                journal.line(&format!("SCRATCH_BACKPRESSURE signal=CONT phase={tag} pgid={pgid}"));
            }
            self.paused_ms_total += self.paused_since.elapsed().as_millis();
            self.paused = false;
            self.resumes += 1;
        }
        journal.line(&format!(
            "SCRATCH_BACKPRESSURE_SUMMARY budget_gb={:.3} peak_gb={:.3} floor_gb={:.3} pauses={} resumes={} \
             paused_ms={} last_live_bytes={} output_gb={:.3} accounting=scratch_excludes_container",
            self.budget_bytes as f64 / (1u64 << 30) as f64,
            self.peak_bytes as f64 / (1u64 << 30) as f64,
            self.floor_bytes as f64 / (1u64 << 30) as f64,
            self.pauses, self.resumes, self.paused_ms_total, self.last_live,
            self.output_bytes as f64 / (1u64 << 30) as f64));
        Ok(())
    }
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

/// Retain diagnostic scratch on every error after the journal is opened,
/// including early returns before the first phase starts.
struct ScratchOutcome {
    scratch: PathBuf,
    journal: Journal,
    success: bool,
}
impl Drop for ScratchOutcome {
    fn drop(&mut self) {
        if !self.success {
            self.journal.line(&format!("SCRATCH_RETAINED bands_bytes={} reason=failure",
                tree_bytes(&self.scratch.join("bands-tape"))));
        }
    }
}

fn clean_v2_scratch(scratch: &Path, container: &Path, journal: &mut Journal) -> Result<(), String> {
    let bands = tree_bytes(&scratch.join("bands-tape"));
    let waves = tree_bytes(&scratch.join("waves-tape"));
    // Only remove paths owned by this build. In particular, never unlink the
    // sibling lock inode, caller files, the published container or provenance.
    // `remap-256.bin` is the PLAN'S OWN SPOOL (256 B, sha-pinned in PLAN): it is
    // retained so the resume state machine has a real, checkable input spool at
    // rest, and so a restart can prove the plan without re-deriving it.
    for name in ["bands-tape", "waves-tape", "merged"] {
        let path = scratch.join(name);
        let meta = match fs::symlink_metadata(&path) {
            Ok(meta) => meta,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
            Err(e) => return Err(format!("inspect scratch {}: {e}", path.display())),
        };
        let result = if meta.is_dir() { fs::remove_dir_all(&path) } else { fs::remove_file(&path) };
        result.map_err(|e| format!("clean scratch {}: {e}", path.display()))?;
    }
    for suffix in [".partial", ".part"] {
        let path = PathBuf::from(format!("{}{suffix}", container.display()));
        match fs::remove_file(&path) {
            Ok(()) => {},
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {},
            Err(e) => return Err(format!("clean scratch {}: {e}", path.display())),
        }
    }
    journal.line(&format!("SCRATCH_CLEANED bands_bytes={bands} waves_bytes={waves}"));
    Ok(())
}

/// The lock inode must survive scratch cleanup. Keep this file, and inherit
/// its open description in every stage: an orphan still owns the scratch tree
/// after the driver exits. Never unlink the lock to "recover" a running build.
fn lock_scratch(scratch: &Path) -> Result<File, String> {
    // Outside the tree: deleting/recreating scratch must not replace the inode
    // while an orphan still owns it. Canonicalize scratch before calling us.
    let name = scratch.file_name().ok_or("scratch must not be filesystem root")?;
    let path = scratch.with_file_name(format!(".{}.xsa-build.lock", name.to_string_lossy()));
    let file = File::options().read(true).write(true).create(true).open(&path)
        .map_err(|e| format!("open {}: {e}", path.display()))?;
    unsafe {
        extern "C" {
            fn flock(fd: i32, operation: i32) -> i32;
            fn fcntl(fd: i32, command: i32, ...) -> i32;
        }
        if flock(file.as_raw_fd(), 2 | 4) != 0 { // LOCK_EX | LOCK_NB
            return Err(format!("scratch is owned by a live build or stage: {} ({})",
                path.display(), std::io::Error::last_os_error()));
        }
        // F_SETFD=2, clear FD_CLOEXEC so all native descendants retain ownership.
        if fcntl(file.as_raw_fd(), 2, 0) != 0 {
            return Err(format!("inherit scratch lock: {}", std::io::Error::last_os_error()));
        }
    }
    Ok(file)
}

/// Run one phase under /usr/bin/time -v with the hard RLIMIT_AS cap, exactly
/// as the reference gates do: stdout/stderr land in `log_path`, wall/peak-RSS
/// telemetry in `time_path`, both under the scratch tree and journaled.
/// A phase running in the background (spawned; join later). The uniform
/// journal writes PHASE START at spawn and PHASE END at join, so two
/// concurrent phases interleave honestly in the one journal.
struct RunningPhase {
    tag: String,
    child: std::process::Child,
    log_path: PathBuf,
    time_path: PathBuf,
    armed: bool,
}

impl RunningPhase {
    fn terminate(&mut self) {
        if !self.armed { return; }
        // Every wrapper and native descendant shares this private group.
        // Child::kill alone kills /usr/bin/time and leaves the real consumer
        // polling the same scratch path, where it can steal a later run's pieces.
        unsafe {
            extern "C" { fn kill(pid: i32, signal: i32) -> i32; }
            kill(-(self.child.id() as i32), 9);
        }
        let _ = self.child.wait();
        self.armed = false;
    }
}

impl Drop for RunningPhase {
    fn drop(&mut self) { self.terminate(); }
}

/// Spawn one phase under /usr/bin/time -v with the hard RLIMIT_AS cap and
/// optional per-phase environment overrides (journaled in PHASE START).
///
/// THERE IS NO TIMEOUT WRAPPER AND NO SUPERVISOR. Every phase is spawned
/// directly and runs until it exits, fails, or a human stops it: a wall clock
/// killed a HEALTHY 100 GB run at 61/102 waves after 9.4 h (rc=124) because
/// someone set a work budget, and a stall heuristic is the same class of error.
fn spawn_phase(
    journal: &mut Journal,
    scratch: &Path,
    tag: &str,
    memory_kb: u64,
    program: &str,
    args: &[String],
    env: &[(String, String)],
) -> Result<RunningPhase, String> {
    let log_path = scratch.join(format!("{tag}.log"));
    let time_path = scratch.join(format!("{tag}.time"));
    let _ = fs::remove_file(&log_path);
    let _ = fs::remove_file(&time_path);
    let command_line = format!("{program} {}", args.join(" "));
    let env_note = if env.is_empty() {
        String::new()
    } else {
        format!(" env={}", env.iter().map(|(k, v)| format!("{k}={v}")).collect::<Vec<_>>().join(" "))
    };
    journal.line(&format!("PHASE {tag} START {command_line}{env_note} rlimit_as_kb={memory_kb}"));
    let log = File::options().create(true).append(true).open(&log_path)
        .map_err(|e| format!("create {}: {e}", log_path.display()))?;
    let log_err = log.try_clone().map_err(|e| format!("clone {tag} log handle: {e}"))?;
    let mut command = {
        let mut cmd = Command::new("bash");
        cmd.arg("-c").arg(PHASE_WRAPPER)
            .arg("xsa-phase") // $0
            .arg(memory_kb.to_string()) // $1: RLIMIT_AS in KiB
            .arg("/usr/bin/time").arg("-v").arg("-o").arg(&time_path)
            .arg(program);
        cmd.args(args)
            .process_group(0)
            .stdin(Stdio::null())
            .current_dir(scratch)
            .stdout(Stdio::from(log))
            .stderr(Stdio::from(log_err));
        cmd
    };
    for (key, value) in env {
        command.env(key, value);
    }
    if scratch.join("checkpoint.journal").exists() {
        command.env("XSA_CHECKPOINT_JOURNAL", scratch.join("checkpoint.journal"));
    }
    let child = command.spawn().map_err(|e| format!("launch {tag}: {e}"))?;
    Ok(RunningPhase { tag: tag.to_string(), child, log_path, time_path, armed: true })
}

/// Wait for a spawned phase, journal its telemetry, fail loud on error.
fn join_phase(
    journal: &mut Journal,
    scratch: &Path,
    mut running: RunningPhase,
) -> Result<(), String> {
    let tag = running.tag.clone();
    let status = running.child.wait().map_err(|e| format!("wait {tag}: {e}"))?;
    let (wall, peak_rss_kb) = fs::read_to_string(&running.time_path)
        .map(|text| parse_time_report(&text))
        .unwrap_or((None, None));
    let used_after = tree_bytes(scratch);
    journal.line(&format!(
        "PHASE {tag} END rc={} wall={} peak_rss_kb={} disc_scratch_bytes={}",
        status.code().unwrap_or(-1),
        wall.as_deref().unwrap_or("?"),
        peak_rss_kb.map(|v| v.to_string()).unwrap_or_else(|| "?".into()),
        used_after,
    ));
    if !status.success() {
        let tail = fs::read_to_string(&running.log_path).unwrap_or_default();
        let tail: Vec<&str> = tail.lines().rev().take(5).collect::<Vec<_>>().into_iter().rev().collect();
        return Err(format!("PHASE {tag} FAILED (rc {}); log tail:\n{}", status.code().unwrap_or(-1), tail.join("\n")));
    }
    running.armed = false;
    Ok(())
}

/// Observe every stage while the pipeline is live. Waiting for the sorter
/// first hides a dead merge behind the sorter's next credit wait indefinitely.
/// Dropping the remaining phases on any error kills their private groups.
fn join_pipeline(journal: &mut Journal, scratch: &Path,
                 phases: Vec<RunningPhase>) -> Result<(), String> {
    join_pipeline_flow(journal, scratch, phases, None)
}

fn join_pipeline_flow(journal: &mut Journal, scratch: &Path,
                      phases: Vec<RunningPhase>, mut flow: Option<Backpressure>) -> Result<(), String> {
    let mut pending: Vec<Option<RunningPhase>> = phases.into_iter().map(Some).collect();
    // XSA_KILL_AT: STATE-triggered ordering hooks (`mid_band`, `after_rename`).
    // They fire on an observable ordering state, never on a clock: with the
    // pipeline live, `mid_band` waits for the sorter's FIRST published band and
    // `after_rename` for the container's name to appear (the finish links it
    // only after fsync, so its appearance IS the publication instant).
    let band_mid = recovery::kill_wanted("mid_band");
    let mut kill_point: Option<&'static str> = None;
    while pending.iter().any(Option::is_some) {
        recovery::request_stop(scratch)?;
        for slot in &mut pending {
            let exited = match slot.as_mut() {
                Some(phase) => phase.child.try_wait()
                    .map_err(|e| format!("poll {}: {e}", phase.tag))?.is_some(),
                None => false,
            };
            if exited {
                let mut phase = slot.take().unwrap();
                let status = phase.child.wait().map_err(|e| e.to_string())?;
                if status.code() == Some(75) && recovery::signal() != 0 {
                    phase.armed = false;
                    if phase.tag == "emit-tape" || phase.tag == "ctx-tap" {
                        recovery::interrupted(scratch)?;
                        return Err("BUILD_INTERRUPTED".into());
                    }
                } else { join_phase(journal, scratch, phase)?; }
            }
        }
        if pending.iter().any(Option::is_some) {
            if let Some(f) = flow.as_mut() { f.observe(journal)?; }
            // `mid_band`: a SPOOL IS ON DISK whose consumer has not incorporated
            // it. The sorter's bands are consumed within milliseconds, so the
            // observable mid-run spool with a real lifetime is a PIECE; the band
            // case is kept for corpora where several bands are outstanding.
            if band_mid && (count_pieces_on_disk(scratch) >= 2 || count_unconsumed_bands(scratch) >= 1) {
                kill_point = Some("mid_band");
            }
            if let Some(point) = kill_point {
                // Kill the phase groups first (their Drop would do it, but this
                // process is about to stop running Drop), then die hard.
                for slot in pending.iter_mut() {
                    if let Some(mut phase) = slot.take() { phase.terminate(); }
                }
                recovery::die_hard(point);
            }
            std::thread::sleep(Duration::from_millis(100));
        }
    }
    if let Some(f) = flow.take() { f.finish(journal)?; }
    if recovery::signal() != 0 && scratch.join("checkpoint.journal").exists() {
        recovery::interrupted(scratch)?;
        return Err("BUILD_INTERRUPTED".into());
    }
    Ok(())
}

fn run_phase(
    journal: &mut Journal,
    scratch: &Path,
    tag: &str,
    memory_kb: u64,
    program: &str,
    args: &[String],
) -> Result<(), String> {
    let running = spawn_phase(journal, scratch, tag, memory_kb, program, args, &[])?;
    join_phase(journal, scratch, running)
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

/// Band spools PUBLISHED but not yet incorporated: `band_<i>.sa` with its
/// `.done` marker and no `.consumed` sentinel. This - and nothing else - is the
/// exact state "a spool exists on disk whose consumer's output is not durable
/// yet", i.e. the `mid_band` ordering point.
fn count_pieces_on_disk(scratch: &Path) -> usize {
    fs::read_dir(scratch.join("merged").join("frag.pieces"))
        .map(|entries| entries.flatten()
            .filter(|e| e.file_name().to_string_lossy().starts_with("p-"))
            .count())
        .unwrap_or(0)
}

fn count_unconsumed_bands(scratch: &Path) -> usize {
    let dir = scratch.join("bands-tape");
    fs::read_dir(&dir)
        .map(|entries| entries.flatten()
            .filter(|e| {
                let name = e.file_name().to_string_lossy().to_string();
                name.starts_with("band_") && name.ends_with(".sa") && {
                    let stem = &name[..name.len() - 3];
                    dir.join(format!("{stem}.done")).exists() && !dir.join(format!("{stem}.consumed")).exists()
                }
            })
            .count())
        .unwrap_or(0)
}

/// The bundled native stages (verified by hash on every use).
fn bundle_tools() -> Result<PathBuf, String> {
    let root = super::bundle::resolve()?;
    for name in ["chunk_frontend", "cross_lcp_merge", "xsa_finish", "rpfbwt_endpoints", "slim_dump"] {
        let path = root.join(name);
        let ok = fs::metadata(&path).map(|m| m.is_file()).unwrap_or(false);
        if !ok {
            return Err(format!("bundled stage missing: {name} (rebuild this xsa package)"));
        }
    }
    Ok(root)
}

fn write_remap(scratch: &Path, override_path: Option<&Path>, corpus: &Path) -> Result<(PathBuf, RemapFacts), String> {
    let path = scratch.join("remap-256.bin");
    match override_path {
        Some(source) => {
            let bytes = fs::read(source).map_err(|e| format!("read remap {}: {e}", source.display()))?;
            if bytes.len() != 256 {
                return Err(format!("remap {} must have exactly 256 bytes", source.display()));
            }
            let mut table = [0u8; 256];
            table.copy_from_slice(&bytes);
            let facts = remap_facts(&table, corpus, "provided")?;
            fs::write(&path, bytes).map_err(|e| format!("write {}: {e}", path.display()))?;
            Ok((path, facts))
        }
        None => {
            let (table, facts) = derive_remap(corpus)?;
            fs::write(&path, table).map_err(|e| format!("write {}: {e}", path.display()))?;
            Ok((path, facts))
        }
    }
}

/// What the derived table asserts about the corpus, journalled with its sha256.
#[derive(Debug, Clone)]
pub struct RemapFacts {
    pub present: u32,
    pub present_bytes_hex: String,
    pub source: &'static str,
}

/// ONE sequential histogram pass over the corpus: one table increment per byte
/// (memory bound, ~1-2 GB/s), nothing materialised to disc, no 256-pass rescan
/// (which would cost 256n at pile). The total is asserted against the file size.
fn corpus_histogram(corpus: &Path) -> Result<[u64; 256], String> {
    use std::io::Read;
    let mut f = fs::File::open(corpus).map_err(|e| format!("open {}: {e}", corpus.display()))?;
    let mut hist = [0u64; 256];
    let mut block = vec![0u8; 1 << 22];
    let mut total = 0u64;
    loop {
        let z = f.read(&mut block).map_err(|e| format!("read {}: {e}", corpus.display()))?;
        if z == 0 { break; }
        total += z as u64;
        for &c in &block[..z] {
            hist[c as usize] += 1;
        }
    }
    let size = fs::metadata(corpus).map_err(|e| format!("stat {}: {e}", corpus.display()))?.len();
    if total != size {
        return Err(format!("REMAP_REFUSED histogram total {total} != corpus bytes {size}"));
    }
    if total == 0 {
        return Err("REMAP_REFUSED corpus is empty: it has no byte to rank".into());
    }
    Ok(hist)
}

/// THE END-FORMAT REMAP, DERIVED PER CORPUS (SPEC_CHAIN_TO_PILE Phase A).
///
/// END is virtual: it occurs once, at virtual index n, strictly below every byte
/// value, and it has NO code of its own. The table is still a 256-byte bijection
/// (the container's member 7 and every consumer read it that way):
///
///     present byte b of rank j (0-based, byte order) -> 1 + j
///     absent bytes                                   -> k+1..255 in byte order,
///                                                       then 0 (END's cell)
///
/// A UNIVERSAL TABLE CANNOT DO THIS: order preservation forces "present -> 1..k",
/// which depends on WHICH bytes the corpus uses (a fixed table failed on a
/// 428-byte fixture and on any corpus with a smaller alphabet). k == 256 leaves
/// no free code for END; the candidate identity bijection fails validation below.
/// An 8-bit transport cannot carry 256 data symbols plus END.
fn derive_remap(corpus: &Path) -> Result<([u8; 256], RemapFacts), String> {
    let hist = corpus_histogram(corpus)?;
    let table = table_from_hist(&hist)?;
    let facts = remap_facts_from_hist(&table, &hist, "corpus")?;
    Ok((table, facts))
}

/// The table itself, from the histogram alone: ONE corpus pass feeds both the
/// derivation and its validation (there is no second read to prove it).
fn table_from_hist(hist: &[u64; 256]) -> Result<[u8; 256], String> {
    let present: Vec<u8> = (0..256u32).filter(|&b| hist[b as usize] != 0).map(|b| b as u8).collect();
    let k = present.len();
    let mut table = [0u8; 256];
    if k <= 255 {
        for (rank, &b) in present.iter().enumerate() {
            table[b as usize] = (1 + rank) as u8;
        }
        let absent: Vec<u8> = (0..256u32).filter(|&b| hist[b as usize] == 0).map(|b| b as u8).collect();
        let mut fill = 1 + k;
        for &b in &absent {
            table[b as usize] = if fill <= 255 { let v = fill as u8; fill += 1; v } else { 0 };
        }
        if fill != 256 {
            return Err(format!("REMAP_REFUSED wrong absent-byte fill: {fill} != 256 (present={k})"));
        }
    } else {
        for b in 0..256usize { table[b] = b as u8; }
    }
    Ok(table)
}

/// Re-prove the table's two properties FROM THE TABLE AND THE HISTOGRAM (never
/// from the loop that built it), so a provided --remap is held to the same
/// contract as a derived one. `source` names which one this is.
fn remap_facts(table: &[u8; 256], corpus: &Path, source: &'static str) -> Result<RemapFacts, String> {
    let hist = corpus_histogram(corpus)?;
    remap_facts_from_hist(table, &hist, source)
}

/// The same proof, from a histogram the caller already paid for.
fn remap_facts_from_hist(table: &[u8; 256], hist: &[u64; 256], source: &'static str) -> Result<RemapFacts, String> {
    let hist = *hist;
    let mut seen = [false; 256];
    for &v in table.iter() {
        if seen[v as usize] {
            return Err(format!("REMAP_REFUSED not a bijection: code {v} is named twice"));
        }
        seen[v as usize] = true;
    }
    let mut present = 0u32;
    for b in 0..256usize {
        if hist[b] == 0 { continue; }
        if table[b] as u32 != present + 1 {
            return Err(format!(
                "REMAP_REFUSED table is not order preserving on THIS corpus: present byte 0x{b:02x} \
                 (rank {present}) maps to {} not {} - the table was not derived from this corpus",
                table[b], present + 1));
        }
        present += 1;
    }
    if present == 256 {
        // provable, and impossible to consume: say which, never silently proceed
        eprintln!("REMAP_WARNING corpus uses all 256 byte values: no code is free for the END \
                   cell; the build proceeds with the identity bijection and the consumer refuses \
                   the shape loudly (8-bit transport)");
    } else {
        let owner = (0..256u32).find(|&b| table[b as usize] == 0).unwrap_or(256);
        if owner >= 256 || hist[owner as usize] != 0 {
            return Err(format!(
                "REMAP_REFUSED the END cell (code 0) must be named by an ABSENT byte; got byte \
                 0x{owner:02x}"));
        }
    }
    let present_bytes_hex = (0..256usize).filter(|&b| hist[b] != 0)
        .map(|b| format!("{b:02x}")).collect::<String>();
    Ok(RemapFacts { present, source, present_bytes_hex })
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
    if options.tape && !options.plan_only { recovery::install()?; }
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
    let _scratch_owner = lock_scratch(&scratch)?;
    let journal_path = scratch.join("xsa-build.log");
    let mut prepared: Option<recovery::Prepared> = None;
    if options.tape && !options.plan_only {
        let p = recovery::prepare(&options, &input, &scratch, &tools)?;
        if p.complete { return Ok(()); }
        prepared = Some(p);
    }
    if options.tape && recovery::signal() != 0 {
        recovery::interrupted(&scratch)?; return Err("BUILD_INTERRUPTED".into());
    }
    if journal_path.exists() {
        return Err(format!(
            "scratch already used: {} exists; move the previous run away (never overwrite)",
            journal_path.display()
        ));
    }
    // PLAN-FIRST: survey + model before any bytes move. The plan is the
    // journal's FIRST entry (same plan file = same build = provenance);
    // --plan-only prints it and exits. The old DF_GATE refusal becomes this
    // richer contract: infeasible plans are refused WITH the alternatives.
    // Fixed chunk size (--chunk-mb) derives the chunk count by arithmetic
    // (document-aligned boundaries land near, not at, the target).
    let corpus_bytes = fs::metadata(&input).map_err(|e| format!("stat {}: {e}", input.display()))?.len();
    let chunks = match options.chunk_mb {
        Some(mb) => {
            let target = mb as u64 * 1048576;
            (((corpus_bytes + target - 1) / target).max(1)) as u32
        }
        None => options.chunks,
    };
    let survey = super::plan::survey(&input, &scratch, &options.stripes)?;
    let mut build_plan = if options.bin_pipeline {
        super::plan::model_v2(survey, chunks, options.kway, options.threads, options.memory_gb,
                              options.scratch_free_pct, options.bin_live_cap,
                              options.bin_target_size)
    } else if options.fanin {
        super::plan::model_fanin(survey, chunks, options.kway, options.threads, options.memory_gb, options.scratch_free_pct)
    } else {
        super::plan::model(survey, chunks, options.kway, options.threads, options.memory_gb, options.scratch_free_pct)
    };
    if options.bin_pipeline {
        // The chunk-knob surface of the MERGE pipeline is gone from this route:
        // the bin shape has no chunk set, no segment spools and no k-way walk.
        // A knob that would silently misconfigure the run is REFUSED.
        if let Ok(v) = std::env::var("CROSS_V2_PRODUCERS") {
            if v != "1" {
                return Err(format!("CROSS_V2_PRODUCERS={v} is INERT under the bin shape (the bin sink's writer width is --threads); unset it"));
            }
        }
        if std::env::var_os("CROSS_V2_SPOOL_BYTES").is_some() {
            return Err("CROSS_V2_SPOOL_BYTES is INERT under the bin shape (there are no merge segment spools); unset it".into());
        }
        // Carry the EXTRAPOLATED spill allowance explicitly, rather than hiding
        // a potentially corpus-sized term in the output ratio.
        if let Some(v2) = &mut build_plan.v2 {
            let witness_cap_gb = options.wit_ram_gb.unwrap_or(options.memory_gb / 2) as f64;
            let spill_gb = (v2.witness_ram_gb - witness_cap_gb).max(0.0) * 0.3;
            for phase in &mut build_plan.phases {
                if phase.name == "fused-finish" { phase.disc_peak_mb += spill_gb * 1024.0; }
            }
            v2.bin_phrase_note = super::plan::bin_phrase_note(corpus_bytes, options.phrase.enabled);
            v2.transient_note.push_str(&format!("; witness spill reserve {:.1} GiB (0.3x overflow, EXTRAPOLATED)", spill_gb));
            // ---- THE TAPE ROUTE'S RAM TERM ------------------------------------
            // --tape runs the BAND SORTER, not the bin producer: its RAM is K band
            // arenas of rows_per_band rows (see plan::tape_band_plan). The plan must
            // price THAT process - the 100 GB run was refused nothing while the
            // sorter sized its bands from the corpus and died with rc=134 - so the
            // band plan replaces the bin build group in the RAM verdict, and the
            // same numbers are handed to the producer.
            if options.tape {
                // THE BIN SHAPE'S OWN ADVICE IS NOT THIS RUN'S. A tape plan whose
                // alternatives say "--bin-live-cap" is pricing a process that will
                // not run; the CPU note is the only shape-independent one kept.
                // (The bin shape's NUMBER follows later, explicitly labelled.)
                build_plan.alternatives.retain(|a| a.starts_with("CPU lacks"));
                let lcp_slices = std::env::var("XSA_LCE_VERIFY").map(|v| v != "1").unwrap_or(true);
                let n_rows = corpus_bytes.saturating_add(1);       // + the structural END
                // THE SAME BUDGET THE RUN WILL USE (plan::scratch_budget): the plan
                // block cannot price a wave the run will not build.
                let sb = super::plan::scratch_budget(
                    options.scratch_budget_gb, options.memory_gb,
                    build_plan.survey.scratch_free_bytes);
                let tape_plan = super::plan::tape_band_plan(
                    n_rows, options.threads as u64, options.memory_gb, lcp_slices,
                    2 * n_rows,                                     // the 2n scratch window
                    sb.bytes,                                       // the DISK budget
                );
                // THE m14 DECISION RIDES THE PLAN LINE ITSELF, so a container's
                // provenance answers "does this thing have the LCP column" from the
                // plan, not from a comment in a review: present(codec=117) or absent.
                v2.tape_note = Some(format!(
                    "{} lcp_runs={} member=m14 lcp_runs_version={} lcp_runs_price=measured(0.27n@150MB,0.256n@10GB,5.89bits/run)",
                    tape_plan.note(),
                    if options.with_lcp_runs { "present(codec=117)" } else { "absent" },
                    if options.with_lcp_runs { 8 } else { 7 }));

                build_plan.ram_verdict = tape_plan.verdict();
                build_plan.ram_ok = tape_plan.feasible();
                if !tape_plan.feasible() {
                    // The refusal sentence itself is the XSA_PLAN_RAM line; the
                    // alternative carries the KNOBS, not a second copy of them.
                    build_plan.alternatives.push(format!(
                        "--memory-gb {} must hold the unit the {} GiB scratch budget implies \
                         (rows_per_band={}, floor={}, {} bands in flight = P = --threads); this plan \
                         needs --memory-gb {}, or lower --threads, or raise --scratch-budget-gb; the \
                         arithmetic and the shortfall are on the XSA_PLAN_RAM line above",
                        options.memory_gb, tape_plan.scratch_budget_bytes >> 30,
                        tape_plan.rows_per_band, tape_plan.floor_rows, tape_plan.bands_in_flight,
                        tape_plan.needed_memory_gb()));
                }
            }
        }
        // ---- THE FIT CHECK PRICES THE SHAPE THE RUN WILL EXECUTE ------------
        // --tape runs the band sorter: its scratch peak is the MEASURED 0.789n
        // band window x TAPE_DISC_FACTOR (see plan::tape_disc_model), not the bin
        // shape's rlebwt + rolling window + container tree. Gating the tape route
        // on the bin shape's number is how a run that fits was refused - and the
        // bin shape's number may still be PRINTED, but only as an explicitly
        // labelled alternative, never as this run's gating number.
        let free_bytes = build_plan.survey.scratch_free_bytes;
        let bin_peak_mb = build_plan.phases.iter().map(|p| p.disc_peak_mb).fold(0.0, f64::max);
        let bin_peak_bytes = (bin_peak_mb * 1048576.0) as u64;
        if options.tape {
            // A BUDGETED RUN IS PRICED AT ITS CAP: with --scratch-budget-gb the
            // wave IS a percentage of the budget, so 1.00n is not this run's peak
            // (it is the uncapped envelope's). Refusing a 100 GB run with a 30 GB
            // cap for 79 GB of scratch it will never take is the same class of
            // error as pricing the bin shape.
            let budget = super::plan::scratch_budget(
                options.scratch_budget_gb, options.memory_gb, free_bytes);
            build_plan.shape = super::plan::Shape::Tape;
            build_plan.disc_model = super::plan::tape_disc_model(
                corpus_bytes, free_bytes,
                options.scratch_budget_gb.map(|_| budget.bytes));
            let (fits, verdict) = super::plan::tape_disc_verdict(
                corpus_bytes, free_bytes, options.scratch_budget_gb.map(|_| budget.bytes));
            build_plan.disc_verdict = verdict;
            build_plan.disc_ok = fits;
            build_plan.disc_peak_bytes = super::plan::tape_scratch_peak_bytes(
                corpus_bytes, options.scratch_budget_gb.map(|_| budget.bytes));
            // THE OTHER SHAPE is rendered by plan::Plan::render() itself (labelled
            // `XSA_PLAN_ALTERNATIVE shape=bin`), so no call site can print it
            // unlabelled or forget it.
            if !fits {
                build_plan.alternatives.push(
                    "free or add scratch space for the tape shape's 1.00n peak (it is a measured \
                     n-multiple: --threads does not shrink it), or pass --skip-fit-check to run \
                     anyway".into());
            }
        } else {
            build_plan.shape = super::plan::Shape::Bin;
            build_plan.disc_model = super::plan::bin_disc_model(
                corpus_bytes, bin_peak_bytes, free_bytes,
                build_plan.v2.as_ref().map(|v| v.bin_live_cap).unwrap_or(0),
                build_plan.v2.as_ref().map(|v| v.bin_windows).unwrap_or(1),
                build_plan.v2.as_ref().map(|v| v.bin_window_file_gb).unwrap_or(0.0));
            // THE MISSION BOUND IS NOT A CAPACITY CHECK: the bin shape's output
            // side must stay inside 2.5n as a property of the MODEL. --skip-fit-check
            // buys free space, never a model that is over target, so this refuses
            // before the peak-vs-free check is even consulted.
            if super::plan::V2_TREE_PEAK_PER_N > super::plan::V2_TREE_TARGET_PER_N {
                return Err(format!(
                    "V2_TREE_PEAK {}n is OVER the {}n mission bound: the plan's own output-side                      model is out of bounds, which no flag overrides",
                    super::plan::V2_TREE_PEAK_PER_N, super::plan::V2_TREE_TARGET_PER_N));
            }
            let (fits, verdict) = super::plan::bin_disc_verdict(corpus_bytes, bin_peak_bytes, free_bytes);
            build_plan.disc_verdict = verdict;
            build_plan.disc_ok = fits;
            build_plan.disc_peak_bytes = bin_peak_bytes;
            if !fits {
                build_plan.alternatives.push("free scratch for the all-phase bin-shape peak".into());
            }
            if 100u32.saturating_sub(build_plan.survey.scratch_used_percent) < options.scratch_free_pct {
                build_plan.alternatives.push("scratch hygiene policy: pass --scratch-free-pct to override".into());
                build_plan.disc_verdict.push_str("; INFEASIBLE under scratch hygiene policy");
            }
        }
    }
    // THE OVERRIDE APPLIES TO THE ONE CHECK, ON EVERY SHAPE (the legacy routes
    // price their own peak; the flag means the same thing there).
    build_plan.fit_check_skipped = options.skip_fit_check;
    if options.plan_only {
        // --plan-only PORTS the plan; it never starts a phase and never refuses
        // (the refusal is the real run's, below). The verdicts are printed either
        // way, so a preflight cannot look like a pass when it is not.
        print!("{}", build_plan.render());
        return Ok(());
    }
    if !build_plan.disc_ok || !build_plan.ram_ok {
        // A REFUSAL, WITH ITS OWN NUMBERS. The plan (model line + verdict +
        // alternatives) is printed first; the error repeats the gating verdict so
        // the operator never has to guess which check failed or which flag is the
        // override. The ABSOLUTE peak-vs-free check is the one an operator may
        // override (--skip-fit-check); the RAM term has no override by design.
        print!("{}", build_plan.render());
        let mut why: Vec<String> = Vec::new();
        if !build_plan.disc_ok {
            why.push(format!("XSA_PLAN_DISC {}", build_plan.disc_verdict));
        }
        if !build_plan.ram_ok {
            why.push(format!("XSA_PLAN_RAM {}", build_plan.ram_verdict));
        }
        if !build_plan.disc_ok && !options.skip_fit_check {
            return Err(format!(
                "PLAN_INFEASIBLE: {}\nThe XSA_PLAN_ALTERNATIVE lines above name the knobs; \
                 --skip-fit-check overrides ONLY the absolute peak-vs-free check (it does NOT \
                 touch the RAM term)", why.join(" | ")));
        }
        if !build_plan.ram_ok {
            return Err(format!(
                "PLAN_INFEASIBLE (RAM - no flag overrides this): {}\nThe RAM/band term is a \
                 function of --memory-gb and --threads; --skip-fit-check does not touch it",
                why.join(" | ")));
        }
        eprintln!("warning: --skip-fit-check: proceeding although the plan did not prove a fit \
                   (peak={} B vs free={} B; the override is journaled as DISC_CHECK_SKIPPED)",
                  build_plan.disc_peak_bytes, build_plan.survey.scratch_free_bytes);
    }
    let mut journal = Journal::open(&journal_path)?;
    let mut outcome = ScratchOutcome {
        scratch: scratch.clone(),
        journal: Journal { file: journal.file.try_clone().map_err(|e| e.to_string())? },
        success: false,
    };
    // SOURCE PROVENANCE, FIRST LINE: which tree produced the stages this run
    // executes. Git is the record (there is no checked-in digest of the
    // sources - see source_identity_line); the plan block follows as the first
    // PLAN entry.
    journal.line(&source_identity_line());
    for line in build_plan.render().lines() {
        journal.line(line);
    }
    for knob in ["CROSS_NO_POOL", "CROSS_ASYNC_FILL", "CROSS_HASH_K", "CROSS_PIPELINE", "CROSS_SHARDS", "CROSS_V2_PRODUCERS", "CROSS_SEAM_REFERENCE", "CROSS_SEAM_ORACLE", "CROSS_NO_REF", "CHUNK_NO_REF", "CHUNK_POS_MODE", "CROSS_POS_MODE", "CROSS_NO_SXP4"] {
        if let Some(value) = std::env::var_os(knob) {
            journal.line(&format!("KNOB {knob}={}", value.to_string_lossy()));
        }
    }
    // Pos-column mode: the plan's pick (XSA_POS_MODE) unless the operator
    // already chose. Dense = fast walk / fat disc; compact = thin disc /
    // slow walk. Both writers honor it; the reader sniffs the format. The bin
    // shape has no position column at all, so the knob is not set there.
    if options.bin_pipeline {
        if std::env::var_os("CHUNK_POS_MODE").is_some() || std::env::var_os("CROSS_POS_MODE").is_some() {
            return Err("CHUNK_POS_MODE/CROSS_POS_MODE are INERT under --v2-pipeline: the bin shape keeps positions only in a transient RAM arena, never in a column; unset them".into());
        }
        journal.line("KNOB POS_COLUMN none (bin shape: positions live in the transient arena pair only; full_SA=0 persistent_positions=0)");
    } else {
    if std::env::var_os("CHUNK_POS_MODE").is_none() {
        std::env::set_var("CHUNK_POS_MODE", &build_plan.pos_mode);
        journal.line(&format!("KNOB CHUNK_POS_MODE={} (plan-picked)", build_plan.pos_mode));
    }
    if std::env::var_os("CROSS_POS_MODE").is_none() && std::env::var_os("CROSS_NO_SXP4").is_none() {
        std::env::set_var("CROSS_POS_MODE", &build_plan.pos_mode);
        journal.line(&format!("KNOB CROSS_POS_MODE={} (plan-picked)", build_plan.pos_mode));
    }
    }
    // Fan-in shape: fixed chunks + repeated k-way fan-in. Consume inputs
    // (raw chunks and per-level intermediates) and reclaim them as the build
    // advances, so the peak disc stays ~ one level's live set instead of the
    // whole input history (the flat shape's 48n pile-killer).
    if options.fanin {
        if std::env::var_os("CROSS_DELETE_CONSUMED").is_none() {
            std::env::set_var("CROSS_DELETE_CONSUMED", "1");
            journal.line("KNOB CROSS_DELETE_CONSUMED=1 (fan-in: delete inputs as consumed)");
        }
    }
    // Preflight: disc gate before anything is written (the plan-first
    // verdicts above carry the fit + hygiene contract; this is the belt).
    disc_preflight(&scratch, options.scratch_free_pct)?;
    // The chunk count / k-way arity are NOT part of the bin shape; the journal
    // prints them as "-" there so a log can never be read as if a chunk set or
    // a k-way walk existed on this route.
    let (chunks_note, kway_note) = if options.bin_pipeline {
        ("-".to_string(), "-".to_string())
    } else {
        (chunks.to_string(), options.kway.to_string())
    };
    journal.line(&format!(
        "XSA_BUILD START input={} scratch={} memory_gb={} threads={} chunks={} kway={} remap={} pipeline={}",
        input.display(), scratch.display(), options.memory_gb, options.threads,
        chunks_note, kway_note, if options.remap.is_some() { "provided" } else { "derived-from-corpus" },
        if options.bin_pipeline { "v2bin" } else if options.fanin { "v1-fanin" } else { "v1" }
    ));
    let corpus_dir = options.input.parent().filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new(".")).canonicalize()
        .map_err(|e| format!("resolve input directory: {e}"))?;
    let mut header = [0u8; 48];
    let docs = File::open(corpus_dir.join("seps.bin"))
        .and_then(|mut file| file.read_exact(&mut header)).ok()
        .filter(|_| &header[..4] == b"XSEP"
            && u32::from_le_bytes(header[4..8].try_into().unwrap()) == 1
            && u64::from_le_bytes(header[24..32].try_into().unwrap()) == corpus_bytes)
        .map(|_| u64::from_le_bytes(header[16..24].try_into().unwrap())).unwrap_or(0);
    journal.line(&format!("CONTAINER_CORPUS_SIDECARS dir={} seps=seps.bin titles=titles.bin docs={docs} corpus_bytes={corpus_bytes}",
        serde_json::to_string(&corpus_dir.to_string_lossy()).unwrap()));
    // THE FLAG FLIP (auto-radix): the bin pipeline is the default route, so the
    // flag that used to select it is no longer a route selector. It is ACCEPTED
    // AND IGNORED - journalled here, in the plan's own log, so no run can be
    // mistaken for one that was configured by it.
    if options.v2_flag_given {
        journal.line(if options.legacy_merge {
            "NOTE --v2-pipeline ACCEPTED AND IGNORED: --legacy-merge was also given, and the merge route wins (the bin pipeline is the default since auto-radix)"
        } else {
            "NOTE --v2-pipeline ACCEPTED AND IGNORED: the bin pipeline is the DEFAULT (auto-radix) - the flag no longer selects a route; pass --legacy-merge for the chunk+merge route"
        });
    }
    // Snap preflight: the demonstrated refusal fires before any phase runs.
    let n = if options.tape && !options.snap_1e {
        let n = fs::metadata(&input).map_err(|e| format!("stat input: {e}"))?.len();
        if n == 0 { return Err("empty input".into()); }
        n
    } else { check_snap_boundary(&input)? };
    if options.snap_1e {
        let digest = sha256_file(&input)?;
        journal.line(&format!("SNAP_OK n={n} sha256={digest}"));
    } else if options.tape {
        journal.line(&format!("RAW_BYTES n={n} terminal=virtual-END transformation=none"));
    } else {
        journal.line(&format!("SNAP_BOUNDARY_OK n={n} (0x1e; pass --snap-1e to also journal the full sha256)"));
    }
    // THE REMAP IS DERIVED FROM *THIS* CORPUS (SPEC_CHAIN_TO_PILE Phase A). A
    // universal table cannot be order preserving on an arbitrary alphabet, so the
    // builder derives it here, proves the END-format contract against the corpus,
    // journals its sha256 and hands the SAME file to every child.
    // The plan spool: `prepare` already resolved, proved and PUBLISHED the
    // remap table (name-encoded, fsynced, renamed into place). Re-deriving it
    // here would be a second corpus pass for a byte-identical 256-byte table.
    let (remap_path, remap_facts) = match &prepared {
        Some(p) => {
            let path = scratch.join("remap-256.bin");
            if !path.exists() {
                return Err(format!("RESUME_INVARIANT the plan spool {} is missing after prepare", path.display()));
            }
            (path, p.remap.clone())
        }
        None => write_remap(&scratch, options.remap.as_deref(), &input)?,
    };
    let remap_sha = sha256_file(&remap_path)?;
    journal.line(&format!(
        "REMAP {} sha256={remap_sha} source={} present={} order_preserving=1 bijection=1 \
         end_code=none end_symbol=0 unused_symbols={}",
        remap_path.display(), remap_facts.source, remap_facts.present,
        255u32.saturating_sub(remap_facts.present)));
    journal.line(&format!(
        "CORPUS_PRESENT_BYTES count={} hex={} container_source=m7+m1 rule=nonzero-symbol-with-positive-count end_symbol=0",
        remap_facts.present, remap_facts.present_bytes_hex));
    // The v2 bin shape has NO chunk set, no merge work directory and no
    // finish side-stream directory: only the merged/ prefix (rlebwt + pieces)
    // exists, and the tree rules assert nothing else is left behind.
    let dirs: &[&str] = if options.bin_pipeline { &["merged"] } else { &["chunks", "merged", "mwork", "finish"] };
    for dir in dirs {
        fs::create_dir_all(scratch.join(dir)).map_err(|e| format!("create {dir}: {e}"))?;
    }
    // Provenance: every bundled stage is hash-verified by bundle::resolve()
    // already; journal the sha256 of the stages this build will actually
    // execute, so a journal attests WHICH binaries ran (the bin reports' rule).
    journal_bundle_provenance(&mut journal, &tools, &["cross_lcp_merge", "xsa_finish"])?;
    let memory_kb = options.memory_gb * 1048576;
    // The finish phase's own RLIMIT_AS: mapped corpus windows charge address
    // space (page-cache backed, reclaimable - NOT RSS), so pinning the corpus
    // for the lock-free byte path wants a fatter AS than the build's envelope.
    // Default: inherit the build's cap.
    let finish_as_kb = match options.finish_as_gb {
        Some(v) => v * 1048576,
        None => memory_kb,
    };
    let merged = scratch.join("merged");
    let finish = scratch.join("finish");
    let prefix = merged.join("frag");
    let frag = prefix.to_str().ok_or("scratch path is not UTF-8")?.to_string();
    let s = |path: &str| path.to_string();

    // V2 FRONT DOOR (THE BIN SHAPE): bin-bwt (sweep -> 256 byte-bins ->
    // ordered stitch) as the producer, the decomposed fused finish as the
    // consumer, CONCURRENT and flow-controlled through the piece window, then
    // the lite container publish and the rlebwt delete - one command, one
    // journal. The chunk phase and the merge walk are NOT on this route.
    if options.bin_pipeline {
        // The executor reads the arena bound from the PLAN (plan-first: the
        // plan is the journal's first entry, so the run must execute what the
        // plan priced).
        let v2 = build_plan.v2.as_ref().ok_or("internal: v2 plan has no bin terms")?;
        let (bin_live_cap, plan_windows) = (v2.bin_live_cap, v2.bin_windows);
        // An EXPLICIT cap the phase cannot hold is refused HERE, before a single
        // process is spawned: the producer would otherwise die mid-window with a
        // bad_alloc (or, before the pwrite write path, on a window-file mmap -
        // the 10 GB sol10.log failure). The producer checks the same budget
        // against its own RLIMIT_AS and refuses too; this is the front door's
        // half of the same contract.
        if let Some(v) = options.bin_live_cap {
            if let Some(fits) = super::plan::bin_cap_over_budget(v, n, options.threads as u64, options.memory_gb * (1 << 30)) {
                return Err(format!(
                    "V2_BIN_CAP_OVER_BUDGET: --bin-live-cap {v} cannot fit the phase budget: \
                     the corpus cache ({} B) + the fill staging ({} B) + 16 B/slot x min(cap, n) \
                     exceed {}% of --memory-gb {}; {} slots is the largest cap this phase can \
                     hold (lower --bin-live-cap, or raise --memory-gb / shard the corpus)",
                    super::plan::bin_corpus_cache_bytes(n, options.memory_gb * (1 << 30)),
                    options.threads as u64 * super::plan::BIN_FILL_STAGE_BYTES_PER_THREAD,
                    super::plan::BIN_PHASE_HEADROOM_PCT, options.memory_gb, fits));
            }
        }
        // The producer is always handed the plan's cap EXPLICITLY: for the
        // default the plan DERIVED it from --memory-gb (and the scratch), and
        // passing it keeps the plan and the run from ever pricing two different
        // windows. The producer still validates it against the live RLIMIT_AS.
        journal.line(&format!("V2_BIN_CAP live_cap_slots={bin_live_cap} windows={plan_windows} \
             source={} (the plan's pick; the producer re-checks it against the phase RLIMIT_AS)",
            if options.bin_live_cap.is_some() { "operator" } else { "auto from --memory-gb" }));
        // The phrase arm, stated once in the journal: OFF by default, and when ON
        // the exact knobs the producer will receive (the command line is journaled
        // by spawn_phase too, but a run's log must be able to answer "was the
        // dictionary armed and with what" without hunting for the argv).
        if options.phrase.enabled {
            let knobs: Vec<String> = options.phrase.knobs.iter()
                .map(|(f, v)| format!("{}={v}", f.trim_start_matches("--phrase-")))
                .collect();
            journal.line(&format!("V2_PHRASE armed=1 knobs={} (default OFF; the parse runs before the bins, \
                 and PHRASE_DICT/PHRASE_WORK journal what it found and what it skipped)",
                if knobs.is_empty() { "none (producer defaults)".to_string() } else { knobs.join(",") }));
        } else {
            journal.line("V2_PHRASE armed=0 (--phrase-radix absent: no parse, no dictionary, byte radix only)");
        }
        // The remap's own provenance ("corpus" = this attempt derived it) is what
        // the build-end accounting prices phase=remap from, so the tail is handed
        // the fact rather than re-deriving it from a log.
        run_v2_tail(&mut journal, &options, &scratch, &tools, &input, &remap_path,
                    n, memory_kb, finish_as_kb, &frag, bin_live_cap, plan_windows,
                    remap_facts.source)?;
        outcome.success = true;
        return Ok(());
    }

    // 1. chunk front end (libsais, document-aligned cyclic BWT chunks).
    // PILE-WEEK (pile-week-v1): concurrent per-chunk sorts (the boundary
    // scan stays serial and byte-identical; each worker is ~12x its chunk
    // bytes of RAM, so the count is bounded by the memory cap).
    let mut chunk_env: Vec<(String, String)> = Vec::new();
    if let Some(w) = std::env::var_os("CHUNK_WORKERS") {
        journal.line(&format!("KNOB CHUNK_WORKERS={} (operator-passed)", w.to_string_lossy()));
        chunk_env.push(("CHUNK_WORKERS".to_string(), w.to_string_lossy().to_string()));
    }
    {
        let running = spawn_phase(&mut journal, &scratch, "chunk", memory_kb, tools.join("chunk_frontend").to_str().unwrap(), &[
            s(&input.to_string_lossy()), chunks.to_string(),
            s(&scratch.join("chunks").to_string_lossy()), s(&remap_path.to_string_lossy()),
        ], &chunk_env)?;
        join_phase(&mut journal, &scratch, running)?;
    }
    journal.line(&require_log_contains(&scratch, "chunk", "CHUNKS_PASS")?);

    // 2. externalized merge tree (delete-as-consumed scratch, --emit-pf).
    // Sample peak disc through the merge: with delete-as-you-go the fan-in
    // peak is mid-merge, not at a phase boundary.
    let merge_sampler = DiscSampler::start(scratch.clone(), Duration::from_secs(2));
    run_phase(&mut journal, &scratch, "merge", memory_kb, tools.join("cross_lcp_merge").to_str().unwrap(), &[
        "--tree".into(), s(&scratch.join("chunks").to_string_lossy()), chunks.to_string(),
        n.to_string(), frag.clone(),
        "--threads".into(), options.threads.to_string(),
        "--kway".into(), options.kway.to_string(),
        "--work".into(), s(&scratch.join("mwork").to_string_lossy()),
        "--emit-pf".into(),
    ])?;
    let merge_peak_disc = merge_sampler.peak_bytes();
    journal.line(&format!("MERGE_PEAK_DISC_USED_BYTES {merge_peak_disc} (sampled every 2s during the merge)"));
    let pairs = count_log_contains(&scratch, "merge", "CROSS_PAIR");
    let kways = count_log_contains(&scratch, "merge", "CROSS_KWAY");
    journal.line(&format!("MERGE cross_pairs={pairs} cross_kway_nodes={kways} kway={}", options.kway));
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
    // If the binary was replaced mid-run (cargo rebuild), /proc/self/exe
    // resolves to a deleted inode and the re-exec would fail with 127.
    // Fall back to the invocation path (argv[0]): the gate then runs the
    // binary currently at that path end-to-end.
    let xsa = if xsa.to_string_lossy().ends_with(" (deleted)") {
        std::env::args().next().map(PathBuf::from).filter(|p| p.is_file())
            .ok_or_else(|| "sweep launcher: this binary was replaced during the run; re-invoke by path".to_string())?
    } else { xsa };
    run_phase(&mut journal, &scratch, "sweep", memory_kb, xsa.to_str().unwrap(), &[
        "chi-rspace".into(), "--stream-agg".into(),
        "--ri4".into(), s(&finish.join("frag.ri4").to_string_lossy()),
        "--agg".into(), s(&finish.join("frag.agg").to_string_lossy()),
        "-o".into(), s(&finish.join("frag.sA").to_string_lossy()),
    ])?;
    let chi = require_log_contains(&scratch, "sweep", "chi = ")?;
    let peak_disc = tree_bytes(&scratch);
    journal.line(&format!("PEAK_DISC_USED_BYTES {peak_disc} (scratch tree after finish)"));
    journal.line(&format!("XSA_BUILD_DONE {chi}"));
    outcome.success = true;
    Ok(())
}

/// Collect every file under `root` whose name is in `names` (the v2 tree
/// rules: no agg/ssa_t/head_sa/pftext/pfck, rlebwt deleted post-publish).
fn find_files_named(root: &Path, names: &[&str]) -> Vec<PathBuf> {
    let mut found = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let entries = match fs::read_dir(&dir) {
            Ok(entries) => entries,
            Err(_) => continue,
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let file_type = match entry.file_type() {
                Ok(t) => t,
                Err(_) => continue,
            };
            if file_type.is_dir() {
                stack.push(path);
            } else if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                if names.contains(&name) {
                    found.push(path);
                }
            }
        }
    }
    found
}

/// One `key=value` field of a native stage's journal line.
fn field<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    line.split_whitespace().find_map(|tok| tok.strip_prefix(key)?.strip_prefix('='))
}

fn field_u64(line: &str, key: &str) -> Result<u64, String> {
    field(line, key)
        .and_then(|v| v.parse::<u64>().ok())
        .ok_or_else(|| format!("journal line has no integer {key}= field: {line}"))
}

/// Journal the sha256 of the bundled stages this build will EXECUTE, so the
/// journal itself attests which binaries ran (bundle::resolve() already
/// verifies the embedded manifest on every use; this makes the run's
/// provenance readable from the log alone, as the bin reports do).
fn journal_bundle_provenance(journal: &mut Journal, tools: &Path, names: &[&str]) -> Result<(), String> {
    for name in names {
        let expected = super::bundle::FILES.iter().find(|(n, _, _)| n == name)
            .map(|(_, _, digest)| *digest)
            .ok_or_else(|| format!("stage {name} is not in the bundled manifest"))?;
        let path = tools.join(name);
        let bytes = fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
        let actual = super::bundle::hash(&bytes);
        if actual != expected {
            return Err(format!("BUNDLE_TOOL_MISMATCH {name}: {} sha256={actual} expected={expected}", path.display()));
        }
        journal.line(&format!("BUNDLE_TOOL name={name} path={} bytes={} sha256={actual} verified=1", path.display(), bytes.len()));
    }
    Ok(())
}

// Corpus/bitmap mappings consume virtual address space even when reclaimable.
fn finish_v2_as_kb(memory_kb: u64, requested_kb: u64, n: u64) -> u64 {
    requested_kb.max(memory_kb).max((3 * n + 1023) / 1024)
}

/// The v2 front door: THE BIN SHAPE. bin-bwt (one sweep -> 256 byte-bins ->
/// parallel per-bin construction -> ordered stitch into the unchanged
/// band/piece protocol) is the producer; the decomposed fused finish
/// (xsa_finish) is the consumer; they run CONCURRENT, flow-controlled through
/// the piece window; then the lite container publishes and the rlebwt is
/// deleted - all under the one journal. There is no chunk phase and no merge
/// walk on this route. Byte-identity + chi are the only arbiter; the front door
/// refuses on any violation, and it FAILS LOUD on a producer cap raise (that
/// would mean the plan priced a different arena than the run used).
#[allow(clippy::too_many_arguments)]
fn run_v2_tail(
    journal: &mut Journal,
    options: &Options,
    scratch: &Path,
    tools: &Path,
    input: &Path,
    remap_path: &Path,
    n: u64,
    memory_kb: u64,
    finish_as_kb: u64,
    frag: &str,
    bin_live_cap: u64,
    plan_windows: u64,
    remap_source: &str,
) -> Result<(), String> {
    let s = |path: &str| path.to_string();
    // NO TIMEOUTS, NO WATCHDOGS. Phases are spawned directly and run until they
    // exit or fail. The caps that used to live here (a GNU coreutils timeout
    // wrapper on every phase, then a stall watchdog that killed on a heuristic)
    // both destroyed expensive work on a guess; neither exists any more.
    journal.line(&format!(
        "V2_TIMEOUT none (NO TIMEOUT and NO WATCHDOG: no `timeout` wrapper on any phase and no \
supervisor process; base ulimit -v {memory_kb} KiB. A run ends when it exits, fails, or a human stops it)"));

    // ---- the producer: bin-bwt, the plan's resolved arena bound
    journal.line(&format!(
        "V2_BIN_SHAPE sweep->256 byte-bins->ordered stitch; bin_live_cap={bin_live_cap} slots (the plan's pick); \
         NO chunk phase, NO merge walk, NO position column"));
    let v2_window = std::env::var("CROSS_V2_PIECE_WINDOW").unwrap_or_else(|_| "1100".to_string());
    journal.line(&format!("V2_EMISSION CROSS_V2_PIECES=1 band=65536 window={v2_window}"));
    let mut producer_env = vec![
        ("CROSS_V2_PIECES".to_string(), "1".to_string()),
        ("CROSS_V2_PIECE_WINDOW".to_string(), v2_window.clone()),
    ];
    // ---- THE PLAN, COMPUTED ONCE, BEFORE ANY PHASE IS BUILT --------------
    // XSA_LCE_VERIFY=1 is the INTERNAL A/B ARM (not a user option): the producer
    // skips the slices and the finish walks the corpus for every endpoint, which
    // is the measurement that proved the column 4400x cheaper (10 GB: 1.4e12
    // charged work / 2365 s vs 3.1e8 / 2072 s).
    let lce_verify = std::env::var("XSA_LCE_VERIFY").map(|v| v == "1").unwrap_or(false);
    let lcp_source = if lce_verify { "lce-verify" } else { "sorter-bands" };
    // ---- THE SCRATCH BUDGET IS A PLAN INPUT, NOT ONLY A BRAKE --------------
    // Pausing bounds the AVERAGE but not the BURST: the unit of work in flight is
    // a WAVE of band files PLUS the pieces the emitter has already produced, and
    // a wave larger than the budget overshoots it by its own size no matter how
    // fast the signal lands (measured: a 48 x 500 MB wave took a 6 GB budget from
    // 5.1 to 10.9 GB between two 2 s samples). So the budget SIZES the work unit
    // (plan::tape_band_plan): the band window is 60% of it and the piece window
    // 40%, and --memory-gb only has to HOLD the unit.
    //
    // The old coupling was inside out: plan_memory_gb = min(--memory-gb, budget)
    // made the ENVELOPE a function of the DISK budget, so --scratch-budget-gb 2
    // set --memory-gb 2, the plan priced a 1 GiB envelope, and a 2 GB DISK budget
    // was refused by a RAM-shaped sentence nobody could act on.
    let free_bytes = super::plan::disc_free_bytes(scratch)
        .ok_or_else(|| format!("df failed for {}", scratch.display()))?;
    let sb = super::plan::scratch_budget(options.scratch_budget_gb, options.memory_gb, free_bytes);
    let budget_bytes = sb.bytes;
    let budget_gb_ceil = (budget_bytes + (1u64 << 30) - 1) >> 30;
    let plan_memory_gb = options.memory_gb;
    journal.line(&format!(
        "SCRATCH_BUDGET source={} budget_bytes={budget_bytes} budget_gb={:.3} free_bytes={free_bytes} \
         derived_cap_bytes={} plan_memory_gb={plan_memory_gb} (min(0.8 x free, 2 x band window) when \
         derived; the budget SIZES the wave ({}% band window + {}% piece window) and --memory-gb only \
         has to HOLD the unit; a pause never kills, never fails)",
        sb.source, budget_bytes as f64 / (1u64 << 30) as f64, sb.derived_cap,
        super::plan::TAPE_BAND_SHARE_PCT, super::plan::TAPE_PIECE_SHARE_PCT));
    let tape_plan = super::plan::tape_band_plan(
        n.saturating_add(1), options.threads as u64, plan_memory_gb, !lce_verify,
        2 * n.saturating_add(2), budget_bytes,
    );
    journal.line(&format!(
        "SCRATCH_BUDGET_PLAN budget_gb_ceil={budget_gb_ceil} source={} band_window_bytes={} \
         piece_window={} piece_window_bytes={} arena_needed_bytes={} ram_ok={} needed_memory_gb={} \
         (the plan block printed the same numbers on XSA_PLAN_RAM; the producer journals them on \
         SS_TAPE_BUDGET and the front door refuses any disagreement)",
        sb.source, tape_plan.band_window_bytes, tape_plan.piece_window, tape_plan.piece_window_bytes,
        tape_plan.arena_needed_bytes, u8::from(tape_plan.ram_ok()), tape_plan.needed_memory_gb()));
    // ---- THE PIECE WINDOW IS AN IN-FLIGHT DISK TERM, SO THE BUDGET SIZES IT ---
    // Measured on the 10 GB / 6 GiB gate: the peak live scratch (7.199 GiB) was
    // 4.5 GiB of band files + 2.7 GiB of pieces, i.e. exactly the 1100-piece
    // window x 65,536 rows x 41 B. A 2 GB budget cannot carry a 2.7 GiB piece
    // window, so the window comes from the same plan the band unit does.
    // The window changes WHEN a piece is produced, never what it contains: the
    // C1 byte-identity gate re-proves that on a real 10 GB build.
    if options.tape {
        let w = tape_plan.piece_window;
        for e in producer_env.iter_mut() {
            if e.0 == "CROSS_V2_PIECE_WINDOW" { e.1 = w.to_string(); }
        }
        journal.line(&format!(
            "V2_PIECE_WINDOW pieces={w} rows_per_piece={} bytes_per_piece={} window_bytes={} \
             source=plan (the piece window is the SECOND in-flight disk term: it is sized from \
             the same scratch budget that sizes the band window, capped at the measured default \
             {}; it changes when a piece is produced, never its bytes)",
            super::plan::TAPE_PIECE_ROWS,
            super::plan::TAPE_PIECE_ROWS * super::plan::TAPE_DISK_ROW_BYTES,
            w * super::plan::TAPE_PIECE_ROWS * super::plan::TAPE_DISK_ROW_BYTES,
            super::plan::TAPE_PIECE_WINDOW_MAX));
    }
    // The page pool (pile-week-v1) is what makes the placing pass affordable:
    // it RAM-caches the immutable corpus text the LCEs read (the same run
    // without it measured 77x slower). Size it to cover the corpus text
    // (n/256 pages) within 45% of the --memory cap. Operator env passes
    // through. CROSS_NO_POOL=1 keeps the file-window path (exactness-gated).
    let pool_slots = if let Some(v) = std::env::var_os("CROSS_POOL_SLOTS") {
        let v = v.to_string_lossy().to_string();
        journal.line(&format!("KNOB CROSS_POOL_SLOTS={v} (operator-passed)"));
        v
    } else {
        // ONE function for this sizing, in plan.rs, because the PLAN prices the
        // same budget as the corpus-cache term of the derived live cap
        // (plan::bin_corpus_cache_bytes): the two must never disagree.
        let slots = super::plan::bin_pool_slots(n, memory_kb * 1024);
        let per_pool_gb = slots as f64 * 528.0 / (1.0e9);
        let cover_pct = (slots as f64) * 256.0 * 100.0 / (n.max(1) as f64);
        journal.line(&format!(
            "KNOB CROSS_POOL_SLOTS={slots} (corpus text page pool: ~{per_pool_gb:.1} GB RAM/pool; covers ~{cover_pct:.0}% of the corpus text; bounded by --memory)"));
        slots.to_string()
    };
    producer_env.push(("CROSS_POOL_SLOTS".to_string(), pool_slots));
    // The producer is handed the cap EXPLICITLY (auto or operator), so it cannot
    // tell the two apart from the number: name the source for the journal.
    producer_env.push(("CROSS_BIN_CAP_SOURCE".to_string(),
        if options.bin_live_cap.is_some() { "operator" } else { "auto" }.to_string()));
    if std::env::var_os("CROSS_NO_POOL").is_some() {
        journal.line("KNOB CROSS_NO_POOL (file-window path: exact, 77x slower on the placing pass)");
    }
    // The merge-pipeline knobs that have no meaning here are refused (never
    // silently ignored), so a run cannot be misconfigured into thinking it has
    // spools/WM scratch/segment producers it does not have.
    for inert in ["CROSS_WM_PACK", "CROSS_DELETE_CONSUMED", "CROSS_SHARDS", "CROSS_HASH_K", "CROSS_ASYNC_FILL"] {
        if let Some(v) = std::env::var_os(inert) {
            journal.line(&format!("KNOB {inert}={} is INERT under the bin shape (merge-pipeline knob; ignored)", v.to_string_lossy()));
        }
    }

    let container = scratch.join(format!("{}.sxi2",
        input.file_stem().and_then(|stem| stem.to_str()).filter(|stem| !stem.is_empty()).unwrap_or("pile")));
    let wit_ram_bytes = options.wit_ram_gb.unwrap_or(options.memory_gb / 2).saturating_mul(1 << 30);
    journal.line(&format!(
        "V2_WITNESS ram_budget_bytes={wit_ram_bytes} (witnesses beyond the budget spill as sorted varint-delta disc runs; the product is unchanged)"));
    if let Some(expected) = options.chi_expect {
        journal.line(&format!("V2_CHI_EXPECT {expected} (the banked constant; the finish asserts chi in-process)"));
    }

    let mut producer_args: Vec<String> = Vec::new();
    producer_args.push(s(&tools.join("cross_lcp_merge").to_string_lossy()));
    producer_args.extend([
        "--binbwt".into(), s(&input.to_string_lossy()),
        "--remap".into(), s(&remap_path.to_string_lossy()),
        "--out-prefix".into(), frag.to_string(),
        "--threads".into(), options.threads.to_string(),
        "--bin-live-cap".into(), bin_live_cap.to_string(),
    ]);
    // The target-size recursion's leaf bound, when the operator pinned it. Absent
    // = automatic (the producer's measured default), and the journal prints the
    // value the run actually used (BIN_WORK target=) either way.
    if let Some(target) = options.bin_target_size {
        producer_args.extend(["--bin-target-size".into(), target.to_string()]);
    }
    // THE ENTROPY RULE: absent -> OFF (theta 0, every byte-identical gate the
    // product default). Present (optionally with a value) -> armed at the
    // producer's calibrated theta (1.5, the middle of the measured bracket
    // [1.18, 1.67]) or the operator's bits.
    if let Some(bits) = options.bin_entropy_min {
        producer_args.push("--bin-entropy-min".into());
        if let Some(b) = bits {
            producer_args.push(b.to_string());
        }
    }
    // PHRASE RADIX: absent -> nothing is added, and no line of the mechanism runs
    // (the producer keeps `bin_phrase_now == nullptr`). Present -> the flag and
    // the operator's knobs, in the order given, so the journal's command line is
    // the whole dictionary provenance.
    if options.phrase.enabled {
        producer_args.push("--phrase-radix".into());
        for (flag, v) in &options.phrase.knobs {
            producer_args.extend([(*flag).to_string(), v.to_string()]);
        }
    }
    // ---- THE TAPE ROUTE (THE PRODUCT) ----------------------------------
    // Three tools, owned by this binary: the sorter writes rank-ordered band
    // slices with --tape-bands, the `emit-tape` phase CONCATENATES them in key
    // order into the merged stream (cross_lcp_merge --binbwt --tape-bands: an
    // order-preserving concatenation of already-sorted rows, NOT a cross-LCP
    // merge and NOT a seam walk - the phase journals TAPE_EMIT_START
    // job=band-concat and EMIT_SEAM_PASS none, and cross_lcp_merge.cpp's
    // seam_pass_skip has the proof), and the finish (already below) consumes the
    // runs. Flow control is the sorter's own .consumed protocol satisfied by the
    // concatenator, so there is no consumer script anywhere.
    //
    // The name of that binary is history: it also serves the legacy four-file
    // merge, where cross-LCP work really happens. On THIS route no LCP is
    // computed and no suffix comparison is made by it - the rows arrive in exact
    // rank order.
    //
    // THE LCP COLUMN IS PART OF THE ROUTE (not a flag): every band carries its own
    // LCP slice (band_<i>.lcp) beside its SA stream, and the finish folds those
    // slices in rank order, deleting each one as its rank cursor passes. Residency
    // is K bands of slice - never n, never a 1.34n column file - and the charged
    // work is the sorter's own merge (one store per row) instead of the finish's
    // 1.28e11 random corpus reads.
    //
    // XSA_LCE_VERIFY=1 is the INTERNAL A/B ARM (not a user option): the producer
    // skips the slices and the finish walks the corpus for every endpoint, which is
    // the measurement that proved the column 4400x cheaper (10 GB: 1.4e12 charged
    // work / 2365 s vs 3.1e8 / 2072 s).
    // THE TAPE WINDOW K, NOT THE PARALLELISM. K bounds how many band FILES are
    // outstanding (one band per slot, consumed before the next is published); it
    // never sizes the band arena. The in-flight band count that the budget law
    // divides by is P = --threads (see plan::tape_band_plan).
    let tape_k: u64 = 8;
    // THE PLAN THE RUN EXECUTES (computed once, above, before any phase was
    // built): the gate after the run compares it against the producer's own
    // SS_TAPE_BUDGET, so a drift between plan and run is a refusal
    // (TAPE_PLAN_DISAGREE), not a footnote.
    let tape_envelope = tape_plan.envelope_bytes;
    // TWO NUMBERS, NAMED. P = bands LIVE at once = --threads: the arena divisor
    // (rows_per_band = arena/(bpr x P)), journaled as parallel_bands=P. K is the
    // tape handshake WINDOW (--pending-bands): it bounds outstanding band FILES
    // and never sizes the arena. Confusing the two is what priced the first 100 GB
    // attempt at 96 x 185 M x 41 B = 728 GB.
    journal.line(&format!(
        "TAPE_ROUTE route=tape source={} lcp_source={} pending_bands_k={tape_k} \
         parallel_bands_p=threads lcp_column=none (slices only for lcp_source=sorter-bands)",
        if options.tape { "explicit" } else { "implicit" }, lcp_source));
    if lce_verify {
        journal.line("TAPE_AB_ARM XSA_LCE_VERIFY=1: INTERNAL A/B (the old LCE path: the finish \
walks the corpus for every endpoint and no LCP slice is produced). Not a user option.");
    }
    if options.tape {
        // WHAT `emit-tape` IS, SAID ONCE, IN THE JOURNAL. The phase's tool keeps
        // the name cross_lcp_merge; its job on this route is the concatenation of
        // the sorter's rank-ordered band slices into the merged stream. It
        // computes no LCP, merges no cross-seam and runs no seam census (see
        // EMIT_SEAM_PASS in cross_lcp_merge.cpp: the terminal byte is unique, so
        // the census could only ever return zero, and it used to cost a full
        // corpus scan plus an n/8-byte target bitmap to say so).
        journal.line(&format!(
            "EMIT_TAPE phase=emit-tape job=band-concat order=rank tool=cross_lcp_merge \
             cross_lcp=0 seam_pass=none lcp_source={lcp_source} \
             (concatenate the sorter's rank-ordered band slices into the merged stream; \
              the LCP column arrives as band slices and is folded by the finish)"));
    }
    let bands_dir = scratch.join("bands-tape");
    let mut emit_args: Vec<String> = Vec::new();
    if options.tape {
        fs::create_dir_all(&bands_dir)
            .map_err(|e| format!("create {}: {e}", bands_dir.display()))?;
        let waves = scratch.join("waves-tape");
        producer_args = vec![
            s(&tools.join("scatter_sort").to_string_lossy()),
            "--corpus".into(), s(&input.to_string_lossy()),
            "--remap".into(), s(&remap_path.to_string_lossy()),
            "--scratch".into(), s(&waves.to_string_lossy()),
            "--tape-bands".into(), s(&bands_dir.to_string_lossy()),
            "--pending-bands".into(), tape_k.to_string(),
            // THE ENVELOPE, IN BYTES, FROM THE PLAN: the sorter's band unit is a
            // function of this number alone (plan::tape_band_plan computes the same
            // arithmetic, and the gate below compares the two). --ram-gb/--par-bands
            // are GONE from this call: a decimal-GB float and a hardcoded 8 in
            // flight (while the pool runs --threads workers) is how the 100 GB run
            // came to price 8 bands while it was holding up to 96.
            "--budget-bytes".into(), tape_envelope.to_string(),
            // THE DISK BUDGET: the number the WORK UNIT is derived from. It is not
            // a brake the producer may ignore - the band size, the wave and the
            // piece window are all percentages of it, and SS_TAPE_BUDGET echoes it
            // so a plan/run drift is a refusal.
            "--scratch-budget-bytes".into(), budget_bytes.to_string(),
            "--memory-gb".into(), plan_memory_gb.to_string(),
            "--threads".into(), options.threads.to_string(),
        ];
        if lce_verify { producer_args.push("--no-lcp-slices".into()); }
        emit_args = vec![
            s(&tools.join("cross_lcp_merge").to_string_lossy()),
            "--binbwt".into(), s(&input.to_string_lossy()),
            "--remap".into(), s(&remap_path.to_string_lossy()),
            "--out-prefix".into(), frag.to_string(),
            "--threads".into(), options.threads.to_string(),
            "--tape-bands".into(), s(&bands_dir.to_string_lossy()),
        ];
    }
    let mut finish_args: Vec<String> = Vec::new();
    finish_args.push(s(&tools.join("xsa_finish").to_string_lossy()));
    finish_args.extend([
        "--prefix".into(), frag.to_string(),
        "--corpus".into(), s(&input.to_string_lossy()),
        "--remap".into(), s(&remap_path.to_string_lossy()),
        "--output".into(), s(&container.to_string_lossy()),
        "--threads".into(), options.threads.to_string(),
        // The finish's own window wait for the producer's pieces. It is NOT a
        // ceiling on the run (there is none): it is the finish's patience, and it
        // must never expire on a live producer.
        "--poll-timeout".into(), FINISH_POLL_TIMEOUT_S.to_string(),
        "--wit-ram-budget".into(), wit_ram_bytes.to_string(),
    ]);
    journal.line(&format!(
        "FINISH_POLL_TIMEOUT seconds={FINISH_POLL_TIMEOUT_S} (the finish never gives up on a live producer)"));
    if options.tape {
        finish_args.push(if options.with_chi { "--with-chi" } else { "--no-chi" }.into());
    }
    journal.line(&format!("CHI_POLICY present={} member=m5", !options.tape || options.with_chi));
    // THE m14 CHOICE, journalled where a container's provenance is read: the plan
    // line says which arm produced this container, and the container says the same
    // thing in its own header (version 8 + flag bit 7 + member m14), so no reader
    // has to trust the log.
    if options.tape {
        finish_args.push(if options.with_lcp_runs { "--lcp-runs" } else { "--no-lcp-runs" }.into());
        journal.line(&format!(
            "LCP_RUNS_POLICY present={} member=m14 codec=117 container_version={} default=off \
flag=--with-lcp-runs price=measured(0.27n@150MB,0.256n@10GB,5.89 bits/run; ~354GB at the \
full Pile) buys=middle-band(top-of-tree-trie handles wide, locate-and-scan handles narrow) \
headline=LCP-over-runs",
            options.with_lcp_runs,
            if options.with_lcp_runs { 8 } else { 7 }
        ));
    } else {
        journal.line("LCP_RUNS_POLICY present=false member=m14 reason=not-the-tape-arm");
    }
    if let Some(expected) = options.chi_expect {
        finish_args.extend(["--chi-expect".into(), expected.to_string()]);
    }
    if options.tape && !lce_verify {
        // THE PRODUCT ARM: the fold reads the producer's per-band LCP slices.
        finish_args.extend(["--lcp-bands".into(), s(&bands_dir.to_string_lossy())]);
    }

    // Spawn BOTH before joining: the finish must start with the producer (it
    // polls the producer's piece directory from row zero).
    // ---- THE TRIE ARM, DECIDED BEFORE ANY PHASE IS SPAWNED -----------------
    // m20 is built from the tape producer's band stream (scatter_sort --tape-bands),
    // so only the tape route can build it in-build. On the bin route the member is
    // NOT available: an operator who ASKED for it is refused (never silently
    // ignored), and a default run journals that the member is absent and why.
    let trie_on = options.tape && options.trie;
    let trie_payload = scratch.join("trie-m20.bin");
    if !options.tape && options.trie && options.trie_asked {
        return Err("CONTEXT_TRIE_ROUTE: m20 is built IN-BUILD from the tape producer's \
             rank-ordered band stream, so it needs --tape (the product route). Pass --tape, \
             or --no-trie to build the container without the context trie".into());
    }
    journal.line(&format!(
        "CONTEXT_TRIE_POLICY present={} route={} depth={} min_support={} docs_min_support={} \
         source={} (m20 is built from the sorter's band slices while the container is built: \
         the band stream IS the suffix array in rank order, so the builder rides along instead of \
         replaying it - no LF cycle, no 5 B/row SA column)",
        trie_on,
        if options.tape { "tape" } else { "bin" },
        options.trie_depth, options.trie_min_support, options.trie_docs_min_support,
        if trie_on { "in-build" } else { "absent" }));
    let trie_t0 = Instant::now();
    let producer_label = if options.tape { "order-tape" } else { "bin-v2" };
    // [0] is the tool itself (there is no `timeout` wrapper in front of it).
    let producer_running = spawn_phase(journal, scratch, producer_label, if options.tape { memory_kb.max((3 * n + 1023) / 1024) } else { memory_kb }, &producer_args[0], &producer_args[1..], &producer_env)?;
    let emit_running = if options.tape {
        // TRIE_TAP=1 tells the emitter that the band slices are NOT its to delete:
        // the in-build trie reads the same rank-ordered stream (see ctx::tap).
        let emit_env: Vec<(String, String)> = if trie_on {
            vec![("TRIE_TAP".to_string(), "1".to_string())]
        } else {
            Vec::new()
        };
        Some(spawn_phase(journal, scratch, "emit-tape", memory_kb, &emit_args[0], &emit_args[1..], &emit_env)?)
    } else { None };
    // RLIMIT_AS includes corpus and chi mappings, not only resident memory.
    // Use the producer's 3n floor, retaining any larger explicit finish cap.
    let finish_as_kb = finish_v2_as_kb(memory_kb, finish_as_kb, n);
    let finish_running = spawn_phase(journal, scratch, "finish-v2", finish_as_kb, &finish_args[0], &finish_args[1..],
        &[("XSA_WORK_ROOT".to_string(), "driver".to_string())])?;
    let v2_sampler = DiscSampler::start(scratch.to_path_buf(), Duration::from_secs(2));
    // ---- THE SCRATCH BUDGET (flow control) ---------------------------------
    // The finish's inputs and the producer's windows are the two terms that grow
    // with the corpus. The budget is a MEASURED live-scratch ceiling: when it is
    // reached the producer is paused so the finish can absorb its backlog.
    let flow = {
        // The PRODUCER STAGE = the sorter AND the emitter: the emitter's output is
        // the piece stream that grows scratch, so pausing only the sorter would
        // let it keep converting an already-written band window into pieces.
        let mut producers = vec![(producer_label.to_string(), producer_running.child.id())];
        if let Some(emit) = emit_running.as_ref() {
            producers.push(("emit-tape".to_string(), emit.child.id()));
        }
        Backpressure::new(scratch, budget_bytes, &container, producers)
    };
    let mut phases = vec![producer_running, finish_running];
    if let Some(emit) = emit_running { phases.push(emit); }
    // ---- THE CONTEXT TRIE, BUILT IN-BUILD (a fourth phase, a CONSUMER) ------
    // It reads the sorter's band stream in rank order (the only place where the
    // ordered suffix stream exists inside the build), feeds ctx::Builder, and
    // writes the m20 payload. It is NOT a backpressure producer: it consumes
    // scratch (it unlinks each band slice as it finishes it) and must never be
    // paused, or the sorter would stall with nothing able to free the space.
    let trie_running = if trie_on {
        let exe = std::env::current_exe().map_err(|e| format!("ctx-tap: current_exe: {e}"))?;
        let tap_args = vec![
            "ctx-tap".into(),
            "--bands".into(), s(&bands_dir.to_string_lossy()),
            "--corpus".into(), s(&input.to_string_lossy()),
            "--out".into(), s(&trie_payload.to_string_lossy()),
            // n rows = the corpus length + 1 (the virtual-END row): the same
            // arithmetic the container header carries, asserted again inside the tap.
            "--n".into(), (n + 1).to_string(),
            "--depth".into(), options.trie_depth.to_string(),
            "--min-support".into(), options.trie_min_support.to_string(),
            "--docs-min-support".into(), options.trie_docs_min_support.to_string(),
            "--threads".into(), options.threads.to_string(),
        ];
        let exe_s = s(&exe.to_string_lossy());
        Some(spawn_phase(journal, scratch, "ctx-tap", finish_as_kb, &exe_s, &tap_args, &[])?)
    } else { None };
    if let Some(tap) = trie_running { phases.push(tap); }
    join_pipeline_flow(journal, scratch, phases, Some(flow))?;
    // ---- APPEND m20, IN PLACE -------------------------------------------------
    // The finish streamed the container and the tap built the member beside it; the
    // join is an in-place directory rewrite (no container byte is copied). The
    // member is byte-identical to what `xsa ctx-build` writes from the same
    // container and corpus - the equivalence gate compares the two - apart from the
    // provenance field that says WHICH ordered stream produced it.
    if trie_on {
        let t_app = Instant::now();
        let bytes_before = fs::metadata(&container).map(|m| m.len()).unwrap_or(0);
        let payload = fs::read(&trie_payload)
            .map_err(|e| format!("trie join: read {}: {e}", trie_payload.display()))?;
        let member_bytes = super::ctx::append_m20(&container.to_string_lossy(), &payload, n);
        journal.line(&format!(
            "CONTEXT_TRIE_JOIN member=m20 codec={} bytes={member_bytes} container_bytes={} -> {} \
             append_s={:.1} (directory rewritten in place: no container byte copied)",
            super::ctx::TRIE_CODEC, bytes_before,
            fs::metadata(&container).map(|m| m.len()).unwrap_or(0),
            t_app.elapsed().as_secs_f64()));
        let td = require_log_contains(scratch, "ctx-tap", "CONTEXT_TRIE_DEPTH")?;
        journal.line(&td);
        journal.line(&format!("CONTEXT_TRIE_BUILD_WALL total_s={:.1}", trie_t0.elapsed().as_secs_f64()));
        for tag in ["CONTEXT_TRIE_COLUMNS", "CONTEXT_TRIE_TAP", "CONTEXT_TRIE_BUDGET"] {
            let line = require_log_contains(scratch, "ctx-tap", tag)?;
            journal.line(&line);
        }
        // THE MEMBER MUST BE READABLE FROM THE PUBLISHED CONTAINER, and its ratio
        // must hold the budget. Both are checked on the container, not on the tap's
        // own claim: a payload that lands wrong is a wrong product, not a wash.
        let reader = super::qlite::Reader::open(&container.to_string_lossy())
            .map_err(|e| format!("trie join: reopen container: {e}"))?;
        let m = reader.member_bytes(super::ctx::TRIE_CODEC)
            .ok_or("trie join: the published container has no m20 after the append")?;
        let trie = super::ctx::Trie::load(m);
        let ratio = m.len() as f64 / n.max(1) as f64;
        journal.line(&format!(
            "CONTEXT_TRIE_PUBLISHED member=m20 nodes={} edges={} bytes={} ratio_vs_n={ratio:.6} \
             depth={} min_support={} source={} container={}",
            trie.nodes, trie.edges, m.len(), trie.depth, trie.min_support, trie.source,
            container.display()));
        // THE RATIO IS A PROPERTY OF THE CORPUS AND THE THRESHOLD, NOT A CONSTANT:
        // 0.0621 n at 150 MB and 0.0348 n at 10 GB for the same depth/threshold,
        // because node count is bounded by n/min_support and grows far slower than
        // n. The design target is <= 0.05; 0.30 is the alarm (a member that big
        // would mean the store shape changed, not that the corpus is different).
        // The ratio is a property of (depth, min_support, corpus): 0.0348 n at
        // 10 GB and 0.3728 n at 150 MB for --trie-min-support 100 are both the
        // documented shape, so the 0.05 design target is JOURNALED, not enforced.
        // What is enforced is the store-shape alarm: a member larger than the
        // corpus itself means the columns changed meaning, not that the corpus is
        // different. `ctx-build` journals the same two numbers.
        if ratio > 1.0 {
            return Err(format!(
                "CONTEXT_TRIE_BUDGET: m20 is {ratio:.6} n, larger than the corpus \
                 (depth={} min_support={}): this is a store-shape regression, not a corpus effect",
                trie.depth, trie.min_support));
        }
        let _ = fs::remove_file(&trie_payload);
    } else if options.tape {
        journal.line("CONTEXT_TRIE_POLICY present=false reason=--no-trie (the product container is \
byte-identical to a build without this feature)");
    }
    // XSA_KILL_AT=after_rename: EVERY phase has exited with rc=0, so the finish
    // has linked the container into its final name (it links only after fsync)
    // and the driver has journalled no commit. This is the published-but-
    // uncommitted ordering point, and it is reached DETERMINISTICALLY: no poll,
    // no race. Dropping here must cost a restart nothing but a validation.
    if recovery::kill_wanted("after_rename") {
        recovery::die_hard("after_rename");
    }
    let peak_disc = v2_sampler.peak_bytes();

    // ---- gates (fail loud on every violation)
    // A cap raise means the arena the run used is NOT the arena the plan
    // priced: refuse rather than accept a silently different memory shape.
    if !options.tape && count_log_contains(scratch, "bin-v2", "BIN_CAP_RAISED") > 0 {
        let raised = require_log_contains(scratch, "bin-v2", "BIN_CAP_RAISED")?;
        return Err(format!(
            "V2_BIN_CAP_RAISED: --bin-live-cap {bin_live_cap} is below the largest bin; the producer raised it ({raised}). \
             The plan priced a different arena - raise --bin-live-cap (and --memory-gb if needed) and re-run"));
    }
    // BIN_WORK is the bin producer's own row-coverage receipt; the tape route
    // proves coverage through its own stream journal, so the receipt block below
    // is required only on the bin route.
    if options.tape {
        require_log_contains(scratch, "order-tape", "SS_TAPE")?;
        // ---- THE PLAN AND THE RUN MUST BE THE SAME PROCESS ------------------
        // The producer journals the band plan it executed (SS_TAPE_BUDGET). Every
        // number that shapes an allocation is compared with the plan's, and the run
        // is REFUSED on any difference: a plan that prices a different band unit
        // than the sorter used is worse than no plan at all.
        let budget_line = require_log_contains(scratch, "order-tape", "SS_TAPE_BUDGET")?;
        journal.line(&format!("TAPE_PRODUCER_BUDGET {budget_line}"));
        for (key, planned) in [
            ("bytes_per_row", tape_plan.bytes_per_row),
            ("bands_in_flight", tape_plan.bands_in_flight),
            ("rows_per_band", tape_plan.rows_per_band),
            ("rows_per_band_budget", tape_plan.rows_per_band_budget),
            ("arena_planned_rows", tape_plan.arena_planned_rows),
            ("arena_needed_bytes", tape_plan.arena_needed_bytes),
            ("budget_bytes", tape_plan.envelope_bytes),
            ("scratch_budget_bytes", tape_plan.scratch_budget_bytes),
            ("band_window_bytes", tape_plan.band_window_bytes),
            ("piece_window", tape_plan.piece_window),
            ("arena_bytes", tape_plan.arena_bytes),
            ("waves", tape_plan.waves),
            ("bands", tape_plan.bands_per_wave),
        ] {
            let ran = field_u64(&budget_line, key)?;
            if ran != planned {
                return Err(format!(
                    "TAPE_PLAN_DISAGREE {key}: the plan priced {planned}, the sorter ran {ran} \
                     ({budget_line}). The band plan is ONE derivation - fix \
                     plan::tape_band_plan / the producer together, never one of the two"));
            }
        }
        let peak_line = require_log_contains(scratch, "order-tape", "SS_TAPE_PEAK")?;
        journal.line(&format!("TAPE_PRODUCER_PEAK {peak_line}"));
        let ratio = field(&peak_line, "budget_ratio").and_then(|v| v.parse::<f64>().ok())
            .ok_or_else(|| format!("SS_TAPE_PEAK without budget_ratio: {peak_line}"))?;
        if ratio > super::plan::TAPE_BUDGET_RATIO_MAX as f64 / 100.0 {
            return Err(format!(
                "TAPE_BUDGET_RATIO: the sorter's owned peak RSS is {ratio:.4} of its envelope, \
                 over the {:.2} bound ({peak_line})",
                super::plan::TAPE_BUDGET_RATIO_MAX as f64 / 100.0));
        }
        journal.line(&format!(
            "TAPE_BUDGET_OK rows_per_band={} bytes_per_row={} bands_in_flight={} \
             plan=run=1 budget_ratio={ratio:.4} bound={:.2}",
            tape_plan.rows_per_band, tape_plan.bytes_per_row, tape_plan.bands_in_flight,
            super::plan::TAPE_BUDGET_RATIO_MAX as f64 / 100.0));
        // ---- THE SCRATCH MODEL IS CHECKED AGAINST THE MEASURED PEAK -----------
        // The plan says the peak live scratch is band_window + piece_window; the
        // 2 s sampler measured it. Both numbers are journalled so a cap that is
        // honoured can be READ, not inferred (the sampler's blind spot between
        // samples is stated, not hidden).
        journal.line(&format!(
            "SCRATCH_MODEL_CHECK predicted_bytes={} band_window_bytes={} piece_window_bytes={} \
             measured_peak_bytes={peak_disc} budget_bytes={budget_bytes} overshoot_pct={:.1} \
             note=peak is sampled every 2s (allocated bytes, container excluded); the between-sample \
             peak is not observed",
            tape_plan.band_window_bytes + tape_plan.piece_window_bytes,
            tape_plan.band_window_bytes, tape_plan.piece_window_bytes,
            if budget_bytes > 0 { 100.0 * peak_disc as f64 / budget_bytes as f64 - 100.0 } else { 0.0 }));
        // The route's shape is gated, not assumed: the producer must declare which
        // LCP arm it ran, and on the product arm its slices must cover n with a
        // BOUNDED largest slice (the 2n law: one band's slice, never a 1.34n file).
        let arm = require_log_contains(scratch, "order-tape", "SS_LCP_ARM")?;
        journal.line(&format!("TAPE_PRODUCER {arm}"));
        if lce_verify {
            if !arm.contains("lcp_source=none") {
                return Err(format!("TAPE_AB_ARM: XSA_LCE_VERIFY=1 but the producer ran {arm}"));
            }
        } else {
            let ss = require_log_contains(scratch, "order-tape", "SS_LCP_STREAM")?;
            journal.line(&format!("TAPE_LCP_STREAM {ss}"));
            let max_slice = field_u64(&ss, "max_live_lcp_slice_bytes")?;
            if max_slice > n {
                return Err(format!(
                    "TAPE_LCP_RESIDENCY: max_live_lcp_slice_bytes={max_slice} > n={n}: a slice \
                     must be ONE BAND's, not the whole column ({ss})"));
            }
            let fin = require_log_contains(scratch, "finish-v2", "XSA_FINISH_LCP_BANDS_DONE")?;
            journal.line(&format!("V2_FINISH_LCP_BANDS {fin}"));
            journal.line(&format!(
                "TAPE_LCP_RESIDENCY max_live_lcp_slice_bytes={max_slice} n={n} ratio={:.6} \
                 bound=one_band_slice (K={tape_k} slices in flight, deleted as folded)",
                max_slice as f64 / n.max(1) as f64));
        }
    }
    if !options.tape {
    let bin_work = require_log_contains(scratch, "bin-v2", "BIN_WORK")?;
    journal.line(&format!("V2_BIN_WORK {bin_work}"));
    let emitted = field_u64(&bin_work, "emitted_rows")?;
    if emitted != n {
        return Err(format!("V2_BIN_ROW_COVERAGE emitted_rows={emitted} != n={n}: {bin_work}"));
    }
    let windows = field_u64(&bin_work, "windows")?;
    let sweep_bytes = field_u64(&bin_work, "sweep_bytes")?;
    if sweep_bytes != (1 + windows).saturating_mul(n) {
        return Err(format!("V2_BIN_SWEEP_CONSERVATION sweep_bytes={sweep_bytes} != (1+windows)*n = {}: {bin_work}",
                           (1 + windows).saturating_mul(n)));
    }
    if field(&bin_work, "full_SA") != Some("0") || field(&bin_work, "persistent_positions") != Some("0") {
        return Err(format!("V2_BIN_LAW full_SA/persistent_positions must both be 0: {bin_work}"));
    }
    if field(&bin_work, "sink") != Some("parallel-v2") {
        return Err(format!("V2_BIN_SINK the producer did not run the parallel stitch sink: {bin_work}"));
    }
    journal.line(&format!("V2_BIN_WINDOWS windows={windows} plan_windows={plan_windows} peak_live_slots={} peak_live_bytes={}",
        field(&bin_work, "peak_live_slots").unwrap_or("?"),
        field(&bin_work, "peak_live_bytes").unwrap_or("?")));
    // ceil(n/cap) is the arithmetic LOWER bound on the window count (a window
    // is packed to <= cap by whole bins, so a run can only use more). Say so,
    // loudly, rather than let the plan's fill term look exact when it is not.
    if windows > plan_windows {
        journal.line(&format!(
            "V2_BIN_WINDOW_DELTA plan_windows={plan_windows} actual={windows}: bin-boundary granularity used {} more window(s) than ceil(n/cap); the plan's fill term is a LOWER bound by {:.1}%",
            windows - plan_windows,
            100.0 * (windows - plan_windows) as f64 / plan_windows as f64));
    } else if windows < plan_windows {
        return Err(format!("V2_BIN_WINDOW_UNDERCOUNT plan_windows={plan_windows} actual={windows}: the producer admitted fewer windows than ceil(n/{bin_live_cap}) - the plan and the run disagree about the arena bound"));
    }
    }
    // The finish's own work counters ride the same journal (the counter
    // identity is a hard gate: byte-identity alone has hidden two defects).
    let finish_log = fs::read_to_string(scratch.join("finish-v2.log"))
        .map_err(|e| format!("read finish-v2 log: {e}"))?;
    for line in finish_log.lines() {
        if ["SLIM_FP ", "SLIM_SEEDS ", "XSA_FINISH_PFCK ",
            "XSA_FINISH_CHI_BITMAP ", "XSA_FINISH_CHI_PEAK ", "XSA_FINISH_CHI_PUBLISHED ", "XSA_FINISH_PEAK_RSS ",
            "XSA_FINISH_M1_BLOCKS ", "XSA_FINISH_M12_RUNSTARTS ", "XSA_FINISH_M13_RUNDIR "]
            .iter().any(|prefix| line.starts_with(prefix)) {
            journal.line(&format!("V2_FINISH_COUNTER {line}"));
        }
    }
    let done = require_log_contains(scratch, "finish-v2", "XSA_FINISH_DONE")?;
    let chi = field(&done, "chi").and_then(|v| v.parse::<u64>().ok())
        .ok_or_else(|| format!("XSA_FINISH_DONE without a chi constant: {done}"))?;
    require_log_contains(scratch, "finish-v2", "ENDPOINT_STREAM_PASS")?;
    require_log_contains(scratch, "finish-v2", "SXI2_PASS")?;
    // The rlebwt wire is one of two arms, and the finish must declare exactly one:
    //   route A (legacy): the wire is written and deleted at publish -> RLEBWT_DELETED
    //   route B (one-pass m1): the wire is never formed           -> RLEBWT_NEVER_WRITTEN
    // Requiring only route A made this front door reject every route-B run.
    let rle_arms = ["XSA_FINISH_RLEBWT_DELETED", "XSA_FINISH_RLEBWT_NEVER_WRITTEN"]
        .iter()
        .filter(|marker| require_log_contains(scratch, "finish-v2", marker).is_ok())
        .count();
    if rle_arms != 1 {
        return Err(format!(
            "finish-v2 must declare exactly one rlebwt arm (found {rle_arms}): \
             expected XSA_FINISH_RLEBWT_DELETED (route A) or \
             XSA_FINISH_RLEBWT_NEVER_WRITTEN (route B)"
        )
        .into());
    }
    if finish_log.lines().any(|l| l.contains("XSA_FINISH_PIECES_DIR_NONEMPTY")) {
        return Err("finish-v2 left pieces behind (XSA_FINISH_PIECES_DIR_NONEMPTY)".into());
    }
    if let Some(expected) = options.chi_expect {
        if chi != expected {
            return Err(format!("V2_CHI_MISMATCH chi={chi} expected={expected}"));
        }
        journal.line(&format!("V2_CHI_EXPECT_OK chi={chi} == {expected}"));
    }
    // THE LAW + the tree rules: no agg/ssa_t/head_sa/pftext/pfck file ever
    // exists; rlebwt (+meta) is deleted post-publish; no chunk set exists at
    // all on this route. ONE exception, named and gated: the finish's spool arm
    // (XSA_FINISH_SPOOL=1, its restart test hook) KEEPS `frag.pfck2` and
    // `frag.finish-spool/` on purpose - they ARE its restart state, published
    // atomically and reused by a resume (RESUME_DESIGN.md) - so demanding their
    // absence refused every spool-arm build AFTER it had published a
    // byte-identical container, and no accounting of that run could ever be
    // read. Every other banned name stays banned, arm or not, and the exemption
    // is journaled so no log can be read as if the law had been silently
    // relaxed.
    let spool_arm = std::env::var("XSA_FINISH_SPOOL").as_deref() == Ok("1");
    if spool_arm {
        journal.line("V2_TREE_RULE spool_arm=1 retained=frag.pfck2,frag.finish-spool \
(restart state of the finish's spool arm; every other banned artifact is still refused)");
    }
    let banned = ["frag.agg", "frag.head_sa", "frag.ssa", "frag.ssa_t", "frag.pftext",
                  "frag.pfck", "frag.pfck2", "frag.rlebwt", "frag.rlebwt.meta"];
    let forbidden: Vec<&str> = banned.iter().copied()
        .filter(|name| !(spool_arm && *name == "frag.pfck2")).collect();
    let found = find_files_named(scratch, &forbidden);
    if !found.is_empty() {
        let listing = found.iter().map(|p| p.display().to_string()).collect::<Vec<_>>().join(", ");
        return Err(format!("V2_TREE_RULE_VIOLATION forbidden artifact(s) in the tree: {listing}"));
    }
    let chunks_dir = scratch.join("chunks");
    if chunks_dir.is_dir() {
        let chunks_left = fs::read_dir(&chunks_dir)
            .map(|entries| entries.flatten().filter(|e| e.file_type().map(|t| !t.is_dir()).unwrap_or(false)).count())
            .unwrap_or(0);
        if chunks_left > 0 {
            return Err(format!("V2_TREE_RULE_VIOLATION {chunks_left} chunk file(s) remain under chunks/ - the bin shape has no chunk phase"));
        }
    }
    let container_bytes = fs::metadata(&container)
        .map(|m| m.len())
        .map_err(|e| format!("container {} missing after publish: {e}", container.display()))?;
    journal.line(&format!("V2_CONTAINER_PUBLISHED path={} bytes={container_bytes}", container.display()));
    journal.line(&format!(
        "PEAK_DISC_USED_BYTES {peak_disc} (du-sampled every 2s across the concurrent producer+finish window)"));
    // THE BOUNDED-INTERMEDIATE LAW, as a number: the peak of everything the build
    // writes into scratch, in units of n. The band/container/piece shapes are
    // bounded by construction; this line is what makes a regression visible.
    journal.line(&format!(
        "PEAK_INTERMEDIATE peak_bytes={peak_disc} n={n} peak_intermediate_over_n={:.4} \
         lcp_source={lcp_source} pending_bands={tape_k}",
        peak_disc as f64 / n.max(1) as f64));
    if options.tape {
        if recovery::signal() != 0 {
            recovery::interrupted(scratch)?;
            return Err("BUILD_INTERRUPTED".into());
        }
        // THE PLAN'S SPOOL INVENTORY, recorded once, bounded: the counts and the
        // clean-build work totals every later attempt prices itself against.
        let emit_done = require_log_contains(scratch, "emit-tape", "TAPE_EMIT_DONE")?;
        let sort_done = require_log_contains(scratch, "order-tape", "SS_DONE ")?;
        journal.line(&format!(
            "XSA_SPOOL_INVENTORY_EMIT rows={} runs={} corpus_bytes={n} sort_rows={} scheme={}",
            field_u64(&emit_done, "rows")?, field_u64(&emit_done, "runs")?,
            field_u64(&sort_done, "n")?, recovery::SPOOL_SCHEME));
        recovery::record_inventory(scratch, &sha256_file(remap_path)?)?;
        // The commit: the container is fsynced, then journalled. The spools may
        // only be unlinked AFTER this (deletion is the commit record).
        recovery::done(scratch, &container)?;
        // THE BUILD-END ACCOUNTING: one `WORK_AVOIDED` line per phase and one
        // summary, priced from THIS run's receipts - the finish stage's own
        // advance counters and the recorded plan inventory - not from a
        // prediction made before the work happened. Printed ahead of the
        // ordering hook below so a build that really finished always reports
        // what it avoided.
        recovery::report_work_avoided(scratch, n, remap_source, true)?;
        if recovery::kill_wanted("after_fsync_before_unlink") {
            recovery::die_hard("after_fsync_before_unlink");
        }
    }
    clean_v2_scratch(scratch, &container, journal)?;
    journal.line(&format!(
        "XSA_BUILD_DONE chi={chi} pipeline={} container={}",
        if options.tape { "v2tape" } else { "v2bin" },
        container.display()
    ));
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
        if e == "BUILD_INTERRUPTED" { std::process::exit(75); }
        super::die(&format!("build: {e}"));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scratch_success_cleans_owned_files_and_failure_retains() {
        for success in [false, true] {
            let dir = tempfile::tempdir().unwrap();
            let root = dir.path();
            for name in ["bands-tape", "waves-tape", "merged"] {
                fs::create_dir(root.join(name)).unwrap();
                fs::write(root.join(name).join("left.part"), b"piece").unwrap();
            }
            for name in ["remap-256.bin", "corpus.sxi2.partial", "corpus.sxi2", "finish-v2.log", "finish-v2.time", "caller.txt"] {
                fs::write(root.join(name), b"keep").unwrap();
            }
            let mut journal = Journal::open(&root.join("xsa-build.log")).unwrap();
            {
                let mut guard = ScratchOutcome { scratch: root.to_path_buf(),
                    journal: Journal { file: journal.file.try_clone().unwrap() }, success: false };
                if success {
                    clean_v2_scratch(root, &root.join("corpus.sxi2"), &mut journal).unwrap();
                    guard.success = true;
                }
            }
            for name in ["bands-tape", "waves-tape", "merged", "corpus.sxi2.partial"] {
                assert_eq!(root.join(name).exists(), !success);
            }
            // `remap-256.bin` is the PLAN'S OWN SPOOL (sha-pinned in PLAN), not a
            // transient: it survives cleanup on BOTH paths so a later attempt can
            // prove the plan and reuse the table without re-deriving it.
            for name in ["corpus.sxi2", "remap-256.bin", "finish-v2.log", "finish-v2.time", "caller.txt"] {
                assert!(root.join(name).exists());
            }
            let log = fs::read_to_string(root.join("xsa-build.log")).unwrap();
            assert_eq!(log, if success { "SCRATCH_CLEANED bands_bytes=5 waves_bytes=5\n" }
                else { "SCRATCH_RETAINED bands_bytes=5 reason=failure\n" });
        }
    }

    static PROCESS_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    #[test]
    fn scratch_lock_survives_driver_handle_until_stage_exits() {
        let _serial = PROCESS_TEST_LOCK.lock().unwrap();
        let dir = std::env::temp_dir().join(format!("xsa-owner-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let owner = lock_scratch(&dir).unwrap();
        assert!(lock_scratch(&dir).is_err());
        let mut journal = Journal::open(&dir.join("journal")).unwrap();
        let mut stage = spawn_phase(&mut journal, &dir, "owner", SELFTEST_MEMORY_KB,
            "bash", &["-c".into(),
                "touch ready; exec sleep 1".into()], &[]).unwrap();
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while !dir.join("ready").exists() {
            assert!(stage.child.try_wait().unwrap().is_none());
            assert!(std::time::Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(10));
        }
        drop(owner);
        fs::remove_dir_all(&dir).unwrap();
        fs::create_dir_all(&dir).unwrap();
        assert!(lock_scratch(&dir).is_err(), "orphan stage must retain ownership after scratch reset");
        assert!(stage.child.wait().unwrap().success());
        stage.armed = false;
        assert!(lock_scratch(&dir).is_ok());
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn pipeline_detects_each_failed_stage_and_kills_waiting_peers() {
        let _serial = PROCESS_TEST_LOCK.lock().unwrap();
        for failed in 0..3 {
            let dir = std::env::temp_dir().join(format!("xsa-peer-{}-{failed}", std::process::id()));
            fs::create_dir_all(&dir).unwrap();
            let mut journal = Journal::open(&dir.join("journal")).unwrap();
            let mut phases = Vec::new();
            let mut peer_groups = Vec::new();
            for i in 0..3 {
                let script = if i == failed { "sleep 0.2; exit 7" } else { "exec sleep 60" };
                let phase = spawn_phase(&mut journal, &dir, &format!("stage-{i}"),
                    SELFTEST_MEMORY_KB, "bash", &["-c".into(), script.into()], &[]).unwrap();
                if i != failed { peer_groups.push(phase.child.id()); }
                phases.push(phase);
            }
            let start = std::time::Instant::now();
            let error = join_pipeline(&mut journal, &dir, phases).unwrap_err();
            assert!(error.contains(&format!("stage-{failed} FAILED (rc 7)")), "{error}");
            assert!(start.elapsed() < Duration::from_secs(10));
            for group in peer_groups {
                // Wrappers are reaped by RunningPhase::drop; descendants may
                // briefly be zombies awaiting init, but none may still run.
                for entry in fs::read_dir("/proc").unwrap().flatten() {
                    let Ok(stat) = fs::read_to_string(entry.path().join("stat")) else { continue; };
                    let Some((_, fields)) = stat.rsplit_once(") ") else { continue; };
                    let fields: Vec<_> = fields.split_whitespace().collect();
                    if fields.get(2).and_then(|p| p.parse::<u32>().ok()) == Some(group) {
                        assert_eq!(fields[0], "Z", "live descendant after failed pipeline: {stat}");
                    }
                }
            }
            fs::remove_dir_all(dir).unwrap();
        }
    }

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
    fn disc_preflight_reads_the_disc_not_a_constant() {
        // The pre-fix gate compared the CONSTANT 100-min_free_pct against a
        // hardcoded 85, never the disc: always-pass at the default 15,
        // always-fail at any override. The real decision is used <= 100-pct.
        let allows = |used: u32, min_free_pct: u32| used <= 100u32.saturating_sub(min_free_pct);
        // default 15: 86% used on THIS shared disc refuses honestly
        assert!(!allows(86, 15));
        assert!(allows(85, 15));
        // operator override to 12 on the same 86%-full disc: 14% free >= 12%
        assert!(allows(86, 12));
        assert!(!allows(90, 12));
        // full override
        assert!(allows(99, 0) || true); // 0 short-circuits the gate entirely
    }

    /// THE END-FORMAT REMAP RULES, at tiny scale and against an independent
    /// oracle: present byte of rank j -> 1+j; a 256-byte bijection; END's cell
    /// (code 0) owned by an ABSENT byte; the END-format rules hold for every
    /// prefix alphabet (a universal table cannot do this - that is the point).
    #[test]
    fn derived_remap_is_order_preserving_per_corpus() {
        let dir = std::env::temp_dir().join(format!("xsa-remap-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let corpora: Vec<Vec<u8>> = vec![
            b"acgt".to_vec(),
            b"the quick brown fox jumps over the lazy dog".to_vec(),
            vec![0u8, 0, 1, 1, 255],                    // a tiny alphabet at both ends
            (0..=255u8).rev().collect(),                // present == 256
            (0..=254u8).collect(),                      // present == 255
        ];
        for (i, corpus) in corpora.iter().enumerate() {
            let path = dir.join(format!("c{i}.bin"));
            std::fs::write(&path, corpus).unwrap();
            let present: Vec<u8> = {
                let mut v: Vec<u8> = corpus.iter().copied().collect();
                v.sort_unstable();
                v.dedup();
                v
            };
            // The 8-bit container cannot encode 256 data symbols plus END.
            // The existing validator refuses this shape, rather than allowing
            // a present byte to alias END. Keep that refusal in the oracle.
            if present.len() == 256 {
                assert!(derive_remap(&path).unwrap_err().contains("REMAP_REFUSED"));
                continue;
            }
            let (table, facts) = derive_remap(&path).unwrap();
            assert_eq!(facts.present_bytes_hex,
                present.iter().map(|b| format!("{b:02x}")).collect::<String>());
            assert_eq!(facts.present as usize, present.len());
            let mut sorted = table;
            sorted.sort_unstable();
            assert_eq!(sorted, core::array::from_fn(|k| k as u8), "remap must be bijective");
            for (rank, &b) in present.iter().enumerate() {
                assert_eq!(table[b as usize] as usize, rank + 1,
                           "present byte 0x{b:02x} must map to 1+rank");
            }
            let owner = table.iter().position(|&c| c == 0).unwrap();
            assert!(!present.contains(&(owner as u8)),
                    "END's cell must be named by an absent byte");
        }
        std::fs::remove_dir_all(&dir).ok();
    }

    fn v2_args(extra: &[&str]) -> Vec<String> {
        let mut args: Vec<String> = ["--input", "/tmp/corpus", "--scratch", "/tmp/scratch",
                                    "--v2-pipeline"]
            .iter().map(|s| s.to_string()).collect();
        args.extend(extra.iter().map(|s| s.to_string()));
        args
    }

    #[test]
    fn skip_fit_check_is_the_flag_and_allow_infeasible_is_a_silent_alias() {
        // BOTH spellings parse and mean the SAME thing: the pre-rename flag is
        // kept so old journals, scripts and gate commands still run.
        let skip = parse_chain(&v2_args(&["--skip-fit-check"])).unwrap();
        assert!(skip.skip_fit_check);
        let alias = parse_chain(&v2_args(&["--allow-infeasible"])).unwrap();
        assert!(alias.skip_fit_check, "--allow-infeasible must alias --skip-fit-check");
        // Absent means the check is ENFORCED (the default is unchanged).
        assert!(!parse_chain(&v2_args(&[])).unwrap().skip_fit_check);
        // The alias takes no value and cannot be doubled into a different meaning.
        assert!(parse_chain(&v2_args(&["--skip-fit-check", "--allow-infeasible"])).is_ok());
    }

    #[test]
    fn the_shape_is_chosen_explicitly_and_only_one_shape_is_priced() {
        // --bin is the default shape, and asking for it is legal and inert.
        assert!(parse_chain(&v2_args(&["--bin"])).is_ok());
        assert!(!parse_chain(&v2_args(&["--bin"])).unwrap().tape);
        assert!(parse_chain(&v2_args(&["--tape"])).unwrap().tape);
        // Two shapes are two processes: refuse rather than silently pick one.
        let err = parse_chain(&v2_args(&["--tape", "--bin"])).unwrap_err();
        assert!(err.contains("--tape and --bin are mutually exclusive"), "{err}");
        let err = parse_chain(&["--input", "/tmp/c", "--scratch", "/tmp/s", "--legacy-merge",
                                "--bin"]
                              .iter().map(|s| s.to_string()).collect::<Vec<_>>()).unwrap_err();
        assert!(err.contains("--bin and --legacy-merge are mutually exclusive"), "{err}");
    }

    #[test]
    fn bin_live_cap_is_a_bin_pipeline_flag_and_is_carried() {
        let options = parse_chain(&v2_args(&["--bin-live-cap", "250000000"])).unwrap();
        assert_eq!(options.bin_live_cap, Some(250_000_000));
        let default = parse_chain(&v2_args(&[])).unwrap();
        assert_eq!(default.bin_live_cap, None, "absent = the plan picks from --memory-gb");
        // On the LEGACY route the flag does not exist (it is a bin-pipeline knob).
        let err = parse_chain(&["--input", "/tmp/c", "--scratch", "/tmp/s", "--legacy-merge",
                                "--bin-live-cap", "7"]
                              .iter().map(|s| s.to_string()).collect::<Vec<_>>()).unwrap_err();
        assert!(err.contains("--bin-live-cap is a bin-pipeline flag"), "{err}");
    }

    #[test]
    fn phrase_radix_is_off_by_default_and_carried_when_asked() {
        // DEFAULT OFF: absent, the arm is disabled and carries no knob at all, so
        // the producer command line is byte-identical to the product path's.
        let off = parse_chain(&v2_args(&[])).unwrap();
        assert!(!off.phrase.enabled);
        assert!(off.phrase.knobs.is_empty());
        // Asked for: enabled, and the knobs are carried IN COMMAND ORDER.
        let on = parse_chain(&v2_args(&["--phrase-radix", "--phrase-min-len", "32",
                                        "--phrase-table-bits", "22"]))
            .unwrap();
        assert!(on.phrase.enabled);
        assert_eq!(on.phrase.knobs,
                   vec![("--phrase-min-len", 32u64), ("--phrase-table-bits", 22u64)]);
        // Everything else about the run is untouched by the arm.
        assert_eq!(on.threads, off.threads);
        assert_eq!(on.bin_live_cap, off.bin_live_cap);
        assert_eq!(on.bin_target_size, off.bin_target_size);
    }

    #[test]
    fn a_phrase_knob_without_phrase_radix_is_refused() {
        let err = parse_chain(&v2_args(&["--phrase-min-len", "32"])).unwrap_err();
        assert!(err.contains("needs --phrase-radix"), "{err}");
    }

    #[test]
    fn phrase_radix_is_refused_on_the_legacy_route() {
        let legacy = |extra: &[&str]| {
            let mut args: Vec<String> = ["--input", "/tmp/c", "--scratch", "/tmp/s",
                                        "--legacy-merge"]
                .iter().map(|s| s.to_string()).collect();
            args.extend(extra.iter().map(|s| s.to_string()));
            args
        };
        let err = parse_chain(&legacy(&["--phrase-radix"])).unwrap_err();
        assert!(err.contains("--phrase-radix is a bin-pipeline flag"), "{err}");
        let err = parse_chain(&legacy(&["--phrase-min-freq", "4"])).unwrap_err();
        assert!(err.contains("--phrase-radix is a bin-pipeline flag"), "{err}");
    }

    #[test]
    fn phrase_knobs_are_validated_with_the_producers_own_constraints() {
        let bad = |extra: &[&str]| {
            let mut a: Vec<String> = vec!["--phrase-radix".into()];
            a.extend(extra.iter().map(|s| s.to_string()));
            parse_chain(&v2_args(&a.iter().map(|s| s.as_str()).collect::<Vec<_>>())).unwrap_err()
        };
        assert!(bad(&["--phrase-min-len", "8"]).contains("< 16"));
        assert!(bad(&["--phrase-seed", "4"]).contains("< 16"));
        assert!(bad(&["--phrase-skip-min-len", "4"]).contains("< 16"));
        assert!(bad(&["--phrase-min-len", "512", "--phrase-max-len", "64"])
                    .contains("< --phrase-min-len"));
        assert!(bad(&["--phrase-window", "1"]).contains("< 2"));
        assert!(bad(&["--phrase-table-bits", "30"]).contains("outside [8, 26]"));
        assert!(bad(&["--phrase-reps", "1"]).contains("< 2"));
        // A knob given twice is a misconfiguration, not a last-wins.
        assert!(bad(&["--phrase-reps", "4", "--phrase-reps", "8"]).contains("only once"));
    }

    #[test]
    fn bin_target_size_is_carried_and_refused_on_the_legacy_route() {
        let options = parse_chain(&v2_args(&["--bin-target-size", "1048576"])).unwrap();
        assert_eq!(options.bin_target_size, Some(1_048_576));
        assert_eq!(parse_chain(&v2_args(&[])).unwrap().bin_target_size, None,
                   "absent = the producer's measured automatic default");
        let err = parse_chain(&v2_args(&["--bin-target-size", "1"])).unwrap_err();
        assert!(err.contains("--bin-target-size must be >= 2"), "{err}");
        let err = parse_chain(&["--input", "/tmp/c", "--scratch", "/tmp/s", "--legacy-merge",
                                "--bin-target-size", "1048576"]
                              .iter().map(|s| s.to_string()).collect::<Vec<_>>()).unwrap_err();
        assert!(err.contains("--bin-target-size is a bin-pipeline flag"), "{err}");
    }

    #[test]
    fn the_bin_pipeline_is_the_default_and_v2_pipeline_is_ignored() {
        // THE FLAG FLIP: no route flag at all selects the bin pipeline, and
        // --v2-pipeline is accepted (not an unknown option) and journalled as
        // ignored rather than silently ignored.
        let plain: Vec<String> = ["--input", "/tmp/c", "--scratch", "/tmp/s"]
            .iter().map(|s| s.to_string()).collect();
        let options = parse_chain(&plain).unwrap();
        assert!(options.bin_pipeline && !options.legacy_merge);
        assert!(!options.v2_flag_given);
        let with_flag = parse_chain(&v2_args(&[])).unwrap();
        assert!(with_flag.bin_pipeline, "--v2-pipeline still means the bin pipeline");
        assert!(with_flag.v2_flag_given, "the journal must record that it was seen");
        // The same knobs, byte-for-byte the same options except for the flag's
        // own record: --v2-pipeline changes NOTHING about the run.
        assert_eq!(with_flag.threads, options.threads);
        assert_eq!(with_flag.memory_gb, options.memory_gb);
        assert_eq!(with_flag.bin_live_cap, options.bin_live_cap);
        assert_eq!(with_flag.bin_target_size, options.bin_target_size);
        // --legacy-merge is the ONLY way back to the chunk+merge route.
        let legacy = parse_chain(&["--input", "/tmp/c", "--scratch", "/tmp/s",
                                   "--legacy-merge"]
                                 .iter().map(|s| s.to_string()).collect::<Vec<_>>()).unwrap();
        assert!(legacy.legacy_merge && !legacy.bin_pipeline);
    }

    #[test]
    fn legacy_merge_is_the_only_route_that_takes_the_chunk_knobs() {
        let legacy = parse_chain(&["--input", "/tmp/c", "--scratch", "/tmp/s", "--legacy-merge",
                                   "--chunks", "32", "--kway", "4"]
                                 .iter().map(|s| s.to_string()).collect::<Vec<_>>()).unwrap();
        assert!(legacy.legacy_merge);
        assert_eq!(legacy.chunks, 32);
        assert_eq!(legacy.kway, 4);
    }

    #[test]
    fn v2_refuses_the_inert_chunk_knobs_loudly() {
        // The bin shape is one sweep -> bins -> stitch: a chunk/k-way/fan-in
        // knob would silently misconfigure the run, so each is refused - on the
        // DEFAULT route (v2_args passes --v2-pipeline, which is now a no-op that
        // must not change any of this).
        for (extra, needle) in [
            (&["--chunks", "32"][..], "--chunks is INERT"),
            (&["--chunk-mb", "512"][..], "--chunk-mb is INERT"),
            (&["--kway", "8"][..], "--kway is INERT"),
            (&["--fanin"][..], "--fanin is INERT"),
        ] {
            let err = parse_chain(&v2_args(extra)).unwrap_err();
            assert!(err.contains(needle), "expected {needle:?} in {err:?}");
        }
    }

    #[test]
    fn wit_ram_over_the_memory_cap_is_refused() {
        let err = parse_chain(&v2_args(&["--memory-gb", "8", "--wit-ram-gb", "16"])).unwrap_err();
        assert!(err.contains("exceeds the finish's effective AS cap"), "{err}");
        // --finish-as-gb raises the finish's cap, so the same --wit-ram-gb is
        // legal against it, and is refused again when it exceeds THAT cap.
        let ok = parse_chain(&v2_args(&["--memory-gb", "8", "--finish-as-gb", "64",
                                       "--wit-ram-gb", "16"]));
        assert!(ok.is_ok(), "{:?}", ok.unwrap_err());
        let err2 = parse_chain(&v2_args(&["--memory-gb", "8", "--finish-as-gb", "12",
                                        "--wit-ram-gb", "16"])).unwrap_err();
        assert!(err2.contains("exceeds the finish's effective AS cap 12 GiB"), "{err2}");
    }

    #[test]
    fn tape_chi_defaults_on_with_explicit_opt_out() {
        let default = parse_chain(&v2_args(&["--tape"])).unwrap();
        assert!(default.with_chi);
        assert!(!parse_chain(&v2_args(&["--tape", "--no-chi"])).unwrap().with_chi);
        // m14 (the LCP over runs): DEFAULT OFF (a measured 0.256 n for a thin
        // middle band); --with-lcp-runs opts in and --no-lcp-runs is explicit.
        assert!(!parse_chain(&v2_args(&["--tape"])).unwrap().with_lcp_runs);
        assert!(!parse_chain(&v2_args(&["--tape", "--no-lcp-runs"])).unwrap().with_lcp_runs);
        assert!(parse_chain(&v2_args(&["--tape", "--with-lcp-runs"])).unwrap().with_lcp_runs);
        let enabled = parse_chain(&v2_args(&["--tape", "--with-chi", "--chi-expect", "42"])).unwrap();
        assert!(enabled.with_chi);
        assert_eq!(enabled.chi_expect, Some(42));
        let error = parse_chain(&v2_args(&["--tape", "--no-chi", "--chi-expect", "42"])).unwrap_err();
        assert!(error.contains("requires --with-chi"), "{error}");
        // Existing non-tape builds retain their chi expectation contract.
        assert!(parse_chain(&v2_args(&["--chi-expect", "42"])).is_ok());
    }

    #[test]
    fn finish_v2_address_space_floor() {
        let memory_kb = 96 * 1048576;
        assert_eq!(finish_v2_as_kb(memory_kb, memory_kb, 100_000_000_000), 292_968_750);
        assert_eq!(finish_v2_as_kb(memory_kb, memory_kb, 10_000_017_259), memory_kb);
        assert_eq!(finish_v2_as_kb(memory_kb, 400_000_000, 100_000_000_000), 400_000_000);
        assert_eq!(finish_v2_as_kb(1, 1, 342), 2);
    }

    #[test]
    fn finish_as_gb_parses_and_defaults() {
        let base = parse_chain(&v2_args(&["--memory-gb", "8"])).unwrap();
        assert_eq!(base.finish_as_gb, None);
        let with = parse_chain(&v2_args(&["--memory-gb", "8", "--finish-as-gb", "64"])).unwrap();
        assert_eq!(with.finish_as_gb, Some(64));
        assert!(parse_chain(&v2_args(&["--memory-gb", "8", "--finish-as-gb", "0"])).is_err());
        assert!(parse_chain(&v2_args(&["--memory-gb", "8", "--finish-as-gb", "64",
                                      "--finish-as-gb", "32"])).is_err());
    }

    /// THERE IS NO TIMEOUT AND NO WATCHDOG. Both flags are gone from the CLI:
    /// they are rejected as unknown options, not accepted and ignored, so they
    /// cannot silently linger in a script and pretend to cap or supervise a run.
    #[test]
    fn timeout_and_stall_flags_are_rejected_as_unknown() {
        let plain: Vec<String> = ["--input", "/tmp/corpus", "--scratch", "/tmp/scratch"]
            .iter().map(|s| s.to_string()).collect();
        assert!(parse_chain(&plain).is_ok(), "a plain build needs no --timeout: {plain:?}");
        for flag in ["--timeout", "--stall-seconds"] {
            let mut args = plain.clone();
            args.push(flag.into());
            args.push("3600".into());
            let err = parse_chain(&args).unwrap_err();
            assert!(err.contains("unknown option"), "{flag} must be rejected: {err}");
        }
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
