// AMENDMENT R: the laws, ported mechanically. DIAL_LAW (asserted),
// SCRATCH_LAW (2xN with per-component receipts), the cooperative
// stop flag (no signals), rows_sorted_total == the phrase count.
use std::fs;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::Instant;

pub static STOP_REQUESTED: AtomicBool = AtomicBool::new(false);

pub fn env_u64(name: &str, dflt: u64) -> u64 {
    std::env::var(name).ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(dflt)
}

pub struct Laws {
    pub stop_flag: String,
    pub dial_bytes: u64,
    pub scratch_budget: u64,
    pub corpus_bytes: u64,
    pub modeb_dir: String,
    pub merged_dir: String,
    pub bands_dir: String,
    peak: Mutex<u64>,
}

pub struct ScratchC { pub pairs: u64, pub columns: u64, pub pieces: u64, pub bands: u64 }

fn du_dir(d: &str, filter: Option<(&str, usize)>, out: &mut u64) {
    let rd = match fs::read_dir(d) { Ok(r) => r, Err(_) => return };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        if name.starts_with('.') { continue; }
        let md = match e.metadata() { Ok(m) => m, Err(_) => continue };
        if md.is_dir() { continue; }
        if let Some((pfx, plen)) = filter {
            if name.as_bytes().get(..plen) != Some(&pfx.as_bytes()[..plen]) {
                continue;
            }
        }
        *out += md.len();
    }
}

impl Laws {
    pub fn new(stop_flag: String, dial: u64, budget: u64, corpus: u64,
               modeb: String, merged: String, bands: String) -> Laws {
        Laws { stop_flag, dial_bytes: dial, scratch_budget: budget,
               corpus_bytes: corpus, modeb_dir: modeb, merged_dir: merged,
               bands_dir: bands, peak: Mutex::new(0) }
    }

    // THE COOPERATIVE STOP FLAG (no signals ever)
    pub fn stop_check(&self, where_: &str) {
        if Path::new(&self.stop_flag).exists() {
            eprintln!("STOP_OBSERVED where={} exit=75", where_);
            std::process::exit(75);
        }
    }

    // THE SCRATCH_LAW: peak live scratch <= the budget at every gate;
    // the components are NAMED (the operator's clause: the fat is
    // named, not summed). The columns are distinct/parse-proportional
    // (legal sublinear), named separately.
    pub fn scratch_components(&self) -> ScratchC {
        let mut c = ScratchC { pairs: 0, columns: 0, pieces: 0, bands: 0 };
        for dir in [self.modeb_dir.clone(), format!("{}/L2", self.modeb_dir)] {
            let rd = match fs::read_dir(dir) { Ok(r) => r, Err(_) => continue };
            for e in rd.flatten() {
                let name = e.file_name().to_string_lossy().to_string();
                if name.starts_with('.') { continue; }
                let md = match e.metadata() { Ok(m) => m, Err(_) => continue };
                if md.is_dir() { continue; }
                let is_pairs = ["pbin_", "pchunk_", "bin_", "arr_"]
                    .iter().any(|p| name.starts_with(p));
                if is_pairs { c.pairs += md.len(); } else { c.columns += md.len(); }
            }
        }
        du_dir(&self.merged_dir, None, &mut c.pieces);
        du_dir(&self.bands_dir, None, &mut c.bands);
        c
    }

    pub fn scratch_gate(&self, phase: &str) {
        self.stop_check(phase);
        let c = self.scratch_components();
        let live = c.pairs + c.pieces + c.bands;
        {
            let mut p = self.peak.lock().unwrap();
            if live > *p { *p = live; }
        }
        eprintln!("SS_SCRATCH phase={} pairs_live={} pieces={} bands={} \
                   columns_legal_sublinear={} asserted={} budget_bytes={} \
                   rule=amendment_b4_the_scratch_law;operator_clause_2xN;fat_is_named_not_summed",
                  phase, c.pairs, c.pieces, c.bands, c.columns, live,
                  self.scratch_budget);
        if live > self.scratch_budget {
            super::col::die(&format!(
                "SCRATCH_LAW: phase {} holds pairs_live={} pieces={} bands={} \
                 (columns, legal sublinear: {}) against the scratch budget {} - \
                 the machine must operate in less than N space; this is a \
                 machine bug, not a tuning outcome",
                phase, c.pairs, c.pieces, c.bands, c.columns,
                self.scratch_budget));
        }
    }

    pub fn scratch_peak(&self) {
        let c = self.scratch_components();
        let p = *self.peak.lock().unwrap();
        eprintln!("SS_SCRATCH_PEAK peak_live_bytes={} budget_bytes={} \
                   pairs_live={} pieces={} bands={} columns_legal_sublinear={} \
                   rule=amendment_b4_the_scratch_law;operator_clause_2xN;the_fat_is_named",
                  p, self.scratch_budget, c.pairs, c.pieces, c.bands, c.columns);
    }

    // THE DIAL_LAW (asserted like the C++): a phase's anon peak
    // against the workspace dial
    pub fn dial_gate(&self, phase: &str, anon_peak: u64, eps: u64) {
        eprintln!("SS_PHASE_RSS phase={} anon_peak_kb={} dial_kb={} eps_kb={} \
                   rule=budget_ratio_asserted", phase, anon_peak / 1024,
                  self.dial_bytes / 1024, eps / 1024);
        if anon_peak > self.dial_bytes + eps {
            super::col::die(&format!(
                "DIAL_LAW: phase {} peaked at {} bytes anon against the dial {} \
                 - a phase holds a structure past the workspace dial (the \
                 one-mode law is broken)", phase, anon_peak, self.dial_bytes));
        }
    }
}

pub fn now_s(t0: Instant) -> f64 { t0.elapsed().as_secs_f64() }
