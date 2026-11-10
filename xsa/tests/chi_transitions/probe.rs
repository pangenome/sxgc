// Appended to a TEMPORARY COPY of qlite.rs by run.py; no product changes.
pub fn transition_study() {
    use std::hint::black_box;
    let a: Vec<String> = std::env::args().collect();
    assert!(a.len() >= 3, "probe CONTAINER CORPUS [N=256]");
    let count: usize = a.get(3).map(|s| s.parse().unwrap()).unwrap_or(256);
    let reader = Reader::open(&a[1]).unwrap();
    let corpus_file = File::open(&a[2]).unwrap();
    let corpus = unsafe { Mmap::map(&corpus_file).unwrap() };
    assert_eq!(corpus.len() as u64 + 1, reader.n);
    println!("meta,n={},r={},bytes={},contexts={count}",reader.n,reader.r,reader.map.len());
    for m in &reader.members { println!("member,{},{},{},{},{}",m.id,m.codec,m.offset,m.bytes,m.count); }
    let idx = Index::open(reader, 1).unwrap();
    let cand = Candidates::of(&idx);
    let mut cx = idx.ctx();
    if a.get(4).is_some_and(|v| v == "ladder") { transition_ladder(&mut cx, &cand, &corpus, count); return; }
    let mut seed = 0x932ab572ac384ecdu64;
    println!("sample,id,len,width,degree,search_us,rank_hist_proxy_us,ladder1024_proxy_us,rank_ops,anchor_us,lf_steps,transition_hot_us,exact_rank_us");
    let mut pool: Vec<Vec<(u8,u64)>> = Vec::new();
    for id in 0..count {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        let len = [8usize,32,128,1024][id % 4];
        let off = seed as usize % (corpus.len()-len-1);
        let pat = &corpus[off..off+len];
        let work: Vec<u8> = pat.iter().rev().map(|b| idx.reader.sigma[*b as usize]).collect();
        let ranks0 = cx.ranks;
        let t = Instant::now(); let (l,r) = cx.search(&work); let search_us=t.elapsed().as_secs_f64()*1e6;
        assert!(r>l);
        // Exact next-byte counts on the forward container: independent searches.
        let t = Instant::now(); let mut hist=[0u64;256];
        extension_counts(&mut cx,&cand,&work,&mut hist);
        let exact_us=t.elapsed().as_secs_f64()*1e6;
        let degree=hist.iter().filter(|&&v|v>0).count();
        assert_eq!(hist.iter().sum::<u64>(),r-l); // sampled contexts do not hit EOF
        // Companion-index PRIMITIVE SURROGATE only: this is PRECEDING on this file.
        let t=Instant::now(); let mut preceding=[0u64;256];
        for (i,&s) in cand.syms.iter().enumerate() { preceding[cand.bytes[i] as usize]=cx.rank(s,r)-cx.rank(s,l); }
        black_box(preceding); let hist_us=t.elapsed().as_secs_f64()*1e6;
        // 1024 independent nonempty interval extension primitives; no claiming
        // these are real reversed-corpus rolling states.
        let t=Instant::now(); let s=idx.reader.sigma[corpus[off+len] as usize];
        for _ in 0..1024 { black_box((cx.rank(s,l),cx.rank(s,r))); }
        let ladder_us=t.elapsed().as_secs_f64()*1e6;
        let ranks=cx.ranks-ranks0;
        let mut anchor_us=-1.0; let mut steps=0u64;
        // Exhaustive occurrence gather on narrow intervals only, no truncation.
        if r-l<=64 {
            let t=Instant::now(); let mut gathered=[0u64;256];
            for row in l..r { let (pos,st)=cx.locate_steps(row); steps+=st; let next=pos as usize+len;
                assert!(next<corpus.len()); gathered[corpus[next] as usize]+=1; }
            anchor_us=t.elapsed().as_secs_f64()*1e6;
            assert_eq!(hist,gathered);
        }
        pool.push(hist.iter().enumerate().filter(|(_,v)|**v>0).map(|(b,&v)|(b as u8,v)).collect());
        let t=Instant::now();
        for j in 0..1024 { let node=black_box(j%pool.len()); black_box(pool[node].iter().find(|(b,_)|*b==pat[j%len])); }
        black_box(&pool[id]); let hot_us=t.elapsed().as_secs_f64()*1e6;
        println!("sample,{id},{len},{},{degree},{search_us:.3},{hist_us:.3},{ladder_us:.3},{ranks},{anchor_us:.3},{steps},{hot_us:.3},{exact_us:.3}",r-l);
        use std::io::Write; std::io::stdout().flush().unwrap();
    }
    println!("parity,passed,{count}");
}

// A real prepend ladder on bytes fed backwards from the ORIGINAL corpus.
// This corresponds to a companion for reverse(T), not a built companion for T.
fn transition_ladder(cx: &mut Ctx<'_>, cand: &Candidates, corpus: &[u8], count: usize) {
    use std::hint::black_box;
    let mut states = vec![(0u64, 0u64);1025];
    states[0]=(0,cx.idx.reader.n);
    let base=50_000_001_000usize;
    assert!(base<corpus.len() && count+1024<base);
    println!("ladder,id,width,extend_us,hist_us,rank_ops");
    for step in 0..1024+count {
        let off=base-step;
        let sym=cx.idx.reader.sigma[corpus[off] as usize];
        let ranks0=cx.ranks;
        let t=Instant::now();
        for j in (1..=1024.min(step+1)).rev() {
            let (l,r)=states[j-1];
            states[j]=if l==r { (0,0) } else {
                let b=cx.idx.c[sym as usize]; (b+cx.rank(sym,l),b+cx.rank(sym,r))
            };
        }
        let extend_us=t.elapsed().as_secs_f64()*1e6;
        let (l,r)=states[1024];
        if step>=1024 {
            let t=Instant::now();let mut hist=[0u64;256];
            for (i,&s) in cand.syms.iter().enumerate() { hist[cand.bytes[i] as usize]=cx.rank(s,r)-cx.rank(s,l); }
            let hist_us=t.elapsed().as_secs_f64()*1e6;
            assert_eq!(hist.iter().sum::<u64>(),r-l);
            black_box(hist);
            let ranks=cx.ranks-ranks0;
            if step<1027 {
                let work:Vec<u8>=corpus[off..off+1024].iter().rev().map(|b|cx.idx.reader.sigma[*b as usize]).collect();
                assert_eq!(cx.search(&work),(l,r));
            }
            println!("ladder,{},{},{extend_us:.3},{hist_us:.3},{ranks}",step-1024,r-l);
            use std::io::Write;std::io::stdout().flush().unwrap();
        }
    }
    println!("ladder_parity,passed,3 fresh-search intervals and all histogram widths");
}
