//! The BUILD CONFIGURATOR: `xsa build` is plan-first.
//!
//! SURVEY -> MODEL -> PLAN FILE -> BUILD (or refuse with alternatives).
//! The plan is the scratch journal's FIRST entry, written before any bytes
//! move: same plan file = same build = provenance. `--plan-only` prints the
//! plan and exits without building.
//!
//! The model's constants are MEASURED (the gated runs of this lane:
//! fragment gates 1-3; 10GB gate 4 pending its run) and versioned here so
//! they update with gates — the planner-calibration loop compares the plan's
//! projected columns against the measured phase telemetry after each gated
//! run and corrects the table where they diverge.

use std::fs;
use std::path::Path;

/// Versioned, gate-measured model constants. v2 = fragment-scale gates
/// (1.08 GB corpus, 32 chunks, 48 threads, flat k=32, cap 6 GiB:
/// chunk 3:24, merge 1:03:15, endpoints 5:56, slim 20:02, sweep 1:04)
/// PLUS the post-gate-3 cost model: the merge loads are PURE READS with
/// the chunker-persisted order columns (persist-at-chunk-time, 05cdafe) -
/// load_read below; load_derive is the walk-derivation fallback for
/// legacy chunk sets (CROSS_NO_PERSIST), measured ~1.5 s/MB serial
/// (~0.7 MB/s - the pile-scale bottleneck that motivated persisting).
/// Chunk artifacts: 9.9x corpus (.crle runs) + 8x (persisted pos columns).
pub const CONSTANTS_VERSION: u32 = 3;

/// Per-byte wall rates (seconds per corpus MB), fragment-calibrated.
pub struct Rates {
    pub chunk_s_per_mb: f64,
    pub merge_load_s_per_mb: f64,       // pure-read loads (persisted pos columns)
    pub merge_load_derive_s_per_mb: f64, // walk-derivation fallback (legacy chunk sets)
    pub merge_anchor_s_per_mb: f64,
    pub merge_walk_s_per_mb: f64,
    pub merge_emit_s_per_mb: f64,
    pub endpoints_s_per_mb: f64,
    pub slim_s_per_mb: f64,
    pub sweep_s_per_mb: f64,
}

/// The v1 table (fragment gates; merge split into load/anchor/walk/emit from
/// the CROSS_PHASE telemetry of gate 3).
pub const RATES_V2: Rates = Rates {
    chunk_s_per_mb: 204.0 / 1082.0,
    merge_load_s_per_mb: 60.0 / 1082.0,        // 8x-corpus pos reads at pool bandwidth
    merge_load_derive_s_per_mb: 1650.0 / 1082.0,
    merge_anchor_s_per_mb: 104.0 / 1082.0,
    merge_walk_s_per_mb: 1109.0 / 1082.0,
    merge_emit_s_per_mb: 931.0 / 1082.0,
    endpoints_s_per_mb: 356.0 / 1082.0,
    slim_s_per_mb: 1202.0 / 1082.0,
    sweep_s_per_mb: 64.0 / 1082.0,
};

/// Disc/RAM shape constants (measured).
/// Pos-column MODE exchange rate (the 1GB A/B, gated 10/10 byte-identical):
/// dense (SXP3, 8 bytes/position) walks 2.3x faster; compact (SXP4,
/// zigzag group deltas) takes 2.2-2.4x less disc. The plan PICKS the mode
/// per scale: dense when the disc budget allows it, compact when it does
/// not (crle v3 0.8x + pos per mode + ref ~0).
pub const CRLE_PER_CORPUS_MB: f64 = 0.8;        // SXCR v3 run records (1GB measured 0.78x)
pub const POS_DENSE_PER_CORPUS_MB: f64 = 8.0;   // SXP3 dense 8n
pub const POS_COMPACT_PER_CORPUS_MB: f64 = 3.6; // SXP4 (100MB 3.29x, 1GB 3.60x - larger scale wins)
pub const MERGE_WALK_DENSE_S_PER_MB: f64 = 693.0 / 1000.0;   // 1GB gate5-B (SXP3 pos)
pub const MERGE_WALK_COMPACT_S_PER_MB: f64 = 1598.0 / 1000.0; // 1GB SXP4 gate (windowed decode)
pub fn chunk_artifacts_per_corpus_mb(pos_mode: &str) -> f64 {
    CRLE_PER_CORPUS_MB
        + if pos_mode == "dense" { POS_DENSE_PER_CORPUS_MB } else { POS_COMPACT_PER_CORPUS_MB }
}
pub const MERGE_SCRATCH_PER_CORPUS_MB: f64 = 9.1; // 100MB flat v3+SXP4 measured delta (m2+hash+wm+bwt+outputs, persisted loads keep pos on the chunk set)
pub const CHUNK_BUILD_RAM_PER_CHUNK_MB: f64 = 27.0; // libsais doubled SA + PLCP + text (measured peak/chunk at fragment)
pub const MERGE_RSS_GB: f64 = 4.0; // bounded design: pools + memory-aware side parallelism
pub const DISC_MAX_USED_PERCENT: u32 = 85;

