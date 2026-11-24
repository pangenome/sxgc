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
mod w5;
mod unit;
mod scan_order;
mod overlap;

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

    // Separate opt-in for the checked 32-bit rung1 *sink*. This does not
    // promote either sandbox constructor (or the external window planner).
    let resident_sink = laws::env_u64("XSA_TEST_RESIDENT_TRIPLES",0)==1;
    let external = laws::env_u64("XSA_TEST_EXTERNAL_TRIPLES",0)==1;
    let fixture = laws::env_u64("XSA_TEST_OVERLAP_STREAM",0)==1;
    let triples = resident_sink || external;
    if triples || fixture {
        if resident_sink {
            let three=laws::env_u64("XSA_GATE_PACKED_3G",0)==1;
            let fit=if three {file_n==3_000_000_000} else {matches!(file_n, 100_000_503 | 1_000_000_665)};
            if external || fixture || !fit || std::env::var_os("TRIE_TAP").is_some() {
                die("resident triples: bounded sandbox/rung1 or explicit exact 3 GB product sink only");
            }
        } else if file_n > 101_000_000 || std::env::var_os("TRIE_TAP").is_some(){die("overlap fixture path: <=101 MB, product sink only");}
        let volatile=std::env::var_os("XSA_GATE_VOLATILE_PIECES");
        assert!(volatile.is_none() || resident_sink,"volatile handoff is a fresh resident-product gate only");
        let mut cutter=unit::PieceCutter::with_piece_target(&out_prefix,file_n+1,laws::env_u64("CROSS_V2_PIECE_WINDOW",1100),volatile.as_deref().map(Path::new));
        let mut lcp=Vec::with_capacity(262144);let mut live=std::collections::VecDeque::new();
        let (mut base,mut u,mut ends)=(0,0,0);
        let mut publish=|lcp:&mut Vec<u64>|{if !lcp.is_empty(){unit::emit_lcp_slice(&bands_dir,u,lcp.len(),lcp,base,&mut live,laws::env_u64("SS_LCP_SLICE_WINDOW",128),&laws);base+=lcp.len() as u64;u+=1;lcp.clear();laws.scratch_gate("overlap-slice");}};
        let mut feed=|p:u64,h:u64,raw:u8|{let sym=if p==0{0}else if remap.is_empty(){raw}else{remap[raw as usize]};
            ends+=u64::from(sym==0);cutter.feed_row(p,sym);lcp.push(h);if lcp.len()==262144{publish(&mut lcp);}
        };
        if triples {
            let mut stream=std::io::BufReader::with_capacity(1<<20,std::io::stdin().lock());
            let mut row=[0u8;12];
            for r in 0..=file_n {stream.read_exact(&mut row).expect("truncated external row stream");
                let col=|i:usize|u32::from_le_bytes(row[i..i+4].try_into().unwrap()) as u64;let(p,h,b)=(col(0),col(4),col(8));
                assert!(p<=file_n && (r==0)==(p==file_n) && h<=file_n-p && (if p==0{b==256}else{b<=255}),"invalid external row");feed(p,h,b as u8);
            }assert_eq!(stream.read(&mut row[..1]).expect("external EOF"),0,"extra external rows");
        }else{
            let hints=laws::env_u64("XSA_OVERLAP_PHASE_ADVICE",0)==1;
            overlap::construct(t,|stage|{laws.stop_check(stage);if hints {let adv=match stage {"@distribute"=>Some(memmap2::Advice::Normal),"@refill"|"@exact-sort"|"@emit"=>Some(memmap2::Advice::Random),_=>None};if let Some(a)=adv{mmap.advise(a).expect("phase madvise");}}},feed);
        }
        publish(&mut lcp);assert_eq!(base,file_n+1);assert_eq!(ends,1);cutter.finish();
        fs::write(format!("{}/producer.done",bands_dir),b"").expect("producer.done");laws.scratch_gate("overlap-done");laws.scratch_peak();
        eprintln!("SS_DONE_R overlap_stream rows={} total_s={:.2}",base,laws::now_s(t_all));return;
    }
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
    let _lv2 = levels::build_level2(d.pa.u32s(), p.m0, &modeb_dir,
                                   threads, &laws);
    if _lv2.is_some() {
        eprintln!("SS_R_LEVELS_TAKEN plan=2 rule=levels_are_data");
    } else {
        eprintln!("SS_R_LEVELS_TAKEN plan=1 rule=the_census_arithmetic");
    }
    laws.scratch_gate("levels");

    // ---- THE W5 LCP CARRY + THE OCCURRENCE LISTS (level 0: over
    //      the level-1 parse; the SA from the single-level kernel
    //      or the induced column when the levels module lands)
    {
        // The two paths produce the same parse_sa.bin consumed below.
        let t0s = std::time::Instant::now();
        let sa_p = format!("{}/parse_sa.bin", modeb_dir);
        if let Some(l) = &_lv2 {
            let dir = format!("{}/L2", modeb_dir);
            let sa = l.parse_sa2.u32s();
            let mut rank = col::ColMap::map_rw(&format!("{}/rank.bin", dir), l.m2, 4);
            for (r, &i) in sa.iter().enumerate() { rank.u32s_mut()[i as usize] = r as u32; }
            let pa = l.pa2.u32s();
            let ps = l.pstart2.u64s();
            let refs = l.dict_ref2.u64s();
            let lens = l.dict_len2.u32s();
            let text = l.be.u8s();
            let w = w5::build(text, pa, ps, sa, refs, lens, l.m2, &dir);
            let (start, list) = w5::occ_lists(pa, rank.u32s(), l.m2, l.dict_n2, &dir);
            let mut upper = unit::LevelCtx { t: text, stride: 4, rows_n: p.m0, file_n: l.be_bytes,
                pa, m: l.m2, sa, rank: rank.u32s(), dref: refs, dlen: lens,
                dict_n: l.dict_n2, dict_bytes_total: lens.iter().map(|&n| u64::from(n)).sum(),
                pstart: ps, ostart: start.u32s(), olist: list.u32s(), lcp: w.lcp.u64s(),
                lcp_tree: w.lcp_tree.u64s(), lcp_seg_n: w.lcp_seg_n, modeb_dir: dir, laws: &laws };
            unit::run_level(&mut upper, &mut unit::Sink::SaColumn { out: col::ColMap::map_rw(&sa_p, p.m0, 4) });
        } else {
            let mut sa = col::ColMap::map_rw(&sa_p, p.m0, 4);
            let mut ti: Vec<i32> = Vec::with_capacity(p.m0 as usize);
            for j in 0..p.m0 as usize {
                ti.push(d.pa.u32s()[j] as i32);
            }
            unsafe {
                let sa_slice = std::slice::from_raw_parts_mut(
                    sa.ptr as *mut i32, p.m0 as usize);
                let rc = levels::sa_kernel_int_raw(
                    ti.as_ptr() as *mut i32, sa_slice,
                    d.dict_n.min(2147483647) as i32);
                if rc != 0 { die(&format!("LIBSAIS_INT rc={}", rc)); }
            }
        }
        eprintln!("SS_PARSE_SORT parse_rows={} kernel={} sort_s={:.2}", p.m0,
                  if _lv2.is_some() { "induced" } else { "libsais_int_ffi" }, laws::now_s(t0s));
        let sa = col::ColMap::map_ro(&sa_p, p.m0, 4);
        // the rank scatter
        let rank_p = format!("{}/parse_rank.bin", modeb_dir);
        {
            let mut rk = col::ColMap::map_rw(&rank_p, p.m0, 4);
            for i in 0..p.m0 as usize {
                rk.u32s_mut()[sa.u32s()[i] as usize] = i as u32;
            }
        }
        let rank = col::ColMap::map_ro(&rank_p, p.m0, 4);
        // the phrase starts column (the tiling prefix sums)
        let ps_p = format!("{}/pstart.bin", modeb_dir);
        {
            let mut w = col::ColWriter::open(&ps_p);
            let mut acc: u64 = 0;
            for i in 0..p.m0 as usize {
                w.put_u64(acc);
                acc += d.len.u32s()[d.pa.u32s()[i] as usize] as u64;
            }
            w.close();
        }
        let pstart = col::ColMap::map_ro(&ps_p, p.m0, 8);
        let w5 = w5::build(t, d.pa.u32s(), pstart.u64s(), sa.u32s(),
                           d.refpos.u64s(), d.len.u32s(), p.m0,
                           &modeb_dir);
        let _ = &w5;
        let (ostart, olist) = w5::occ_lists(
            d.pa.u32s(), rank.u32s(), p.m0, d.dict_n, &modeb_dir);
        laws.scratch_gate("w5");

        // ---- THE UNIT (pairsbins + emit, fused): level 0
        let sa2 = col::ColMap::map_ro(&sa_p, p.m0, 4);
        let lcp = col::ColMap::map_ro(
            &format!("{}/parse_lcp.bin", modeb_dir), p.m0, 8);
        let tree = col::ColMap::map_ro(
            &format!("{}/lcp_tree.bin", modeb_dir),
            2 * (if p.m0 > 1 { p.m0 - 1 } else { 1 }), 8);
        let mut sink = unit::Sink::Product {
            out_prefix: out_prefix.clone(),
            bands_dir: bands_dir.clone(),
            remap: if remap.is_empty() { None } else { Some(remap.clone()) },
            trie_tap: std::env::var("TRIE_TAP").is_ok(),
        };
        let mut ctx = unit::LevelCtx {
            t, stride: 1, rows_n: file_n, file_n,
            pa: d.pa.u32s(), m: p.m0,
            sa: sa2.u32s(), rank: rank.u32s(),
            dref: d.refpos.u64s(), dlen: d.len.u32s(),
            dict_n: d.dict_n, dict_bytes_total: d.dict_bytes_total,
            pstart: pstart.u64s(),
            ostart: ostart.u32s(), olist: olist.u32s(),
            lcp: lcp.u64s(),
            lcp_tree: tree.u64s(),
            lcp_seg_n: if p.m0 > 1 { p.m0 - 1 } else { 1 },
            modeb_dir: modeb_dir.clone(),
            laws: &laws,
        };
        let stats = unit::run_level(&mut ctx, &mut sink);
        eprintln!("SS_DONE_R rows_emitted={} end_rows={} \
                   rule=amendment_r_the_unit_ran",
                  stats.rows_emitted, stats.end_rows);
    }

    eprintln!("SS_DONE_R producer_complete=true total_s={:.2}",
              laws::now_s(t_all));
    laws.scratch_peak();
}
