//! Driver-side resume state: the filesystem is the journal.
//!
//! The tape route's intermediates are SPOOLS. A spool's NAME encodes its place
//! in the deterministic plan (corpus + knobs + tool shas), its publication is
//! `.part` -> fdatasync -> rename -> directory fsync, and its DELETION is the
//! commit record: the driver may unlink a spool only once the consumer's output
//! is durable, so its ABSENCE means "already incorporated". A restart derives
//! the plan from corpus + knobs, enumerates what the filesystem holds, and
//! reports exactly what is present, what is missing, and what it must recompute.
//!
//! This state machine is deliberately honest about what it can and cannot
//! reuse today: `recomputed=<n>` is the number of plan spools this attempt has
//! to re-derive, and the WORK_AVOIDED lines price it in the phase's own units
//! (corpus bytes scanned, rows sorted, rows emitted, runs folded). Nothing here
//! infers avoided work from a wall clock or a skip flag.
use super::*;
use std::sync::atomic::AtomicI32;
static SIGNAL: AtomicI32 = AtomicI32::new(0);
extern "C" fn handler(sig: i32) {
    let _ = SIGNAL.compare_exchange(0, sig, Ordering::SeqCst, Ordering::SeqCst);
}
pub(super) fn install() -> Result<(), String> {
    unsafe {
        extern "C" {
            fn signal(sig: i32, handler: usize) -> usize;
        }
        for sig in [1, 2, 15] {
            if signal(sig, handler as *const () as usize) == usize::MAX {
                return Err("install build signal handler".into());
            }
        }
    }
    Ok(())
}
pub(super) fn signal() -> i32 {
    SIGNAL.load(Ordering::SeqCst)
}

// ===================== THE SPOOL SCHEME =====================================
/// Version of the spool naming/placement scheme the plan pins. A restart may
/// only reuse spools published under the SAME scheme: `p-<band>-<shard>` inside
/// `<prefix>.pieces`, `band_<i>.sa` inside `bands-tape`, the `remap-256.bin`
/// plan spool, and the `<stem>.sxi2` container. Bumping this makes every older
/// scratch a refusal instead of a silent misinterpretation.
pub(super) const SPOOL_SCHEME: u32 = 1;

/// `XSA_KILL_AT=<point>`: a deterministic ORDERING hook, so the publication
/// sequence can be exercised without a race. It is a test hook, never a build
/// knob: it is excluded from `knobs_sha256` (see `prepare`) so a killed run and
/// its restart share one plan.
pub(super) const KILL_POINTS: [&str; 3] = ["after_fsync_before_unlink", "after_rename", "mid_band"];

fn kill_at(point: &str) -> bool {
    match std::env::var("XSA_KILL_AT") {
        Ok(v) => {
            if v.trim().is_empty() { return false; }
            for want in v.split(',') {
                let want = want.trim();
                if !KILL_POINTS.contains(&want) {
                    eprintln!("XSA_KILL_AT: unknown point {want:?}; known points: {}", KILL_POINTS.join(","));
                    std::process::exit(2);
                }
                if want == point { return true; }
            }
            false
        }
        Err(_) => false,
    }
}

/// Die the way SIGKILL dies: no unwinding, no Drop, no journal record. The
/// caller has already terminated the phase groups; this process writes nothing.
pub(super) fn die_hard(point: &str) -> ! {
    eprintln!("XSA_KILL_AT hit point={point} pid={} (test hook: SIGKILL-equivalent exit)", std::process::id());
    std::process::exit(137);
}

pub(super) fn kill_wanted(point: &str) -> bool { kill_at(point) }

// ===================== WORK ACCOUNTING ======================================
/// The CLEAN-BUILD work each expensive phase performs, in the phase's own unit.
/// Recorded once by the driver when a run commits (SPOOL_INVENTORY), so a later
/// attempt can price what it avoided without re-deriving anything.
#[derive(Debug, Default, Clone, Copy)]
struct WorkTotals {
    /// phase=remap: corpus bytes scanned (one sequential histogram/identity pass).
    bytes: u64,
    /// phase=sort and phase=emit: corpus rows (the virtual END included).
    rows: u64,
    /// phase=finish: BWT runs folded into the container.
    runs: u64,
}

impl WorkTotals {
    fn read(existing: Option<&str>, scratch: &Path) -> WorkTotals {
        if let Some(text) = existing {
            if let Some(line) = text.lines().rev().find(|l| l.starts_with("SPOOL_INVENTORY ")) {
                let g = |k: &str| super::field(line, k).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
                let t = WorkTotals { bytes: g("work_bytes"), rows: g("work_rows"), runs: g("work_runs") };
                if t.bytes != 0 || t.rows != 0 || t.runs != 0 { return t; }
            }
        }
        // No commit record yet: read the phase logs of the attempt that is on
        // disk. They are the ground truth of what those phases actually did.
        let log = |tag: &str| fs::read_to_string(scratch.join(format!("{tag}.log"))).unwrap_or_default();
        let num = |text: &str, tag: &str, key: &str| -> u64 {
            text.lines().rev().find(|l| l.contains(tag))
                .and_then(|l| super::field(l, key)).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0)
        };
        let sort = log("order-tape");
        let emit = log("emit-tape");
        WorkTotals {
            bytes: 0,
            rows: num(&sort, "SS_DONE ", "n").max(num(&emit, "TAPE_EMIT_DONE ", "rows")),
            runs: num(&emit, "TAPE_EMIT_DONE ", "runs"),
        }
    }
}

/// What the plan required: the spool counts and the work totals. Counts come
/// from the last commit record; absent one, from the spool directory itself
/// (a spool that exists is a spool the plan required).
#[derive(Debug, Default, Clone, Copy)]
struct Inventory {
    bands: u64,
    pieces: u64,
    work: WorkTotals,
}

