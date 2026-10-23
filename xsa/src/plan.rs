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
            s.push_str(&format!("XSA_PLAN_V2_TREE tree_peak={:.3}n bound={:.1}n measured_at=1GB-G2 (output side; the bin producer's own addition is the rolling window files, priced above); whole_box(corpus+tree)={:.3}n; position column disc=0.000n (transient rolling window files only)\n", V2_TREE_PEAK_PER_N, V2_TREE_TARGET_PER_N, 1.0 + V2_TREE_PEAK_PER_N));
            s.push_str(&format!("XSA_PLAN_V2_WITNESS {}\n", v.witness_note));
            s.push_str(&format!("XSA_PLAN_V2_TRANSIENT {}\n", v.transient_note));
            s.push_str(&format!("XSA_PLAN_V2_CAP {}\n", v.bin_cap_note));
            s.push_str(&format!("XSA_PLAN_V2_RAMSPLIT {}\n", v.ram_split_note));
            s.push_str(&format!(
                "XSA_PLAN_V2_BIN live_cap_slots={} windows={} window_file_gb={:.1} ram_group_gb={:.1} count_s={:.0} fill_s={:.0} build_s={:.0} stitch_s={:.0} producer_s={:.0} finish_s={:.0}\n",
                v.bin_live_cap, v.bin_windows, v.bin_window_file_gb, v.bin_ram_group_gb,
                v.bin_count_s, v.bin_fill_s, v.bin_build_s, v.bin_stitch_s, v.bin_producer_s, v.bin_finish_s,
            ));
            // THE TARGET-SIZE TREE: the leaf bound is the only shape knob, and the
            // depth below it is an output of the byte census. Both are printed so
            // the plan cannot be read as if a depth cap decided the leaves.
            s.push_str(&format!(
                "XSA_PLAN_V2_TARGET rows={} leaves={} levels_per_row={:.2} largest_leaf_rows<={} rule=subdivide-while-rows>target (depth is an OUTPUT of the census; the depth cap is a fail-loud backstop, not the rule)\n",
                v.bin_target_rows, v.bin_leaves, v.bin_levels_mean, v.bin_target_rows,
            ));
            // THE POSITION STORE, stated once: the window is a FILE, the RAM is
            // one group. This is the disc-windows amendment to D2 (the rolling
            // amendment): positions materialize transiently in bounded rolling
            // window files, never as a persistent per-suffix column.
            s.push_str(&format!(
                "XSA_PLAN_V2_POSITION_STORE window_file_gb={:.2} = credit({}) x cap({}) x {} B ON DISC (one rolling file per live window, unlinked when built); ram_group_gb={:.2} = {} B/slot x the group being sorted in place; position_io_gb={:.1} = 16 x n / (8+8) B a position written once and read once; position_io_s={:.0} at the 5.238 GB/s device floor (overlapped by the fill and the build, so not on the critical path); persistent_position_column=0 full_SA=0\n",
                v.bin_window_file_gb, BIN_WINDOW_CREDIT, v.bin_live_cap, BIN_WINDOW_SLOT_BYTES,
                v.bin_ram_group_gb, BIN_GROUP_SLOT_BYTES,
                16.0 * v.corpus_gb, BIN_WINDOW_IO_NS_PER_BYTE * v.corpus_gb,
            ));
            s.push_str(&format!(
                "XSA_PLAN_V2_BIN_END e2e_s={:.0} e2e={} producer={} finish={} rule=max(producer,finish) (concurrent, flow-controlled through the piece window)\n",
                v.bin_e2e_s, hms(v.bin_e2e_s), hms(v.bin_producer_s), hms(v.bin_finish_s),
            ));
            s.push_str(&format!("XSA_PLAN_V2_BIN_NOTE {}\n", v.bin_note));
            if let Some(note) = &v.bin_phrase_note {
                s.push_str(&format!("XSA_PLAN_V2_PHRASE {note}\n"));
            }
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
/// The placing pass, per window, at the DEVICE FLOOR. This term was re-priced
/// by the parallel-fill scatter and is no longer a serial full-corpus sweep:
/// the fill is a prefix-sum scatter over NT chunks (measured 213.006 s ->
/// 4.045 s at 10 GB/6 windows = 14.8 GB/s RAM-cached, 21.1 GB/s on a quiet box,
/// 16.8 GB/s warm on the 100 GB snapshot). A 1.31 TB corpus cannot be cached, so
/// the plan uses the measured DEVICE floor for 96 independent O_DIRECT streams,
/// 5.238 GB/s - i.e. 1/5.238e9 s per byte = 0.191 ns/byte per window pass.
/// The window count is what makes this a term at all: every window re-reads the
/// whole corpus (sweep_bytes = (1+windows) x n, asserted on every run).
pub const BIN_FILL_FIRST_NS_PER_BYTE: f64 = 1.0e9 / 5.238e9;
pub const BIN_FILL_WINDOW_NS_PER_BYTE: f64 = 1.0e9 / 5.238e9;
/// THE WINDOW FILES' OWN I/O (disc-windows): every position is written to a
/// window file exactly once and read back exactly once, 8 B each way, so the
/// run moves 16n bytes of position I/O. It overlaps the fill (the fill IS the
/// writer) and the build (the bulk preads are the loader), so it is printed as
/// its own term and NOT added to the critical path; at the device floor it is
/// 16n/5.238 GB/s = 0.382 ns/byte, i.e. ~1.2 h at pile scale.
pub const BIN_WINDOW_IO_NS_PER_BYTE: f64 = 16.0e9 / 5.238e9;
/// The ONE ordered stitch thread (band writers overlapped): 11.41 ns/row at
/// 1 GB = 11.408 s/GB. Thread-count insensitive by construction - it is a
/// single ordered consumer of the bins.
pub const BIN_STITCH_NS_PER_BYTE: f64 = 11.408;
/// The decomposed finish's OWN work: 141.68 s wall at 1 GB minus 22.74 s of
/// producer backpressure (piece wait) = 118.9 s/GB.
pub const FINISH_DECOMP_NS_PER_BYTE: f64 = 118.9;
/// THE POSITION STORE IS A FILE NOW (disc-windows): a window's positions live in
/// a bounded ROLLING FILE in the scratch tree, one 8 B arena word per admitted
/// slot (tail|symbol|hit), written by the fill through a MAP_SHARED mapping and
/// read back one bulk pread per bin. `cap` slots admit `cap` positions, and the
/// fill may run `BIN_WINDOW_CREDIT` windows ahead of the build, so the live
/// scratch term is credit x cap x 8 B - ON DISC, not in RAM.
pub const BIN_WINDOW_SLOT_BYTES: u64 = 8;
/// Windows the fill may run ahead of the build (bit6/binbwt_slice.h,
/// BinWindowOpts::credit; the product default). Live window-file bytes = this x
/// cap x 8 B, and each file is unlinked the moment its bins are built.
pub const BIN_WINDOW_CREDIT: u64 = 2;
/// The BUILD's RAM working set: one bin slice PAIR (arena + scratch), 8 B per
/// slot each. A bin is indivisible (its slice is sorted in place), so the RAM
/// term is 16 B x the largest bin a window admits - the number that forces
/// sharding at pile scale, and the number the operator's 100 GB envelope binds.
pub const BIN_GROUP_SLOT_BYTES: u64 = 16;
/// The fill's WRITE-path staging buffers, per thread (bit6/binbwt_slice.h
/// `bin_fill_stage_bytes_per_thread`): the per-(thread, bin) append buffers the
/// scatter pwrites from, water-filled over the window's bins. Bounded by the
/// thread count, so it is a fixed term of the cap derivation - and it MUST
/// match the producer's constant, since both compute the same cap. 32 MiB is
/// MEASURED: the pwrite rate is a function of the write size (16 KB 3.3 GB/s,
/// 64 KB 6.2, 256 KB 8.2) and 32 MiB/thread water-fills to ~128 KB per stream,
/// where 16 MiB measures worse (2.61 s vs 2.05 s for the 1 GB fill) and 64 MiB
/// worse still (2.83 s, the staging stops fitting in cache).
pub const BIN_FILL_STAGE_BYTES_PER_THREAD: u64 = 32 << 20;
/// The leaf-sort staging budget, per thread (bit6/binbwt_slice.h
/// `bin_leaf_stage_bytes_per_thread`): the bounded span a leaf reads in ONE bulk
/// pread before its comparison sort runs (PART 1 of the prefetch charter). It is
/// a constant of the same derivation as the fill staging above, so the plan and
/// the producer price the identical RAM.
pub const BIN_LEAF_STAGE_BYTES_PER_THREAD: u64 = 8 << 20;
/// Every RAM term of a phase must fit inside this percentage of --memory-gb
/// (bit6/binbwt_slice.h `bin_phase_headroom_pct`).
pub const BIN_PHASE_HEADROOM_PCT: u64 = 90;
/// One corpus page-pool slot's RAM: two 256 B page buffers (cross_lcp_merge.cpp,
/// SharedPages: PS = 256, two buffers per slot). The corpus cache
/// (CorpusText) caches the whole corpus in RAM when it fits this budget.
pub const BIN_POOL_BYTES_PER_SLOT: u64 = 2 * 256;
// ---- PHRASE RADIX (tools/binbwt/PHRASE_RADIX_DESIGN.md) --------------------
// The phrase arm is OFF unless --phrase-radix is given, and when it is on the
// producer pays a PARSE before the bins start. These are measurements, not
// guesses (bit6/phrase_radix.h journals them as PHRASE_DICT sweep_s/sort_s/
// reps_s/parse_s):
//   parse_s  3.780 at 100 MB, 26.936 at 1 GB, 261.310 at 10 GB  -> ~27 s/GiB
//   samples  8,469,786 at 100 MB, 83,920,908 at 1 GB, 834,562,176 at 10 GB
//            -> 0.0834 winnow samples per corpus byte (2/(W+1) for W=24)
//   peak_samples is the WHOLE sample array in RAM at once: 8 B x 0.0834 n
//   = 0.667n. That is the arm's real memory term and the plan must show it.
pub const BIN_PHRASE_PARSE_S_PER_GB: f64 = 27.0;
pub const BIN_PHRASE_SAMPLES_PER_N: f64 = 0.0834;
pub const BIN_PHRASE_SAMPLE_BYTES: f64 = 8.0;
/// The plan's phrase line: None when the arm is off (the default), so a plan of
/// the product path does not mention a mechanism it will not run.
pub fn bin_phrase_note(corpus_bytes: u64, armed: bool) -> Option<String> {
    if !armed {
        return None;
    }
    let n_gb = corpus_bytes as f64 / (1u64 << 30) as f64;
    let parse_s = BIN_PHRASE_PARSE_S_PER_GB * n_gb;
    let samples = BIN_PHRASE_SAMPLES_PER_N * corpus_bytes as f64;
    let sample_gb = samples * BIN_PHRASE_SAMPLE_BYTES / (1u64 << 30) as f64;
    let rate = BIN_PHRASE_SAMPLES_PER_N;
    let per_gb = BIN_PHRASE_PARSE_S_PER_GB;
    Some(format!(
        "armed=1 (--phrase-radix) default=OFF product_path=unchanged parse_s={parse_s:.0} \
         (measured {per_gb:.0} s/GiB: 3.78 s at 100 MB, 26.94 s at 1 GB, 261.31 s at 10 GB) \
         parse_samples={samples:.0} peak_sample_ram_gb={sample_gb:.1} (8 B x {rate:.4} n, the winnow's sampled k-mers, held all at once) \
         dict_ram<=pool_max (default 16 MiB) verify=byte-compared (a hash collision can only LOSE a skip, never produce a wrong order) \
         effect=construction work is unchanged except at nodes a phrase certifies (PHRASE_WORK skips=; measured 0 skips at 100 MB, 1 GB and 10 GB with the shipped dictionary)"
    ))
}
/// The page-pool slot count the front door passes as CROSS_POOL_SLOTS: cover the
/// corpus text (n/128) within 45% of the memory cap, clamped to [2^20, 2^27].
/// One function, called by the plan (to price the cache term) and by the chain
/// (to set the env), so the two can never disagree.
pub fn bin_pool_slots(n: u64, mem_bytes: u64) -> u64 {
    let pow2ceil = |mut x: u64| {
        if x <= 1 { return 1; }
        x -= 1;
        let mut p = 1u64;
        while p < x { p <<= 1; }
        p
    };
    let want = pow2ceil((n / 128).max(1));
    let cap = pow2ceil((mem_bytes * 45 / 100) / (3 * 528));
    want.min(cap).clamp(1 << 20, 1u64 << 27)
}
/// The corpus text the producer holds in RAM: CorpusText caches the whole corpus
/// when it fits the pool budget, so the term is min(n, 512 B x pool slots).
pub fn bin_corpus_cache_bytes(n: u64, mem_bytes: u64) -> u64 {
    (BIN_POOL_BYTES_PER_SLOT * bin_pool_slots(n, mem_bytes)).min(n)
}
/// The cap derivation, term by term (bit6/binbwt_slice.h `BinCapTerms`, which
/// implements the same arithmetic against the live RLIMIT_AS).
pub struct BinCapTerms {
    pub limit: u64,   // --memory-gb in bytes (the phase's RLIMIT_AS)
    pub usable: u64,  // headroom x limit
    pub cache: u64,   // the corpus text in RAM
    pub stage: u64,   // the fill's pwrite staging buffers
    pub group: u64,   // what is left for the arena+scratch pair
    pub cap: u64,     // derived live cap, slots
    pub disc_cap: u64,// the scratch side's own bound on the cap
    pub bound_by: &'static str,
}
/// THE FILE(R): memory x 90% = cache + staging + 16 B/slot x min(cap, n), and
/// SCRATCH x 90% = credit x cap x 8 B + the producer's own disc output (rlebwt +
/// piece window). The first bound is the arena pair the build sorts a bin in;
/// the second exists so the rolling window files can never be planned into a
/// scratch tree that has no room for them.
pub fn bin_derive_live_cap(
    n: u64, threads: u64, mem_bytes: u64, free_bytes: u64,
    producer_disc_bytes: u64,
) -> BinCapTerms {
    let usable = mem_bytes * BIN_PHASE_HEADROOM_PCT / 100;
    let cache = bin_corpus_cache_bytes(n, mem_bytes);
    let stage = threads * (BIN_FILL_STAGE_BYTES_PER_THREAD + BIN_LEAF_STAGE_BYTES_PER_THREAD);
    let group = usable.saturating_sub(cache + stage);
    let addr_cap = (group / BIN_GROUP_SLOT_BYTES).max(1).min(n.max(1));
    let disc_budget = (free_bytes * BIN_PHASE_HEADROOM_PCT / 100)
        .saturating_sub(producer_disc_bytes);
    let disc_cap = (disc_budget / (BIN_WINDOW_CREDIT * BIN_WINDOW_SLOT_BYTES))
        .max(1).min(n.max(1));
    let (cap, bound_by) = if addr_cap <= disc_cap {
        (addr_cap, "address space (--memory-gb): corpus cache + fill staging + 16 B/slot arena pair")
    } else {
        (disc_cap, "scratch: credit x window file + the producer's own disc output")
    };
    BinCapTerms { limit: mem_bytes, usable, cache, stage, group,
                  cap, disc_cap: disc_cap, bound_by }
}
/// Does an EXPLICIT cap fit the phase budget? `None` = it does, `Some(largest)`
/// = it does not and `largest` is the most this phase could hold.
pub fn bin_cap_over_budget(
    cap: u64, n: u64, threads: u64, mem_bytes: u64,
) -> Option<u64> {
    let usable = mem_bytes * BIN_PHASE_HEADROOM_PCT / 100;
    let fixed = bin_corpus_cache_bytes(n, mem_bytes)
        + threads * (BIN_FILL_STAGE_BYTES_PER_THREAD + BIN_LEAF_STAGE_BYTES_PER_THREAD);
    let need = fixed.saturating_add(
        BIN_GROUP_SLOT_BYTES.saturating_mul(cap.min(n.max(1))));
    if need <= usable { return None; }
    Some(usable.saturating_sub(fixed) / BIN_GROUP_SLOT_BYTES)
}
/// The per-bin construction critical path, seconds per 1 GB fixture, at the
/// thread counts it was measured at (t=8/32: bin-bwt v1 build_s; t=96: the v2
/// producer-only run). The build is memory-bandwidth bound long before 96
/// threads (24.7 ns/symbol-thread at t=96 vs 5.46 uncontended), so the curve is
/// interpolated in log2(threads) between measured points and clamped.
/// The 96-thread point is the 10 GB rung with the target-size default (300.4 s
/// build for 10 GB = 30.04 s/GB, RESULTS_AUTO_RADIX.md), not the 1 GB fixture
/// (18-23 s/GB): a bigger corpus holds deeper repeats, and the plan must price
/// the corpus it is asked about. Both anchors are measured; the larger one is
/// used because the pile is larger than both.
pub const BIN_BUILD_POINTS: [(f64, f64); 3] = [(8.0, 107.93), (32.0, 35.86), (96.0, 30.04)];

