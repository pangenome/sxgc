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

use std::collections::HashSet;
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
        // The two in-flight spellings of a published file (`X.partial`, `X.part`)
        // are the SAME family as the published name: the finish renames, and the
        // ctx-tap writes `trie-m20.bin.partial` before it renames. Classifying the
        // transient spelling separately is how a family goes unaccounted.
        let base = name.strip_suffix(".partial").or_else(|| name.strip_suffix(".part"))
            .unwrap_or(name.as_str()).to_string();
        // THE PRODUCT: the container and its in-flight spellings. It is on this
        // filesystem and it must be in the ceiling (incident B's missing term),
        // even though the tape budget itself excludes it.
        if base.ends_with(".sxi2") { return Some(Term::Container); }
        let named = ["remap-256.bin", "checkpoint.journal", "trie-m20.bin", "stop.requested",
                     "xsa-build.log", "caller.txt"];
        if named.contains(&base.as_str()) { return Some(Term::Other); }
        if base.ends_with(".log") || base.ends_with(".time") { return Some(Term::Other); }
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
///
/// HARD LINKS ARE COUNTED ONCE (dev+ino), exactly as `du` counts them: the finish
/// links the container beside itself for the in-place m20/m15 rewrite, so for a
/// moment two names hold one inode. Counting both would over-state the disk
/// footprint by a whole container and break the S1 identity with `du`.
pub fn sample(root: &Path) -> Sample {
    const MAX_EXAMPLES: usize = 6;
    use std::os::unix::fs::MetadataExt;
    let mut seen: HashSet<(u64, u64)> = HashSet::new();
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
            if md.nlink() > 1 && !seen.insert((md.dev(), md.ino())) { continue; }
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
///
/// CLAMPED TO HALF THE FILESYSTEM: on a 100 GB scratch device the literal 500 GB
/// floor would be a ceiling of ZERO, i.e. a build that cannot take its first
/// byte. The clamp is journaled (`reserve_clamped=1`) and it never touches the
/// big device the rule was written for (10% of it is already the larger arm).
pub fn default_reserve(fs_total: u64) -> u64 {
    std::cmp::max(fs_total / 10, 500u64 << 30).min(fs_total / 2)
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
            "the new bound {} B must be ABOVE the live number that actually occurred {} B \
             (the old ceiling was {} B, and the kill was a surprise)", c.bytes, live_actual, old_ceiling);
        // WITH THE RECOMMENDED KNOBS (pos 20 GB, not 2 TB) the bound sits far BELOW
        // the number the old governor blew at: the run is stopped at ~0.8 TB, not
        // 2.3 TB. THIS is the number to relaunch the pile with.
        let rec_budgets = [gb(20.0), bands, merged, pieces, container, other];
        let rec_knob: u64 = rec_budgets.iter().sum();
        let rec = derive_ceiling(free_at_start, reserve, &rec_budgets, measured_slack(rec_knob));
        eprintln!("INCIDENT_B recommended pos_gb=20 bound_gb={:.3} live_actual_gb={:.3}",
            rec.bytes as f64 / GIB, live_actual as f64 / GIB);
        assert!(rec.bytes < live_actual,
            "with the recommended knobs the bound ({}) must land BELOW the live number that \
             actually occurred ({live_actual} B): the build is refused/stopped early, not 2.3 TB in",
            rec.bytes);
    }

    /// THE THIRD CASE, from the pile 30 minutes after the second incident (main's
    /// binary, 2026-11-14 ~14:30):
    ///
    ///   pos_budget 100 GB, --scratch-budget-gb 250, margin 50 (constant)
    ///   ceiling = 422,122,547,200 B = pos + budget + margin (the merged term absent)
    ///   live    = 423,759,282,176 B = positions + bands + merged 339 GB
    ///   over by 1,636,734,976 B, killed at t+57 min right after the first position
    ///   window opened.
    ///
    /// The merged-fragment term alone (339 GB, growing toward the container) broke
    /// it, by a hair. The fix must predict it.
    #[test]
    fn third_incident_the_merged_term_alone_is_what_breaks_it() {
        let pos = 100_000_000_000u64;                  // 100 GB, decimal (the knob)
        let tape_budget = 250u64 << 30;                // --scratch-budget-gb 250
        let margin = 50u64 << 30;
        let merged = 339_000_000_000u64;               // measured fragment family
        let container = 0u64;                          // not yet published
        let bands = tape_budget * super::super::plan::TAPE_BAND_SHARE_PCT / 100;
        let pieces = tape_budget - bands;
        let old_ceiling = pos + tape_budget + margin;  // 422,122,547,200
        assert_eq!(old_ceiling, 422_122_547_200,
            "the old formula must reproduce the journaled ceiling exactly");
        let live_actual = 423_759_282_176u64;
        let budgets = [pos, bands, merged, pieces, container, margin];
        let knob_arm: u64 = budgets.iter().sum();
        let slack = measured_slack(knob_arm);
        let free_at_start = (5700u64) << 30;           // 5.7 TB free at the pile
        let reserve = default_reserve(14u64 << 40);
        let c = derive_ceiling(free_at_start, reserve, &budgets, slack);
        eprintln!(
            "INCIDENT_3 old_ceiling_bytes={old_ceiling} live_actual_bytes={live_actual} \
             over_by_bytes={} new_ceiling_bytes={} new_ceiling_gb={:.3} arm={} knob_arm_gb={:.3} \
             fs_arm_gb={:.3} slack_gb={:.3}",
            live_actual - old_ceiling, c.bytes, c.bytes as f64 / GIB, c.arm.as_str(),
            c.knob_arm as f64 / GIB, c.fs_arm as f64 / GIB, c.slack as f64 / GIB);
        assert!(c.bytes > live_actual,
            "the new ceiling {} B must be ABOVE the live number that occurred {} B (the old one, \
             {} B, was below it: that is the surprise)", c.bytes, live_actual, old_ceiling);
        // THE PRINTED NUMBER IS THE CONTRACT: with the target at its default the
        // runtime ceiling IS the projected live peak, so the plan's number and the
        // runtime's number are the same number.
        assert_eq!(c.bytes, c.knob_arm,
            "with the default target the ceiling IS the projected live peak (term_sum + slack)");
        assert_eq!(c.knob_arm, knob_arm + slack);
        assert_eq!(c.arm, Arm::Knobs, "5.7 TB free - reserve is not the binding arm here");
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
            ("trie-m20.bin.partial", Some(Term::Other)),
            ("trie-m20.bin.part", Some(Term::Other)),
            ("order-tape.log.partial", Some(Term::Other)),
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

    /// A second NAME for one inode occupies one inode of space: the finish links
    /// the container beside itself for the in-place member rewrite, so this is the
    /// exact case that must not double-count (S1 compares against `du`, which
    /// de-duplicates hard links).
    #[test]
    fn hard_links_are_counted_once() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        fs::write(root.join("a.sxi2"), vec![7u8; 65536]).unwrap();
        fs::hard_link(root.join("a.sxi2"), root.join("a.sxi2.partial")).unwrap();
        let s = sample(root);
        eprintln!("HARDLINK {}", s.terms_line());
        assert_eq!(s.files, 2, "both names are visited");
        assert!(s.term(Term::Container) < 65536 * 2, "one inode, one charge: {}", s.term(Term::Container));
        assert!(s.term(Term::Container) >= 65536);
    }

    #[test]
    fn fs_space_reads_a_real_filesystem() {
        let s = fs_space(Path::new("/")).unwrap();
        eprintln!("FS_SPACE total_bytes={} free_bytes={}", s.total, s.free);
        assert!(s.total > 0 && s.free > 0 && s.free <= s.total);
    }
}