impl Inventory {
    fn read(existing: Option<&str>, scratch: &Path) -> Inventory {
        let (mut bands, mut pieces) = (0, 0);
        if let Some(text) = existing {
            if let Some(line) = text.lines().rev().find(|l| l.starts_with("SPOOL_INVENTORY ")) {
                let g = |k: &str| super::field(line, k).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
                bands = g("bands");
                pieces = g("pieces");
            }
        }
        Inventory {
            bands: bands.max(count_bands(scratch)),
            pieces: pieces.max(count_pieces(scratch)),
            work: WorkTotals::read(existing, scratch),
        }
    }
}

/// Published band spools: `bands-tape/band_<i>.sa` whose `.done` marker exists
/// (the sorter publishes the payload first, the marker after: a `.sa` without
/// `.done` is a spool still being written and is never counted).
fn count_bands(scratch: &Path) -> u64 {
    let dir = scratch.join("bands-tape");
    let mut n = 0;
    if let Ok(entries) = fs::read_dir(&dir) {
        for entry in entries.flatten() {
            let name = entry.file_name();
            let name = name.to_string_lossy();
            if let Some(rest) = name.strip_prefix("band_") {
                if let Some(idx) = rest.strip_suffix(".sa") {
                    if idx.chars().all(|c| c.is_ascii_digit()) && dir.join(format!("band_{idx}.done")).exists() {
                        n += 1;
                    }
                }
            }
        }
    }
    n
}

/// Piece spools present in `<prefix>.pieces` (`p-<band>-<shard>`). A `.part`
/// in flight is not a published spool and is not counted.
fn count_pieces(scratch: &Path) -> u64 {
    let dir = scratch.join("merged").join("frag.pieces");
    let mut n = 0;
    if let Ok(entries) = fs::read_dir(&dir) {
        for entry in entries.flatten() {
            let name = entry.file_name();
            let name = name.to_string_lossy();
            if name.starts_with("p-") && entry.file_type().map(|t| t.is_file()).unwrap_or(false) {
                n += 1;
            }
        }
    }
    n
}

fn inventory_recorded(existing: Option<&str>) -> bool {
    existing.map(|t| t.lines().any(|l| l.starts_with("SPOOL_INVENTORY "))).unwrap_or(false)
}

/// The exact line the operator asked for.
fn print_state(present: u64, missing: u64, recomputed: u64, pieces: u64, resume: bool) {
    println!(
        "RESUME_STATE spools_present={present} spools_missing={missing} recomputed={recomputed} \
pieces_present={pieces} resume={}",
        if resume { "true" } else { "false" }
    );
}

/// A phase's work, in the phase's OWN unit. `performed` is what THIS attempt
/// did, `total` what a clean build does; `avoided = total - performed`.
struct WorkPhase {
    tag: &'static str,
    performed: u64,
    total: u64,
}

impl WorkPhase {
    fn avoided_frac(&self) -> f64 {
        if self.total == 0 { 0.0 } else { (self.total - self.performed) as f64 / self.total as f64 }
    }
}

/// THE LINE, DEFINED ONCE, FOR BOTH SIDES OF THE PIPELINE (this function and
/// `workAvoided` in xsa/runtime/bit6/xsa_finish.cpp, which prints the identical
/// field order and units). A phase's clean build performs `total` units of its
/// own work; THIS attempt performed `performed` of them; so `avoided = total -
/// performed` units were not redone, and `avoided_frac = avoided / total` is 0
/// when the attempt did all of the phase's work and 1 when it did none. `basis`
/// names how `total` was derived: the plan (a quantity the plan fixes: corpus
/// bytes, rows), the recorded SPOOL_INVENTORY of a committed run (the finish's
/// run count, which is an OUTPUT of the data and is never guessed), or the
/// finish stage's own advance counters (the only place PARTIAL work is
/// measurable).
///
/// There is deliberately NO bare `ratio` field. This very field name once meant
/// performed/total in the native finish and avoided/total here, so a gate
/// reading `ratio<1.0` passed a full replay on one side and failed a no-work
/// resume on the other. One name, one meaning, one emitter per phase.
fn print_work_avoided(tag: &'static str, total: u64, performed: u64, basis: &str) -> WorkPhase {
    let performed = performed.min(total);
    let avoided = total - performed;
    let frac = if total == 0 { 0.0 } else { avoided as f64 / total as f64 };
    println!(
        "WORK_AVOIDED phase={tag} performed={performed} total={total} avoided={avoided} \
avoided_frac={frac:.6} basis={basis}"
    );
    WorkPhase { tag, performed, total }
}

/// ONE summary line, at build end, over the phases whose total is KNOWN (> 0):
/// `avoided_frac` is the aggregate avoided/total in those phases' own units, and
/// `unknown_total` counts the phases an attempt could not price at all (they stay
/// in the per-phase listing, unscored, instead of inventing avoided work).
fn print_work_summary(phases: &[WorkPhase]) {
    let scored: Vec<&WorkPhase> = phases.iter().filter(|p| p.total > 0).collect();
    let performed: u64 = scored.iter().map(|p| p.performed).sum();
    let total: u64 = scored.iter().map(|p| p.total).sum();
    let frac = if total == 0 { 0.0 } else { (total - performed) as f64 / total as f64 };
    let per = phases.iter().map(|p| format!("{}={:.6}", p.tag, p.avoided_frac()))
        .collect::<Vec<_>>().join(" ");
    println!(
        "WORK_AVOIDED_SUMMARY performed={performed} total={total} avoided_frac={frac:.6} \
phases={} basis=sum_over_phases_with_total_gt_0 unknown_total={} per_phase={per}",
        scored.len(),
        phases.len() - scored.len()
    );
}