/// THE TARGET SIZE (rows) - the leaf bound of the recursion, i.e. the largest
/// number of rows a single comparison sort may take (bit6/binbwt_slice.h,
/// bin_sort_rec). Depth is an OUTPUT of the data, so this is the one shape knob
/// and the plan must price what the producer will actually use.
///
/// MEASURED LADDER (tools/binbwt/RESULTS_AUTO_RADIX.md, 1 GB fixture, 96t, one
/// window, --bin-probe, box load 65-96 labelled): the leaf sort pays log2(m)
/// LCE walks per row while a bucketing level pays ONE coded symbol per row, so
/// the exchange rate runs steeply TOWARD SMALL leaves:
///
/// ```text
///   target     build_s   build_cpu_s   symbols      comparisons  leaf_rows_max
///   24 (old)    20.55      1928        101.1e9      3.00e9       4.74e6 (!)
///   262144      17.29      1544        205.0e9      19.0e9       0.77e6
///   1e6         18.16      1627        222.3e9      21.1e9       1.00e6
///   4e6         42.62      3628        248.0e9      23.6e9       3.97e6
///   8e6         45.08      3349        260.2e9      24.9e9       7.93e6
///   16e6        57.65      3715        272.5e9      26.3e9      15.92e6
///   32e6        57.60      2049        280.6e9      27.2e9      29.49e6
/// ```
///
/// The operator's candidate range (8-32M rows) measures 2-3x SLOWER than
/// 256K-1.3M here, and the reason is the trade the numbers show: at 8-32M the
/// tree is 1-16 levels deep and the leaf sorts own log2(m) x LCE, while at
/// 256K-1.3M the tree is 40-200 levels deep and each level is one cheap coded
/// symbol per row. The default is therefore 1M rows: at the bottom of the
/// measured flat region (256K-1.3M, all within 6% of each other on build_s) and
/// 1M leaves per 1 TB of corpus, i.e. ~4 x 10^4 leaves per thread at pile scale,
/// so the queue never starves. It also keeps the largest serial stretch (one
/// leaf sort) at a million rows.
pub const BIN_TARGET_ROWS: u64 = 1 << 20;
/// The automatic rule the producer applies when the target is not pinned,
/// mirrored here so the plan prices the run the producer will perform:
/// clamp(n / (8 x threads), 256K, 1M). It scales DOWN for small corpora (a
/// 100 MB fixture still yields thousands of leaves) and saturates at the 1M rows
/// that BOTH ladders picked (1 GB: 17.29 s build at 262144, 18.16 at 1e6;
/// 10 GB: 458.5 s wall at 262144, 444.1 at 1e6, 485.1 at 4e6). 4M+ is never
/// chosen: the ladder measures it worse, not better.
/// level_elems per row - the emergent tree depth, weighted by the rows coded at
/// each level - as a function of the leaf bound. MEASURED on the target-size
/// ladder (1 GB fixture, 96t, RESULT_AUTO_RADIX.md; the 10 GB probe reproduces
/// the ordering): the tree gets shallower as the leaf bound grows, ~0.5 levels
/// per doubling of the target, which is exactly the trade the ladder prices (a
/// level is one coded symbol per row; a leaf is log2(m) LCE walks per row).
/// Interpolated in log2(target) and clamped outside the measured range.
pub const BIN_LEVELS_POINTS: [(f64, f64); 6] =
    [(2.62144e5, 3.49), (1.0e6, 2.75), (4.0e6, 1.75), (8.0e6, 1.31), (1.6e7, 0.88), (3.2e7, 0.61)];

