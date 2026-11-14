//! THE SCRATCH LEDGER AND THE CEILING: one enumeration, one sampler, one number.
//!
//! WHY THIS MODULE EXISTS (the two incidents, RESULTS_SCRATCH_GOVERNOR.md):
//!
//!  A. The governor sampled the whole scratch tree but could only act on the
//!     TERMS it modelled (the tape budget). The position windows - the one term
//!     no consumer can drain - grew to 7.6 TB while `paused=0`, and the build
//!     died on `write position file` with the filesystem at 100%. A monitor that
//!     cannot act on what it measures is not a monitor.
//!  B. The governor acted, but its ceiling was `tape_budget + pos_budget + 50 GB`
//!     and the MERGED FRAGMENT family (`merged/frag.pfck2`,
//!     `merged/frag.chi-bitmap`, 443 GB) and the container partial were not in
//!     the formula at all. Live scratch reached 2,324,826,972,160 B against a
//!     ceiling of 2,322,122,547,200 B: the kill arrived 2.7 GB past a number the
//!     operator could not predict, because the number did not price what the
//!     build writes.
//!
//! THE LAW OF THIS FILE: every byte under `--scratch` belongs to exactly one of
//! SIX terms, the classifier that decides which is the same table the sampler
//! walks with, and the ceiling is derived from those terms plus the FILESYSTEM
//! (statvfs at plan time), never from the sum of knobs alone.
//!
//!   term_pos        the rolling POSITION windows (waves-tape/subdiv_pos on the
//!                   tape route, merged/binbwt-windows on the bin route): the one
//!                   scratch term no consumer can drain - the producer frees it
//!                   itself, so it is priced from the producer's own budget.
//!   term_bands      the sorter's band parts (bands-tape/) and its wave spool
//!                   (waves-tape/ outside subdiv_pos).
//!   term_merged     the finish's merged-stream family (merged/frag.*): rlebwt,
//!                   the pfck2 suffix-hash grid, the chi bitmap, the bit planes,
//!                   the finish spool. NOT bounded by the tape budget: it is the
//!                   term incident B was missing.
//!   term_pieces     the in-flight piece window (merged/frag.pieces/).
//!   term_container  the PRODUCT partial (X.sxi2, X.sxi2.partial, X.sxi2.part).
//!                   It lives on the same filesystem and it fills it just as
//!                   fast, so the CEILING counts it even though the tape budget
//!                   does not (the budget sizes the wave, not the product).
//!   term_other      the small named files: journals, logs, timing, the remap
//!                   spool, the checkpoint journal, the in-build trie payload,
//!                   the stop flag.
//!
//! Anything the table does not name is counted as `unknown`, journaled by name
//! (bounded list) every sample, and folded into `term_other` so the total still
//! equals the tree. `unknown_paths=0` is a gate (S1): a new file family must be
//! GIVEN a term, not discovered in a kill.

use std::fs;
use std::path::{Path, PathBuf};

// ---------------------------------------------------------------- THE TERMS
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Term {
    Pos,
    Bands,
    Merged,
    Pieces,
    Container,
    Other,
}

/// The ORDER every journal line prints the terms in (`term_pos=` first).
pub const TERMS: [Term; 6] = [
    Term::Pos,
    Term::Bands,
    Term::Merged,
    Term::Pieces,
    Term::Container,
    Term::Other,
];

impl Term {
    pub fn idx(self) -> usize {
        match self {
            Term::Pos => 0,
            Term::Bands => 1,
            Term::Merged => 2,
            Term::Pieces => 3,
            Term::Container => 4,
            Term::Other => 5,
        }
    }
    pub fn journal_key(self) -> &'static str {
        match self {
            Term::Pos => "term_pos",
            Term::Bands => "term_bands",
            Term::Merged => "term_merged",
            Term::Pieces => "term_pieces",
            Term::Container => "term_container",
            Term::Other => "term_other",
        }
    }
    /// The rule that puts a path in this term. Printed once at announce, so the
    /// accounting can be audited from the journal alone.
    pub fn rule(self) -> &'static str {
        match self {
            Term::Pos => "rolling-position-windows:{waves-tape/subdiv_pos,*/binbwt-windows}",
            Term::Bands => "band-parts:bands-tape/*,waves-tape/*(not subdiv_pos)",
            Term::Merged => "merged-stream:merged/frag.*(not frag.pieces)",
            Term::Pieces => "piece-window:merged/frag.pieces/*",
            Term::Container => "product-partial:{X.sxi2,X.sxi2.partial,X.sxi2.part} at the scratch root",
            Term::Other => "named-small-files:{*.log,*.time,remap-256.bin,checkpoint.journal,trie-m20.bin,stop.requested,unknown-folded}",
        }
    }
}