/// The finish stage's own advance counters, when this attempt ran the finish and
/// it was in spool mode: `(adv_total, adv_performed)`. Only the finish can price
/// the runs it RESTORED from its durable checkpoint instead of re-folding them,
/// so these are what turn phase=finish from a coarse replanned/not-replanned
/// statement into a measured partial one. A malformed line is a refusal, never a
/// quietly recomputed guess.
fn finish_advance_counters(scratch: &Path) -> Result<Option<(u64, u64)>, String> {
    let text = match fs::read_to_string(scratch.join("finish-v2.log")) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(format!("read finish-v2 log: {e}")),
    };
    let Some(line) = text.lines().rev().find(|l| l.starts_with("FINISH_ADVANCE_COUNTERS ")) else {
        return Ok(None);
    };
    let num = |k: &str| -> Result<u64, String> {
        super::field(line, k).and_then(|v| v.parse::<u64>().ok())
            .ok_or_else(|| format!("FINISH_ADVANCE_COUNTERS without an integer {k}= field: {line}"))
    };
    let (total, performed) = (num("adv_total")?, num("adv_performed")?);
    if performed > total {
        return Err(format!(
            "FINISH_ADVANCE_COUNTERS adv_performed={performed} > adv_total={total}: \
             the finish advanced more runs than the run total ({line})"));
    }
    Ok(Some((total, performed)))
}

/// The m1 run total a committed run recorded (`SPOOL_INVENTORY work_runs`), else
/// the phase logs of the attempt on disk. 0 means UNKNOWN: an unknown total is
/// never guessed and never scored as avoided work.
fn recorded_runs(scratch: &Path) -> u64 {
    let existing = fs::read_to_string(scratch.join("checkpoint.journal")).ok();
    Inventory::read(existing.as_deref(), scratch).work.runs
}

/// THE BUILD-END ACCOUNTING. One line per phase, then one summary: printed once,
/// at the end of an attempt (a completed or adopted container, or the end of a
/// pipeline run), never as a prediction before the work happens.
///
/// The phase set is fixed by WHO CAN MEASURE IT:
///   remap  - corpus bytes; performed is 0 from a reused plan spool.
///   sort   - corpus rows (the virtual END included), planned units.
///   emit   - corpus rows (the virtual END included), planned units.
///   finish - m1 runs: from the stage's OWN counters when it ran (exact, so a
///            restored checkpoint shows up as genuinely avoided work), else from
///            the recorded total with `performed` = 0 or all of it.
/// `performed` for sort/emit is stated in PLAN units on purpose: comparing the
/// sorter's own `SS_DONE n` with the plan's n+1 would report one phantom avoided
/// row, which is the same class of defect as the old shared `ratio`.
pub(super) fn report_work_avoided(
    scratch: &Path,
    n: u64,
    remap_source: &str,
    pipeline_ran: bool,
) -> Result<(), String> {
    let replay = |total: u64| if pipeline_ran { total } else { 0 };
    let mut phases: Vec<WorkPhase> = Vec::new();
    phases.push(print_work_avoided(
        "remap", n, if remap_source == "corpus" { n } else { 0 }, "plan=corpus_bytes"));
    phases.push(print_work_avoided(
        "sort", n + 1, replay(n + 1), "plan=corpus_rows_plus_end"));
    phases.push(print_work_avoided(
        "emit", n + 1, replay(n + 1), "plan=corpus_rows_plus_end"));
    match if pipeline_ran { finish_advance_counters(scratch)? } else { None } {
        Some((total, performed)) => phases.push(print_work_avoided(
            "finish", total, performed, "stage=finish_advance_counters")),
        None => {
            let runs = recorded_runs(scratch);
            phases.push(print_work_avoided(
                "finish", runs, replay(runs),
                if runs == 0 { "unknown(no-recorded-runs)" } else { "recorded=m1_runs" }));
        }
    }
    print_work_summary(&phases);
    Ok(())
}

/// The publication of a spool: `.part` -> fdatasync -> rename -> directory
/// fsync. A spool only exists under its plan name once its bytes are durable.
fn publish_spool(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let mut part = path.as_os_str().to_owned();
    part.push(".part");
    let part = PathBuf::from(part);
    {
        let mut f = File::options().write(true).create(true).truncate(true).open(&part)
            .map_err(|e| format!("create {}: {e}", part.display()))?;
        f.write_all(bytes).map_err(|e| format!("write {}: {e}", part.display()))?;
        f.sync_all().map_err(|e| format!("fdatasync {}: {e}", part.display()))?;
    }
    fs::rename(&part, path).map_err(|e| format!("publish {}: {e}", path.display()))?;
    let dir = path.parent().ok_or("spool path has no parent")?;
    File::open(dir).and_then(|f| f.sync_all())
        .map_err(|e| format!("fsync spool directory {}: {e}", dir.display()))
}

// ===================== PLAN ================================================
fn append(scratch: &Path, record: &str) -> Result<(), String> {
    let mut f = File::options()
        .create(true)
        .append(true)
        .open(scratch.join("checkpoint.journal"))
        .map_err(|e| e.to_string())?;
    writeln!(f, "{record}")
        .and_then(|_| f.sync_all())
        .map_err(|e| e.to_string())?;
    File::open(scratch)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())
}
fn remove(path: &Path) -> Result<(), String> {
    match fs::symlink_metadata(path) {
        Ok(m) => if m.is_dir() {
            fs::remove_dir_all(path)
        } else {
            fs::remove_file(path)
        }
        .map_err(|e| e.to_string()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e.to_string()),
    }
}
fn compare(old: &str, new: &str) -> Result<(), String> {
    let fields = |line: &str| -> std::collections::BTreeMap<String, String> {
        line.split_whitespace()
            .skip(1)
            .filter_map(|f| f.split_once('=').map(|(k, v)| (k.to_owned(), v.to_owned())))
            .collect()
    };
    let a = fields(old);
    let b = fields(new);
    if !old.starts_with("PLAN ") {
        return Err("RESUME_REFUSED field=PLAN missing or malformed".into());
    }
    for key in a.keys().chain(b.keys()) {
        if a.get(key) != b.get(key) {
            return Err(format!(
                "RESUME_REFUSED field={key} stored={:?} current={:?}",
                a.get(key),
                b.get(key)
            ));
        }
    }
    Ok(())
}