pub fn bin_levels_per_row(target: u64) -> f64 {
    let t = (target.max(2) as f64).log2();
    let first = BIN_LEVELS_POINTS[0];
    let last = BIN_LEVELS_POINTS[BIN_LEVELS_POINTS.len() - 1];
    if t <= first.0.log2() { return first.1; }
    if t >= last.0.log2() { return last.1; }
    for pair in BIN_LEVELS_POINTS.windows(2) {
        let (a, b) = (pair[0], pair[1]);
        if t <= b.0.log2() {
            let frac = (t - a.0.log2()) / (b.0.log2() - a.0.log2());
            return a.1 + frac * (b.1 - a.1);
        }
    }
    last.1
}

pub fn bin_target_rows_auto(n: u64, threads: u32) -> u64 {
    let share = n / (8 * threads.max(1) as u64).max(1);
    share.clamp(1 << 18, 1 << 20)
}

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
    pub bin_live_cap: u64,   // resolved position-window bound, in slots
    pub bin_windows: u64,    // ceil(n / cap)
    pub bin_window_file_gb: f64, // credit x cap x 8 B: LIVE WINDOW-FILE bytes (disc)
    pub bin_ram_group_gb: f64,   // 16 B/slot x the group the build refines (RAM)
    pub bin_target_rows: u64,    // the leaf bound the producer will use
    pub bin_leaves: u64,         // ~ceil(n / target): comparison sorts
    pub bin_levels_mean: f64,    // measured level_elems per row at this target
    pub bin_count_s: f64,
    pub bin_fill_s: f64,
    pub bin_build_s: f64,
    pub bin_stitch_s: f64,
    pub bin_producer_s: f64,
    pub bin_finish_s: f64,
    pub bin_e2e_s: f64,      // max(producer, finish): they run concurrently
    pub bin_note: String,
    pub bin_cap_note: String,
    pub ram_split_note: String, // the derived/live cap's own arithmetic, term by term
    /// `--phrase-radix`: Some(note) only when the operator armed the phrase
    /// dictionary. The plan must never claim the arm is free: the parse is a
    /// real producer-side cost and it is priced here from the measured rate.
    pub bin_phrase_note: Option<String>,
}

