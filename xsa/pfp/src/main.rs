// =============================================================
// AMENDMENT R (THE REWRITE / RUST FIRST): the tape-reader producer,
// transliterated from TAPE_VISION.md + Amendments W, B, L into the
// xsa Rust workspace. The C++ pfp_sort.cpp is the REFERENCE ORACLE
// (frozen); byte-identity is the law - the product must not change,
// only the language. The G0 oracle stays C++ deliberately.
//
// Module map (R2): remap -> parse -> dictsort -> census -> levels
// -> LCP carry (W5) -> pairsbins (B+B4) -> emit -> finish wiring.
// Port order (R5): each module G0-gated before the next.
// =============================================================
mod col;
mod laws;
mod parse;
mod dictsort;
mod levels;

use col::die;
use std::io::Read as _;
use std::fs;
use std::path::Path;

fn main() {
    let t_all = std::time::Instant::now();
    // ---- the CLI (the C++ producer's shape)
    let mut corpus_path = String::new();
    let mut remap_path = String::new();
    let mut out_prefix = String::new();
    let mut bands_dir = String::new();
    let mut threads = 24u64;
    let mut memory_gb = 196u64;
    let mut budget_bytes = 0u64;
    let mut scratch_budget_bytes = 0u64;
    let mut args = std::env::args().skip(1);
    macro_rules! nxt {
        () => { args.next().unwrap_or_else(|| die("missing a value")) }
    }
    while let Some(a) = args.next() {
        match a.as_str() {
            "--corpus" => corpus_path = nxt!(),
            "--remap" => remap_path = nxt!(),
            "--out-prefix" => out_prefix = nxt!(),
            "--bands" => bands_dir = nxt!(),
            "--threads" => threads = nxt!().parse().unwrap(),
            "--memory-gb" => memory_gb = nxt!().parse().unwrap(),
            "--budget-bytes" => budget_bytes = nxt!().parse().unwrap(),
            "--scratch-budget-bytes" =>
                scratch_budget_bytes = nxt!().parse().unwrap(),
            _ => die(&format!("unknown argument {}", a)),
        }
    }
    if corpus_path.is_empty() || out_prefix.is_empty() {
        die("usage: pfp_sort_r --corpus F [--remap F] --out-prefix P \
             --bands D --threads N --memory-gb G --budget-bytes N \
             --scratch-budget-bytes N");
    }
    // the dirs (the C++ layout: <prefix>/../modeb + merged + bands)
    let merged_dir = Path::new(&out_prefix).parent()
        .map(|p| p.to_string_lossy().to_string())
        .unwrap_or_else(|| ".".to_string());
    let modeb_dir = format!("{}/modeb", merged_dir);
    if bands_dir.is_empty() { bands_dir = format!("{}/bands", merged_dir); }
    for d in [&merged_dir, &modeb_dir, &bands_dir] {
        let _ = fs::create_dir_all(d);
    }
    let stop_flag = format!("{}/stop.requested", modeb_dir);
    let _ = fs::remove_file(&stop_flag);

    let corpus = fs::File::open(&corpus_path)
        .unwrap_or_else(|_| die("open corpus"));
    let file_n = corpus.metadata()
        .unwrap_or_else(|_| die("stat corpus")).len();
    eprintln!("pfp_sort_r: THE TAPE READER (AMENDMENT R) - Rust first; \
               the C++ is the reference oracle");
    eprintln!("SS_TAPE_START corpus_bytes={} out_prefix={} threads={} \
               memory_gb={} rule=amendment_r_transliteration",
              file_n, out_prefix, threads, memory_gb);

    // ---- THE REMAP (256 bytes; the product rows carry remapped
    //      symbols; the parse operates on the raw bytes - the remap
    //      is order-preserving so the orders agree)
    let remap: Vec<u8> = if remap_path.is_empty() {
        Vec::new()
    } else {
        let mut f = fs::File::open(&remap_path)
            .unwrap_or_else(|_| die("open remap"));
        let mut v = vec![0u8; 256];
        f.read_exact(&mut v).unwrap_or_else(|_| die("remap short read"));
        v
    };
    eprintln!("SS_REMAP symbols={} sha_note=order_preserving_used_set",
              if remap.is_empty() { 0 } else {
                  remap.iter().filter(|&&b| b != 0).count().max(
                      remap.iter().enumerate()
                          .filter(|(i, &b)| b != *i as u8).count()) });

    // the workspace dial (XSA_BAND_WORKSPACE_BYTES; the default = the
    // memory envelope)
    let workspace = laws::env_u64("XSA_BAND_WORKSPACE_BYTES",
                                   memory_gb * 1_000_000_000);
    // THE SCRATCH LAW: the default is 2xN (the operator's clause)
    let scratch_budget = if scratch_budget_bytes != 0 {
        scratch_budget_bytes
    } else {
        2 * file_n
    };
    let laws = laws::Laws::new(stop_flag, workspace, scratch_budget,
                               file_n, modeb_dir.clone(),
                               merged_dir.clone(), bands_dir.clone());
    eprintln!("SS_SCRATCH scratch_budget_bytes={} rule=operator_clause_2xN",
              scratch_budget);
    rayon::ThreadPoolBuilder::new()
        .num_threads(threads.max(1) as usize)
        .build_global()
        .unwrap_or_else(|_| die("rayon pool"));

    // ---- THE CORPUS (mmap RO - the only text; never written)
    let mmap = unsafe {
        memmap2::Mmap::map(&corpus).unwrap_or_else(|_| die("mmap corpus"))
    };
    let t: &[u8] = &mmap[..];

    // ---- THE PARSE (STROKE P)
    laws.stop_check("parse-enter");
    let p = parse::parse(&corpus, file_n, &modeb_dir, &laws);
    laws.scratch_gate("parse");

    // ---- THE DICT SORT + THE PA
    let d = dictsort::dictsort(t, p.m0, &p.occ_path, &p.hist2,
                               &modeb_dir, workspace, threads, &laws);
    laws.scratch_gate("dictsort");

    eprintln!("SS_CENSUS_FRONT m0={} distinct={} dict_bytes={} \
               prefix_relations={} condensation_x={:.2}",
              p.m0, d.dict_n, d.dict_bytes_total, d.prefix_relations,
              p.m0 as f64 / d.dict_n.max(1) as f64);

    // ---- THE LEVELS (from the spec): the census arithmetic by
    //      number; None means one level (the deepest = this one)
    let lv2 = levels::build_level2(d.pa.u32s(), p.m0, &modeb_dir,
                                   threads, &laws);
    if lv2.is_some() {
        eprintln!("SS_R_LEVELS_TAKEN plan=2 rule=levels_are_data");
    } else {
        eprintln!("SS_R_LEVELS_TAKEN plan=1 rule=the_census_arithmetic");
    }
    laws.scratch_gate("levels");

    // ---- THE PORT ORDER CONTINUES (R5): levels -> W5 -> pairsbins
    //      -> emit -> finish wiring. Each lands with its own gate.
    eprintln!("SS_R_STOPPED_AT modules_done=remap,parse,dictsort \
               next=levels,w5,pairsbins,emit,finish rule=amendment_r_port_order");

    eprintln!("SS_DONE_R front_only=true total_s={:.2}",
              laws::now_s(t_all));
    laws.scratch_peak();
}