/// ONE sequential pass over the corpus that yields its SHA-256, its byte
/// histogram and its length. The histogram is what both the remap derivation
/// and the remap validation need, so a resume that reuses the published
/// `remap-256.bin` spool costs exactly this single read - never a second one.
fn corpus_identity(path: &Path) -> Result<(String, [u64; 256], u64), String> {
    let mut f = File::open(path).map_err(|e| format!("open {}: {e}", path.display()))?;
    let mut ctx = Sha256::new();
    let mut hist = [0u64; 256];
    let mut block = vec![0u8; 1 << 22];
    let mut total = 0u64;
    loop {
        let z = f.read(&mut block).map_err(|e| format!("read {}: {e}", path.display()))?;
        if z == 0 { break; }
        total += z as u64;
        ctx.update(&block[..z]);
        for &c in &block[..z] { hist[c as usize] += 1; }
    }
    let size = fs::metadata(path).map_err(|e| format!("stat {}: {e}", path.display()))?.len();
    if total != size {
        return Err(format!("CORPUS_IDENTITY read {total} != stat {size} for {}", path.display()));
    }
    Ok((format!("{:x}", ctx.finalize()), hist, total))
}

/// The plan's spool inventory, one bounded record: the counts `RESUME_STATE`
/// needs and the clean-build work totals `WORK_AVOIDED` needs.
pub(super) fn record_inventory(scratch: &Path, remap_sha: &str) -> Result<(), String> {
    // The counts are what the RUN published, read from its own receipts: by the
    // time this commits, every band and piece has been consumed and unlinked, so
    // the tree says 0. The sorter journals one SS_BAND per band spool and the
    // emit journals one PIECE_DURABLE per piece spool.
    let bands = fs::read_to_string(scratch.join("order-tape.log")).unwrap_or_default()
        .lines().filter(|l| l.starts_with("SS_BAND ")).count() as u64;
    let pieces = fs::read_to_string(scratch.join("checkpoint.journal")).unwrap_or_default()
        .lines().filter(|l| l.starts_with("PIECE_DURABLE ")).count() as u64;
    let emitted = fs::read_to_string(scratch.join("xsa-build.log"))
        .unwrap_or_default()
        .lines()
        .rev()
        .find(|l| l.starts_with("XSA_SPOOL_INVENTORY_EMIT "))
        .map(str::to_string)
        .unwrap_or_default();
    let rows = super::field(&emitted, "rows").and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
    let runs = super::field(&emitted, "runs").and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
    let work_bytes = super::field(&emitted, "corpus_bytes").and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
    append(scratch, &format!(
        "SPOOL_INVENTORY scheme={SPOOL_SCHEME} remap_sha256={remap_sha} bands={bands} pieces={pieces} \
work_bytes={work_bytes} work_rows={rows} work_runs={runs}"))
}

/// The container's own header, validated. Publication is
/// `write .partial -> fsync -> close -> link -> unlink .partial`, so a file that
/// EXISTS under the final name is complete and durable: this validation is what
/// lets a restart adopt it instead of rebuilding the whole pipeline.
fn validate_container(path: &Path, rows: u64, with_chi: bool) -> Result<(u64, String), String> {
    let meta = fs::metadata(path).map_err(|e| format!("stat {}: {e}", path.display()))?;
    let bytes = meta.len();
    if bytes < 64 { return Err(format!("CONTAINER_INVALID {} is {bytes} B, under the 64 B header", path.display())); }
    let mut f = File::open(path).map_err(|e| format!("open {}: {e}", path.display()))?;
    let mut head = vec![0u8; 64];
    f.read_exact(&mut head).map_err(|e| format!("read header {}: {e}", path.display()))?;
    let u64at = |o: usize| u64::from_le_bytes(head[o..o + 8].try_into().unwrap());
    let u32at = |o: usize| u32::from_le_bytes(head[o..o + 4].try_into().unwrap());
    if &head[0..4] != b"SXI2" { return Err(format!("CONTAINER_INVALID {} magic {:?} != SXI2", path.display(), &head[0..4])); }
    let version = u32at(4);
    if version != 6 { return Err(format!("CONTAINER_INVALID {} version={version} != 6", path.display())); }
    let (n, k, members, hdr, length, flags) = (u64at(8), u64at(16), u32at(32), u32at(36), u64at(40), u64at(48));
    if n != rows { return Err(format!("CONTAINER_INVALID {} n={n} != the plan's rows {rows}", path.display())); }
    if k != 1 { return Err(format!("CONTAINER_INVALID {} k={k} != 1", path.display())); }
    if hdr as u64 != 64 + 40 * members as u64 {
        return Err(format!("CONTAINER_INVALID {} header size {hdr} != 64+40*{members}", path.display()));
    }
    if length != bytes {
        return Err(format!("CONTAINER_INVALID {} declares {length} B, file is {bytes} B (truncated publish)", path.display()));
    }
    if (flags & 1 == 1) != with_chi {
        return Err(format!("CONTAINER_INVALID {} flags={flags:#x} disagree with plan with_chi={with_chi}", path.display()));
    }
    // The header carries its own CRC at +56, computed by the writer over the
    // WHOLE header with that field still zero: recompute exactly that.
    let mut dir = vec![0u8; hdr as usize];
    File::open(path).map_err(|e| format!("open {}: {e}", path.display()))?
        .read_exact(&mut dir).map_err(|e| format!("read header {}: {e}", path.display()))?;
    let stored = u32::from_le_bytes(dir[56..60].try_into().unwrap());
    let mut zeroed = dir.clone();
    zeroed[56..60].copy_from_slice(&[0, 0, 0, 0]);
    if container_crc(zeroed) != stored {
        return Err(format!("CONTAINER_INVALID {} header CRC mismatch", path.display()));
    }
    // Every member's payload CRC is the FORMAT's own check (the writer stores
    // ~crc(payload) at +32 and the reader's verify mode recomputes it). An
    // adoption without it would commit a file whose header is perfect and whose
    // bytes are not, so the whole payload is walked - one sequential read.
    let mut f = File::open(path).map_err(|e| format!("open {}: {e}", path.display()))?;
    let mut buf = vec![0u8; 1 << 20];
    for m in 0..members as usize {
        let d = &dir[64 + 40 * m..64 + 40 * (m + 1)];
        let id = u32::from_le_bytes(d[0..4].try_into().unwrap());
        let off = u64::from_le_bytes(d[8..16].try_into().unwrap());
        let len = u64::from_le_bytes(d[16..24].try_into().unwrap());
        let want = u32::from_le_bytes(d[32..36].try_into().unwrap());
        if off < hdr as u64 || off + len > length {
            return Err(format!("CONTAINER_INVALID {} member {id} [{off},{}) outside [{} , {length})",
                path.display(), off + len, hdr));
        }
        use std::io::Seek;
        f.seek(std::io::SeekFrom::Start(off)).map_err(|e| format!("seek {}: {e}", path.display()))?;
        let mut crc = !0u32;
        let mut left = len;
        while left != 0 {
            let z = usize::try_from(left.min(buf.len() as u64)).unwrap();
            f.read_exact(&mut buf[..z]).map_err(|e| format!("read member {id} of {}: {e}", path.display()))?;
            crc = container_crc_update(crc, &buf[..z]);
            left -= z as u64;
        }
        if !crc != want {
            return Err(format!("CONTAINER_INVALID {} member {id} payload CRC {:08x} != stored {:08x}",
                path.display(), !crc, want));
        }
    }
    Ok((bytes, super::sha256_file(path)?))
}