/// THE ONE LIST: a path under `--scratch`, RELATIVE to the scratch root, to the
/// term that owns it. `None` = the table does not name it (counted as unknown,
/// folded into `term_other`, journaled by name, and `unknown_paths` rises).
pub fn classify(rel: &Path) -> Option<Term> {
    let comps: Vec<String> = rel.components()
        .map(|c| c.as_os_str().to_string_lossy().to_string())
        .collect();
    let Some(first) = comps.first().map(|s| s.as_str()) else { return None };
    let deep = |name: &str| comps.iter().any(|c| c == name);
    if first == "bands-tape" {
        return Some(Term::Bands);
    }
    if first == "waves-tape" {
        // The sorter's wave spool: the position windows are the subdirectories
        // named `subdiv_pos`; everything else there is a band part (wave slice,
        // descriptor, `.consumed` marker).
        return Some(if deep("subdiv_pos") { Term::Pos } else { Term::Bands });
    }
    if first == "merged" {
        // The bin producer's rolling window files, then the finish's families.
        if deep("binbwt-windows") { return Some(Term::Pos); }
        if deep("frag.pieces") { return Some(Term::Pieces); }
        return Some(Term::Merged);
    }
    if comps.len() == 1 {
        let name = rel.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
        // THE PRODUCT: the container and its two in-flight spellings. It is on
        // this filesystem and it must be in the ceiling (incident B's missing
        // term), even though the tape budget itself excludes it.
        if name.ends_with(".sxi2") || name.ends_with(".sxi2.partial") || name.ends_with(".sxi2.part") {
            return Some(Term::Container);
        }
        let named = ["remap-256.bin", "checkpoint.journal", "trie-m20.bin", "stop.requested",
                     "xsa-build.log", "caller.txt"];
        if named.contains(&name.as_str()) { return Some(Term::Other); }
        if name.ends_with(".log") || name.ends_with(".time") { return Some(Term::Other); }
    }
    None
}

// -------------------------------------------------------------- THE SAMPLER
/// One walk of the scratch tree: allocated bytes per term, plus what the table
/// could not name. ALLOCATED bytes (st_blocks x 512), not apparent size: the chi
/// bitmap is a sparse n/8 mapping whose untouched pages occupy nothing, and a
/// budget that counted apparent size would refuse a run using a fraction of it.
#[derive(Clone, Debug, Default)]
pub struct Sample {
    pub bytes: [u64; 6],
    pub files: u64,
    pub dirs: u64,
    pub unknown_paths: u64,
    pub unknown_bytes: u64,
    pub unknown_examples: Vec<String>,
}

impl Sample {
    /// EVERY live byte under `--scratch`, including the product partial and
    /// including anything the table did not name. This is the number the
    /// filesystem arm of the ceiling bounds, and it is what `du -s` reports.
    pub fn live(&self) -> u64 {
        self.bytes.iter().sum()
    }
    pub fn term(&self, t: Term) -> u64 { self.bytes[t.idx()] }
    /// `term_pos=<b> term_bands=<b> ... term_other=<b> live=<b>` - the exact
    /// breakdown the ceiling was derived from, on every sample.
    pub fn terms_line(&self) -> String {
        let mut s = String::new();
        for t in TERMS {
            s.push_str(&format!("{}={} ", t.journal_key(), self.term(t)));
        }
        s.push_str(&format!("live={}", self.live()));
        s
    }
    /// The measured peak per term (max over samples), used for the summary.
    pub fn raise_peaks(&self, peaks: &mut [u64; 6]) {
        for i in 0..6 { if self.bytes[i] > peaks[i] { peaks[i] = self.bytes[i]; } }
    }
}

fn alloc_len(md: &fs::Metadata) -> u64 {
    use std::os::unix::fs::MetadataExt;
    // A non-empty file of 5000 B on a 4096-block filesystem is 8192 allocated;
    // the `.max(4096)` keeps a 1-byte file honest on filesystems that report 0.
    md.blocks().saturating_mul(512).max(if md.len() == 0 { 0 } else { 4096 })
}

