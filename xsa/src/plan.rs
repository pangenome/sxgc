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
pub const CONSTANTS_VERSION: u32 = 6;

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
pub const FINISH_ARTIFACTS_PER_CORPUS_MB: f64 = 21.1; // 100MB dense finish footprint (agg 12n + ri4/head_sa/sA); the biggest disc term after cheap chunks
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
    pub name: String,
    pub wall_s: f64,
    pub disc_peak_mb: f64,
    pub rss_gb: f64,
}

/// One fan-in level (fixed-chunk + repeated k-way fan-in shape). `in_count`
/// chunks merge in groups of <= kway into `out_count`; every level runs the
/// SAME certified k-way merge, and its output is a valid next-level input
/// (the progressive-shape invariant).
pub struct FaninLevel {
    pub level: u32,
    pub in_count: u32,
    pub out_count: u32,
    pub groups: u32,
    pub level_side_gb: f64,     // total input side bytes held at this level
    pub group_scratch_gb: f64,  // one group's merge transient (m2/hash/wm/bwt)
    pub wall_s: f64,
}

/// The fan-in plan: arithmetic over levels, per-level side sizes, peak disc
/// with delete-as-you-go, and the projected wall.
pub struct FaninPlan {
    pub chunk_bytes_mb: f64,
    pub levels: Vec<FaninLevel>,
    pub peak_disc_mb: f64,
    pub total_merge_wall_s: f64,
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
    pub fanin: Option<FaninPlan>,
    /// The v2 pipeline per-term table (model_v2 only).
    pub v2: Option<V2PerTerms>,
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
        PhaseRow { name: "chunk".into(), wall_s: r.chunk_s_per_mb * mb, disc_peak_mb: chunk_disc_mb, rss_gb: chunk_ram_gb },
        PhaseRow { name: "merge(load)".into(), wall_s: r.merge_load_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + merge_disc_mb, rss_gb: MERGE_RSS_GB },   // pure reads; derivation fallback is r.merge_load_derive_s_per_mb (x27 slower)
        PhaseRow { name: "merge(anchor)".into(), wall_s: r.merge_anchor_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + merge_disc_mb, rss_gb: MERGE_RSS_GB },
        PhaseRow { name: "merge(walk)".into(), wall_s: walk_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + merge_disc_mb, rss_gb: MERGE_RSS_GB },   // pos-mode exchange rate applied
        PhaseRow { name: "merge(emit)".into(), wall_s: r.merge_emit_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + merge_disc_mb + outputs_disc_mb, rss_gb: MERGE_RSS_GB },
        PhaseRow { name: "endpoints".into(), wall_s: r.endpoints_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + outputs_disc_mb, rss_gb: 0.4 },
        PhaseRow { name: "slim".into(), wall_s: r.slim_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + outputs_disc_mb, rss_gb: 0.04 },
        PhaseRow { name: "sweep".into(), wall_s: r.sweep_s_per_mb * mb, disc_peak_mb: chunk_disc_mb + outputs_disc_mb, rss_gb: 0.01 },
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
    Plan { survey, chunks, kway, threads, cap_gb, phases, chunk_bytes_mb, pos_mode: pos_mode.to_string(), disc_verdict, ram_verdict, alternatives, fanin: None, v2: None }
}

fn hms(s: f64) -> String {
    let s = s.max(0.0) as u64;
    format!("{}:{:02}:{:02}", s / 3600, (s % 3600) / 60, s % 60)
}

