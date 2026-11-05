//! Recovery for tape builds: THE FILESYSTEM IS THE JOURNAL.
//!
//! There is no progress journal. Every intermediate of the tape route is a
//! SELF-DESCRIBING SPOOL (bit6/spool.h): its name encodes its place in the
//! deterministic plan, its 176-byte header carries the plan identity, the plan
//! position, the row/run counts and the SHA-256 of its payload, and it is
//! published atomically (`<name>.part` -> fdatasync -> hash -> rename ->
//! fsync(dir)). A spool is deleted only when the state that consumed it is
//! itself durable, so its ABSENCE means "already incorporated".
//!
//! Restart therefore reads the filesystem: it derives the plan from the corpus +
//! knobs (the corpus digest, the remap, the tool hashes and the knob digest are
//! all in the PLAN record and are re-checked), enumerates the spools that are
//! present, verifies each against the plan, and recomputes exactly the ones that
//! are missing. Nothing is silently skipped: the two producer stages refuse a
//! spool whose header does not match this plan, and the driver reports the exact
//! counts (`RESUME_STATE`) and the rows the restart did NOT recompute
//! (`WORK_AVOIDED`).
//!
//! What cannot be resumed this way is named in RESUME.md: the finish accumulates
//! PREFIX-DEPENDENT state (the m1 codec bit positions, the chi bitmap, the m13
//! strides and the in-memory directories), so its spool contents are not local
//! to their position and it re-runs. The container also cannot be appended
//! across producer pieces (RESUME_DESIGN.md, member table), so resumption lives
//! at the spool layer and the container is assembled once, at the end.
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

/// The spool census of a scratch tree, as the plan expects it.
///
/// `bands`   - scatter_sort band spools (`bands-tape/band_<i>.sa`) and the LCP
///             slices that record the same band's rank range.
/// `pieces`  - the merge's piece stream (`merged/*.pieces/p-<band>-<shard>`).
///
/// A present spool is not yet a verified spool: the stages verify every header
/// (plan identity, position, size, payload hash) before they adopt one, and a
/// failed verification recomputes that spool from the corpus.
pub(super) struct Census {
    pub bands_present: u64,
    pub lcp_present: u64,
    pub pieces_present: u64,
    /// Band spools the sorter will not produce again.
    pub bands_pending: u64,
    pub rows_retained: u64,
}

fn band_index(name: &str, suffix: &str) -> Option<u64> {
    let stem = name.strip_prefix("band_")?.strip_suffix(suffix)?;
    stem.parse::<u64>().ok()
}

pub(super) fn census(scratch: &Path) -> Result<Census, String> {
    let mut c = Census {
        bands_present: 0,
        lcp_present: 0,
        pieces_present: 0,
        bands_pending: 0,
        rows_retained: 0,
    };
    if let Ok(entries) = fs::read_dir(scratch.join("bands-tape")) {
        let mut sa: std::collections::BTreeSet<u64> = Default::default();
        let mut lcp: std::collections::BTreeSet<u64> = Default::default();
        let mut tomb: std::collections::BTreeSet<u64> = Default::default();
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if let Some(i) = band_index(&name, ".sa") {
                sa.insert(i);
            } else if let Some(i) = band_index(&name, ".lcp") {
                lcp.insert(i);
            } else if let Some(i) = band_index(&name, ".consumed") {
                tomb.insert(i);
            }
        }
        // A band is DONE (and will not be recomputed) iff its LCP slice is
        // present AND (its SA spool is present OR it was consumed into a durable
        // piece stream). This is exactly the sorter's own adoption rule.
        for i in &lcp {
            if sa.contains(i) || tomb.contains(i) {
                c.bands_pending += 1;
            }
        }
        c.bands_present = sa.len() as u64;
        c.lcp_present = lcp.len() as u64;
    }
    if let Ok(entries) = fs::read_dir(scratch.join("merged")) {
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if !name.ends_with(".pieces") {
                continue;
            }
            if let Ok(pieces) = fs::read_dir(scratch.join("merged").join(&name)) {
                for p in pieces.flatten() {
                    let n = p.file_name().to_string_lossy().into_owned();
                    if n.starts_with("p-") && !n.ends_with(".partial") {
                        c.pieces_present += 1;
                    }
                }
            }
        }
    }
    Ok(c)
}

/// The plan identity every spool carries: SHA-256 of the PLAN record itself, so
/// a spool produced under a different corpus, remap, tool binary or knob set can
/// never be adopted. Truncated to 32 bytes (the header field) by construction.
pub(super) struct Plan {
    pub record: String,
    pub id_hex: String,
}