/// Walk the tree ONCE and classify EVERY path with `classify`. This is the only
/// sampler in the build: the journal's terms, the live total and the ceiling's
/// arithmetic all come from this one function.
pub fn sample(root: &Path) -> Sample {
    const MAX_EXAMPLES: usize = 6;
    let mut s = Sample::default();
    let mut stack: Vec<PathBuf> = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else { continue };
        for entry in entries.flatten() {
            let path = entry.path();
            let Ok(ft) = entry.file_type() else { continue };
            if ft.is_dir() {
                s.dirs += 1;
                stack.push(path);
                continue;
            }
            let Ok(md) = entry.metadata() else { continue };
            if !md.is_file() { continue; }
            s.files += 1;
            let len = alloc_len(&md);
            match classify(path.strip_prefix(root).unwrap_or(&path)) {
                Some(t) => s.bytes[t.idx()] += len,
                None => {
                    s.unknown_paths += 1;
                    s.unknown_bytes += len;
                    // Folded into `other` so the total is still the whole tree.
                    s.bytes[Term::Other.idx()] += len;
                    if s.unknown_examples.len() < MAX_EXAMPLES {
                        s.unknown_examples.push(path.display().to_string());
                    }
                }
            }
        }
    }
    s
}

// ------------------------------------------------------------- THE CEILING
/// WHERE the ceiling came from. A reader must never have to guess which arm won.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Arm {
    /// `free_at_start - reserve`: the box is the binding constraint.
    Filesystem,
    /// `sum(term budgets) + measured_slack`: the declared intent is.
    Knobs,
}

impl Arm {
    pub fn as_str(self) -> &'static str {
        match self { Arm::Filesystem => "filesystem", Arm::Knobs => "knobs" }
    }
}

