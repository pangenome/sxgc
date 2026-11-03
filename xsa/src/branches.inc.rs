// Included in qlite to share the immutable index and per-worker navigation cache.
#[derive(Debug)]
struct BranchResult {
    id: u64,
    l: u64,
    r: u64,
    eof: u64,
    other: u64,
    branches: Vec<(u8, u64)>,
    search_us: f64,
    gather_us: f64,
    lf_steps: u64,
    rank_steps: u64,
}

/// The candidate byte set: the container's PRESENT bytes, in DECREASING corpus
/// frequency (ties by ascending byte value).
///
/// "Present" is the m1 C table's own statement: total[sigma[b]] > 0. Rank 0 is
/// the structural END's rank (the writer maps byte 0xff there and the C table
/// folds the virtual END into it), exactly the rank every existing query path
/// already treats as un-queryable, so it is never a candidate. The empty
/// context is answered from the same totals.
///
/// Frequency order is not cosmetic: the per-byte work is a one-sided extension
/// chain with an early exit, and the top-`k` closure (`other`) is only known
/// after every candidate has been counted, but ordering by frequency puts the
/// answers a client actually ranks first in the cheapest part of the loop and
/// keeps the reported branches stable for ties.
struct Candidates {
    bytes: Vec<u8>,
    /// sigma[b] per candidate: the symbol the container's C table and rank
    /// column are indexed by (the m7 permutation is the container's, not ours).
    syms: Vec<u8>,
}

impl Candidates {
    fn of(idx: &Index) -> Self {
        let sigma = &idx.reader.sigma;
        let mut present: Vec<(u64, u8)> = (0..256u32)
            .filter(|&b| {
                let s = sigma[b as usize];
                s != 0 && idx.total[s as usize] > 0
            })
            .map(|b| (idx.total[sigma[b as usize] as usize], b as u8))
            .collect();
        present.sort_unstable_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
        Candidates {
            bytes: present.iter().map(|p| p.1).collect(),
            syms: present.iter().map(|p| sigma[p.1 as usize]).collect(),
        }
    }
}

/// One-sided right-extension counts by PURE RANK ARITHMETIC: no locate, no LF
/// walk, no corpus byte.
///
/// `work` is the context's remapped symbols REVERSED (work[0] is the byte just
/// before the right end). Backward search prepends, and the LAST prepended
/// symbol is the LEFTMOST one, so the sequence `c, p[m-1], p[m-2], ... p[0]`
/// yields the interval of `context || c`: for candidate `c` start at `c`'s own
/// interval `[C[c], C[c]+total[c])` - `rank(c,0)` and `rank(c,n)` are answered
/// by the C table, so that step is free - and prepend the context right to
/// left, ABANDONING the moment the interval is empty (for a narrow context most
/// bytes die in the first couple of steps). The width of the surviving interval
/// IS the count: `count(context.c) = |[l,r) of context.c|`, so the answer needs
/// no positions at all.
///
/// Not `rank(c,r)-rank(c,l)` over the context interval: that counts the byte
/// BEFORE the context (the C table below ranks preceding bytes), which is a
/// different distribution.
fn extension_counts(
    cx: &mut Ctx<'_>,
    cand: &Candidates,
    work: &[u8],
    counts: &mut [u64; 256],
) -> u64 {
    let ranks_before = cx.ranks;
    for (i, &sym) in cand.syms.iter().enumerate() {
        let base = cx.idx.c[sym as usize];
        let mut a = base;
        let mut z = base + cx.idx.total[sym as usize];
        for &p in work {
            if a == z {
                break; // empty: this byte is absent after this context
            }
            let pb = cx.idx.c[p as usize];
            a = pb + cx.rank(p, a);
            z = pb + cx.rank(p, z);
        }
        counts[cand.bytes[i] as usize] = z - a;
    }
    cx.ranks - ranks_before
}

