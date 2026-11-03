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
    steps: u64,
}
fn branch_result(cx: &mut Ctx<'_>, corpus: &[u8], id: u64, pat: &[u8], k: usize) -> BranchResult {
    let start = Instant::now();
    let sigma = cx.idx.reader.sigma;
    let absent = pat
        .iter()
        .any(|&b| sigma[b as usize] == 0 || cx.idx.total[sigma[b as usize] as usize] == 0);
    let work: Vec<u8> = pat.iter().rev().map(|&b| sigma[b as usize]).collect();
    let (l, r) = if absent { (0, 0) } else { cx.search(&work) };
    let mut search_us = start.elapsed().as_secs_f64() * 1e6;
    let start = Instant::now();
    let mut extension_us = 0.;
    let mut counts = [0u64; 256];
    let mut eof = 0;
    let mut steps = 0;
    if pat.is_empty() {
        // Empty context occurs at all corpus boundaries, including EOF.
        for b in 0..256 {
            if sigma[b] != 0 {
                counts[b] = cx.idx.total[sigma[b] as usize];
            }
        }
        eof = 1;
    } else if r - l > 64 {
        // Right extensions: count(context || byte), NOT rank over [l,r).
        // Avoid an unbounded number of sparse-anchor LF walks on common text.
        let extension_start = Instant::now();
        let mut extension = Vec::with_capacity(work.len() + 1);
        extension.push(0);
        extension.extend_from_slice(&work);
        for b in 0..256 {
            if sigma[b] == 0 || cx.idx.total[sigma[b] as usize] == 0 {
                continue;
            }
            extension[0] = sigma[b];
            let (a, z) = cx.search(&extension);
            counts[b] = z - a;
        }
        extension_us = extension_start.elapsed().as_secs_f64() * 1e6;
        search_us += extension_us;
        eof = (r - l)
            .checked_sub(counts.iter().sum::<u64>())
            .expect("right extensions exceed interval");
        assert!(eof <= 1, "multiple corpus EOF occurrences");
    } else {
        for row in l..r {
            let (pos, st) = cx.locate_steps(row);
            steps += st;
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
        steps,
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
    let corpus = if let Some(path) = corpus_path {
        let f = File::open(path).unwrap_or_else(|e| die(&e.to_string()));
        unsafe { Mmap::map(&f) }.unwrap_or_else(|e| die(&e.to_string()))
    } else {
        let documents=discover_documents(container,docs.as_deref(),reader.n).unwrap_or_else(||die("branches: anchor+LF fallback needs corpus.bin and matching sidecars (--docs DIR), or --corpus FILE"));
        documents.context_corpus().unwrap_or_else(|e| die(&e))
    };
    if corpus.len() as u64 + 1 != reader.n {
        die("branches: corpus length does not match index")
    }
    let index = Index::open(reader, jobs).unwrap_or_else(|e| die(&e));
    eprintln!(
        "BRANCH_READY navigation=anchor+LF corpus_bytes={} workers={jobs}",
        corpus.len()
    );
    let started = Instant::now();
    let mut count = 0u64;
    let mut search = 0.;
    let mut gather = 0.;
    let mut steps = 0u64;
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
        steps += r.steps;
    };
    let stdout = std::io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    if serve {
        let (tx, rx) = std::sync::mpsc::sync_channel::<(u64, Vec<u8>)>(jobs * 2);
        let rx = std::sync::Mutex::new(rx);
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
                let mut input_us = 0.;
                loop {
                    let t = Instant::now();
                    let request = read_branch_request(&mut input)?;
                    input_us += t.elapsed().as_secs_f64() * 1e6;
                    let Some(p) = request else { break };
                    permit_rx.recv().unwrap();
                    tx.send((id, p)).unwrap();
                    id += 1;
                }
                Ok(input_us)
            });
            for _ in 0..jobs {
                let rx = &rx;
                let tx = results.clone();
                let idx = &index;
                let corpus = &corpus;
                scope.spawn(move || {
                    let mut cx = idx.ctx();
                    loop {
                        let job = rx.lock().unwrap().recv();
                        let Ok((id, p)) = job else { break };
                        tx.send(branch_result(&mut cx, corpus, id, &p, k)).unwrap();
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
            emit(branch_result(&mut cx, &corpus, id as u64, p, k), &mut out);
        }
    }
    out.flush().unwrap();
    eprintln!("BRANCH_STATS contexts={count} contexts_per_s={:.3} search_us_per_context={:.3} gather_us_per_context={:.3} output_ipc_us_per_context={:.3} input_ipc_us_per_context={:.3} lf_steps={steps}",count as f64/started.elapsed().as_secs_f64(),search/count.max(1) as f64,gather/count.max(1) as f64,ipc/count.max(1) as f64,input_ipc/count.max(1) as f64);
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
            steps: 0,
        };
        let frame = branch_frame(&r);
        assert_eq!(frame.len(), 59);
        assert_eq!(&frame[..8], &42u64.to_le_bytes());
        assert_eq!(&frame[48..50], &1u16.to_le_bytes());
        assert_eq!(frame[50], 255);
        assert_eq!(&frame[51..], &7u64.to_le_bytes());
    }
}