/// The container CRC: the writer's own `crc()` polynomial/loop (bit6/sxi_format
/// .hpp), so the validation is the FORMAT's check and not a new one.
fn container_crc_update(mut crc: u32, bytes: &[u8]) -> u32 {
    for &b in bytes {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ 0xEDB8_8320 } else { crc >> 1 };
        }
    }
    crc
}

fn container_crc(h: Vec<u8>) -> u32 {
    !container_crc_update(!0u32, &h)
}

#[derive(Debug, Clone)]
pub(super) struct Prepared {
    /// The container was already complete (committed or adopted): nothing to run.
    pub complete: bool,
    /// The corpus-derived remap table the plan pinned, with the facts the REMAP
    /// journal line reports. `prepare` proved it against this corpus.
    pub remap: super::RemapFacts,
}

pub(super) fn prepare(
    o: &Options,
    input: &Path,
    scratch: &Path,
    tools: &Path,
) -> Result<Prepared, String> {
    // ---- ONE corpus pass: its sha256, its byte histogram, its length.
    let (corpus, hist, n) = corpus_identity(input)?;
    let p = super::super::plan::tape_band_plan(
        n + 1,
        o.threads as u64,
        o.memory_gb,
        std::env::var("XSA_LCE_VERIFY").as_deref() != Ok("1"),
        2 * (n + 2),
        // THE SAME DISK BUDGET THE RUN WILL USE (plan::scratch_budget): the resume
        // preflight records rows_per_band in the checkpoint, so a preflight priced
        // from a different budget would refuse a run it just wrote the plan for.
        super::super::plan::scratch_budget(
            o.scratch_budget_gb, o.memory_gb,
            super::super::plan::disc_free_bytes(scratch)
                .ok_or_else(|| format!("df failed for {}", scratch.display()))?,
        ).bytes,
    );
    let checkpoint = scratch.join("checkpoint.journal");
    let container = scratch.join(format!(
        "{}.sxi2",
        input
            .file_stem()
            .and_then(|s| s.to_str())
            .filter(|s| !s.is_empty())
            .unwrap_or("pile")
    ));
    let existing = match fs::read_to_string(&checkpoint) {
        Ok(s) => Some(s),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(e) => return Err(e.to_string()),
    };
    let stored_first = existing.as_deref().and_then(|t| t.lines().next()).unwrap_or("");

    // ---- REFUSAL GATES. Nothing in the scratch may change before all of these
    // have passed: a refusal is a diagnosis, never a mutation.
    if !o.no_resume {
        if let Some(stored) = super::field(stored_first, "corpus_sha256") {
            if stored != corpus {
                return Err(format!(
                    "RESUME_REFUSED field=corpus_sha256 stored={stored} current={corpus}"
                ));
            }
        }
    }

    // ---- THE PLAN'S SPOOLS, AS THE FILESYSTEM SEES THEM.
    let inv = if o.no_resume { Inventory::default() } else { Inventory::read(existing.as_deref(), scratch) };
    let spool_path = scratch.join("remap-256.bin");
    let stored_remap = super::field(stored_first, "remap_sha256").map(str::to_string);
    let mut remap_recomputed = false;

    // ---- THE REMAP SPOOL: reuse, re-derive, or take the operator's.
    let (table, remap_source): ([u8; 256], &'static str) = if let Some(path) = &o.remap {
        let bytes = fs::read(path).map_err(|e| format!("read remap {}: {e}", path.display()))?;
        if bytes.len() != 256 {
            return Err(format!("remap {} must have exactly 256 bytes", path.display()));
        }
        let mut t = [0u8; 256];
        t.copy_from_slice(&bytes);
        (t, "provided")
    } else if !o.no_resume {
        match &stored_remap {
            Some(stored) if spool_path.exists() && super::sha256_file(&spool_path)? == *stored => {
                let bytes = fs::read(&spool_path).map_err(|e| format!("read {}: {e}", spool_path.display()))?;
                if bytes.len() != 256 {
                    return Err(format!("RESUME_REFUSED field=remap_spool size={} != 256", bytes.len()));
                }
                let mut t = [0u8; 256];
                t.copy_from_slice(&bytes);
                (t, "corpus-reused")
            }
            Some(_) => {
                // The plan pins a remap this scratch no longer holds: exactly one
                // spool is missing, and it is re-derived from the corpus.
                remap_recomputed = true;
                (super::table_from_hist(&hist)?, "corpus")
            }
            None => (super::table_from_hist(&hist)?, "corpus"),
        }
    } else {
        (super::table_from_hist(&hist)?, "corpus")
    };
    // The table is proved against THIS corpus's histogram on every path,
    // including the reused and the operator-provided one.
    let facts = super::remap_facts_from_hist(&table, &hist, remap_source)?;
    let remap_sha = format!("{:x}", Sha256::digest(table));

    let knobs = std::env::vars()
        // XSA_KILL_AT is a TEST HOOK, not a build knob: a killed run and its
        // restart must share one plan, so the hook never enters knobs_sha256.
        .filter(|(k, _)| {
            (k.starts_with("CROSS_") || k.starts_with("SS_") || k.starts_with("XSA_"))
                && k != "XSA_KILL_AT"
        })
        .collect::<std::collections::BTreeMap<_, _>>();
    let knobs_sha = format!(
        "{:x}",
        Sha256::digest(
            format!(
                "{knobs:?} wit={:?} finish_as={:?} snap={} chi_expect={:?}",
                o.wit_ram_gb, o.finish_as_gb, o.snap_1e, o.chi_expect
            )
            .as_bytes()
        )
    );
    let plan=format!("PLAN n={n} corpus_sha256={corpus} remap_sha256={remap_sha} rows_per_band={} parallel_bands={} waves={} sorter_sha={} merger_sha={} finish_sha={} driver_sha={} convention=replay-v1-run-ranks spool_scheme={SPOOL_SCHEME} knobs_sha256={knobs_sha} with_chi={} memory_gb={} lce_verify={}",p.rows_per_band,o.threads,p.waves,super::sha256_file(&tools.join("scatter_sort"))?,super::sha256_file(&tools.join("cross_lcp_merge"))?,super::sha256_file(&tools.join("xsa_finish"))?,super::sha256_file(&std::env::current_exe().map_err(|e|e.to_string())?)?,o.with_chi,o.memory_gb,std::env::var("XSA_LCE_VERIFY").unwrap_or_default());

    // ---- PLAN IDENTITY + SPOOL STATE.
    let committed = existing.as_deref().and_then(|t| {
        t.lines().rev().find_map(|l| l.strip_prefix("BUILD_DONE container="))
    }).map(str::to_string);
    let container_present = container.exists();
    let mut adopted = false;

    if let Some(old) = &existing {
        if !o.no_resume {
            compare(old.lines().next().unwrap_or(""), &plan)?;
        }
    } else if scratch.join("xsa-build.log").exists() && !o.no_resume {
        return Err("RESUME_REFUSED field=PLAN legacy scratch has no recovery journal; use a new scratch or --no-resume".into());
    }

    // `required` counts the plan's spools; a derived spool whose consumer's
    // output is durable is INCORPORATED, not missing (deletion is the commit).
    // The input spool (`remap-256.bin`) is never incorporated: it is pinned by
    // the plan and every phase reads it, so an absent one is always recomputed.
    let (mut present, mut missing, mut recomputed, resume);
    // `--no-resume` outranks every reuse path: it discards the previous
    // attempt's checkpoint, intermediates and container and starts a new plan.
    let reuse_commit = if o.no_resume { None } else { committed.as_ref() };
    if let Some(done_sha) = reuse_commit {
        let actual = super::sha256_file(&container)?;
        if actual != *done_sha {
            return Err("RESUME_REFUSED field=container_sha256".into());
        }
        resume = true;
        // Spools present at rest: the container, and the plan's input spool when
        // the scratch still holds it. Every DERIVED spool (bands, pieces) is
        // absent because the commit authorised its deletion: incorporated.
        present = u64::from(container_present);
        missing = 0;
        recomputed = 0;
        if !spool_path.exists() || remap_recomputed {
            missing = 1;
            recomputed = 1;
        } else {
            present += 1;
        }
    } else if existing.is_some() && !o.no_resume && container_present && committed.is_none() {
        // The container is PUBLISHED but not journalled: the finish's
        // fsync-then-link ordering means an existing file under the final name
        // is complete. Validate the format and adopt it - the alternative is
        // rebuilding the whole pipeline to re-derive bytes that are already
        // durable.
        match validate_container(&container, n + 1, o.with_chi) {
            Ok((bytes, sha)) => {
                println!("RESUME_ADOPT_CONTAINER path={} bytes={bytes} sha256={sha} validation=sxi2-header-crc+member-bounds+length", container.display());
                adopted = true;
                resume = true;
                present = 1 + u64::from(spool_path.exists()); // container (+ remap spool)
                missing = if spool_path.exists() { 0 } else { 1 };
                recomputed = missing;
            }
            Err(e) => return Err(format!("{e} (a published container that fails its own format is CORRUPTION: refusing rather than silently rebuilding)")),
        }
    } else {
        resume = existing.is_some() && !o.no_resume;
        if resume {
            let bands_present = count_bands(scratch);
            let pieces_present = count_pieces(scratch);
            let remap_present = u64::from(spool_path.exists() && !remap_recomputed);
            present = remap_present + bands_present + pieces_present + u64::from(container_present);
            // A spool the plan required and the filesystem does not hold, whose
            // consumer's output is NOT durable, has to be re-derived. (The
            // remap spool is an INPUT: it is never "incorporated".)
            missing = (1 - remap_present)
                + inv.bands.saturating_sub(bands_present)
                + inv.pieces.saturating_sub(pieces_present)
                + u64::from(!container_present);
            // The driver-side recomputation unit is one plan spool.
            recomputed = missing;
        } else {
            // No attempt of this plan was ever journalled here (a fresh scratch,
            // or --no-resume): nothing is being resumed and nothing is missing.
            present = 0;
            missing = 0;
            recomputed = 0;
        }
    }
    let pieces_present = count_pieces(scratch);
    print_state(present, missing, recomputed, pieces_present, resume);
    if resume && !(reuse_commit.is_some() || adopted) && !inventory_recorded(existing.as_deref()) {
        // Say it out loud instead of reporting a half-known inventory as if it
        // were complete: the plan's band/piece COUNTS are only recorded when a
        // run commits (they are outputs of the data, never plan guesses), so a
        // restart into a never-committed scratch knows only the spools on disk.
        println!("RESUME_INVENTORY_ABSENT reason=no_committed_SPOOL_INVENTORY required_known=0 rule=bands+pieces counted from disk only; the container is the one spool always required");
    }

    // ---- WORK AVOIDED. Printed at BUILD END only: when the container is
    // already complete (committed or adopted) the build ends here, so this is
    // the end; otherwise nothing has run yet and the accounting belongs to the
    // pipeline's end (chain::run_chain calls this same function there).
    let done_path = reuse_commit.is_some() || adopted;
    if done_path {
        report_work_avoided(scratch, n, remap_source, false)?;
    }

    // ---- NO MUTATION ABOVE THIS POINT (apart from the caller's ownership lock).
    // `--no-resume` never reaches this: it falls through to the clean rebuild.
    if done_path {
        // Commit path: the container is durable and verified. Publish the plan
        // spool if it is the one thing missing, then reclaim the consumed spools.
        if !spool_path.exists() || remap_recomputed {
            publish_spool(&spool_path, &table)?;
            println!("RESUME_SPOOL_REPUBLISHED path={} sha256={remap_sha} role=input recomputed=1",
                spool_path.display());
        }
        for name in ["bands-tape", "waves-tape", "merged", "stop.requested"] {
            remove(&scratch.join(name))?;
        }
        for suffix in [".part", ".partial"] {
            remove(&PathBuf::from(format!("{}{suffix}", container.display())))?;
        }
        File::open(&container)
            .and_then(|f| f.sync_all())
            .map_err(|e| e.to_string())?;
        File::open(scratch)
            .and_then(|f| f.sync_all())
            .map_err(|e| e.to_string())?;
        let sha = super::sha256_file(&container)?;
        if adopted {
            // The adopted container is the commit; the run that produced it left
            // its receipts, so record the plan's inventory before committing.
            record_inventory(scratch, &remap_sha)?;
            append(scratch, &format!("BUILD_DONE container={sha}"))?;
        } else {
            println!("BUILD_ALREADY_DONE container={sha}");
        }
        return Ok(Prepared { complete: true, remap: facts });
    }

    // Replay path: this attempt must (re)derive every missing spool.
    if existing.is_some() || o.no_resume {
        for name in [
            "bands-tape",
            "waves-tape",
            "merged",
            "stop.requested",
            "xsa-build.log",
        ] {
            remove(&scratch.join(name))?;
        }
        remove(&container)?;
        for suffix in [".part", ".partial"] {
            remove(&PathBuf::from(format!("{}{suffix}", container.display())))?;
        }
        if o.no_resume {
            remove(&checkpoint)?;
            remove(&spool_path)?;
        }
    }
    // Publish the plan spool only after every refusal has passed AND the scratch
    // is in its starting state: it is an input of the replay, not a leftover.
    if !spool_path.exists() || remap_recomputed || o.no_resume {
        publish_spool(&spool_path, &table)?;
    }
    if !checkpoint.exists() {
        append(scratch, &plan)?;
    } else {
        // A killed append can leave a final unterminated record. Preserve it as
        // an ignored torn record; never concatenate a new record onto it.
        let old = existing.as_ref().unwrap();
        if !old.ends_with('\n') {
            append(scratch, "")?;
        }
        append(scratch, "REPLAY from_key=0 reason=incomplete_build")?;
    }
    Ok(Prepared { complete: false, remap: facts })
}