/// The FAN-IN shape: fixed chunk size + repeated k-way fan-in until one
/// artifact remains. Levels are arithmetic: count -> ceil(count/kway) -> ...
/// -> 1. Every level runs the SAME certified k-way merge (one code path,
/// the progressive-shape invariant: level N's output is a valid level N+1
/// input), and inputs are reclaimed as consumed (CROSS_DELETE_CONSUMED), so
/// the peak disc is ~ one level's live set + one group's merge transient,
/// NOT the whole input history. This is the pile's path (flat k=count is
/// DISC_FAIL at pile scale: ~18 TB peak vs ~1.8 TB free).
pub fn model_fanin(
    survey: Survey, chunks: u32, kway: u32, threads: u32, cap_gb: u64,
    hygiene_free_pct: u32,
) -> Plan {
    let mb = survey.corpus_bytes as f64 / (1024.0 * 1024.0);
    let r = &RATES_V2;
    let chunk_bytes_mb = mb / chunks as f64;
    let chunk_ram_gb = chunk_bytes_mb * CHUNK_BUILD_RAM_PER_CHUNK_MB / 1024.0;
    let free_mb = survey.scratch_free_bytes as f64 / (1024.0 * 1024.0);
    // Pos-mode exchange rate (same rule as the flat model).
    let dense_total_mb = mb * chunk_artifacts_per_corpus_mb("dense") + mb * MERGE_SCRATCH_PER_CORPUS_MB + mb * 1.4;
    let pos_mode = if dense_total_mb < free_mb * 0.9 { "dense" } else { "compact" };
    let walk_s_per_mb = if pos_mode == "dense" { r.merge_walk_s_per_mb.max(MERGE_WALK_DENSE_S_PER_MB) } else { r.merge_walk_s_per_mb.max(MERGE_WALK_COMPACT_S_PER_MB) };
    let f_chunk = chunk_artifacts_per_corpus_mb(pos_mode);
    // A merged level's artifact set is the .crle (fewer runs across boundaries)
    // + the per-position .pos + the .ref; measured ~ the chunk set magnitude.
    let f_merged = f_chunk;
    let merge_rate_s_per_mb = r.merge_load_s_per_mb + r.merge_anchor_s_per_mb + walk_s_per_mb + r.merge_emit_s_per_mb;

    let mut levels: Vec<FaninLevel> = Vec::new();
    let mut count = chunks;
    let mut level = 0u32;
    let mut total_merge_wall_s = 0.0f64;
    let mut merge_peak_mb = 0.0f64;
    while count > 1 {
        let out_count = (count + kway - 1) / kway;
        let groups = out_count; // ceil(count/kway) groups (last may be short)
        let f = if level == 0 { f_chunk } else { f_merged };
        let level_side_mb = mb * f;
        // Largest group covers kway/count of the corpus (the last, short group
        // is smaller); bound the merge transient by the largest group.
        let group_frac = (kway.min(count)) as f64 / count as f64;
        let group_scratch_mb = mb * MERGE_SCRATCH_PER_CORPUS_MB * group_frac;
        let wall_s = merge_rate_s_per_mb * mb; // every level merges the whole corpus once
        if level_side_mb + group_scratch_mb > merge_peak_mb {
            merge_peak_mb = level_side_mb + group_scratch_mb;
        }
        total_merge_wall_s += wall_s;
        levels.push(FaninLevel { level, in_count: count, out_count, groups, level_side_gb: level_side_mb / 1024.0, group_scratch_gb: group_scratch_mb / 1024.0, wall_s });
        count = out_count;
        level += 1;
    }

    let outputs_disc_mb = mb * 1.4;
    let chunk_disc_mb = mb * f_chunk;
    let mut phases = vec![
        PhaseRow { name: "chunk".into(), wall_s: r.chunk_s_per_mb * mb, disc_peak_mb: chunk_disc_mb, rss_gb: chunk_ram_gb },
    ];
    // Merge phases: one row per level. disc_peak rises to the level's live set
    // plus one group transient, then falls as inputs are reclaimed.
    for l in &levels {
        phases.push(PhaseRow {
            name: format!("merge(L{})", l.level),
            wall_s: l.wall_s,
            disc_peak_mb: l.level_side_gb * 1024.0 + l.group_scratch_gb * 1024.0,
            rss_gb: MERGE_RSS_GB,
        });
    }
    // Finish consumes the merge's .pftext/.pfck; intermediates are already
    // reclaimed, so only the merged outputs + finish artifacts are live.
    let finish_peak_mb = outputs_disc_mb + mb * FINISH_ARTIFACTS_PER_CORPUS_MB;
    phases.push(PhaseRow { name: "endpoints".into(), wall_s: r.endpoints_s_per_mb * mb, disc_peak_mb: finish_peak_mb, rss_gb: 0.4 });
    phases.push(PhaseRow { name: "slim".into(), wall_s: r.slim_s_per_mb * mb, disc_peak_mb: finish_peak_mb, rss_gb: 0.04 });
    phases.push(PhaseRow { name: "sweep".into(), wall_s: r.sweep_s_per_mb * mb, disc_peak_mb: finish_peak_mb, rss_gb: 0.01 });

    let peak_mb = merge_peak_mb.max(chunk_disc_mb).max(finish_peak_mb);
    let fits = peak_mb < free_mb;
    let free_pct = 100u32.saturating_sub(survey.scratch_used_percent);
    let hygiene_ok = free_pct >= hygiene_free_pct;
    let disc_ok = fits && (hygiene_ok || hygiene_free_pct == 0);
    let disc_verdict = if disc_ok {
        if hygiene_ok {
            format!("FEASIBLE (fan-in): peak ~{:.0} GB fits {:.0} GB free ({}% used)", peak_mb / 1024.0, free_mb / 1024.0, survey.scratch_used_percent)
        } else {
            format!("FEASIBLE with hygiene override (fan-in): peak ~{:.0} GB fits {:.0} GB free ({}% used)", peak_mb / 1024.0, free_mb / 1024.0, survey.scratch_used_percent)
        }
    } else if !fits {
        format!("INFEASIBLE (fan-in): peak ~{:.0} GB vs {:.0} GB free ({}% used)", peak_mb / 1024.0, free_mb / 1024.0, survey.scratch_used_percent)
    } else {
        format!("INFEASIBLE (fan-in hygiene): peak ~{:.0} GB fits {:.0} GB free but only {}% free < the {}% policy", peak_mb / 1024.0, free_mb / 1024.0, free_pct, hygiene_free_pct)
    };
    let chunk_ram_ok = chunk_ram_gb < cap_gb as f64 * 0.8;
    let ram_verdict = format!(
        "{}: chunk front end ~{:.1} GB ({}x chunk bytes) vs cap {} GiB; merge ~{:.1} GB (bounded)",
        if chunk_ram_ok { "FEASIBLE" } else { "INFEASIBLE" }, chunk_ram_gb, CHUNK_BUILD_RAM_PER_CHUNK_MB as u32, cap_gb, MERGE_RSS_GB,
    );
    let mut alternatives = Vec::new();
    if !disc_ok {
        alternatives.push("raise --kway (fewer levels) or free scratch space".to_string());
    }
    if !chunk_ram_ok {
        alternatives.push(format!("lower --chunk-mb (smaller chunks cut the front end's ~{}x-chunk-bytes RAM linearly)", CHUNK_BUILD_RAM_PER_CHUNK_MB as u32));
    }
    let fanin = FaninPlan { chunk_bytes_mb, levels, peak_disc_mb: peak_mb, total_merge_wall_s };
    Plan { survey, chunks, kway, threads, cap_gb, phases, chunk_bytes_mb, pos_mode: pos_mode.to_string(), disc_verdict, ram_verdict, alternatives, fanin: Some(fanin), v2: None }
}