/// The survey: everything the model needs from the machine.
pub struct Survey {
    pub corpus_bytes: u64,
    pub scratch_free_bytes: u64,
    pub scratch_used_percent: u32,
    pub mem_available_bytes: u64,
    pub cores: usize,
    pub cx16: bool,
    pub stripe_dirs: Vec<(String, u64, u32)>, // (dir, free bytes, used %)
}

fn disc_bytes_and_percent(path: &Path) -> Option<(u64, u32)> {
    let out = std::process::Command::new("df")
        .arg("-B1").arg("--output=avail,pcent").arg(path)
        .output().ok()?;
    let text = String::from_utf8_lossy(&out.stdout);
    let last = text.lines().rev().next()?;
    let mut it = last.split_whitespace();
    let free: u64 = it.next()?.parse().ok()?;
    let pct: u32 = it.next()?.trim_end_matches('%').parse().ok()?;
    Some((free, pct))
}

pub fn survey(corpus: &Path, scratch: &Path, stripes: &[String]) -> Result<Survey, String> {
    let corpus_bytes = fs::metadata(corpus)
        .map_err(|e| format!("stat {}: {e}", corpus.display()))?.len();
    let (scratch_free_bytes, scratch_used_percent) = disc_bytes_and_percent(scratch)
        .ok_or_else(|| format!("df failed for {}", scratch.display()))?;
    let mem_available_bytes = fs::read_to_string("/proc/meminfo")
        .ok().and_then(|t| {
            t.lines().find(|l| l.starts_with("MemAvailable:"))
                .and_then(|l| l.split_whitespace().nth(1))
                .and_then(|v| v.parse::<u64>().ok())
        }).unwrap_or(0) * 1024;
    let cores = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1);
    let cx16 = std::arch::is_x86_feature_detected!("cmpxchg16b");
    let mut stripe_dirs = Vec::new();
    for d in stripes {
        if let Some((free, pct)) = disc_bytes_and_percent(Path::new(d)) {
            stripe_dirs.push((d.clone(), free, pct));
        }
    }
    Ok(Survey { corpus_bytes, scratch_free_bytes, scratch_used_percent, mem_available_bytes, cores, cx16, stripe_dirs })
}

/// One phase row of the plan.
pub struct PhaseRow {
    pub name: &'static str,
    pub wall_s: f64,
    pub disc_peak_mb: f64,
    pub rss_gb: f64,
}

pub struct Plan {
    pub survey: Survey,
    pub chunks: u32,
    pub kway: u32,
    pub threads: u32,
    pub cap_gb: u64,
    pub phases: Vec<PhaseRow>,
    pub chunk_bytes_mb: f64,
    pub pos_mode: String,
    pub disc_verdict: String,
    pub ram_verdict: String,
    pub alternatives: Vec<String>,
}

