//! Conservative recovery: replay immutable input, never reuse consumed stages.
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
pub(super) fn prepare(
    o: &Options,
    input: &Path,
    scratch: &Path,
    tools: &Path,
) -> Result<bool, String> {
    let n = fs::metadata(input).map_err(|e| e.to_string())?.len();
    let p = super::super::plan::tape_band_plan(
        n + 1,
        o.threads as u64,
        o.memory_gb,
        std::env::var("XSA_LCE_VERIFY").as_deref() != Ok("1"),
        2 * (n + 2),
    );
    let corpus = sha256_file(input)?;
    if !o.no_resume {
        if let Ok(old) = fs::read_to_string(scratch.join("checkpoint.journal")) {
            if let Some(stored) = old.lines().next().and_then(|l| {
                l.split_whitespace()
                    .find_map(|f| f.strip_prefix("corpus_sha256="))
            }) {
                if stored != corpus {
                    return Err(format!(
                        "RESUME_REFUSED field=corpus_sha256 stored={stored} current={corpus}"
                    ));
                }
            }
        }
    }
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
    let plan=format!("PLAN n={n} corpus_sha256={corpus} remap_sha256={remap} rows_per_band={} parallel_bands={} waves={} sorter_sha={} merger_sha={} finish_sha={} driver_sha={} convention=replay-v1-run-ranks knobs_sha256={knobs_sha} with_chi={} memory_gb={} lce_verify={}",p.rows_per_band,o.threads,p.waves,sha256_file(&tools.join("scatter_sort"))?,sha256_file(&tools.join("cross_lcp_merge"))?,sha256_file(&tools.join("xsa_finish"))?,sha256_file(&std::env::current_exe().map_err(|e|e.to_string())?)?,o.with_chi,o.memory_gb,std::env::var("XSA_LCE_VERIFY").unwrap_or_default());
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
    if let Some(old) = &existing {
        if !o.no_resume {
            compare(old.lines().next().unwrap_or(""), &plan)?;
            if let Some(done) = old
                .lines()
                .rev()
                .find_map(|l| l.strip_prefix("BUILD_DONE container="))
            {
                if sha256_file(&container)? != done {
                    return Err("RESUME_REFUSED field=container_sha256".into());
                }
                // A kill after BUILD_DONE but before normal cleanup must not
                // strand the last wave indefinitely. Verify first, then clean.
                for name in [
                    "bands-tape",
                    "waves-tape",
                    "merged",
                    "remap-256.bin",
                    "stop.requested",
                ] {
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
                return Ok(true);
            }
        }
    } else if scratch.join("xsa-build.log").exists() && !o.no_resume {
        return Err("RESUME_REFUSED field=PLAN legacy scratch has no recovery journal; use a new scratch or --no-resume".into());
    }
    // No mutation above this point (apart from the caller's ownership lock).
    if existing.is_some() || o.no_resume {
        for name in [
            "bands-tape",
            "waves-tape",
            "merged",
            "remap-256.bin",
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
        }
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
    Ok(false)
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
        &format!("BUILD_DONE container={}", sha256_file(container)?),
    )
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn legacy_route_cannot_install_tape_recovery() {
        let args = [
            "--tape",
            "--legacy-merge",
            "--input",
            "corpus",
            "--scratch",
            "scratch",
        ]
        .map(String::from);
        assert!(parse_chain(&args)
            .unwrap_err()
            .contains("mutually exclusive"));
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
        assert!(!prepare(&o, &input, &scratch, &tools).unwrap());
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
        assert!(prepare(&o, &input, &scratch, &tools).unwrap());
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
        assert!(!prepare(&o, &input, &scratch, &tools).unwrap());
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