pub(super) fn plan(
    o: &Options,
    input: &Path,
    scratch: &Path,
    tools: &Path,
) -> Result<Plan, String> {
    let n = fs::metadata(input).map_err(|e| e.to_string())?.len();
    let p = super::super::plan::tape_band_plan(
        n + 1,
        o.threads as u64,
        o.memory_gb,
        std::env::var("XSA_LCE_VERIFY").as_deref() != Ok("1"),
        2 * (n + 2),
    );
    let corpus = sha256_file(input)?;
    let table = if let Some(path) = &o.remap {
        fs::read(path).map_err(|e| e.to_string())?
    } else {
        derive_remap(input)?.0.to_vec()
    };
    let remap = format!("{:x}", Sha256::digest(&table));
    let knobs = std::env::vars()
        .filter(|(k, _)| k.starts_with("CROSS_") || k.starts_with("SS_") || k.starts_with("XSA_"))
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
    let record = format!("PLAN n={n} corpus_sha256={corpus} remap_sha256={remap} rows_per_band={} parallel_bands={} waves={} sorter_sha={} merger_sha={} finish_sha={} driver_sha={} convention=fs-spools-v1 knobs_sha256={knobs_sha} with_chi={} memory_gb={} lce_verify={}",
        p.rows_per_band, o.threads, p.waves,
        sha256_file(&tools.join("scatter_sort"))?,
        sha256_file(&tools.join("cross_lcp_merge"))?,
        sha256_file(&tools.join("xsa_finish"))?,
        sha256_file(&std::env::current_exe().map_err(|e| e.to_string())?)?,
        o.with_chi, o.memory_gb,
        std::env::var("XSA_LCE_VERIFY").unwrap_or_default());
    let id_hex = format!("{:x}", Sha256::digest(record.as_bytes()));
    let _ = scratch;
    Ok(Plan { record, id_hex })
}

/// Prepare a tape build: verify the plan, then ADOPT the spools that are present
/// instead of clearing the tree. Returns `(already_done, resume, plan_id)`.
pub(super) fn prepare(
    o: &Options,
    input: &Path,
    scratch: &Path,
    tools: &Path,
) -> Result<(bool, bool, String), String> {
    let plan = plan(o, input, scratch, tools)?;
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
    let mut resume = false;
    if let Some(old) = &existing {
        if !o.no_resume {
            compare(old.lines().next().unwrap_or(""), &plan.record)?;
            if let Some(done) = old
                .lines()
                .rev()
                .find_map(|l| l.strip_prefix("BUILD_DONE container="))
            {
                if sha256_file(&container)? != done {
                    return Err("RESUME_REFUSED field=container_sha256".into());
                }
                // COMMITTED: the container is durable and verified. Only now may
                // the finished build's spools be deleted.
                for name in ["bands-tape", "waves-tape", "merged", "remap-256.bin", "stop.requested"] {
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
                eprintln!("BUILD_ALREADY_DONE container={done}");
                return Ok((true, false, plan.id_hex));
            }
            resume = true;
        }
    } else if scratch.join("xsa-build.log").exists() && !o.no_resume {
        return Err("RESUME_REFUSED field=PLAN legacy scratch has no recovery journal; use a new scratch or --no-resume".into());
    }
    // No mutation above this point (apart from the caller's ownership lock).
    if o.no_resume {
        for name in ["bands-tape", "waves-tape", "merged", "remap-256.bin", "stop.requested", "xsa-build.log"] {
            remove(&scratch.join(name))?;
        }
        remove(&container)?;
        for suffix in [".part", ".partial"] {
            remove(&PathBuf::from(format!("{}{suffix}", container.display())))?;
        }
        remove(&checkpoint)?;
    } else if resume {
        // ---- THE RESTART READS THE FILESYSTEM -----------------------------
        // Nothing that is a verified spool is deleted here. What IS removed is
        // only what cannot be a spool:
        //   * the uncommitted container (no BUILD_DONE) and its .part,
        //   * the scatter staging (waves-tape: rows re-read from the corpus),
        //   * the finish's OWN scratch in merged/ (the finish re-runs; its
        //     spools are prefix-dependent accumulations, see RESUME.md). The
        //     merge's piece stream inside merged/*.pieces is KEPT.
        remove(&container)?;
        for suffix in [".part", ".partial"] {
            remove(&PathBuf::from(format!("{}{suffix}", container.display())))?;
        }
        remove(&scratch.join("waves-tape"))?;
        remove(&scratch.join("stop.requested"))?;
        if let Ok(entries) = fs::read_dir(scratch.join("merged")) {
            for e in entries.flatten() {
                let name = e.file_name().to_string_lossy().into_owned();
                if name.ends_with(".pieces") {
                    continue;
                }
                remove(&e.path())?;
            }
        }
    }
    if !checkpoint.exists() {
        append(scratch, &plan.record)?;
    } else {
        // A killed append can leave a final unterminated record. Preserve it as
        // an ignored torn record; never concatenate a new record onto it.
        let old = existing.as_ref().unwrap();
        if !old.ends_with('\n') {
            append(scratch, "")?;
        }
        append(
            scratch,
            &format!("RESUME from_spools sorter_sha={} convention=fs-spools-v1", sha256_file(&tools.join("scatter_sort"))?),
        )?;
    }
    Ok((false, resume, plan.id_hex))
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
    let census = census(scratch)?;
    let line = format!(
        "BUILD_INTERRUPTED reason={} spools_present={} lcp_present={} pieces_present={} scratch={}",
        signal(),
        census.bands_present,
        census.lcp_present,
        census.pieces_present,
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
        &format!("BUILD_DONE container={}", sha256_file(container)?),
    )
}