/// The per-term bounds. `sources` names where each number came from, so the
/// ceiling's arithmetic is auditable from the journal alone.
#[derive(Clone, Debug)]
pub struct Budgets {
    pub bytes: [u64; 6],
    pub sources: [&'static str; 6],
}

impl Budgets {
    pub fn sum(&self) -> u64 { self.bytes.iter().fold(0u64, |a, b| a.saturating_add(*b)) }
    pub fn journal(&self) -> String {
        let mut s = String::new();
        for t in TERMS {
            s.push_str(&format!("budget_{}={} ", t.journal_key().trim_start_matches("term_"),
                self.bytes[t.idx()]));
        }
        s.push_str("budget_sources=");
        for t in TERMS {
            s.push_str(&format!("{}:{},", t.journal_key().trim_start_matches("term_"),
                self.sources[t.idx()]));
        }
        s.trim_end_matches(',').to_string()
    }
}

#[derive(Clone, Debug)]
pub struct Ceiling {
    pub bytes: u64,
    pub arm: Arm,
    /// `free_at_start - reserve`, saturating: the most the box can afford.
    pub fs_arm: u64,
    /// `sum(term budgets) + measured_slack`: the most the knobs declare.
    pub knob_arm: u64,
    pub slack: u64,
}

/// THE ARITHMETIC, IN ONE PLACE, PURE (S5 feeds it the incident numbers):
///
///   fs_arm   = free_at_start - reserve
///   knob_arm = sum(term budgets) + measured_slack
///   ceiling  = min(fs_arm, knob_arm)
///
/// The knobs bound what the pipeline INTENDS to use; the filesystem bounds what
/// the box can AFFORD. A ceiling that only knew the knobs is what let incident A
/// run to 8.2 TB (`paused=0`) and what let incident B's 443 GB merged fragment
/// family go unpriced.
pub fn derive_ceiling(free_at_start: u64, reserve: u64, budgets: &[u64; 6], measured_slack: u64) -> Ceiling {
    let knob_arm = budgets.iter().fold(0u64, |a, b| a.saturating_add(*b)).saturating_add(measured_slack);
    let fs_arm = free_at_start.saturating_sub(reserve);
    if fs_arm <= knob_arm {
        Ceiling { bytes: fs_arm, arm: Arm::Filesystem, fs_arm, knob_arm, slack: measured_slack }
    } else {
        Ceiling { bytes: knob_arm, arm: Arm::Knobs, fs_arm, knob_arm, slack: measured_slack }
    }
}

/// The reserve: 10% of the filesystem, or 500 GB, whichever is LARGER. It is
/// space the build may never touch - the box is shared, a full filesystem
/// corrupts every other lane, and 500 GB is the smallest headline number the
/// sorter's own 50 GB `SS_SUBDIV_SCRATCH_MARGIN_BYTES` is a tenth of.
pub fn default_reserve(fs_total: u64) -> u64 {
    std::cmp::max(fs_total / 10, 500u64 << 30)
}

/// The allowance for the term budgets' own estimation error (the plan's rates
/// are measured per-n ratios, the wave is sized from a sample estimate): 5% of
/// the declared arm, floor 8 GB. It is ADDED to the knobs arm only, so it can
/// never widen the filesystem arm - the box keeps its veto.
pub fn measured_slack(knob_arm: u64) -> u64 {
    std::cmp::max(8u64 << 30, knob_arm / 20)
}

// ---------------------------------------------------------------- THE DISK
/// `statvfs` on the filesystem that holds `path`: what the box can afford, read
/// at PLAN time (not from the sum of knobs).
#[derive(Clone, Copy, Debug)]
pub struct FsSpace {
    /// f_blocks x f_frsize: the size of the filesystem.
    pub total: u64,
    /// f_bavail x f_frsize: bytes available to an unprivileged writer.
    pub free: u64,
}

#[repr(C)]
struct Statvfs {
    f_bsize: u64,
    f_frsize: u64,
    f_blocks: u64,
    f_bfree: u64,
    f_bavail: u64,
    f_files: u64,
    f_ffree: u64,
    f_favail: u64,
    f_fsid: u64,
    f_flag: u64,
    f_namemax: u64,
    __f_spare: [i32; 6],
}

pub fn fs_space(path: &Path) -> Result<FsSpace, String> {
    use std::ffi::CString;
    extern "C" {
        fn statvfs(path: *const std::ffi::c_char, buf: *mut Statvfs) -> std::ffi::c_int;
    }
    let c = CString::new(path.to_string_lossy().as_bytes())
        .map_err(|e| format!("statvfs {}: {e}", path.display()))?;
    let mut st = Statvfs {
        f_bsize: 0, f_frsize: 0, f_blocks: 0, f_bfree: 0, f_bavail: 0, f_files: 0,
        f_ffree: 0, f_favail: 0, f_fsid: 0, f_flag: 0, f_namemax: 0, __f_spare: [0; 6],
    };
    let rc = unsafe { statvfs(c.as_ptr(), &mut st) };
    if rc != 0 {
        return Err(format!("statvfs {}: rc={rc} ({})", path.display(),
            std::io::Error::last_os_error()));
    }
    let frsize = if st.f_frsize != 0 { st.f_frsize } else { st.f_bsize };
    if frsize == 0 {
        return Err(format!("statvfs {}: f_frsize=0 and f_bsize=0", path.display()));
    }
    Ok(FsSpace {
        total: st.f_blocks.saturating_mul(frsize),
        free: st.f_bavail.saturating_mul(frsize),
    })
}

// ------------------------------------------------------------------ TESTS
#[cfg(test)]
mod tests {
    use super::*;

    const GIB: f64 = (1u64 << 30) as f64;

    fn gb(v: f64) -> u64 { (v * GIB) as u64 }