fn branch_result(
    cx: &mut Ctx<'_>,
    cand: &Candidates,
    corpus: Option<&[u8]>,
    id: u64,
    pat: &[u8],
    k: usize,
) -> BranchResult {
    let start = Instant::now();
    let sigma = cx.idx.reader.sigma;
    let absent = pat
        .iter()
        .any(|&b| sigma[b as usize] == 0 || cx.idx.total[sigma[b as usize] as usize] == 0);
    // Encoded ONCE per context, outside the per-byte loop: remap + reverse.
    let work: Vec<u8> = pat.iter().rev().map(|&b| sigma[b as usize]).collect();
    let (l, r) = if absent { (0, 0) } else { cx.search(&work) };
    let mut search_us = start.elapsed().as_secs_f64() * 1e6;
    let start = Instant::now();
    let mut extension_us = 0.;
    let mut counts = [0u64; 256];
    let mut eof = 0;
    let mut lf_steps = 0;
    let mut rank_steps = 0;
    if pat.is_empty() {
        // Empty context occurs at all corpus boundaries, including EOF.
        for b in 0..256 {
            if sigma[b] != 0 {
                counts[b] = cx.idx.total[sigma[b] as usize];
            }
        }
        eof = 1;
    } else if r - l == 0 {
        // Absent context: width 0, no branch to count and no extension to run.
    } else if corpus.is_some() && r - l <= 64 {
        // A/B ONLY (XSA_BRANCH_GATHER=1): the retired anchor+LF gather path,
        // kept so the rank path can be checked against positions it no longer
        // walks. Never reached unless a corpus was handed to the process.
        let corpus = corpus.unwrap();
        for row in l..r {
            let (pos, st) = cx.locate_steps(row);
            lf_steps += st;
            let next = pos
                .checked_add(pat.len() as u64)
                .expect("branch offset overflow");
            assert!(
                next <= corpus.len() as u64,
                "branch occurrence crosses corpus EOF"
            );
            if next == corpus.len() as u64 {
                eof += 1;
            } else {
                counts[corpus[next as usize] as usize] += 1;
            }
        }
    } else {
        let extension_start = Instant::now();
        rank_steps = extension_counts(cx, cand, &work, &mut counts);
        extension_us = extension_start.elapsed().as_secs_f64() * 1e6;
        search_us += extension_us;
        // Every occurrence of the context is followed by a corpus byte or by the
        // structural END; there is at most one of the latter.
        let sum: u64 = counts.iter().sum();
        eof = (r - l).checked_sub(sum).unwrap_or_else(|| {
            die("branches: right extensions exceed the context interval")
        });
        assert!(eof <= 1, "multiple corpus EOF occurrences");
    }
    let mut branches: Vec<_> = counts
        .iter()
        .enumerate()
        .filter(|(_, c)| **c > 0)
        .map(|(b, c)| (b as u8, *c))
        .collect();
    branches.sort_unstable_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    assert_eq!(counts.iter().sum::<u64>() + eof, r - l);
    branches.truncate(k);
    let other = r - l - eof - branches.iter().map(|b| b.1).sum::<u64>();
    BranchResult {
        id,
        l,
        r,
        eof,
        other,
        branches,
        search_us,
        gather_us: start.elapsed().as_secs_f64() * 1e6 - extension_us,
        lf_steps,
        rank_steps,
    }
}
fn branch_frame(r: &BranchResult) -> Vec<u8> {
    let mut b = Vec::new();
    for v in [r.id, r.l, r.r, r.r - r.l, r.eof, r.other] {
        b.extend(v.to_le_bytes());
    }
    b.extend((r.branches.len() as u16).to_le_bytes());
    for &(ch, n) in &r.branches {
        b.push(ch);
        b.extend(n.to_le_bytes());
    }
    b
}
fn read_branch_request(r: &mut impl std::io::Read) -> std::io::Result<Option<Vec<u8>>> {
    let mut len = [0; 4];
    if r.read(&mut len[..1])? == 0 {
        return Ok(None);
    }
    r.read_exact(&mut len[1..])?;
    let n = u32::from_le_bytes(len) as usize;
    if n > 16 * 1024 * 1024 {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "context exceeds 16 MiB",
        ));
    }
    let mut b = vec![0; n];
    r.read_exact(&mut b)?;
    Ok(Some(b))
}
pub fn branches_command(raw: &[std::ffi::OsString]) {
    let args: Vec<_> = raw
        .iter()
        .map(|s| s.to_string_lossy().into_owned())
        .collect();
    let container = args
        .first()
        .unwrap_or_else(|| die("branches: need container"));
    let mut file = None;
    let mut docs = None;
    let mut corpus_path = None;
    let mut serve = false;
    let mut json = false;
    let mut unordered = false;
    let mut jobs = 1usize;
    let mut k = 256usize;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--serve" => serve = true,
            "--jsonl" => json = true,
            "--unordered" => unordered = true,
            "--contexts" | "--docs" | "--corpus" | "-j" | "-k" => {
                let flag = &args[i];
                i += 1;
                let v = args
                    .get(i)
                    .unwrap_or_else(|| die("branches: missing option value"));
                match flag.as_str() {
                    "--contexts" => file = Some(v.clone()),
                    "--docs" => docs = Some(v.clone()),
                    "--corpus" => corpus_path = Some(v.clone()),
                    "-j" => jobs = v.parse().unwrap_or_else(|_| die("invalid jobs")),
                    _ => k = v.parse().unwrap_or_else(|_| die("invalid k")),
                }
            }
            _ => die("branches: unknown option"),
        }
        i += 1;
    }
    if jobs == 0 || k > 256 || serve == file.is_some() {
        die("branches: require --serve OR --contexts FILE; -j > 0; -k <= 256")
    }
    let reader = Reader::open(container).unwrap_or_else(|e| die(&e));
    // The query reads NO corpus bytes: branch counts are interval widths of
    // `context || c`, i.e. rank arithmetic on the container's own structures.
    // `--corpus`/`--docs` are still accepted (and only then, with
    // XSA_BRANCH_GATHER=1, used) because the retired anchor+LF gather path is
    // kept as the A/B baseline this path is checked against.
    let gather_ab = std::env::var("XSA_BRANCH_GATHER").ok().as_deref() == Some("1");
    let corpus: Option<Mmap> = if gather_ab {
        let c = if let Some(path) = corpus_path {
            let f = File::open(path).unwrap_or_else(|e| die(&e.to_string()));
            unsafe { Mmap::map(&f) }.unwrap_or_else(|e| die(&e.to_string()))
        } else {
            let documents=discover_documents(container,docs.as_deref(),reader.n).unwrap_or_else(||die("branches: XSA_BRANCH_GATHER=1 needs corpus.bin and matching sidecars (--docs DIR), or --corpus FILE"));
            documents.context_corpus().unwrap_or_else(|e| die(&e))
        };
        if c.len() as u64 + 1 != reader.n {
            die("branches: corpus length does not match index")
        }
        Some(c)
    } else {
        if corpus_path.is_some() || docs.is_some() {
            eprintln!(
                "BRANCH_NOTE corpus_argument_ignored=1 (backward-search ranks read no corpus; \
                 pass XSA_BRANCH_GATHER=1 to run the retired anchor+LF baseline)"
            );
        }
        None
    };
    let corpus_ref: Option<&[u8]> = corpus.as_deref();
    let index = Index::open(reader, jobs).unwrap_or_else(|e| die(&e));
    let cand = Candidates::of(&index);
    eprintln!(
        "BRANCH_READY navigation=backward-search-ranks candidates={} gather_ab={} workers={jobs}",
        cand.bytes.len(),
        u8::from(gather_ab)
    );
    let started = Instant::now();
    let mut count = 0u64;
    let mut search = 0.;
    let mut gather = 0.;
    let mut lf_steps = 0u64;
    let mut rank_steps = 0u64;
    let mut ipc = 0.;
    let mut input_ipc = 0.;
    let mut emit = |r: BranchResult, w: &mut dyn Write| {
        let t = Instant::now();
        if serve {
            let b = branch_frame(&r);
            w.write_all(&(b.len() as u32).to_le_bytes()).unwrap();
            w.write_all(&b).unwrap();
            w.flush().unwrap();
        } else if json {
            writeln!(w,"{}",serde_json::json!({"ctx_id":r.id,"l":r.l,"r":r.r,"width":r.r-r.l,"eof":r.eof,"other":r.other,"branches":r.branches.iter().map(|&(byte,count)|serde_json::json!({"byte":byte,"count":count})).collect::<Vec<_>>()})).unwrap();
        } else {
            writeln!(
                w,
                "{}\t{}\t{}\t{}\t{}\t{}\t{:?}",
                r.id,
                r.l,
                r.r,
                r.r - r.l,
                r.eof,
                r.other,
                r.branches
            )
            .unwrap();
        }
        ipc += t.elapsed().as_secs_f64() * 1e6;
        count += 1;
        search += r.search_us;
        gather += r.gather_us;
        lf_steps += r.lf_steps;
        rank_steps += r.rank_steps;
    };
    let stdout = std::io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    if serve {
        // ONE QUEUE PER WORKER, dispatched round-robin by the reader thread: no
        // worker takes a lock shared with another worker to pick up a request,
        // so the only shared state in the hot path is the read-only index (the
        // rank columns, the C table and the candidate list). Scaling is
        // therefore bounded by memory latency, not by dispatch.
        let mut req_tx = Vec::with_capacity(jobs);
        let mut req_rx = Vec::with_capacity(jobs);
        for _ in 0..jobs {
            let (t, r) = std::sync::mpsc::sync_channel::<(u64, Vec<u8>)>(16);
            req_tx.push(t);
            req_rx.push(r);
        }
        // Bound all in-flight requests, including the ordered reorder buffer.
        let (permit_tx, permit_rx) = std::sync::mpsc::sync_channel(jobs * 4);
        for _ in 0..jobs * 4 {
            permit_tx.send(()).unwrap();
        }
        std::thread::scope(|scope| {
            let (results, rr) = std::sync::mpsc::sync_channel(jobs * 2);
            let input = scope.spawn(move || -> std::io::Result<f64> {
                let stdin = std::io::stdin();
                let mut input = stdin.lock();
                let mut id = 0;
                let mut slot = 0usize;
                let mut input_us = 0.;
                loop {
                    let t = Instant::now();
                    let request = read_branch_request(&mut input)?;
                    input_us += t.elapsed().as_secs_f64() * 1e6;
                    let Some(p) = request else { break };
                    permit_rx.recv().unwrap();
                    req_tx[slot].send((id, p)).unwrap();
                    slot += 1;
                    if slot == jobs {
                        slot = 0;
                    }
                    id += 1;
                }
                Ok(input_us)
            });
            for rx in req_rx {
                let tx = results.clone();
                let idx = &index;
                let cand = &cand;
                scope.spawn(move || {
                    let mut cx = idx.ctx();
                    while let Ok((id, p)) = rx.recv() {
                        tx.send(branch_result(&mut cx, cand, corpus_ref, id, &p, k))
                            .unwrap();
                    }
                });
            }
            drop(results);
            let mut next = 0;
            let mut pending = std::collections::BTreeMap::new();
            for r in rr {
                if unordered {
                    emit(r, &mut out);
                    let _ = permit_tx.send(());
                } else {
                    pending.insert(r.id, r);
                    while let Some(r) = pending.remove(&next) {
                        emit(r, &mut out);
                        next += 1;
                        let _ = permit_tx.send(());
                    }
                }
            }
            input_ipc = input
                .join()
                .unwrap()
                .unwrap_or_else(|e| die(&format!("branches: request frame: {e}")));
        });
    } else {
        let data = std::fs::read(file.unwrap()).unwrap_or_else(|e| die(&e.to_string()));
        let mut cx = index.ctx();
        // Newline-delimited byte contexts; use --serve for embedded newlines.
        for (id, p) in data
            .split(|&b| b == b'\n')
            .take(if data.is_empty() {
                0
            } else {
                data.split(|&b| b == b'\n').count() - usize::from(data.ends_with(b"\n"))
            })
            .enumerate()
        {
            emit(branch_result(&mut cx, &cand, corpus_ref, id as u64, p, k), &mut out);
        }
    }
    out.flush().unwrap();
    eprintln!("BRANCH_STATS contexts={count} contexts_per_s={:.3} search_us_per_context={:.3} gather_us_per_context={:.3} output_ipc_us_per_context={:.3} input_ipc_us_per_context={:.3} lf_steps={lf_steps} rank_steps={rank_steps} rank_steps_per_context={:.1}",count as f64/started.elapsed().as_secs_f64(),search/count.max(1) as f64,gather/count.max(1) as f64,ipc/count.max(1) as f64,input_ipc/count.max(1) as f64,rank_steps as f64/count.max(1) as f64);
}