/// The bin plan. `live_cap` is the operator's `--bin-live-cap` (None = let the
/// plan pick from the memory cap; Some(0) = unbounded, one window).
pub fn model_v2(
    survey: Survey, _chunks: u32, _kway: u32, threads: u32, cap_gb: u64,
    hygiene_free_pct: u32, live_cap: Option<u64>, target: Option<u64>,
) -> Plan {
    let n = survey.corpus_bytes;
    let mb = n as f64 / (1024.0 * 1024.0);
    let n_gb = mb / 1024.0;
    let free_mb = survey.scratch_free_bytes as f64 / (1024.0 * 1024.0);
    // ---- the position-arena bound and the window arithmetic
    // One window needs one slot per position of the contiguous bin range it
    // admits; `cap` bounds that live set. The arena pair is 16 B/slot.
    let mem_bytes = cap_gb.saturating_mul(1 << 30);
    // ---- the disc terms the cap's SCRATCH bound needs (pure functions of n) --
    let rlebwt_mb = mb * V2_RLEBWT_PER_N;
    let pfck_mb = mb * V2_PFCK_PER_N;
    let piece_window = std::env::var("CROSS_V2_PIECE_WINDOW").ok()
        .and_then(|v| v.parse::<u64>().ok()).unwrap_or(1100);
    // PRC1: char + u32 len + two packed positions; headers and BPG1 class
    // metadata receive a conservative 32-byte/run allowance.
    let pieces_mb = (n as f64).min(piece_window as f64 * 65536.0) * 32.0 / 1048576.0;
    let container_mb = mb * V2_CONTAINER_PER_N;
    let tree_peak_mb = mb * V2_TREE_PEAK_PER_N;
    let producer_disc_bytes = ((rlebwt_mb + pieces_mb) * 1024.0 * 1024.0) as u64;
    // ---- THE LIVE CAP: DERIVED, NEVER UNBOUNDED ---------------------------
    // The default used to be `85% of --memory-gb / 16 B`, i.e. the arena pair
    // alone, and the 10 GB run died because the window's MAP_SHARED mapping and
    // the corpus cache were not in that arithmetic at all. The derivation is now
    // the honest sum of every RAM term of the producer phase, at
    // BIN_PHASE_HEADROOM_PCT of the phase cap:
    //
    //   memory x 90%  =  corpus cache (min(n, 512 B x pool slots))      [CorpusText]
    //                  + fill staging (threads x 4 MiB)                  [BinFillSink]
    //                  + 16 B/slot x min(cap, n)                        [arena+scratch]
    //   scratch x 90% = credit x cap x 8 B + rlebwt(1.546n) + pieces      [the window files]
    //
    // The window FILE is on the SCRATCH side, not the memory side: since the
    // write path is buffered pwrite it charges no address space at all (that is
    // the other half of this change - see bit6/binbwt_slice.h BinFillSink).
    // The FINISH's own footprint is a separate process under the same cap and is
    // checked against it on its own (ram_verdict), not subtracted here.
    let auto = bin_derive_live_cap(n, threads as u64, mem_bytes,
                                  survey.scratch_free_bytes, producer_disc_bytes);
    let (cap, cap_source) = match live_cap {
        Some(0) => (n, "--bin-live-cap 0: unbounded (one window, no windowing)".to_string()),
        Some(v) => (
            v.min(n.max(1)),
            if v > n { format!("--bin-live-cap {v} exceeds n={n}: capped at n (one window)") }
            else { format!("--bin-live-cap {v} (operator)") },
        ),
        None => (
            auto.cap,
            format!("auto from --memory-gb {cap_gb} (bound by {})",
                    if auto.bound_by.starts_with("address") { "address space" } else { "scratch" }),
        ),
    };
    // An EXPLICIT cap that the phase cannot hold is INFEASIBLE here (and the
    // front door refuses it before spawning anything): the producer would die
    // mid-window with a bad_alloc, which is exactly what the plan exists to
    // prevent.
    let explicit_over_budget = match live_cap {
        Some(v) if v != 0 => bin_cap_over_budget(v, n, threads as u64, mem_bytes),
        _ => None,
    };
    let windows = bin_windows(n, cap);
    // ---- THE POSITION STORE, the disc-windows way --------------------------
    // The window is a FILE: `cap` slots x 8 B on disc, `BIN_WINDOW_CREDIT` of
    // them live at once, each unlinked the moment its bins are built. There is
    // NO RAM arena term for the window any more (the disc-windows pivot): the
    // RAM the build needs is ONE GROUP - the arena+scratch pair it sorts a bin
    // in place in, 16 B per slot of that group. A bin is indivisible, so the
    // group is the largest bin the window admits; the plan carries `cap` as its
    // bound because the producer RAISES `cap` to the largest bin (BIN_CAP_RAISED
    // - a misconfiguration the front door fails loud on), and the honest RAM
    // number for a window is therefore 16 B x cap, i.e. the sharding driver.
    let window_file_gb =
        (BIN_WINDOW_CREDIT * cap * BIN_WINDOW_SLOT_BYTES) as f64 / (1024.0 * 1024.0 * 1024.0);
    let ram_group_gb = (cap as f64 * BIN_GROUP_SLOT_BYTES as f64) / (1024.0 * 1024.0 * 1024.0);
    // ---- THE TARGET-SIZE TREE (levels are emergent from the census) --------
    // The recursion splits a node while it holds more than `target` rows, so the
    // tree's DEPTH is an output of the byte census and the leaf count is
    // arithmetic: ~ceil(n/target) comparison sorts, each of at most `target`
    // rows. `bin_levels_mean` is the measured level_elems/row ratio of the
    // ladder run at this target (RESULTS_AUTO_RADIX.md): it is what the plan
    // means by "depth", and it is the only tree number that is not arithmetic.
    let target_rows = target.unwrap_or_else(|| bin_target_rows_auto(n, threads));
    let leaves = ((n + target_rows - 1) / target_rows).max(1);
    let levels_mean = bin_levels_per_row(target_rows);
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
    // The byte counts themselves are computed above, where the cap derivation
    // needs them (the scratch bound of the cap).
    let producer_disc_mb = rlebwt_mb + pieces_mb;
    let phases = vec![
        PhaseRow { name: "bin(sweep)".into(), wall_s: count_s + fill_s,
                   disc_peak_mb: pieces_mb + window_file_gb * 1024.0,
                   rss_gb: ram_group_gb },
        PhaseRow { name: "bin(build)".into(), wall_s: build_s,
                   disc_peak_mb: pieces_mb + window_file_gb * 1024.0,
                   rss_gb: ram_group_gb },
        PhaseRow { name: "bin(stitch)".into(), wall_s: stitch_s,
                   disc_peak_mb: producer_disc_mb + window_file_gb * 1024.0,
                   rss_gb: ram_group_gb },
        // CONCURRENT with the producer: the decomposed fused finish (pfck2 ->
        // seam classes -> sweep -> witnesses -> container publish), which owns
        // the tree peak.
        PhaseRow { name: "fused-finish".into(), wall_s: finish_s,
                   disc_peak_mb: tree_peak_mb.max(producer_disc_mb),
                   rss_gb: 8.0 * V2_CHI_PER_N * n_gb },
    ];
    // ---- verdicts
    let tree_ok = tree_peak_mb < free_mb;
    // The window files are IN the tree (scratch tree, bounded, unlinked as
    // built): the all-phase disc peak has to carry them next to whatever else is
    // live at that moment.
    let window_file_mb = window_file_gb * 1024.0;
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
    // The RAM term is ONE BUILD GROUP (arena + scratch pair of the bin being
    // sorted in place): 16 B x the group's slots. The window itself is a FILE,
    // so it contributes 0 B of RAM. Every phase runs under the SAME RLIMIT_AS,
    // so 90% of it is the bar; over the bar the run dies with a bad_alloc
    // mid-window, which is exactly what the plan is for.
    // THE PHASE-CAP CHECK, stated in the terms the derivation uses: the build
    // group's arena pair + the corpus cache + the fill staging must fit inside
    // BIN_PHASE_HEADROOM_PCT of --memory-gb. An explicit cap that does not is
    // INFEASIBLE (the front door refuses it before it spawns the producer).
    let cache_bytes = bin_corpus_cache_bytes(n, mem_bytes);
    let stage_bytes =
        threads as u64 * (BIN_FILL_STAGE_BYTES_PER_THREAD + BIN_LEAF_STAGE_BYTES_PER_THREAD);
    let arena_bytes = cap.saturating_mul(BIN_GROUP_SLOT_BYTES)
        .saturating_add(cache_bytes).saturating_add(stage_bytes);
    let arena_ok = arena_bytes <= mem_bytes * BIN_PHASE_HEADROOM_PCT / 100
        && explicit_over_budget.is_none();
    // THE FINISH's own footprint, checked against the SAME phase cap (it is its
    // own process under the same RLIMIT_AS): the witness stream it holds in RAM.
    let finish_bytes = (8.0 * V2_CHI_PER_N * n as f64) as u64;
    let finish_ok = finish_bytes <= mem_bytes * BIN_PHASE_HEADROOM_PCT / 100;
    let witness_ram_gb = 8.0 * V2_CHI_PER_N * n_gb;
    let ram_verdict = format!(
        "{}: RAM term = ONE BUILD GROUP ~{:.1} GiB ({} B/slot x {} group slots - the bin being sorted in place, W={} windows, {}) + the corpus cache {:.1} GiB + the fill staging {:.1} GiB = {:.1} GiB against the usable {}% of --memory-gb {} GiB ({}); the position WINDOW is {:.1} GiB of FILE ({} credit x {} slots x {} B), 0 B of address space (buffered pwrite, no mapping); fused finish witness stream ~{:.0} GB {} the same cap (--wit-ram-gb, default half the cap; the overflow spills as sorted varint-delta disc runs, so an over-cap witness stream costs disc, not feasibility) + its own pfck2 grid",
        if arena_ok { "FEASIBLE" } else { "INFEASIBLE" },
        ram_group_gb, BIN_GROUP_SLOT_BYTES, cap, windows, cap_source,
        cache_bytes as f64 / (1u64 << 30) as f64, stage_bytes as f64 / (1u64 << 30) as f64,
        arena_bytes as f64 / (1u64 << 30) as f64, BIN_PHASE_HEADROOM_PCT, cap_gb,
        if arena_ok { "fits" } else { "DOES NOT FIT - lower --bin-live-cap or raise --memory-gb" },
        window_file_gb,
        BIN_WINDOW_CREDIT, cap, BIN_WINDOW_SLOT_BYTES, witness_ram_gb,
        if finish_ok { "fits" } else { "does not fit (it spills)" },
    );
    let mut alternatives = Vec::new();
    if explicit_over_budget.is_some() {
        alternatives.push(format!(
            "the --bin-live-cap you passed cannot fit the phase budget: at --memory-gb {cap_gb} the corpus cache ({} B) + fill staging ({} B) + 16 B/slot arena pair leave room for {} slots. Lower --bin-live-cap or raise --memory-gb (the producer refuses this cap at startup, before it allocates anything)",
            cache_bytes, stage_bytes,
            bin_cap_over_budget(live_cap.unwrap_or(0), n, threads as u64, mem_bytes).unwrap_or(0)));
    }
    if !arena_ok {
        alternatives.push(format!("lower --bin-live-cap (a build group is {} B/slot: {} slots fit {}% of --memory-gb {cap_gb} once the corpus cache ({} B) and the fill staging ({} B) are paid) or raise --memory-gb; a bin larger than the group raises the cap and the front door fails loud, so SHARDING is the other lever", BIN_GROUP_SLOT_BYTES, (mem_bytes * BIN_PHASE_HEADROOM_PCT / 100).saturating_sub(cache_bytes + stage_bytes) / BIN_GROUP_SLOT_BYTES, BIN_PHASE_HEADROOM_PCT, cache_bytes, stage_bytes));
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
        "no chunk set, no merge transient, no persistent position column: the producer's scratch is the piece window ({:.3} GiB, W={piece_window} bands x 65536 runs), the rlebwt it is publishing ({:.1} GiB) and the ROLLING WINDOW FILES ({:.1} GiB on disc = {} credit x cap x {} B, one file per live window, unlinked the moment its bins are built); the build's RAM is one group ({:.1} GiB, 16 B/slot) and NEVER the window; inputs the finish consumes are unlinked as read",
        pieces_mb / 1024.0, rlebwt_mb / 1024.0, window_file_gb, BIN_WINDOW_CREDIT,
        BIN_WINDOW_SLOT_BYTES, ram_group_gb,
    );
    let bin_note = format!(
        "{} windows = ceil(n/{cap}) (each window re-reads the whole corpus: fill = windows x n at the 5.238 GB/s device floor = {:.2} h at this scale); bins are indivisible so a window is packed to <= cap by whole bins (every measured point matches ceil exactly: 1 GB cap 250M -> 5, 1 GB cap>=n -> 1); a single bin larger than the cap makes the producer raise it (BIN_CAP_RAISED) and the front door FAILS LOUD rather than price a different arena. The TREE: {} leaves = ceil(n/target), each a comparison sort of <= {target_rows} rows (depth is an OUTPUT of the byte census; mean levels/row {:.2} measured at this target, i.e. the emergent depth is {:.0} for the busiest bucket, not a parameter). Terms (s/GB at 1 GB): count {:.3} fill {:.3}+{:.3}/window build {:.3}@{threads}t stitch {:.3} = producer {:.2}; finish {:.2} (own work); e2e = max = {:.2}. Measured 1 GB producer wall 41.68 s and with-finish 142.18 s (flow-limited): the model holds the producer term to {:.1}%",
        windows, fill_s / 3600.0, leaves, levels_mean, (leaves as f64).log(257.0),
        count_s * 1e9 / n as f64, BIN_FILL_FIRST_NS_PER_BYTE, BIN_FILL_WINDOW_NS_PER_BYTE,
        bin_build_s_per_gb(threads), BIN_STITCH_NS_PER_BYTE, producer_s * 1e9 / n as f64,
        FINISH_DECOMP_NS_PER_BYTE, e2e_s * 1e9 / n as f64,
        100.0 * (producer_s * 1e9 / n as f64) / 41.68,
    );
    // THE CAP'S ARITHMETIC, printed: both bounds, every term, and which one binds.
    let cap_note = format!(
        "live_cap_slots={cap} source={cap_source} bound_by={}; \
MEMORY bound: {}% of --memory-gb {} GiB ({} B) = {} B usable - corpus cache {} B \
(min(n, {} B x pool slots {})) - staging {} B ({} threads x ({} B fill + {} B leaf)) = {} B \
/ {} B per build-group slot = {} slots; SCRATCH bound: {}% of {:.1} GiB free = {} B \
- rlebwt+pieces {} B = {} B / (credit {} x {} B per slot) = {} slots; \
window_file_gb={:.2} = credit x cap x 8 B ON DISC (0 B of address space: the fill \
writes with buffered pwrite, never a mapping); the finish is its own process under \
the same cap: witness {:.1} GiB {} headroom_pct={}",
        auto.bound_by, BIN_PHASE_HEADROOM_PCT, cap_gb, mem_bytes, auto.usable,
        auto.cache, BIN_POOL_BYTES_PER_SLOT, bin_pool_slots(n, mem_bytes),
        auto.stage, threads, BIN_FILL_STAGE_BYTES_PER_THREAD,
        BIN_LEAF_STAGE_BYTES_PER_THREAD, auto.group,
        BIN_GROUP_SLOT_BYTES, auto.group / BIN_GROUP_SLOT_BYTES,
        BIN_PHASE_HEADROOM_PCT,
        survey.scratch_free_bytes as f64 / (1u64 << 30) as f64,
        survey.scratch_free_bytes * BIN_PHASE_HEADROOM_PCT / 100, producer_disc_bytes,
        survey.scratch_free_bytes * BIN_PHASE_HEADROOM_PCT / 100 - producer_disc_bytes,
        BIN_WINDOW_CREDIT, BIN_WINDOW_SLOT_BYTES, auto.disc_cap,
        window_file_gb, witness_ram_gb,
        if finish_ok { "fits it," } else { "exceeds it (the finish spills the overflow as sorted varint-delta disc runs; --wit-ram-gb caps the in-RAM part)," },
        BIN_PHASE_HEADROOM_PCT,
    );
    // ---- PART 3: THE FAT-POOL POLICY, RENDERED AS A SPLIT -------------------
    // One line, every RAM term of the two processes, so the trade the cap makes
    // is visible instead of implied by the formula:
    //   pool        the corpus page pool (CROSS_POOL_SLOTS x 512 B) - the HOT SET
    //               and the reason it is fat: the census says the hot set of a
    //               pangenome pile is its BOILERPLATE EPOCH (the shared, repeated
    //               backbone), so the pool is sized to pin the corpus text
    //               whenever --memory-gb allows, and only scales down when it
    //               cannot. At 1 GB and 10 GB it covers the corpus whole; at the
    //               pile it is the declared share below.
    //   staging     fill pwrite buffers (32 MiB/thread) + leaf-sort staging
    //               (8 MiB/thread): the two bounded transient buffers.
    //   arena       what is left: 16 B/slot x cap, the group the build sorts.
    //   finish      the finish is its OWN process under the same cap: witness
    //               stream (--wit-ram-gb) plus its pfck grid.
    let pool_bytes = BIN_POOL_BYTES_PER_SLOT * bin_pool_slots(n, mem_bytes);
    let pool_covers_pct = if n > 0 { 100.0 * (pool_bytes.min(n) as f64) / (n as f64) } else { 0.0 };
    let pool_share_pct = if mem_bytes > 0 { 100.0 * (pool_bytes as f64) / (mem_bytes as f64) } else { 0.0 };
    let fill_stage_bytes = threads as u64 * BIN_FILL_STAGE_BYTES_PER_THREAD;
    let leaf_stage_bytes = threads as u64 * BIN_LEAF_STAGE_BYTES_PER_THREAD;
    let ram_split_note = format!(
        "pool_gb={:.2} pool_share_of_memory_pct={:.1} pool_covers_corpus_pct={:.1} fill_stage_gb={:.2} leaf_stage_gb={:.2} arena_group_gb={:.2} finish_witness_gb={:.2} finish_pfck_grid_gb={:.2} usable_gb={:.2} rule={}",
        pool_bytes as f64 / (1u64 << 30) as f64, pool_share_pct, pool_covers_pct,
        fill_stage_bytes as f64 / (1u64 << 30) as f64,
        leaf_stage_bytes as f64 / (1u64 << 30) as f64,
        auto.group as f64 / (1u64 << 30) as f64,
        witness_ram_gb, pfck_mb / 1024.0,
        auto.usable as f64 / (1u64 << 30) as f64,
        "the pool is sized FIRST and pinned to the whole corpus text whenever --memory-gb \
allows (the hot set is the boilerplate epoch); the two staging buffers are constants \
per thread; the arena pair takes the remainder; the finish is a second process under \
the same cap and owns its witness stream + pfck grid",
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
        bin_window_file_gb: window_file_gb,
        bin_ram_group_gb: ram_group_gb,
        bin_target_rows: target_rows,
        bin_leaves: leaves,
        bin_levels_mean: levels_mean,
        bin_count_s: count_s,
        bin_fill_s: fill_s,
        bin_build_s: build_s,
        bin_stitch_s: stitch_s,
        bin_producer_s: producer_s,
        bin_finish_s: finish_s,
        bin_e2e_s: e2e_s,
        bin_note,
        bin_cap_note: cap_note,
        ram_split_note,
        bin_phrase_note: None,
    };
    Plan { survey, chunks: 1, kway: 1, threads, cap_gb, phases, chunk_bytes_mb: mb,
           pos_mode: "none (no position column in the bin shape)".to_string(),
           disc_verdict, ram_verdict, alternatives, fanin: None, v2: Some(v2) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_phrase_plan_line_is_absent_when_the_arm_is_off_and_priced_when_on() {
        // The product path's plan must not mention a mechanism it will not run.
        assert!(bin_phrase_note(1_000_000_665, false).is_none());
        let note = bin_phrase_note(1_000_000_665, true).expect("on = a line");
        // The parse is a real cost and the plan must state it, at the measured
        // rate (26.94 s at 1 GB), not hide it in the producer term.
        assert!(note.contains("parse_s=25"), "{note}");
        assert!(note.contains("27 s/GiB"), "{note}");
        // The arm's real RAM term: the winnow's whole sample array, 8 B x 0.0834 n.
        assert!(note.contains("peak_sample_ram_gb=0.6"), "{note}");
        assert!(note.contains("default=OFF"), "{note}");
        assert!(note.contains("product_path=unchanged"), "{note}");
        // 10 GB is the rung this exists for: 261 s of parse, 6.3 GiB of samples.
        let big = bin_phrase_note(10_000_000_000, true).unwrap();
        assert!(big.contains("parse_s=251") || big.contains("parse_s=252"), "{big}");
        assert!(big.contains("peak_sample_ram_gb=6.2"), "{big}");
    }

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
    fn the_auto_live_cap_fits_the_phase_budget_with_headroom() {
        // THE CONFIG THAT FAILED LIVE (sol10.log): 10 GB corpus, --memory-gb 100
        // (PHASE_CAP_KB=104857600), 96 threads, default cap. The old default was
        // 85% of the cap / 16 B = 5,704,253,440 slots, which priced the arena
        // pair alone: the window that came out was 44.6 GB of MAP_SHARED and the
        // mapping blew the cap. The derived cap must satisfy the whole equation.
        let n = 10_000_017_258u64;
        let mem = 100u64 << 30;
        let free = 2569u64 << 30;                     // the box's scratch free
        let disc = (1.546 * n as f64) as u64;          // rlebwt + piece window
        let t = bin_derive_live_cap(n, 96, mem, free, disc);
        assert!(t.cache + t.stage + BIN_GROUP_SLOT_BYTES * t.cap <= mem * BIN_PHASE_HEADROOM_PCT / 100,
                "the derived cap's RAM terms must fit the headroom");
        assert_eq!(t.cache, n, "a 10 GB corpus fits the pool budget, so the cache IS the corpus");
        assert_eq!(t.stage, 96 * (BIN_FILL_STAGE_BYTES_PER_THREAD + BIN_LEAF_STAGE_BYTES_PER_THREAD));
        assert!(t.cap < n, "10 GB at --memory-gb 100 must still be windowed");
        assert!(t.cap < 5_704_253_440, "the 85%-of-cap default was over budget");
        assert_eq!(bin_windows(n, t.cap), 2, "the 10 GB rung is two windows");
        // The window FILE is disc: credit x cap x 8 B, and it must fit scratch.
        assert!((BIN_WINDOW_CREDIT * t.cap * BIN_WINDOW_SLOT_BYTES) < free * BIN_PHASE_HEADROOM_PCT / 100 - disc);
        // The 1 GB and 100 MB fixtures stay ONE window at the gate's memory caps
        // (that is what the byte-identity banks were produced with).
        let c1 = bin_derive_live_cap(1_000_000_665, 96, 40u64 << 30, free, disc / 10);
        assert_eq!(c1.cap, 1_000_000_665, "1 GB @ --memory-gb 40 is one window");
        let c100 = bin_derive_live_cap(100_000_503, 96, 24u64 << 30, free, disc / 100);
        assert_eq!(c100.cap, 100_000_503, "100 MB @ --memory-gb 24 is one window");
    }

    #[test]
    fn an_explicit_cap_over_the_budget_reports_the_largest_that_fits() {
        let n = 10_000_017_258u64;
        let mem = 100u64 << 30;
        assert_eq!(bin_cap_over_budget(n, n, 96, mem), Some(largest_fit(n, 96, mem)),
                   "an unbounded cap at 10 GB/100 GiB cannot fit");
        let t = bin_derive_live_cap(n, 96, mem, 2569u64 << 30, 0);
        assert_eq!(bin_cap_over_budget(t.cap, n, 96, mem), None,
                   "the derived cap always fits");
        // A 1 GB fixture at --memory-gb 40 with cap = n fits.
        assert_eq!(bin_cap_over_budget(1_000_000_665, 1_000_000_665, 96, 40u64 << 30), None);
    }

    fn largest_fit(n: u64, threads: u64, mem_bytes: u64) -> u64 {
        let usable = mem_bytes * BIN_PHASE_HEADROOM_PCT / 100;
        let fixed = bin_corpus_cache_bytes(n, mem_bytes)
            + threads * (BIN_FILL_STAGE_BYTES_PER_THREAD + BIN_LEAF_STAGE_BYTES_PER_THREAD);
        usable.saturating_sub(fixed) / BIN_GROUP_SLOT_BYTES
    }

    #[test]
    fn the_pool_sizing_and_the_cache_term_agree() {
        // The chain sets CROSS_POOL_SLOTS with bin_pool_slots; the plan prices
        // the cache with min(n, 512 B x those slots). A 10 GB corpus under 45% of
        // 100 GiB (17.18 GB of pool budget) fits, so the cache term is n itself.
        let n = 10_000_017_258u64;
        let mem = 100u64 << 30;
        let slots = bin_pool_slots(n, mem);
        assert_eq!(slots, 33_554_432, "the 10 GB pool sizing");
        assert_eq!(bin_corpus_cache_bytes(n, mem), n);
        // A pile-size corpus cannot be cached: the term is the pool budget.
        let pile = 1_310_000_000_000u64;
        assert_eq!(bin_corpus_cache_bytes(pile, mem), BIN_POOL_BYTES_PER_SLOT * slots);
        let t = bin_derive_live_cap(pile, 96, mem, 2569u64 << 30, (1.546 * pile as f64) as u64);
        assert!(t.cache + t.stage + BIN_GROUP_SLOT_BYTES * t.cap <= mem * BIN_PHASE_HEADROOM_PCT / 100);
        assert_eq!(t.bound_by.starts_with("address"), true,
                   "at the pile the memory side binds (the scratch side has room)");
    }

    #[test]
    fn build_term_interpolates_and_clamps_on_the_measured_curve() {
        // The measured points themselves.
        assert!((bin_build_s_per_gb(8) - 107.93).abs() < 1e-9);
        assert!((bin_build_s_per_gb(32) - 35.86).abs() < 1e-9);
        // The 96-thread point is the 10 GB rung with the target-size default
        // (300.369 s build / 10 GB = 30.0369 s/GB), not the 1 GB fixture.
        assert!((bin_build_s_per_gb(96) - 30.04).abs() < 1e-9);
        // Clamped outside the measured range (more threads buy nothing: the
        // build is memory-bandwidth bound).
        assert!((bin_build_s_per_gb(1) - 107.93).abs() < 1e-9);
        assert!((bin_build_s_per_gb(256) - 30.04).abs() < 1e-9);
        // Monotone non-increasing in log2(threads) between the points.
        let mut last = f64::INFINITY;
        for t in [8u32, 12, 16, 24, 32, 48, 64, 96] {
            let v = bin_build_s_per_gb(t);
            assert!(v <= last + 1e-9, "build term must not rise with threads (t={t})");
            last = v;
        }
    }
}