/// THE COOPERATIVE STOP FLAG, UNCONDITIONALLY. The driver has no signal path: it
/// writes this file and every phase observes it at its own boundary, journals
/// STOP_OBSERVED and exits 75 (the sorter at a wave, the emitter at a band, the
/// finish at the piece it is waiting for, the trie tap at a band). A phase that
/// never observes it is a bug in that phase (documented as a residual risk).
pub(super) fn write_stop_flag(scratch: &Path) -> Result<(), String> {
    let path = scratch.join("stop.requested");
    if path.exists() { return Ok(()); }
    let f = File::create(&path).map_err(|e| format!("create {}: {e}", path.display()))?;
    f.sync_all().map_err(|e| format!("fsync {}: {e}", path.display()))?;
    Ok(())
}

pub(super) fn request_stop(scratch: &Path) -> Result<(), String> {
    if signal() != 0
        && scratch.join("checkpoint.journal").exists()
        && !scratch.join("stop.requested").exists()
    {
        let f = File::create(scratch.join("stop.requested")).map_err(|e| e.to_string())?;
        f.sync_all().map_err(|e| e.to_string())?;
    }
    Ok(())
}
pub(super) fn interrupted(scratch: &Path) -> Result<(), String> {
    let text = fs::read_to_string(scratch.join("checkpoint.journal")).map_err(|e| e.to_string())?;
    let attempt = text
        .rsplit("REPLAY from_key=0 reason=incomplete_build\n")
        .next()
        .unwrap_or(&text);
    let last = attempt
        .lines()
        .filter(|l| l.starts_with("PIECE_DURABLE "))
        .filter_map(|l| {
            l.split_whitespace()
                .find_map(|f| f.strip_prefix("idx=").and_then(|s| s.parse::<i64>().ok()))
        })
        .max()
        .unwrap_or(-1);
    let line = format!(
        "BUILD_INTERRUPTED reason={} last_durable_piece={last} scratch={}",
        signal(),
        scratch.display()
    );
    append(scratch, &line)?;
    eprintln!("{line}");
    Ok(())
}
pub(super) fn done(scratch: &Path, container: &Path) -> Result<(), String> {
    File::open(container)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())?;
    append(
        scratch,
        &format!("BUILD_DONE container={}", super::sha256_file(container)?),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn the_removed_merge_route_cannot_install_tape_recovery() {
        // The pre-bin chunk+merge route is GONE, so a --tape run can never be
        // mis-tagged as a merge run: the flag refuses by name before any recovery
        // marker could be installed.
        let args = [
            "--tape",
            "--legacy-merge",
            "--input",
            "corpus",
            "--scratch",
            "scratch",
        ]
        .map(String::from);
        assert!(parse_chain(&args).unwrap_err().contains("REMOVED"));
    }
    #[test]
    fn exact_field_refusal() {
        assert!(
            compare("PLAN n=1 corpus_sha256=a", "PLAN n=1 corpus_sha256=b")
                .unwrap_err()
                .contains("field=corpus_sha256")
        );
        assert!(compare("PLAN n=1", "PLAN n=1").is_ok());
        assert!(compare("PLAN n=1 extra=1", "PLAN n=1").is_err());
    }
}

