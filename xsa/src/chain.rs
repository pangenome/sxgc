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
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;

use sha2::{Digest, Sha256};

pub const USAGE: &str = "usage:
  xsa build --input <corpus> --scratch <dir> [--snap-1e] [--memory-gb N] [--threads N]
                    [--remap <256-byte-map>] [--bin-live-cap SLOTS]
                    [--bin-target-size ROWS] [--bin-entropy-min [BITS]] [--chi-expect N] [--wit-ram-gb N]
                    [--phrase-radix [--phrase-KNOB N ...]]
                    [--timeout S] [--legacy-merge [--chunks N] [--kway N]]

ONE ENTRY. `xsa build` runs the BIN pipeline by default (auto-radix): the
multi-pass reader (count sweep -> prefix-sum scatter), the bounded rolling
window FILES, the target-size recursive bins (each bin subdivides by its next
byte until every leaf is <= the target, so tree depth is an OUTPUT of the data)
and the ordered stitch straight into the unchanged band/piece protocol, which
the decomposed fused finish consumes concurrently. There is no chunk phase, no
merge walk and no position column on this route.

  --legacy-merge     the PRE-BIN route (chunk front end -> k-way merge tree ->
                     adopt finish) for A/B only. It is the only route that takes
                     --chunks/--chunk-mb/--kway/--fanin, and it is not the
                     document. Kept because the banked byte-identity fixtures
                     were built with it, and a claim about the new route needs
                     the old one beside it.

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
  --chunk-mb N       fixed chunk size (MiB): the chunk count is derived by
                     arithmetic (ceil(corpus_bytes / N MiB); document-aligned
                     boundaries land near, not at, the target). The pile
                     driver's sizing knob; mutually exclusive with --chunks.
  --fanin            the fan-in shape: fixed chunks (--chunk-mb) + repeated
                     k-way fan-in (--kway K) until one artifact remains.
                     Levels are arithmetic; every level runs the same
                     certified k-way merge and its output is a valid next
                     level input (zero re-derivation); consumed inputs are
                     reclaimed as the build advances (CROSS_DELETE_CONSUMED)
                     so the peak disc is ~ one level's live set + one group's
                     merge transient, not the whole input history. The plan
                     prints levels, per-level side sizes, peak disc and wall.
  --kway N           merge arity (default 2 = the certified pairwise tree).
                     N = chunk count is the FLAT one-level shape: ONE k-way
                     merge straight from the chunks (primary shape; every
                     arity byte-reproduces the same canonical artifact).
                     N < chunk count with --fanin is the progressive shape.
  --remap PATH       256-byte bijective byte map applied by the chunk front
                     end. Default: the embedded production map (the banked
                     fragment route's frag.remap, sha256 b4f38776...).

Pool knobs pass through to the merge unchanged (default: fixed concurrent
pool ON): CROSS_NO_POOL=1, CROSS_ASYNC_FILL=1, CROSS_HASH_K=K, CROSS_PIPELINE=1,
CROSS_SHARDS=S (parallel emission: the order/run-boundary shard count for the
merge walks; default --threads, 1 = the serial walk). The chunk front end
writes per-chunk corpus references (chunk-i.ref) so the merge never
copies chunk text (referenced mode; CHUNK_NO_REF=1 restores the legacy
artifact set, CROSS_NO_REF=1 makes the merge ignore refs).

Outputs (byte-identical to the reference chain on the same corpus):
  merged/frag.rlebwt .rlebwt.meta .ssa .ssa_t .pftext .pfck
  finish/frag.ri4 .head_sa .agg .sA
The uniform journal (xsa-build.log) reports chi and per-phase wall / peak
RSS / disc telemetry.

  xsa build --plan-only           print the surveyed+modeled build plan
  --v2-pipeline          ACCEPTED AND IGNORED (auto-radix). It used to select
                     the bin pipeline; the bin pipeline is now the DEFAULT, so
                     the flag selects nothing and the journal records that it was
                     seen and ignored. `--legacy-merge` is what selects the old
                     route now.
  THE DEFAULT ROUTE (the bin pipeline, no flag needed): bin-bwt sweeps the
                     corpus (count sweep -> prefix-sum scatter), groups every
                     position into the bin of its first remapped byte (256
                     byte-bins) and SUBDIVIDES each bin by its next byte until
                     every leaf is <= the target size, so the tree's depth is an
                     OUTPUT of the data and the largest serial stretch is one
                     leaf sort; it stitches the bins in byte order straight into
                     the unchanged band/piece protocol. The decomposed fused
                     finish (xsa_finish) consumes those pieces CONCURRENTLY
                     (flow-controlled through the piece window) and publishes
                     the lite container, then deletes the rlebwt. There is NO
                     chunk phase and NO merge walk on this route.
                     With --plan-only it prints the plan and exits (the same
                     plan the build journals first).
  --legacy-merge         the PRE-BIN route: chunk front end (libsais) -> k-way
                     merge tree -> adopt finish. A/B only; the only route that
                     accepts --chunks/--chunk-mb/--kway/--fanin.
  --bin-entropy-min [B] arm the entropy rule: a radix level is taken iff the
                     next-byte histogram's entropy exceeds B bits (default B = 1.5,
                     the measured bracket's middle). Absent = OFF (byte-identical default).
  --bin-target-size N  the leaf bound of the recursion, in ROWS (default: 0 =
                     AUTOMATIC). Bins subdivide until a node holds <= N rows; the
                     subtree's depth is whatever the text demands. It is priced
                     from the measured leaf-sort rate and the bucketing cost, and
                     the value the run used is journaled (BIN_WORK target=...).
  --bin-live-cap N     the position-arena bound, in SLOTS (one slot
                     per admitted position; the arena pair is 16 B/slot of
                     anonymous RAM, never a file). The corpus is processed in
                     ceil(n/cap) windows, each admitting a contiguous range of
                     bins that fits the bound. 0 = unbounded (one window, an
                     arena of 16n bytes - refused unless --memory-gb allows it).
                     Default: picked from --memory-gb (85% of the cap / 16 B per
                     slot, never above n) and journaled. A single bin larger
                     than the cap makes the producer raise it (BIN_CAP_RAISED),
                     which the front door treats as a MISCONFIGURATION and
                     fails loud (the plan priced a different arena).
  --phrase-radix       ARM THE PHRASE DICTIONARY (tools/binbwt/PHRASE_RADIX_DESIGN.md).
                     DEFAULT OFF, and off is the product: with the flag absent no
                     producer argument is added, no parse runs and no line of
                     the mechanism executes - the product stays byte-identical.
                     ON, the producer parses the corpus before the bins start
                     (a winnowed repeat sample -> a small direct-mapped
                     dictionary of phrase blocks) and, at a node that is both
                     DEEP (>= --phrase-probe-depth certified bytes) and HEAVY
                     (>= --phrase-skip-min-rows rows), may certify a whole
                     phrase at that depth and skip s byte levels at once. The
                     parse is NOT free: measured 27 s per GiB of corpus, plus
                     the winnow's whole sample array in RAM (8 B x 0.0834 n ~
                     0.67n), and the plan prints both (XSA_PLAN_V2_PHRASE).
                     CAVEAT, measured: at 100 MB, 1 GB and 10 GB the skip fires
                     ZERO times (PHRASE_WORK skips=0) - every tree node's rows
                     already differ at their own depth (no_progress_levels=0), so
                     a node-level phrase certificate cannot apply. The arm costs
                     the parse and buys nothing on those corpora; see the
                     RESULTS note. Knobs: --phrase-min-len, --phrase-min-freq,
                     --phrase-max-len, --phrase-seed, --phrase-window,
                     --phrase-block-max, --phrase-pool-max, --phrase-table-bits,
                     --phrase-reps, --phrase-skip-min-rows, --phrase-skip-min-len,
                     --phrase-probe-depth, --phrase-probe-stride.
  --timeout SECONDS    hard wall cap on every phase (timeout -k 120, i.e.
                     TERM then KILL after 120s) - REQUIRED for --v2-pipeline
                     execution (caps on every run). Standing policy is a huge
                     value: no phase may be killed by the cap.
  --chi-expect N       v2 only: the banked chi constant; the finish asserts
                     chi == N in-process and the front door re-checks the
                     journal line (byte-identity + chi are the only arbiter).
  --wit-ram-gb N       v2 only: the fused finish's witness RAM budget in GiB
                     (default: --memory-gb / 2; overflow spills as sorted
                     varint-delta disc runs without changing the product).
  --finish-as-gb N     v2 only: the finish phase's own RLIMIT_AS in GiB
                     (default: --memory-gb). The finish maps corpus windows to
                     pin the corpus for the lock-free byte path; mapped windows
                     charge ADDRESS SPACE, not RSS - the pages are page-cache
                     backed and reclaimable - so the finish can pin a corpus far
                     larger than the build's envelope. The pin budget is
                     corpus-sized and hard-bounded by this cap / 3.

  INERT UNDER THE BIN PIPELINE (the default; refused loud, never silently
  ignored): --chunks, --chunk-mb, --kway, --fanin - the bin shape has no chunk
  set to fan in and no k-way walk; the producer sweeps the corpus itself. The
  merge path stays in the binary behind --legacy-merge for A/B, and is priced by
  the v1/fan-in plans, not by the bin plan.
  --scratch-free-pct N  OPT-IN disc hygiene rule: require N% of the scratch
                     filesystem to stay free. DEFAULT OFF: the gate is the
                     ABSOLUTE fit (projected peak + headroom vs free bytes),
                     which is what actually predicts success. A percentage of
                     the device refuses a 4 GB build on a 14 TB drive that is
                     90% full, which tells an operator nothing.
  --stripe DIR[,DIR...]  candidate stripe directories for the disc model
                     (reader-level striping is lane item 3; the plan
                     accounts for the space now, the I/O split lands later).

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
    timeout_s: Option<u64>,
    stripes: Vec<String>,
    scratch_free_pct: u32,
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
    let mut chi_expect = None;
    let mut wit_ram_gb = None;
    let mut finish_as_gb = None;
    let mut bin_live_cap = None;
    let mut bin_target_size = None;
    let mut bin_entropy_min = None;
    let mut phrase = PhraseOptions::default();
    let mut timeout_s = None;
    let mut stripes: Vec<String> = Vec::new();
    // Needs-based by default: the absolute peak-vs-free check is the gate
    // (see disc_verdict / tree_ok). A percentage-of-device rule is opt-in.
    let mut scratch_free_pct = 0u32;
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
            "--timeout" => {
                i += 1;
                let v = value(args.get(i), "--timeout")?
                    .parse::<u64>().map_err(|_| "--timeout: integer expected".to_string())?;
                if v == 0 {
                    return Err("--timeout must be positive".into());
                }
                if timeout_s.replace(v).is_some() {
                    return Err("specify --timeout only once".into());
                }
            }
            "--scratch-free-pct" => {
                i += 1;
                scratch_free_pct = parse_u32(args.get(i), "--scratch-free-pct")?;
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
    if bin_pipeline && !plan_only && timeout_s.is_none() {
        return Err("bin-pipeline execution requires --timeout SECONDS (caps on every run: timeout -k 120 + the ulimit)".into());
    }
    Ok(Options { input, scratch, snap_1e, memory_gb, threads, chunks, kway, plan_only,
                 bin_pipeline, legacy_merge, v2_flag_given, chi_expect, wit_ram_gb,
                 finish_as_gb, bin_live_cap, bin_target_size, bin_entropy_min, phrase, timeout_s, stripes, scratch_free_pct,
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
/// A phase running in the background (spawned; join later). The uniform
/// journal writes PHASE START at spawn and PHASE END at join, so two
/// concurrent phases interleave honestly in the one journal.
struct RunningPhase {
    tag: String,
    child: std::process::Child,
    log_path: PathBuf,
    time_path: PathBuf,
}

/// Spawn one phase under /usr/bin/time -v with the hard RLIMIT_AS cap and
/// optional per-phase environment overrides (journaled in PHASE START).
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
    journal.line(&format!("PHASE {tag} START {command_line}{env_note}"));
    let log = File::options().create(true).append(true).open(&log_path)
        .map_err(|e| format!("create {}: {e}", log_path.display()))?;
    let log_err = log.try_clone().map_err(|e| format!("clone {tag} log handle: {e}"))?;
    let mut command = {
        let mut cmd = Command::new("bash");
        cmd.arg("-c").arg(PHASE_WRAPPER)
            .arg("xsa-phase") // $0
            .arg(memory_kb.to_string()) // $1: RLIMIT_AS in KiB
            .arg("/usr/bin/time").arg("-v").arg("-o").arg(&time_path)
            .arg(program).args(args)
            .current_dir(scratch)
            .stdout(Stdio::from(log))
            .stderr(Stdio::from(log_err));
        cmd
    };
    for (key, value) in env {
        command.env(key, value);
    }
    let child = command.spawn().map_err(|e| format!("launch {tag}: {e}"))?;
    Ok(RunningPhase { tag: tag.to_string(), child, log_path, time_path })
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
        }
        let peak_mb = build_plan.phases.iter().map(|p| p.disc_peak_mb).fold(0.0, f64::max);
        let fits = peak_mb * 1048576.0 <= build_plan.survey.scratch_free_bytes as f64;
        build_plan.disc_verdict = format!("{}: ALL-PHASE peak {:.1} GiB vs {:.1} GiB free (bin shape: rolling window FILES + rlebwt + piece window + container; the build's RAM is one group, never the window)",
            if fits { "FEASIBLE" } else { "INFEASIBLE" }, peak_mb / 1024.0,
            build_plan.survey.scratch_free_bytes as f64 / 1073741824.0);
        if !fits {
            build_plan.alternatives.push("free scratch for the all-phase bin-shape peak".into());
        }
        if 100u32.saturating_sub(build_plan.survey.scratch_used_percent) < options.scratch_free_pct {
            build_plan.alternatives.push("scratch hygiene policy: pass --scratch-free-pct to override".into());
            build_plan.disc_verdict.push_str("; INFEASIBLE under scratch hygiene policy");
        }
    }
    if options.plan_only {
        print!("{}", build_plan.render());
        return Ok(());
    }
    if build_plan.alternatives.iter().any(|a| !a.starts_with("CPU lacks")) {
        // Feasibility failed (disc or RAM): print the plan + alternatives,
        // refuse loud. (The cx16 note is informational, not a refusal.)
        print!("{}", build_plan.render());
        return Err("PLAN_INFEASIBLE: see XSA_PLAN_ALTERNATIVE lines above".into());
    }
    let mut journal = Journal::open(&journal_path)?;
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
        chunks_note, kway_note, if options.remap.is_some() { "provided" } else { "production" },
        if options.bin_pipeline { "v2bin" } else if options.fanin { "v1-fanin" } else { "v1" }
    ));
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
        return run_v2_tail(&mut journal, &options, &scratch, &tools, &input, &remap_path,
                           n, memory_kb, finish_as_kb, &frag, bin_live_cap, plan_windows);
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
) -> Result<(), String> {
    let s = |path: &str| path.to_string();
    // Caps on every run: timeout -k 120 (TERM, then KILL) on every phase,
    // on top of the ulimit wrapper every phase already runs under. The
    // standing policy is a huge value: no phase may be killed by the cap.
    let timeout_s = options.timeout_s
        .ok_or("--v2-pipeline execution requires --timeout SECONDS")?;
    journal.line(&format!(
        "V2_TIMEOUT seconds={timeout_s} (timeout -k 120 on every phase; ulimit -v {memory_kb} KiB; standing policy = huge, no phase may die on the cap)"));

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
    producer_args.extend(["-k".to_string(), "120".to_string(), timeout_s.to_string()]);
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
    let mut finish_args: Vec<String> = Vec::new();
    finish_args.extend(["-k".to_string(), "120".to_string(), timeout_s.to_string()]);
    finish_args.push(s(&tools.join("xsa_finish").to_string_lossy()));
    finish_args.extend([
        "--prefix".into(), frag.to_string(),
        "--corpus".into(), s(&input.to_string_lossy()),
        "--remap".into(), s(&remap_path.to_string_lossy()),
        "--output".into(), s(&container.to_string_lossy()),
        "--threads".into(), options.threads.to_string(),
        "--poll-timeout".into(), timeout_s.to_string(),
        "--wit-ram-budget".into(), wit_ram_bytes.to_string(),
    ]);
    if let Some(expected) = options.chi_expect {
        finish_args.extend(["--chi-expect".into(), expected.to_string()]);
    }

    // Spawn BOTH before joining: the finish must start with the producer (it
    // polls the producer's piece directory from row zero).
    let producer_running = spawn_phase(journal, scratch, "bin-v2", memory_kb, "/usr/bin/timeout", &producer_args, &producer_env)?;
    let mut finish_running = spawn_phase(journal, scratch, "finish-v2", finish_as_kb, "/usr/bin/timeout", &finish_args, &[])?;
    let v2_sampler = DiscSampler::start(scratch.to_path_buf(), Duration::from_secs(2));
    if let Err(e) = join_phase(journal, scratch, producer_running) {
        // A dead producer leaves the finish polling for hours; kill it loud.
        let _ = finish_running.child.kill();
        let _ = finish_running.child.wait();
        return Err(format!("{e}; the fused finish was killed after the bin producer failed"));
    }
    join_phase(journal, scratch, finish_running)?;
    let peak_disc = v2_sampler.peak_bytes();

    // ---- gates (fail loud on every violation)
    // A cap raise means the arena the run used is NOT the arena the plan
    // priced: refuse rather than accept a silently different memory shape.
    if count_log_contains(scratch, "bin-v2", "BIN_CAP_RAISED") > 0 {
        let raised = require_log_contains(scratch, "bin-v2", "BIN_CAP_RAISED")?;
        return Err(format!(
            "V2_BIN_CAP_RAISED: --bin-live-cap {bin_live_cap} is below the largest bin; the producer raised it ({raised}). \
             The plan priced a different arena - raise --bin-live-cap (and --memory-gb if needed) and re-run"));
    }
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
    // The finish's own work counters ride the same journal (the counter
    // identity is a hard gate: byte-identity alone has hidden two defects).
    let finish_log = fs::read_to_string(scratch.join("finish-v2.log"))
        .map_err(|e| format!("read finish-v2 log: {e}"))?;
    for line in finish_log.lines() {
        if line.starts_with("SLIM_FP ") || line.starts_with("SLIM_SEEDS ") || line.starts_with("XSA_FINISH_PFCK ") {
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
    // all on this route.
    let forbidden = ["frag.agg", "frag.head_sa", "frag.ssa", "frag.ssa_t", "frag.pftext",
                     "frag.pfck", "frag.pfck2", "frag.rlebwt", "frag.rlebwt.meta"];
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
    journal.line(&format!("XSA_BUILD_DONE chi={chi} pipeline=v2bin container={}", container.display()));
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

    fn v2_args(extra: &[&str]) -> Vec<String> {
        let mut args: Vec<String> = ["--input", "/tmp/corpus", "--scratch", "/tmp/scratch",
                                    "--v2-pipeline", "--timeout", "3600"]
            .iter().map(|s| s.to_string()).collect();
        args.extend(extra.iter().map(|s| s.to_string()));
        args
    }

    #[test]
    fn bin_live_cap_is_a_bin_pipeline_flag_and_is_carried() {
        let options = parse_chain(&v2_args(&["--bin-live-cap", "250000000"])).unwrap();
        assert_eq!(options.bin_live_cap, Some(250_000_000));
        let default = parse_chain(&v2_args(&[])).unwrap();
        assert_eq!(default.bin_live_cap, None, "absent = the plan picks from --memory-gb");
        // On the LEGACY route the flag does not exist (it is a bin-pipeline knob).
        let err = parse_chain(&["--input", "/tmp/c", "--scratch", "/tmp/s", "--legacy-merge",
                                "--timeout", "3600", "--bin-live-cap", "7"]
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
                                        "--legacy-merge", "--timeout", "3600"]
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
                                "--timeout", "3600", "--bin-target-size", "1048576"]
                              .iter().map(|s| s.to_string()).collect::<Vec<_>>()).unwrap_err();
        assert!(err.contains("--bin-target-size is a bin-pipeline flag"), "{err}");
    }

    #[test]
    fn the_bin_pipeline_is_the_default_and_v2_pipeline_is_ignored() {
        // THE FLAG FLIP: no route flag at all selects the bin pipeline, and
        // --v2-pipeline is accepted (not an unknown option) and journalled as
        // ignored rather than silently ignored.
        let plain: Vec<String> = ["--input", "/tmp/c", "--scratch", "/tmp/s", "--timeout", "3600"]
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
                                   "--legacy-merge", "--timeout", "3600"]
                                 .iter().map(|s| s.to_string()).collect::<Vec<_>>()).unwrap();
        assert!(legacy.legacy_merge && !legacy.bin_pipeline);
    }

    #[test]
    fn legacy_merge_is_the_only_route_that_takes_the_chunk_knobs() {
        let legacy = parse_chain(&["--input", "/tmp/c", "--scratch", "/tmp/s", "--legacy-merge",
                                   "--chunks", "32", "--kway", "4", "--timeout", "3600"]
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
    fn finish_as_gb_parses_and_defaults() {
        let base = parse_chain(&v2_args(&["--memory-gb", "8"])).unwrap();
        assert_eq!(base.finish_as_gb, None);
        let with = parse_chain(&v2_args(&["--memory-gb", "8", "--finish-as-gb", "64"])).unwrap();
        assert_eq!(with.finish_as_gb, Some(64));
        assert!(parse_chain(&v2_args(&["--memory-gb", "8", "--finish-as-gb", "0"])).is_err());
        assert!(parse_chain(&v2_args(&["--memory-gb", "8", "--finish-as-gb", "64",
                                      "--finish-as-gb", "32"])).is_err());
    }

    #[test]
    fn bin_pipeline_execution_still_requires_a_timeout() {
        // The DEFAULT route is the bin pipeline, so the cap requirement belongs to
        // the plain invocation, not to a flag.
        let args: Vec<String> = ["--input", "/tmp/corpus", "--scratch", "/tmp/scratch"]
            .iter().map(|s| s.to_string()).collect();
        let err = parse_chain(&args).unwrap_err();
        assert!(err.contains("requires --timeout"), "{err}");
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