pub fn model(
    survey: Survey, chunks: u32, kway: u32, threads: u32, cap_gb: u64,
    hygiene_free_pct: u32,
) -> Plan {
    let mb = survey.corpus_bytes as f64 / (1024.0 * 1024.0);
    let r = &RATES_V2;
    let chunk_bytes_mb = mb / chunks as f64;
    // RAM: the chunk front end sorts a doubled copy of one chunk (SA + PLCP
    // + text): ~27x chunk bytes, measured. The merge's anchor/BWT side
    // parallelism is bounded by its own RLIMIT_AS-aware logic.
    let chunk_ram_gb = chunk_bytes_mb * CHUNK_BUILD_RAM_PER_CHUNK_MB / 1024.0;
    // Disc timeline: chunk artifacts appear first (9.9x), the merge scratch
    // grows to ~20x (m2/hash/wm/pos/bwt, intermediates consume none in
    // flat), outputs ~1.4x stay.
    let outputs_disc_mb = mb * 1.4;
    let merge_disc_mb = mb * MERGE_SCRATCH_PER_CORPUS_MB;
    // Pos-column mode: the exchange rate (dense = fast walk / fat disc,
    // compact = slow walk / thin disc) is a KNOB, not doctrine. Prefer
    // dense whenever the disc budget fits its peak with headroom (wall
    // matters most at scale); fall back to compact when it does not.
    // (A per-level mix - dense chunk sides, compact accumulators - is the
    // refinement if a run is partially tight; not yet wired.)
    let free_mb = survey.scratch_free_bytes as f64 / (1024.0 * 1024.0);
    let dense_total_mb = mb * chunk_artifacts_per_corpus_mb("dense") + merge_disc_mb + outputs_disc_mb;
    let pos_mode = if dense_total_mb < free_mb * 0.9 { "dense" } else { "compact" };
    let chunk_disc_mb = mb * chunk_artifacts_per_corpus_mb(pos_mode);
    let walk_s_per_mb = if pos_mode == "dense" { r.merge_walk_s_per_mb.max(MERGE_WALK_DENSE_S_PER_MB) } else { r.merge_walk_s_per_mb.max(MERGE_WALK_COMPACT_S_PER_MB) };
    let phases = vec![
        PhaseRow { name: "chunk", wall_s: r.chunk_s_per_mb * mb, disc_peak_mb: chunk_disc_mb, rss_gb: chunk_ram_gb },
        PhaseRow { name: "merge(load)", wall_s: r.merge_load_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + merge_disc_mb, rss_gb: MERGE_RSS_GB },   // pure reads; derivation fallback is r.merge_load_derive_s_per_mb (x27 slower)
        PhaseRow { name: "merge(anchor)", wall_s: r.merge_anchor_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + merge_disc_mb, rss_gb: MERGE_RSS_GB },
        PhaseRow { name: "merge(walk)", wall_s: walk_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + merge_disc_mb, rss_gb: MERGE_RSS_GB },   // pos-mode exchange rate applied
        PhaseRow { name: "merge(emit)", wall_s: r.merge_emit_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + merge_disc_mb + outputs_disc_mb, rss_gb: MERGE_RSS_GB },
        PhaseRow { name: "endpoints", wall_s: r.endpoints_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + outputs_disc_mb, rss_gb: 0.4 },
        PhaseRow { name: "slim", wall_s: r.slim_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + outputs_disc_mb, rss_gb: 0.04 },
        PhaseRow { name: "sweep", wall_s: r.sweep_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + outputs_disc_mb, rss_gb: 0.01 },
    ];
    // Verdicts.
    let disc_peak_mb = chunk_disc_mb + merge_disc_mb + outputs_disc_mb;
    // FIT (absolute: projected peak vs free) and HYGIENE (the banked >=15%-
    // free policy, overridable by the operator with an explicit, journaled
    // --scratch-free-pct - a bounded run with many-x headroom on a full
    // drive is fit even when the blunt policy says no).
    let fits = disc_peak_mb < free_mb;
    let free_pct = 100u32.saturating_sub(survey.scratch_used_percent);
    let hygiene_ok = free_pct >= hygiene_free_pct;
    let disc_ok = fits && (hygiene_ok || hygiene_free_pct == 0);
    let disc_verdict = if disc_ok {
        if hygiene_ok {
            format!("FEASIBLE: peak ~{:.0} GB fits {:.0} GB free ({}% used)", disc_peak_mb / 1024.0, free_mb / 1024.0, survey.scratch_used_percent)
        } else {
            format!("FEASIBLE with hygiene override: peak ~{:.0} GB fits {:.0} GB free ({}% used; policy >= {}% free overridden by the operator)",
                     disc_peak_mb / 1024.0, free_mb / 1024.0, survey.scratch_used_percent, hygiene_free_pct)
        }
    } else if !fits {
        format!("INFEASIBLE: peak ~{:.0} GB vs {:.0} GB free ({}% used)", disc_peak_mb / 1024.0, free_mb / 1024.0, survey.scratch_used_percent)
    } else {
        format!("INFEASIBLE (hygiene): peak ~{:.0} GB fits {:.0} GB free but only {}% free < the {}% policy; pass --scratch-free-pct to override",
                 disc_peak_mb / 1024.0, free_mb / 1024.0, free_pct, hygiene_free_pct)
    };
    let chunk_ram_ok = chunk_ram_gb < cap_gb as f64 * 0.8;
    let merge_ram_ok = MERGE_RSS_GB < cap_gb as f64;
    let ram_verdict = format!(
        "{}: chunk front end ~{:.1} GB ({}x chunk bytes) vs cap {} GiB; merge ~{:.1} GB (bounded, memory-aware side parallelism)",
        if chunk_ram_ok && merge_ram_ok { "FEASIBLE" } else { "INFEASIBLE" },
        chunk_ram_gb, CHUNK_BUILD_RAM_PER_CHUNK_MB as u32, cap_gb, MERGE_RSS_GB,
    );
    let mut alternatives = Vec::new();
    if !disc_ok {
        alternatives.push("free or add scratch space (or pass --stripe dirs once reader-level striping lands, lane item 3)".to_string());
        alternatives.push("accumulator mode (merge corpus quarters into one growing object; ~14 corpus passes vs flat's 2; acknowledged disc-frugal fallback)".to_string());
    }
    if !chunk_ram_ok {
        alternatives.push(format!("raise --chunks (smaller chunks cut the front end's ~{}x-chunk-bytes RAM linearly; chunk count becomes the flat k)", CHUNK_BUILD_RAM_PER_CHUNK_MB as u32));
        alternatives.push("or raise --memory-gb (a lower inherited hard limit always wins)".to_string());
    }
    if !merge_ram_ok {
        alternatives.push("raise --memory-gb for the merge phase".to_string());
    }
    if !survey.cx16 {
        alternatives.push("CPU lacks cmpxchg16b: the merge's page pool requires it (CROSS_NO_POOL=1 runs without the pool, slower)".to_string());
    }
    Plan { survey, chunks, kway, threads, cap_gb, phases, chunk_bytes_mb, pos_mode: pos_mode.to_string(), disc_verdict, ram_verdict, alternatives }
}