#[cfg(test)]
mod lifecycle_tests {
    use super::*;
    fn fixture() -> (tempfile::TempDir, Options, PathBuf, PathBuf, PathBuf) {
        let tmp = tempfile::tempdir().unwrap();
        let root = tmp.path();
        let input = root.join("input.bin");
        fs::write(&input, b"abracadabra").unwrap();
        let scratch = root.join("scratch");
        fs::create_dir(&scratch).unwrap();
        let tools = root.join("tools");
        fs::create_dir(&tools).unwrap();
        for name in ["scatter_sort", "cross_lcp_merge", "xsa_finish"] {
            fs::write(tools.join(name), name).unwrap();
        }
        let o = parse_chain(
            &[
                "--tape",
                "--input",
                input.to_str().unwrap(),
                "--scratch",
                scratch.to_str().unwrap(),
                "--threads",
                "16",
                "--memory-gb",
                "24",
            ]
            .map(String::from),
        )
        .unwrap();
        (tmp, o, input, scratch, tools)
    }
    #[test]
    fn mismatch_preserves_scratch_and_names_field() {
        let (_t, o, input, scratch, tools) = fixture();
        prepare(&o, &input, &scratch, &tools).unwrap();
        fs::create_dir(scratch.join("bands-tape")).unwrap();
        fs::write(scratch.join("bands-tape/band_0.done"), b"sentinel").unwrap();
        let before = fs::read(scratch.join("checkpoint.journal")).unwrap();
        fs::write(&input, b"different").unwrap();
        assert!(prepare(&o, &input, &scratch, &tools)
            .unwrap_err()
            .contains("field=corpus_sha256"));
        assert_eq!(
            fs::read(scratch.join("checkpoint.journal")).unwrap(),
            before
        );
        assert_eq!(
            fs::read(scratch.join("bands-tape/band_0.done")).unwrap(),
            b"sentinel"
        );
    }
    #[test]
    fn replay_clears_stale_markers_and_unjournalled_pieces() {
        let (_t, o, input, scratch, tools) = fixture();
        prepare(&o, &input, &scratch, &tools).unwrap();
        for d in ["bands-tape", "waves-tape", "merged"] {
            fs::create_dir(scratch.join(d)).unwrap();
            fs::write(scratch.join(d).join("untrusted.partial"), b"stale").unwrap();
        }
        fs::write(scratch.join("bands-tape/band_0.consumed"), b"").unwrap();
        fs::write(scratch.join("keep-user-file"), b"preserve").unwrap();
        append(&scratch, "PIECE_DURABLE idx=0 sha=historical").unwrap();
        assert!(!prepare(&o, &input, &scratch, &tools).unwrap().complete);
        for d in ["bands-tape", "waves-tape", "merged"] {
            assert!(!scratch.join(d).exists());
        }
        assert!(scratch.join("keep-user-file").exists());
        assert!(fs::read_to_string(scratch.join("checkpoint.journal"))
            .unwrap()
            .contains("REPLAY from_key=0"));
    }
    #[test]
    fn completed_container_is_verified_and_not_rebuilt() {
        let (_t, o, input, scratch, tools) = fixture();
        prepare(&o, &input, &scratch, &tools).unwrap();
        let out = scratch.join("input.sxi2");
        fs::write(&out, b"test-container").unwrap();
        done(&scratch, &out).unwrap();
        fs::create_dir(scratch.join("bands-tape")).unwrap();
        fs::write(scratch.join("bands-tape/left-after-commit"), b"stale").unwrap();
        assert!(prepare(&o, &input, &scratch, &tools).unwrap().complete);
        assert!(!scratch.join("bands-tape").exists());
        fs::write(out, b"corrupted").unwrap();
        assert!(prepare(&o, &input, &scratch, &tools)
            .unwrap_err()
            .contains("field=container_sha256"));
    }
    #[test]
    fn no_resume_replaces_plan_and_preserves_unowned_files() {
        let (_t, mut o, input, scratch, tools) = fixture();
        prepare(&o, &input, &scratch, &tools).unwrap();
        fs::write(scratch.join("keep"), b"keep").unwrap();
        fs::write(&input, b"changed").unwrap();
        o.no_resume = true;
        assert!(!prepare(&o, &input, &scratch, &tools).unwrap().complete);
        assert!(scratch.join("keep").exists());
        assert_eq!(
            fs::read_to_string(scratch.join("checkpoint.journal"))
                .unwrap()
                .lines()
                .count(),
            1
        );
    }
    #[test]
    fn tool_change_refuses_before_cleanup() {
        let (_t, o, input, scratch, tools) = fixture();
        prepare(&o, &input, &scratch, &tools).unwrap();
        fs::write(tools.join("scatter_sort"), b"new-binary").unwrap();
        assert!(prepare(&o, &input, &scratch, &tools)
            .unwrap_err()
            .contains("field=sorter_sha"));
    }
}