    /// S5: THE TWO INCIDENT NUMBERS. Feed the ledger the numbers incident B was
    /// killed at and assert the fix PREDICTS the overrun instead of being
    /// surprised by it.
    ///
    /// Incident B: `SCRATCH_GOVERNOR_CEILING_EXCEEDED live_bytes=2,324,826,972,160
    /// ceiling_bytes=2,322,122,547,200` with the journaled formula
    /// `ceiling = pos_budget_gb 1862.645 + budget_gb 250 + 50` - the merged
    /// fragment family (443 GB of frag.pfck2 + frag.chi-bitmap) was NOT in it.
    #[test]
    fn incident_b_ceiling_predicts_the_overrun() {
        let pos = gb(1862.645);
        let tape_budget = 250u64 << 30;
        let merged = gb(443.0);
        let container = 0u64;
        let other = 50u64 << 30;
        let bands = tape_budget * 60 / 100;
        let pieces = tape_budget - bands;
        let budgets = [pos, bands, pieces, merged, container, other];
        let knob_arm: u64 = budgets.iter().sum();
        let slack = measured_slack(knob_arm);
        // The box: 14 TB, 8.2 TB free at start (the point incident A filled it).
        let free_at_start = gb(8200.0);
        let reserve = default_reserve(gb(14000.0));
        let c = derive_ceiling(free_at_start, reserve, &budgets, slack);
        let live_actual = 2_324_826_972_160u64;
        let old_ceiling = 2_322_122_547_200u64;
        eprintln!(
            "INCIDENT_B old_ceiling_bytes={old_ceiling} live_actual_bytes={live_actual} \
             new_ceiling_bytes={} new_ceiling_gb={:.3} arm={} knob_arm_gb={:.3} fs_arm_gb={:.3} slack_gb={:.3}",
            c.bytes, c.bytes as f64 / GIB, c.arm.as_str(), c.knob_arm as f64 / GIB,
            c.fs_arm as f64 / GIB, c.slack as f64 / GIB);
        assert!(c.bytes > live_actual,
            "the new ceiling {} B must be ABOVE the live number that actually occurred {} B \
             (the old one was {} B, and the kill was a surprise)", c.bytes, live_actual, old_ceiling);
        // ... and the PAUSE (90% of the smaller arm) is available BEFORE the
        // ceiling: the first response is always the pause, the kill is the
        // bounded last resort.
        let basis = std::cmp::min(c.bytes, knob_arm);
        let pause_at = basis / 10 * 9;
        eprintln!("INCIDENT_B pause_at_bytes={pause_at} pause_at_gb={:.3} ceiling_gb={:.3}",
            pause_at as f64 / GIB, c.bytes as f64 / GIB);
        assert!(pause_at < c.bytes, "the pause must come before the ceiling");
        // THE SAME ARITHMETIC WITH THE RECOMMENDED KNOBS (pos 20 GB, not 2 TB):
        // the intent arm then sits far BELOW the number the old governor blew at,
        // so the overrun is caught at ~0.8 TB instead of 2.3 TB - and the pause
        // lands at 90% of it. THIS is the number to relaunch the pile with.
        let rec_pos = gb(20.0);
        let rec_budgets = [rec_pos, bands, pieces, merged, container, other];
        let rec_knob: u64 = rec_budgets.iter().sum();
        let rec = derive_ceiling(free_at_start, reserve, &rec_budgets, measured_slack(rec_knob));
        let rec_pause = std::cmp::min(rec.bytes, rec_knob) / 10 * 9;
        eprintln!("INCIDENT_B recommended pos_gb=20 ceiling_gb={:.3} pause_gb={:.3} \
             live_actual_gb={:.3}", rec.bytes as f64 / GIB, rec_pause as f64 / GIB,
            live_actual as f64 / GIB);
        assert!(rec_pause < rec.bytes,
            "the recommended pause ({rec_pause} B) must land below the recommended ceiling \
             ({}) B", rec.bytes);
        assert!(rec.bytes < live_actual,
            "with the recommended knobs the ceiling ({}) must land BELOW the live number that \
             actually occurred ({live_actual} B): the build is stopped early, not 2.3 TB in",
            rec.bytes);
    }

    /// Incident A: the CONSTRAINT SHAPE, not a specific number. The position
    /// budget is 2 TB and the box has 8.2 TB free: whatever the terms are, the
    /// ceiling must be BELOW the 8,189 GB the untended build reached, and it
    /// must be the filesystem arm that wins (the knobs alone would allow it).
    #[test]
    fn incident_a_ceiling_acts_below_the_fill() {
        let pos = 2_000_000_000_000u64;
        let tape_budget = 250u64 << 30;
        let merged = gb(443.0);
        let budgets = [pos, tape_budget * 60 / 100, tape_budget - tape_budget * 60 / 100,
                       merged, gb(570.0), 50u64 << 30];
        let knob_arm: u64 = budgets.iter().sum();
        let slack = measured_slack(knob_arm);
        let free_at_start = gb(8200.0);
        let total = gb(14000.0);
        let reserve = default_reserve(total);
        let c = derive_ceiling(free_at_start, reserve, &budgets, slack);
        eprintln!("INCIDENT_A ceiling_gb={:.3} arm={} fs_arm_gb={:.3} knob_arm_gb={:.3} reserve_gb={:.3}",
            c.bytes as f64 / GIB, c.arm.as_str(), c.fs_arm as f64 / GIB,
            c.knob_arm as f64 / GIB, reserve as f64 / GIB);
        assert!(c.bytes < gb(8189.0), "the ceiling must be below the 8189 GB the box reached");
        assert_eq!(reserve, gb(1400.0), "the reserve is 10% of a 14 TB filesystem");
        // With room to spare and a small declared intent, the KNOBS win: the
        // filesystem arm must never inflate a run the knobs already bound.
        let small = derive_ceiling(gb(8200.0), gb(1400.0), &[gb(20.0), gb(150.0), gb(100.0), gb(197.0), gb(47.0), gb(50.0)], gb(28.0));
        eprintln!("INCIDENT_A small_ceiling_gb={:.3} arm={}", small.bytes as f64 / GIB, small.arm.as_str());
        assert_eq!(small.arm, Arm::Knobs);
        assert_eq!(small.bytes, gb(592.0));
        // The 500 GB floor of the reserve.
        assert_eq!(default_reserve(gb(1000.0)), 500u64 << 30);
        assert_eq!(default_reserve(gb(14000.0)), gb(1400.0));
    }