#[cfg(test)]
mod branch_protocol_tests {
    use super::*;
    #[test]
    fn arbitrary_bytes_and_truncation() {
        for bytes in [b"".as_slice(), b"-\x1e\x00\xff\n".as_slice()] {
            let mut input = (bytes.len() as u32).to_le_bytes().to_vec();
            input.extend(bytes);
            assert_eq!(
                read_branch_request(&mut input.as_slice()).unwrap(),
                Some(bytes.to_vec())
            );
            if !bytes.is_empty() {
                input.pop();
                assert!(read_branch_request(&mut input.as_slice()).is_err());
            }
        }
        assert_eq!(read_branch_request(&mut [].as_slice()).unwrap(), None);
        assert!(read_branch_request(&mut [1, 0].as_slice()).is_err());
        assert!(read_branch_request(&mut u32::MAX.to_le_bytes().as_slice()).is_err());
    }
    #[test]
    fn response_layout_is_little_endian_and_packed() {
        let r = BranchResult {
            id: 42,
            l: 8,
            r: 20,
            eof: 1,
            other: 4,
            branches: vec![(255, 7)],
            search_us: 0.,
            gather_us: 0.,
            lf_steps: 0,
            rank_steps: 0,
        };
        let frame = branch_frame(&r);
        assert_eq!(frame.len(), 59);
        assert_eq!(&frame[..8], &42u64.to_le_bytes());
        assert_eq!(&frame[48..50], &1u16.to_le_bytes());
        assert_eq!(frame[50], 255);
        assert_eq!(&frame[51..], &7u64.to_le_bytes());
    }
}