fn hms(s: f64) -> String {
    let s = s.max(0.0) as u64;
    format!("{}:{:02}:{:02}", s / 3600, (s % 3600) / 60, s % 60)
}

impl Plan {
    pub fn render(&self) -> String {
        let mut s = String::new();
        let total: f64 = self.phases.iter().map(|p| p.wall_s).sum();
        s.push_str(&format!(
            "XSA_PLAN v={} corpus_bytes={} chunks={} kway={} threads={} cap_gb={} cores={} cx16={} mem_available_gb={:.0}\n",
            CONSTANTS_VERSION, self.survey.corpus_bytes, self.chunks, self.kway, self.threads,
            self.cap_gb, self.survey.cores, self.survey.cx16,
            self.survey.mem_available_bytes as f64 / (1024.0 * 1024.0 * 1024.0),
        ));
        for d in &self.survey.stripe_dirs {
            s.push_str(&format!("XSA_PLAN_STRIPE dir={} free_gb={:.0} used_pct={}\n", d.0, d.1 as f64 / (1024.0 * 1024.0 * 1024.0), d.2));
        }
        s.push_str("XSA_PLAN_PHASES name wall disc_peak_gb rss_gb\n");
        for p in &self.phases {
            s.push_str(&format!(
                "XSA_PLAN_PHASE name={} wall={} wall_s={:.0} disc_peak_gb={:.1} rss_gb={:.2}\n",
                p.name, hms(p.wall_s), p.wall_s, p.disc_peak_mb / 1024.0, p.rss_gb,
            ));
        }
        s.push_str(&format!("XSA_PLAN_TOTAL wall={} wall_s={:.0} chunk_bytes_mb={:.0}\n", hms(total), total, self.chunk_bytes_mb));
        // The pos-mode pick with its arithmetic: dense = 8n pos disc /
        // fast walk; compact = 3.6n / 2.3x slower walk. Dense whenever
        // its peak fits free with 10% headroom, else compact.
        let mb = self.survey.corpus_bytes as f64 / (1024.0 * 1024.0);
        let free_gb = self.survey.scratch_free_bytes as f64 / (1024.0 * 1024.0 * 1024.0);
        let dense_gb = (mb * chunk_artifacts_per_corpus_mb("dense")) / 1024.0;
        let compact_gb = (mb * chunk_artifacts_per_corpus_mb("compact")) / 1024.0;
        s.push_str(&format!(
            "XSA_POS_MODE mode={} chunk_set_gb_dense={:.1} chunk_set_gb_compact={:.1} free_gb={:.1} rule=dense-if-10pct-headroom-else-compact (walk s/MB dense={:.3} compact={:.3})\n",
            self.pos_mode, dense_gb, compact_gb, free_gb, MERGE_WALK_DENSE_S_PER_MB, MERGE_WALK_COMPACT_S_PER_MB,
        ));
        s.push_str(&format!("XSA_PLAN_DISC {}\n", self.disc_verdict));
        s.push_str(&format!("XSA_PLAN_RAM {}\n", self.ram_verdict));
        for a in &self.alternatives {
            s.push_str(&format!("XSA_PLAN_ALTERNATIVE {}\n", a));
        }
        s
    }
}