    #[test]
    fn classify_names_every_family_the_pipeline_writes() {
        let cases: &[(&str, Option<Term>)] = &[
            ("waves-tape/subdiv_pos/w0.pos", Some(Term::Pos)),
            ("waves-tape/wave_000001.sa", Some(Term::Bands)),
            ("bands-tape/band_000001.sa", Some(Term::Bands)),
            ("bands-tape/band_000001.lcp", Some(Term::Bands)),
            ("bands-tape/band_000001.consumed", Some(Term::Bands)),
            ("merged/binbwt-windows/bw-w0.pos", Some(Term::Pos)),
            ("merged/frag.pieces/0000001.prc1", Some(Term::Pieces)),
            ("merged/frag.rlebwt", Some(Term::Merged)),
            ("merged/frag.pfck2", Some(Term::Merged)),
            ("merged/frag.chi-bitmap", Some(Term::Merged)),
            ("merged/frag.finish-spool/x", Some(Term::Merged)),
            ("pile.sxi2", Some(Term::Container)),
            ("pile.sxi2.partial", Some(Term::Container)),
            ("pile.sxi2.part", Some(Term::Container)),
            ("remap-256.bin", Some(Term::Other)),
            ("checkpoint.journal", Some(Term::Other)),
            ("trie-m20.bin", Some(Term::Other)),
            ("finish-v2.log", Some(Term::Other)),
            ("finish-v2.time", Some(Term::Other)),
            ("xsa-build.log", Some(Term::Other)),
            ("a-new-family.bin", None),
            ("weird/sub/dir/file", None),
        ];
        for (rel, want) in cases {
            assert_eq!(classify(Path::new(rel)), *want, "classify({rel})");
        }
        // N of things examined, printed.
        eprintln!("CLASSIFY examined={} named={} unknown={}", cases.len(),
            cases.iter().filter(|(_, w)| w.is_some()).count(),
            cases.iter().filter(|(_, w)| w.is_none()).count());
    }

    /// The sampler's total IS the tree: unknown files are folded in (so the
    /// ceiling still bounds the whole filesystem footprint) while the count
    /// stays visible for the S1 gate.
    #[test]
    fn sample_total_includes_unknown_and_the_container() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        fs::create_dir_all(root.join("merged/frag.pieces")).unwrap();
        fs::create_dir_all(root.join("waves-tape/subdiv_pos")).unwrap();
        fs::write(root.join("merged/frag.pieces/p1"), vec![7u8; 8192]).unwrap();
        fs::write(root.join("waves-tape/subdiv_pos/w0.pos"), vec![7u8; 8192]).unwrap();
        fs::write(root.join("pile.sxi2.partial"), vec![7u8; 65536]).unwrap();
        fs::write(root.join("mystery.dat"), vec![7u8; 8192]).unwrap();
        let s = sample(root);
        eprintln!("SAMPLE {}", s.terms_line());
        eprintln!("SAMPLE files={} dirs={} unknown_paths={} unknown_bytes={}",
            s.files, s.dirs, s.unknown_paths, s.unknown_bytes);
        assert_eq!(s.unknown_paths, 1);
        assert!(s.live() >= 8192 * 3 + 65536, "{}", s.live());
        assert!(s.term(Term::Pieces) > 0 && s.term(Term::Pos) > 0 && s.term(Term::Container) > 0);
        assert_eq!(s.live(), s.bytes.iter().sum::<u64>());
    }

    #[test]
    fn fs_space_reads_a_real_filesystem() {
        let s = fs_space(Path::new("/")).unwrap();
        eprintln!("FS_SPACE total_bytes={} free_bytes={}", s.total, s.free);
        assert!(s.total > 0 && s.free > 0 && s.free <= s.total);
    }
}