impl Plan {
    pub fn render(&self) -> String {
        let mut s = String::new();
        let total: f64 = self.phases.iter().map(|p| p.wall_s).sum();
        s.push_str(&format!(
            "XSA_PLAN v={}{} corpus_bytes={} chunks={} kway={} threads={} cap_gb={} cores={} cx16={} mem_available_gb={:.0}\n",
            CONSTANTS_VERSION, if self.v2.is_some() { " pipeline=v2bin (bin shape: one sweep -> 256 byte-bins -> ordered stitch -> decomposed fused finish; no chunk phase, no merge walk)" } else { "" }, self.survey.corpus_bytes, self.chunks, self.kway, self.threads,
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
        if self.v2.is_some() {
            // The bin producer and the fused finish run CONCURRENTLY, so the
            // sum above is a term sum, not a wall: the end-to-end figure is
            // the max, and it is reported as its own line. There is no chunk
            // set on this route, so no chunk-size figure is printed.
            s.push_str(&format!("XSA_PLAN_TOTAL wall={} wall_s={:.0} (TERM SUM)\n", hms(total), total));
            s.push_str("XSA_PLAN_TOTAL_NOTE the sum above is the TERM SUM; the bin producer and the fused finish overlap, so the wall is max(producer, finish) = XSA_PLAN_V2_BIN_END e2e\n");
        } else {
            s.push_str(&format!("XSA_PLAN_TOTAL wall={} wall_s={:.0} chunk_bytes_mb={:.0}\n", hms(total), total, self.chunk_bytes_mb));
        }
        // The fan-in shape: levels, per-level side sizes, peak disc
        // (delete-as-you-go) and the merge wall, all from arithmetic.
        if let Some(f) = &self.fanin {
            s.push_str(&format!(
                "XSA_FANIN levels={} chunk_bytes_mb={:.1} peak_disc_gb={:.1} merge_wall_s={:.0} merge_wall={}\n",
                f.levels.len(), f.chunk_bytes_mb, f.peak_disc_mb / 1024.0, f.total_merge_wall_s, hms(f.total_merge_wall_s),
            ));
            s.push_str("XSA_FANIN_LEVELS level in out groups level_side_gb group_scratch_gb wall_s\n");
            for l in &f.levels {
                s.push_str(&format!(
                    "XSA_FANIN_LEVEL level={} in={} out={} groups={} level_side_gb={:.1} group_scratch_gb={:.1} wall_s={:.0} wall={}\n",
                    l.level, l.in_count, l.out_count, l.groups, l.level_side_gb, l.group_scratch_gb, l.wall_s, hms(l.wall_s),
                ));
            }
        }
        // The pos-mode pick with its arithmetic: dense = 8n pos disc /
        // fast walk; compact = 3.6n / 2.3x slower walk. Dense whenever
        // its peak fits free with 10% headroom, else compact. The bin shape
        // has NO position column of any kind, so the exchange rate does not
        // apply to it and the line is not printed there.
        let mb = self.survey.corpus_bytes as f64 / (1024.0 * 1024.0);
        let free_gb = self.survey.scratch_free_bytes as f64 / (1024.0 * 1024.0 * 1024.0);
        if self.v2.is_none() {
            let dense_gb = (mb * chunk_artifacts_per_corpus_mb("dense")) / 1024.0;
            let compact_gb = (mb * chunk_artifacts_per_corpus_mb("compact")) / 1024.0;
            s.push_str(&format!(
                "XSA_POS_MODE mode={} chunk_set_gb_dense={:.1} chunk_set_gb_compact={:.1} free_gb={:.1} rule=dense-if-10pct-headroom-else-compact (walk s/MB dense={:.3} compact={:.3})\n",
                self.pos_mode, dense_gb, compact_gb, free_gb, MERGE_WALK_DENSE_S_PER_MB, MERGE_WALK_COMPACT_S_PER_MB,
            ));
        }
        if let Some(v) = &self.v2 {
            s.push_str(&format!(
                "XSA_PLAN_V2_TERMS pipeline=v2bin corpus_gb={:.1} rlebwt_gb={:.1} pfck2_scratch_gb={:.1} pieces_window_gb={:.1} container_gb={:.1} tree_peak_gb={:.1} whole_box_gb={:.1}\n",
                v.corpus_gb, v.rlebwt_gb, v.pfck_gb, v.pieces_window_gb, v.container_gb, v.tree_peak_gb, v.whole_box_gb,
            ));
            s.push_str(&format!("XSA_PLAN_V2_TREE tree_peak={:.3}n bound={:.1}n measured_at=1GB-G2 (output side; the bin producer adds no transient file); whole_box(corpus+tree)={:.3}n; position column disc=0.000n (anonymous RAM arena)\n", V2_TREE_PEAK_PER_N, V2_TREE_TARGET_PER_N, 1.0 + V2_TREE_PEAK_PER_N));
            s.push_str(&format!("XSA_PLAN_V2_WITNESS {}\n", v.witness_note));
            s.push_str(&format!("XSA_PLAN_V2_TRANSIENT {}\n", v.transient_note));
            s.push_str(&format!(
                "XSA_PLAN_V2_BIN live_cap_slots={} windows={} arena_pair_gb={:.1} count_s={:.0} fill_s={:.0} build_s={:.0} stitch_s={:.0} producer_s={:.0} finish_s={:.0}\n",
                v.bin_live_cap, v.bin_windows, v.bin_arena_gb, v.bin_count_s, v.bin_fill_s, v.bin_build_s, v.bin_stitch_s, v.bin_producer_s, v.bin_finish_s,
            ));
            s.push_str(&format!(
                "XSA_PLAN_V2_BIN_END e2e_s={:.0} e2e={} producer={} finish={} rule=max(producer,finish) (concurrent, flow-controlled through the piece window)\n",
                v.bin_e2e_s, hms(v.bin_e2e_s), hms(v.bin_producer_s), hms(v.bin_finish_s),
            ));
            s.push_str(&format!("XSA_PLAN_V2_BIN_NOTE {}\n", v.bin_note));
            let need_gb = self.phases.iter().map(|p| p.disc_peak_mb).fold(0.0, f64::max) / 1024.0;
            s.push_str(&format!(
                "XSA_PLAN_V2_HOSTING all-phase scratch peak {:.1} GiB = {:.3}n on one host; free {:.1} GiB: {} (corpus itself excluded; add {:.1} GiB if co-hosted)\n",
                need_gb, need_gb / v.corpus_gb, free_gb,
                if need_gb <= free_gb { "FITS" } else { "DOES NOT FIT" }, v.corpus_gb));
        }
        s.push_str(&format!("XSA_PLAN_DISC {}\n", self.disc_verdict));
        s.push_str(&format!("XSA_PLAN_RAM {}\n", self.ram_verdict));
        for a in &self.alternatives {
            s.push_str(&format!("XSA_PLAN_ALTERNATIVE {}\n", a));
        }
        s
    }
}


// ------------------------------------------------------------- v2 pipeline
// THE BIN SHAPE: `xsa build --v2-pipeline` drives bin-bwt as the producer (ONE
// sweep -> 256 byte-bins -> parallel per-bin construction -> ordered stitch
// into the unchanged band/piece protocol) and the decomposed fused finish
// (pile_finish) as the consumer, CONCURRENT and flow-controlled through the
// piece window. There is no chunk phase and no merge walk: every chunk /
// k-way / merge-walk term has left this model with the route (the merge path
// survives in the binary behind --radix and in the v1 executor for A/B, and is
// priced by model()/model_fanin(), not here).
//
// UNITS: ns per corpus byte, measured on the 1 GB fixture (n = 1,000,000,665 B)
// with the bin reports' own definitions (tools/binbwt/RESULTS.md,
// RESULTS_FINISH_DECOMP.md, pile_projection_v2.py).
// The v2 pipeline eliminates every whole-column finish artifact (NO
// agg/ssa_t/head_sa/ssa/pftext file ever exists); the product is the lite
// container (m1/m4/m5/m7/m10/m11) and the only pre-lite whole file is
// frag.rlebwt, deleted after the container publishes. The finish's own
// suffix-hash grid (pfck2) is scratch and is deleted before the container pass.
pub const V2_RLEBWT_PER_N: f64 = 1.546;         // measured 1545900600 B
pub const V2_PFCK_PER_N: f64 = 0.364;           // measured 363636648 B (pfck2 scratch; deleted post-sweep)
pub const V2_CONTAINER_PER_N: f64 = 0.468;      // measured 467814822 B (the product)
pub const V2_TREE_PEAK_PER_N: f64 = 1.975;      // MEASURED du peak of the scratch tree (output side; the bin producer adds no transient file)
pub const V2_TREE_TARGET_PER_N: f64 = 2.5;      // the mission's output-side bound
pub const V2_CHI_PER_N: f64 = 0.283;            // 1GB fixture ratio; the pile value is UNMEASURED

/// The counting sweep (one pass over the corpus, `text.span`): v1 banked
/// 1.36 s/GB; the v2 binary measured 0.043 s/GB. The banked figure is kept,
/// i.e. conservative.
pub const BIN_COUNT_NS_PER_BYTE: f64 = 1.36;
/// The placing pass for the FIRST window, measured on the single-window 1 GB
/// run: 5.084 s of fill, i.e. packing (tail|symbol|hit) plus the target merge
/// join.
pub const BIN_FILL_FIRST_NS_PER_BYTE: f64 = 5.084;
/// The marginal placing pass per EXTRA window: v1's banked (15.70-4.23)/4 =
/// 2.87 s/GB per window x 1.232 (the v2 packing+join cost measured against the
/// same v1 pass) = 3.535 s/GB. Each window places only its own bin range.
pub const BIN_FILL_WINDOW_NS_PER_BYTE: f64 = 3.535;
/// The ONE ordered stitch thread (band writers overlapped): 11.41 ns/row at
/// 1 GB = 11.408 s/GB. Thread-count insensitive by construction - it is a
/// single ordered consumer of the bins.
pub const BIN_STITCH_NS_PER_BYTE: f64 = 11.408;
/// The decomposed finish's OWN work: 141.68 s wall at 1 GB minus 22.74 s of
/// producer backpressure (piece wait) = 118.9 s/GB.
pub const FINISH_DECOMP_NS_PER_BYTE: f64 = 118.9;
/// Position-arena bytes per admitted slot: an arena PLUS a scratch arena, 8 B
/// each. Anonymous RAM, never a file: the bin shape has no position column.
pub const BIN_ARENA_BYTES_PER_SLOT: u64 = 16;
/// The per-bin construction critical path, seconds per 1 GB fixture, at the
/// thread counts it was measured at (t=8/32: bin-bwt v1 build_s; t=96: the v2
/// producer-only run). The build is memory-bandwidth bound long before 96
/// threads (24.7 ns/symbol-thread at t=96 vs 5.46 uncontended), so the curve is
/// interpolated in log2(threads) between measured points and clamped.
pub const BIN_BUILD_POINTS: [(f64, f64); 3] = [(8.0, 107.93), (32.0, 35.86), (96.0, 23.489)];

/// The window count of the bin shape: `cap` slots admit at most `cap`
/// positions, so the corpus is placed in ceil(n/cap) windows. A window is
/// packed to `<= cap` by WHOLE bins (bins are indivisible), so this is the
/// arithmetic lower bound and every measured point matches it exactly
/// (1 GB at cap 250M -> 5, 1 GB at cap >= n -> 1).
pub fn bin_windows(n: u64, cap: u64) -> u64 {
    let cap = cap.max(1);
    ((n + cap - 1) / cap).max(1)
}

fn bin_build_s_per_gb(threads: u32) -> f64 {
    let t = (threads.max(1) as f64).ln();
    let first = BIN_BUILD_POINTS[0];
    let last = BIN_BUILD_POINTS[BIN_BUILD_POINTS.len() - 1];
    if t <= first.0.ln() {
        return first.1;
    }
    if t >= last.0.ln() {
        return last.1;
    }
    for pair in BIN_BUILD_POINTS.windows(2) {
        let (a, b) = (pair[0], pair[1]);
        if t <= b.0.ln() {
            let frac = (t - a.0.ln()) / (b.0.ln() - a.0.ln());
            return a.1 + frac * (b.1 - a.1);
        }
    }
    last.1
}

pub struct V2PerTerms {
    pub corpus_gb: f64,
    pub rlebwt_gb: f64,
    pub pfck_gb: f64,
    pub pieces_window_gb: f64,
    pub container_gb: f64,
    pub tree_peak_gb: f64,
    pub whole_box_gb: f64,
    pub witness_ram_gb: f64,
    pub witness_note: String,
    pub transient_note: String,
    // ---- the bin shape (what the executor runs)
    pub bin_live_cap: u64,   // resolved position-arena bound, in slots
    pub bin_windows: u64,    // ceil(n / cap)
    pub bin_arena_gb: f64,   // 16 B/slot x cap, anonymous RAM
    pub bin_count_s: f64,
    pub bin_fill_s: f64,
    pub bin_build_s: f64,
    pub bin_stitch_s: f64,
    pub bin_producer_s: f64,
    pub bin_finish_s: f64,
    pub bin_e2e_s: f64,      // max(producer, finish): they run concurrently
    pub bin_note: String,
}

/// The bin plan. `live_cap` is the operator's `--bin-live-cap` (None = let the
/// plan pick from the memory cap; Some(0) = unbounded, one window).
pub fn model_v2(
    survey: Survey, _chunks: u32, _kway: u32, threads: u32, cap_gb: u64,
    hygiene_free_pct: u32, live_cap: Option<u64>,
) -> Plan {
    let n = survey.corpus_bytes;
    let mb = n as f64 / (1024.0 * 1024.0);
    let n_gb = mb / 1024.0;
    let free_mb = survey.scratch_free_bytes as f64 / (1024.0 * 1024.0);
    // ---- the position-arena bound and the window arithmetic
    // One window needs one slot per position of the contiguous bin range it
    // admits; `cap` bounds that live set. The arena pair is 16 B/slot.
    let mem_bytes = cap_gb.saturating_mul(1 << 30);
    let (cap, cap_source) = match live_cap {
        Some(0) => (n, "--bin-live-cap 0: unbounded (one window)".to_string()),
        Some(v) => (
            v.min(n.max(1)),
            if v > n { format!("--bin-live-cap {v} exceeds n={n}: capped at n (one window)") }
            else { format!("--bin-live-cap {v} (operator)") },
        ),
        None => {
            // 85% of the cap in the arena pair, split two ways (arena+scratch),
            // leaves room for the text window buffers and the sink's band
            // buffers; never above n (more slots than positions is meaningless).
            let auto = (mem_bytes * 85 / 100 / BIN_ARENA_BYTES_PER_SLOT).clamp(1, n.max(1));
            (auto, format!("auto from --memory-gb {cap_gb}: 85% of the cap / 16 B per slot"))
        }
    };
    let windows = bin_windows(n, cap);
    let arena_gb = (cap as f64 * BIN_ARENA_BYTES_PER_SLOT as f64) / (1024.0 * 1024.0 * 1024.0);
    // ---- the terms, each = measured rate x byte count
    let count_s = BIN_COUNT_NS_PER_BYTE * n as f64 / 1e9;
    let fill_s = (BIN_FILL_FIRST_NS_PER_BYTE
        + (windows as f64 - 1.0) * BIN_FILL_WINDOW_NS_PER_BYTE) * n as f64 / 1e9;
    let build_s = bin_build_s_per_gb(threads) * n as f64 / 1e9;
    let stitch_s = BIN_STITCH_NS_PER_BYTE * n as f64 / 1e9;
    let producer_s = count_s + fill_s + build_s + stitch_s;
    let finish_s = FINISH_DECOMP_NS_PER_BYTE * n as f64 / 1e9;
    let e2e_s = producer_s.max(finish_s);
    // ---- the disc side: rlebwt -> pieces window -> pfck2 -> container, with
    // rlebwt deleted after the container publishes (the measured 1.975n peak).
    let rlebwt_mb = mb * V2_RLEBWT_PER_N;
    let pfck_mb = mb * V2_PFCK_PER_N;
    let window = std::env::var("CROSS_V2_PIECE_WINDOW").ok()
        .and_then(|v| v.parse::<u64>().ok()).unwrap_or(1100);
    // PRC1: char + u32 len + two packed positions; headers and BPG1 class
    // metadata receive a conservative 32-byte/run allowance.
    let pieces_mb = (n as f64).min(window as f64 * 65536.0) * 32.0 / 1048576.0;
    let container_mb = mb * V2_CONTAINER_PER_N;
    let tree_peak_mb = mb * V2_TREE_PEAK_PER_N;
    let producer_disc_mb = rlebwt_mb + pieces_mb;
    let phases = vec![
        PhaseRow { name: "bin(sweep)".into(), wall_s: count_s + fill_s,
                   disc_peak_mb: pieces_mb, rss_gb: arena_gb },
        PhaseRow { name: "bin(build)".into(), wall_s: build_s,
                   disc_peak_mb: pieces_mb, rss_gb: arena_gb },
        PhaseRow { name: "bin(stitch)".into(), wall_s: stitch_s,
                   disc_peak_mb: producer_disc_mb, rss_gb: arena_gb },
        // CONCURRENT with the producer: the decomposed fused finish (pfck2 ->
        // seam classes -> sweep -> witnesses -> container publish), which owns
        // the tree peak.
        PhaseRow { name: "fused-finish".into(), wall_s: finish_s,
                   disc_peak_mb: tree_peak_mb.max(producer_disc_mb),
                   rss_gb: 8.0 * V2_CHI_PER_N * n_gb },
    ];
    // ---- verdicts
    let tree_ok = tree_peak_mb < free_mb;
    let free_pct = 100u32.saturating_sub(survey.scratch_used_percent);
    let hygiene_ok = free_pct >= hygiene_free_pct;
    let tree_target_ok = V2_TREE_PEAK_PER_N <= V2_TREE_TARGET_PER_N;
    let disc_ok = tree_ok && tree_target_ok && (hygiene_ok || hygiene_free_pct == 0);
    let whole_box_gb = n_gb * (1.0 + V2_TREE_PEAK_PER_N);
    let disc_verdict = if disc_ok {
        if hygiene_ok {
            format!("FEASIBLE (v2 bin shape): scratch-tree peak ~{:.0} GB ({:.3}n, bound {:.1}n) fits {:.0} GB free; whole-box (corpus + tree) ~{:.0} GB = {:.3}n; position arena 0 B of disc (anonymous RAM)",
                tree_peak_mb / 1024.0, V2_TREE_PEAK_PER_N, V2_TREE_TARGET_PER_N, free_mb / 1024.0, whole_box_gb, 1.0 + V2_TREE_PEAK_PER_N)
        } else {
            format!("FEASIBLE with hygiene override (v2 bin shape): scratch-tree peak ~{:.0} GB ({:.3}n, bound {:.1}n) fits {:.0} GB free ({}% used; policy >= {}% free overridden by the operator); whole-box (corpus + tree) ~{:.0} GB = {:.3}n",
                tree_peak_mb / 1024.0, V2_TREE_PEAK_PER_N, V2_TREE_TARGET_PER_N, free_mb / 1024.0, survey.scratch_used_percent, hygiene_free_pct, whole_box_gb, 1.0 + V2_TREE_PEAK_PER_N)
        }
    } else if !tree_ok {
        format!("INFEASIBLE (v2 bin shape): scratch-tree peak ~{:.0} GB vs {:.0} GB free", tree_peak_mb / 1024.0, free_mb / 1024.0)
    } else if !tree_target_ok {
        format!("INFEASIBLE (v2 bin shape): scratch-tree peak {:.3}n exceeds the {:.1}n mission bound", V2_TREE_PEAK_PER_N, V2_TREE_TARGET_PER_N)
    } else {
        format!("INFEASIBLE (hygiene): scratch-tree peak ~{:.0} GB fits {:.0} GB free but only {}% free < the {}% policy; pass --scratch-free-pct to override",
            tree_peak_mb / 1024.0, free_mb / 1024.0, free_pct, hygiene_free_pct)
    };
    // The arena pair is the producer's whole RAM term; every phase runs under
    // the SAME RLIMIT_AS, so 90% of it is the bar. Over the bar the run dies
    // with a bad_alloc mid-window, which is exactly what the plan is for.
    let arena_bytes = cap.saturating_mul(BIN_ARENA_BYTES_PER_SLOT);
    let arena_ok = arena_bytes <= mem_bytes / 10 * 9;
    let witness_ram_gb = 8.0 * V2_CHI_PER_N * n_gb;
    let ram_verdict = format!(
        "{}: position arena pair ~{:.1} GiB ({} B/slot x {cap} slots, W={windows}, {cap_source}) vs cap {cap_gb} GiB per phase; fused finish witness stream ~{:.0} GB (--wit-ram-gb, default half the cap; overflow spills as sorted varint-delta disc runs) + its own pfck2 grid",
        if arena_ok { "FEASIBLE" } else { "INFEASIBLE" },
        arena_gb, BIN_ARENA_BYTES_PER_SLOT, witness_ram_gb,
    );
    let mut alternatives = Vec::new();
    if !arena_ok {
        alternatives.push(format!("lower --bin-live-cap (the arena pair is 16 B/slot: {} slots would fit 90% of --memory-gb {cap_gb}) or raise --memory-gb", mem_bytes / 10 * 9 / BIN_ARENA_BYTES_PER_SLOT));
    }
    if !disc_ok {
        alternatives.push("free or add scratch space (the bin shape's tree is rlebwt 1.546n + the bounded piece window + the container 0.468n, with rlebwt deleted at publish)".to_string());
    }
    if !survey.cx16 {
        alternatives.push("CPU lacks cmpxchg16b: the shared page pool requires it (CROSS_NO_POOL=1 runs without the pool, 77x slower on the placing pass)".to_string());
    }
    let witness_note = format!(
        "witness stream = 8B x chi; chi/n = {:.3} is the 1GB FIXTURE ratio (the pile value is unmeasured): ~{:.0} GB at this scale; if it exceeds --wit-ram-budget the overflow spills as sorted varint-delta disc runs (est. <= 0.3x raw, EXTRAPOLATED - unmeasured at spill scale) inside the tree",
        V2_CHI_PER_N, witness_ram_gb,
    );
    let transient_note = format!(
        "no chunk set, no merge transient, no position column: the producer's only scratch is the piece window ({:.3} GiB, W={window} bands x 65536 runs) and the rlebwt it is publishing ({:.1} GiB); the arena pair ({:.1} GiB) is anonymous RAM; inputs the finish consumes are unlinked as read",
        pieces_mb / 1024.0, rlebwt_mb / 1024.0, arena_gb,
    );
    let bin_note = format!(
        "{} windows = ceil(n/{cap}); bins are indivisible so a window is packed to <= cap by whole bins (every measured point matches ceil exactly: 1 GB cap 250M -> 5, 1 GB cap>=n -> 1); a single bin larger than the cap makes the producer raise it (BIN_CAP_RAISED) and the front door FAILS LOUD rather than price a different arena. Terms (s/GB at 1 GB): count {:.3} fill {:.3}+{:.3}/window build {:.3}@{threads}t stitch {:.3} = producer {:.2}; finish {:.2} (own work); e2e = max = {:.2}. Measured 1 GB producer wall 41.68 s and with-finish 142.18 s (flow-limited): the model holds the producer term to {:.1}%",
        windows, count_s * 1e9 / n as f64, BIN_FILL_FIRST_NS_PER_BYTE, BIN_FILL_WINDOW_NS_PER_BYTE,
        bin_build_s_per_gb(threads), BIN_STITCH_NS_PER_BYTE, producer_s * 1e9 / n as f64,
        FINISH_DECOMP_NS_PER_BYTE, e2e_s * 1e9 / n as f64,
        100.0 * (producer_s * 1e9 / n as f64) / 41.68,
    );
    let v2 = V2PerTerms {
        corpus_gb: n_gb,
        rlebwt_gb: rlebwt_mb / 1024.0,
        pfck_gb: pfck_mb / 1024.0,
        pieces_window_gb: pieces_mb / 1024.0,
        container_gb: container_mb / 1024.0,
        tree_peak_gb: tree_peak_mb / 1024.0,
        whole_box_gb,
        witness_ram_gb,
        witness_note,
        transient_note,
        bin_live_cap: cap,
        bin_windows: windows,
        bin_arena_gb: arena_gb,
        bin_count_s: count_s,
        bin_fill_s: fill_s,
        bin_build_s: build_s,
        bin_stitch_s: stitch_s,
        bin_producer_s: producer_s,
        bin_finish_s: finish_s,
        bin_e2e_s: e2e_s,
        bin_note,
    };
    Plan { survey, chunks: 1, kway: 1, threads, cap_gb, phases, chunk_bytes_mb: mb,
           pos_mode: "none (no position column in the bin shape)".to_string(),
           disc_verdict, ram_verdict, alternatives, fanin: None, v2: Some(v2) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bin_window_arithmetic_matches_the_measured_points() {
        // 1 GB fixture, the two gated configurations.
        assert_eq!(bin_windows(1_000_000_665, 4_294_967_296), 1, "cap >= n is one window");
        assert_eq!(bin_windows(1_000_000_665, 250_000_000), 5, "the measured --bin-live-cap 250M run had 5 windows");
        // The pile projection's window caps (pile_projection_v2.py).
        assert_eq!(bin_windows(1_310_000_000_000, 20_000_000_000), 66);
        assert_eq!(bin_windows(1_310_000_000_000, 65_000_000_000), 21);
        assert_eq!(bin_windows(1_310_000_000_000, 131_000_000_000), 10);
        assert_eq!(bin_windows(5, 0), 5, "a zero cap must not divide by zero");
    }

    #[test]
    fn build_term_interpolates_and_clamps_on_the_measured_curve() {
        // The measured points themselves.
        assert!((bin_build_s_per_gb(8) - 107.93).abs() < 1e-9);
        assert!((bin_build_s_per_gb(32) - 35.86).abs() < 1e-9);
        assert!((bin_build_s_per_gb(96) - 23.489).abs() < 1e-9);
        // Clamped outside the measured range (more threads buy nothing: the
        // build is memory-bandwidth bound).
        assert!((bin_build_s_per_gb(1) - 107.93).abs() < 1e-9);
        assert!((bin_build_s_per_gb(256) - 23.489).abs() < 1e-9);
        // Monotone non-increasing in log2(threads) between the points.
        let mut last = f64::INFINITY;
        for t in [8u32, 12, 16, 24, 32, 48, 64, 96] {
            let v = bin_build_s_per_gb(t);
            assert!(v <= last + 1e-9, "build term must not rise with threads (t={t})");
            last = v;
        }
    }
}
