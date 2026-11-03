// Included in qlite to share the immutable index and per-worker navigation cache.
//
// MAXIMAL EXACT MATCHES need no container member that branches did not already
// need: left extension is the backward-search rank step, right extension is the
// same rank step applied to the query's own next byte, and the answer is a set
// of intervals. Nothing here reads corpus bytes.
//
// DEFINITION (and the one this file asserts): a match `query[a..b)` is MAXIMAL
// when it occurs and CANNOT BE EXTENDED WITHIN THE QUERY -
//
//   left : a == 0    OR  interval(query[a-1..b)) is empty
//   right: b == qlen OR  interval(query[a..b+1))  is empty
//
// "Within the query" is not a weakening, it is the definition of a MEM: there
// are no bytes to the left of query[0] or to the right of query[qlen-1], so
// those two boundaries are vacuously maximal. (The alternative reading - "no
// occurrence of the match is followed by any present byte" - is not a match set
// at all: sum over present bytes c of width(P||c) = width(P) - eof, so a
// right-maximal-in-the-corpus match would exist only for the corpus suffix.)
//
// COST: O(sum over start a of the match length at a) rank steps, i.e.
// O(qlen * L_max) for a query whose longest match is L_max bytes. Matches die in
// a few bytes on real text, so a 32-byte query is a few hundred rank steps.
// `--all-mems` walks every start; the default (single longest) stops opening new
// starts once `qlen - a <= best_len`, because such a start cannot beat the
// answer and ties keep the earlier start.

/// One maximal exact match: `query[start..start+len)` occurs at BWT rows [l,r),
/// so `occurrences == r - l` by definition of the interval.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Mem {
    start: u32,
    len: u32,
    l: u64,
    r: u64,
}

/// PREPEND one symbol to an interval: the interval of `c || P` from the interval
/// of `P`. This is backward search's per-byte move (an FM-index rank step over
/// the interval) and it is the ONLY extension primitive this file needs.
///
/// An empty interval stays empty, which is what makes the maximality test and
/// the extension loop the same operation.
#[inline]
fn extend_left(cx: &mut Ctx<'_>, l: u64, r: u64, sym: u8) -> (u64, u64) {
    if l == r {
        return (l, r);
    }
    let base = cx.idx.c[sym as usize];
    let (nl, nr) = (base + cx.rank(sym, l), base + cx.rank(sym, r));
    assert!(nl <= nr, "MEM_INTERVAL_INVERTED l={nl} r={nr}");
    (nl, nr)
}

/// Every MEM of `query` (or only the longest one), with the boundary assertions.
///
/// WHY THIS LOOPS OVER END POSITIONS. Backward search prepends, so the O(1) rank
/// step extends an interval to the LEFT. Growing a match to the right is only
/// possible by re-searching a longer prefix, i.e. O(len) rank steps - so the
/// direction that must be cheap in the inner loop is leftward, and a match
/// `query[a..b]` is therefore built by fixing the END `b` and walking `a`
/// downward. For a fixed end the set of starts with a non-empty interval is a
/// contiguous range `[amin[b], b]`, found by walking down until it empties.
///
/// That makes both maximality tests local:
///
///   left  maximal for (amin[b], b): nothing exists below `amin[b]`, and no
///           start above it is left-maximal, so at most ONE MEM ends at `b`;
///   right maximal for (a, b): interval(a..b+1) empty, i.e. `a < amin[b+1]`
///           (`amin[qlen] = qlen` stands for the end of the query).
///
/// Both are ASSERTED before a match is emitted: left by extending the interval
/// once more (O(1)), right by RE-SEARCHING `query[a..b+1)` from an empty interval
/// (O(len), only for emitted matches) rather than trusting `amin[b+1]`.
fn mem_scan(
    cx: &mut Ctx<'_>,
    present: &[bool; 256],
    query: &[u8],
    min_len: usize,
    all: bool,
) -> (Vec<Mem>, u64) {
    let sigma = cx.idx.reader.sigma;
    let n = cx.idx.reader.n;
    let qlen = query.len();
    // amin[b] = smallest start a with a non-empty interval for query[a..b];
    // qlen means "no match ends at b" (b - a + 1 == 0 is impossible).
    let mut amin = vec![qlen; qlen + 1];
    let mut spans = vec![(0u64, 0u64); qlen];
    for b in 0..qlen {
        // A match ending at b is at most b+1 bytes long: with the single-longest
        // answer already that long, this end cannot improve on it.
        if !all && b < min_len.saturating_sub(1) {
            continue;
        }
        let (mut l, mut r) = (0u64, n);
        let mut a = b + 1;
        while a > 0 {
            let byte = query[a - 1];
            if !present[byte as usize] {
                break;
            }
            let (nl, nr) = extend_left(cx, l, r, sigma[byte as usize]);
            if nl == nr {
                break;
            }
            l = nl;
            r = nr;
            a -= 1;
        }
        if a > b {
            continue;
        }
        amin[b] = a;
        spans[b] = (l, r);
    }
    let mut out: Vec<Mem> = Vec::new();
    let mut best: Option<Mem> = None;
    let mut non_maximal = 0u64;
    for b in 0..qlen {
        let a = amin[b];
        if a > b {
            continue;
        }
        let len = b - a + 1;
        if len < min_len {
            continue;
        }
        if !all {
            if let Some(x) = best {
                if len < x.len as usize {
                    continue;
                }
                if len == x.len as usize && a >= x.start as usize {
                    continue; // ties keep the earliest start
                }
            }
        }
        let (l, r) = spans[b];
        // RIGHT-MAXIMALITY, decided by RE-SEARCHING `query[a..b+1)` from an
        // empty interval (O(len) rank steps, emitted candidates only) and
        // cross-checked against the independent `amin[b+1]` route: a start that
        // still has a match one byte longer is not right-maximal, so no MEM ends
        // at `b` at all and the candidate is dropped, not asserted.
        let mut right_max = true;
        if b + 1 < qlen && present[query[b + 1] as usize] {
            let (mut sl, mut sr) = (0u64, n);
            let mut i = b + 1;
            loop {
                let byte = query[i];
                if !present[byte as usize] {
                    break;
                }
                let (nl, nr) = extend_left(cx, sl, sr, sigma[byte as usize]);
                if nl == nr {
                    break;
                }
                sl = nl;
                sr = nr;
                if i == a {
                    right_max = false; // query[a..b+1) occurs
                    break;
                }
                i -= 1;
            }
        }
        // Independent route to the same answer: `amin[b+1] == qlen` means no
        // match ends at b+1, otherwise starts at or above it extend. Two routes,
        // one answer - disagreement is a reader bug, not a data property.
        if b + 1 < qlen {
            let next_min = amin[b + 1];
            assert_eq!(
                right_max,
                (a as usize) < next_min,
                "MEM_AMIN_DISAGREES query_start={a} query_len={len} amin_next={next_min} \
                 researched_right_maximal={right_max}"
            );
        }
        if !right_max {
            // Not a MEM: no maximal match ends at b. Dropped, loudly accounted.
            non_maximal += 1;
            continue;
        }
        // LEFT-MAXIMALITY, asserted: one more byte to the left must empty it.
        if a > 0 {
            let byte = query[a - 1];
            if present[byte as usize] {
                let (xl, xr) = extend_left(cx, l, r, sigma[byte as usize]);
                assert!(
                    xl == xr,
                    "MEM_NOT_LEFT_MAXIMAL query_start={a} query_len={len} extended_width={}",
                    xr - xl
                );
            }
        }
        let width = r - l;
        assert!(width > 0, "MEM_EMPTY_INTERVAL query_start={a} query_len={len}");
        // The count invariant every query path shares: an interval cannot be
        // wider than the text.
        assert_count(width, n, len);
        let m = Mem {
            start: a as u32,
            len: len as u32,
            l,
            r,
        };
        if all {
            out.push(m);
        } else {
            best = Some(m);
        }
    }
    if let Some(b) = best {
        out.push(b);
    }
    (out, non_maximal)
}

/// Longest MEM, ranked by descending length then ascending query_start, at most
/// `k` of them (0 = all).
fn rank_mems(mut mems: Vec<Mem>, k: usize) -> Vec<Mem> {
    mems.sort_unstable_by(|a, b| b.len.cmp(&a.len).then(a.start.cmp(&b.start)));
    if k > 0 {
        mems.truncate(k);
    }
    mems
}

// ------------------------------------------------------------------ serve mode

/// The framed answer for one request: the single longest MEM, and nothing else.
/// `best_len == 0` means "no MEM of at least min_len bytes" (including a query
/// that does not occur at all); `query_start` is then 0.
#[derive(Debug, Clone, Copy)]
struct MemResult {
    id: u64,
    query_start: u32,
    best_len: u32,
    occurrences: u64,
    mem_us: f64,
}

/// Payload is fixed-width, 16 bytes: no byte echo, no variable-length section.
fn mem_frame(r: &MemResult) -> Vec<u8> {
    let mut b = Vec::with_capacity(16);
    b.extend(r.best_len.to_le_bytes());
    b.extend(r.query_start.to_le_bytes());
    b.extend(r.occurrences.to_le_bytes());
    b
}

fn mem_result(
    cx: &mut Ctx<'_>,
    present: &[bool; 256],
    id: u64,
    query: &[u8],
    min_len: usize,
) -> MemResult {
    let t = Instant::now();
    let (mems, _dropped) = mem_scan(cx, present, query, min_len, false);
    let us = t.elapsed().as_secs_f64() * 1e6;
    match mems.first() {
        Some(m) => MemResult {
            id,
            query_start: m.start,
            best_len: m.len,
            occurrences: m.r - m.l,
            mem_us: us,
        },
        None => MemResult {
            id,
            query_start: 0,
            best_len: 0,
            occurrences: 0,
            mem_us: us,
        },
    }
}

fn read_mem_request(r: &mut impl std::io::Read) -> std::io::Result<Option<Vec<u8>>> {
    let mut len = [0; 4];
    if r.read(&mut len[..1])? == 0 {
        return Ok(None);
    }
    r.read_exact(&mut len[1..])?;
    let n = u32::from_le_bytes(len) as usize;
    if n > 16 * 1024 * 1024 {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "query exceeds 16 MiB",
        ));
    }
    let mut b = vec![0; n];
    r.read_exact(&mut b)?;
    Ok(Some(b))
}

pub fn mem_command(raw: &[std::ffi::OsString]) {
    use std::os::unix::ffi::OsStrExt;
    let args: Vec<_> = raw.iter().map(|a| a.to_string_lossy().into_owned()).collect();
    let container = args.first().unwrap_or_else(|| die(MEM_USAGE));
    let mut query: Option<Vec<u8>> = None;
    let mut docs = None;
    let mut serve = false;
    let mut json = false;
    let mut all = false;
    let mut jobs = 1usize;
    let mut min_len = 8usize;
    let mut limit = 8u64;
    let mut k = 0usize;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--serve" => serve = true,
            "--jsonl" => json = true,
            "--all-mems" => all = true,
            "--docs" | "-j" | "-k" | "--min-len" | "--limit" => {
                let flag = args[i].clone();
                i += 1;
                let v = args
                    .get(i)
                    .unwrap_or_else(|| die("mem: missing option value"));
                match flag.as_str() {
                    "--docs" => docs = Some(v.clone()),
                    "-j" => jobs = v.parse().unwrap_or_else(|_| die("mem: invalid -j")),
                    "-k" => k = v.parse().unwrap_or_else(|_| die("mem: invalid -k")),
                    "--min-len" => min_len = v.parse().unwrap_or_else(|_| die("mem: invalid --min-len")),
                    _ => limit = v.parse().unwrap_or_else(|_| die("mem: invalid --limit")),
                }
            }
            other if other.starts_with('-') => {
                die(&format!("mem: unknown option {other}\n{MEM_USAGE}"))
            }
            _ => {
                if query.is_some() {
                    die("mem: exactly one query string per invocation (use --serve for many)");
                }
                query = Some(raw[i].as_bytes().to_vec());
            }
        }
        i += 1;
    }
    let refuse_empty = || -> ! {
        die(
            "mem: empty query: a maximal exact match needs at least one byte\n  \
             (the empty string occurs at every boundary and is not a maximal match, so it has\n  \
             no answer to report; query at least one byte)",
        )
    };
    if jobs == 0 || serve != query.is_none() {
        die("mem: require <query> OR --serve; -j > 0");
    }
    if serve && (json || all || k > 0) {
        die("mem: --serve reports the single longest match per frame; --jsonl/--all-mems/-k apply to file mode");
    }
    if !serve && query.as_ref().is_some_and(|q| q.is_empty()) {
        refuse_empty();
    }
    if !serve && query.as_ref().is_some_and(|q| q.len() > PAT_LEN_MAX as usize) {
        die(&format!("mem: query must be 1..={PAT_LEN_MAX} bytes"));
    }
    let reader = Reader::open(container).unwrap_or_else(|e| {
        die(&format!(
            "{e}\n  supported: SXI2 v6 lite (members m1,m4,m5,m7,m10,m11)\n  \
             migration: a legacy container must be rebuilt with `xsa build`;\n  \
             `xsa mem` replaces `xsa mems` for v6 lite containers."
        ))
    });
    // Positions need the corpus sidecars; interval widths do not. Serve mode and
    // --limit 0 never locate, so they never touch a corpus byte.
    let docs = if serve || limit == 0 {
        None
    } else {
        discover_documents(container, docs.as_deref(), reader.n)
    };
    let corpus = docs.as_ref().map(|d| d.context_corpus().unwrap_or_else(|e| die(&e)));
    if docs.is_none() && !serve && limit > 0 {
        eprintln!("MEM_NOTE docs=none: emitting plain corpus offsets");
    }
    let index = Index::open(reader, jobs).unwrap_or_else(|e| die(&e));
    let sigma = index.reader.sigma;
    let present: [bool; 256] = std::array::from_fn(|b| {
        let s = sigma[b] as usize;
        s != 0 && index.total[s] != 0
    });
    eprintln!(
        "MEM_READY n={} present_bytes={} min_len={min_len} mode={} workers={jobs}",
        index.n(),
        present.iter().filter(|p| **p).count(),
        if serve {
            "serve".to_string()
        } else if all {
            "all-mems".to_string()
        } else {
            "best".to_string()
        }
    );
    let started = Instant::now();
    if serve {
        mem_serve(&index, &present, jobs, min_len, started);
        return;
    }
    let query = query.unwrap();
    let mut cx = index.ctx();
    let t = Instant::now();
    let (raw, dropped) = mem_scan(&mut cx, &present, &query, min_len, all);
    let mems = rank_mems(raw, k);
    let scan_ms = t.elapsed().as_secs_f64() * 1e3;
    let stdout = std::io::stdout();
    let mut w: Box<dyn Write> = Box::new(BufWriter::with_capacity(1 << 20, stdout.lock()));
    let mut located = 0u64;
    let tl = Instant::now();
    for m in &mems {
        let width = m.r - m.l;
        let take = width.min(limit);
        let mut positions = Vec::with_capacity(take as usize);
        for row in m.l..m.l + take {
            positions.push(cx.locate(row));
        }
        located += take;
        let truncated = take < width;
        if json {
            let mut obj = serde_json::json!({
                "query_start": m.start,
                "query_len": m.len,
                "occurrences": width,
                "taken": take,
                "truncated": truncated,
                "positions": positions,
            });
            if let Some(docs) = &docs {
                let mut list = Vec::with_capacity(positions.len());
                for p in &positions {
                    let (d, off, title) = docs.lookup(*p).unwrap_or_else(|e| die(&e));
                    list.push(serde_json::json!({
                        "doc_id": d,
                        "offset": off,
                        "title": String::from_utf8_lossy(title),
                        "pos": p,
                    }));
                }
                obj["docs"] = serde_json::json!(list);
            }
            writeln!(w, "{obj}").unwrap();
        } else {
            write!(w, "{}\t{}\t{}\t{}\t{}", m.start, m.len, width, take, truncated).unwrap();
            for (j, p) in positions.iter().enumerate() {
                let sep = if j == 0 { '\t' } else { ',' };
                if let Some(docs) = &docs {
                    let (d, off, _) = docs.lookup(*p).unwrap_or_else(|e| die(&e));
                    write!(w, "{sep}{d}:{off}:{p}").unwrap();
                } else {
                    write!(w, "{sep}{p}").unwrap();
                }
            }
            if let Some(docs) = &docs {
                // Titles last, one column, joined by '|': titles carry arbitrary
                // bytes, so only the TSV delimiters are escaped (as `query` does).
                w.write_all(b"\t").unwrap();
                for (j, p) in positions.iter().enumerate() {
                    let (_, _, title) = docs.lookup(*p).unwrap_or_else(|e| die(&e));
                    if j > 0 {
                        w.write_all(b"|").unwrap();
                    }
                    for &b in title {
                        match b {
                            b'\t' => w.write_all(b"\\t"),
                            b'\n' => w.write_all(b"\\n"),
                            b'\r' => w.write_all(b"\\r"),
                            b'\\' => w.write_all(b"\\\\"),
                            _ => w.write_all(&[b]),
                        }
                        .unwrap();
                    }
                }
            }
            writeln!(w).unwrap();
        }
    }
    let locate_ms = tl.elapsed().as_secs_f64() * 1e3;
    w.flush().unwrap();
    let total_ms = started.elapsed().as_secs_f64() * 1e3;
    eprintln!(
        "MEM_STATS queries=1 mems={} dropped_non_maximal={dropped} min_len={min_len} present_bytes={} scan_ms={scan_ms:.3} \
         locate_ms={locate_ms:.3} total_ms={total_ms:.3} positions={located} ranks={} lf_steps={}",
        mems.len(),
        present.iter().filter(|p| **p).count(),
        cx.ranks,
        cx.steps
    );
    let _ = corpus; // positions are raw offsets; corpus bytes are not read here
}

fn mem_serve(index: &Index, present: &[bool; 256], jobs: usize, min_len: usize, started: Instant) {
    let stdout = std::io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    // One queue per worker, dispatched round-robin, ordered results - the same
    // shape branches uses, so the only shared state in the hot path is the
    // read-only index.
    let mut req_tx = Vec::with_capacity(jobs);
    let mut req_rx = Vec::with_capacity(jobs);
    for _ in 0..jobs {
        let (t, r) = std::sync::mpsc::sync_channel::<(u64, Vec<u8>)>(16);
        req_tx.push(t);
        req_rx.push(r);
    }
    let (permit_tx, permit_rx) = std::sync::mpsc::sync_channel(jobs * 4);
    for _ in 0..jobs * 4 {
        permit_tx.send(()).unwrap();
    }
    let mut count = 0u64;
    let mut mem_us = 0.;
    let mut input_us = 0.;
    let mut output_us = 0.;
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
                let request = read_mem_request(&mut input)?;
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
            let idx = index;
            scope.spawn(move || {
                let mut cx = idx.ctx();
                while let Ok((id, p)) = rx.recv() {
                    // A zero-length frame is refused here, by name, exactly as
                    // the file-mode empty query is: it has no maximal match to
                    // report and a silently empty answer would be a lie.
                    if p.is_empty() {
                        die(
                            "mem --serve: zero-length query frame: a maximal exact match needs at \
                             least one byte (clients must not send empty requests)",
                        );
                    }
                    tx.send(mem_result(&mut cx, present, id, &p, min_len)).unwrap();
                }
            });
        }
        drop(results);
        let mut next = 0u64;
        let mut pending = std::collections::BTreeMap::new();
        for r in rr {
            pending.insert(r.id, r);
            while let Some(r) = pending.remove(&next) {
                let t = Instant::now();
                let b = mem_frame(&r);
                out.write_all(&(b.len() as u32).to_le_bytes()).unwrap();
                out.write_all(&b).unwrap();
                out.flush().unwrap();
                output_us += t.elapsed().as_secs_f64() * 1e6;
                count += 1;
                mem_us += r.mem_us;
                next += 1;
                let _ = permit_tx.send(());
            }
        }
        input_us = input
            .join()
            .unwrap()
            .unwrap_or_else(|e| die(&format!("mem: request frame: {e}")));
    });
    let secs = started.elapsed().as_secs_f64();
    eprintln!(
        "MEM_STATS queries={count} queries_per_s={:.3} us_per_query={:.3} \
         mem_us_per_query={:.3} output_ipc_us_per_query={:.3} input_ipc_us_per_query={:.3} \
         min_len={min_len} workers={jobs}",
        count as f64 / secs,
        secs * 1e6 / count.max(1) as f64,
        mem_us / count.max(1) as f64,
        output_us / count.max(1) as f64,
        input_us / count.max(1) as f64,
    );
}

const MEM_USAGE: &str = "usage:\n  \
    xsa mem <container> <query> [-k N] [--min-len N] [--docs DIR] [--limit N] [--jsonl] [--all-mems]\n  \
    xsa mem <container> --serve -j N   (frames on stdin/stdout; see SERVING.md)\n  \
  default: the single longest maximal exact match; --all-mems reports every maximal match";

#[cfg(test)]
mod mem_protocol_tests {
    use super::*;
    #[test]
    fn response_layout_is_little_endian_and_packed() {
        let r = MemResult {
            id: 3,
            query_start: 7,
            best_len: 19,
            occurrences: 1 << 40,
            mem_us: 0.,
        };
        let f = mem_frame(&r);
        assert_eq!(f.len(), 16);
        assert_eq!(&f[..4], &19u32.to_le_bytes());
        assert_eq!(&f[4..8], &7u32.to_le_bytes());
        assert_eq!(&f[8..], &(1u64 << 40).to_le_bytes());
    }
    #[test]
    fn request_frames_reject_empty_and_truncated() {
        let mut input = 3u32.to_le_bytes().to_vec();
        input.extend(b"abc");
        assert_eq!(read_mem_request(&mut input.as_slice()).unwrap(), Some(b"abc".to_vec()));
        assert_eq!(read_mem_request(&mut [].as_slice()).unwrap(), None);
        assert!(read_mem_request(&mut [3, 0, 0, 0, b'a'].as_slice()).is_err());
        assert!(read_mem_request(&mut u32::MAX.to_le_bytes().as_slice()).is_err());
        // A zero-length frame is a VALID frame (it parses) and is refused by the
        // worker by name, not by the parser.
        assert_eq!(read_mem_request(&mut 0u32.to_le_bytes().as_slice()).unwrap(), Some(Vec::new()));
    }
    #[test]
    fn ranking_is_longest_then_earliest() {
        let m = |start, len| Mem { start, len, l: 0, r: 0 };
        let r = rank_mems(vec![m(9, 4), m(2, 9), m(5, 9)], 0);
        assert_eq!(r.iter().map(|m| m.start).collect::<Vec<_>>(), vec![2, 5, 9]);
        assert_eq!(rank_mems(vec![m(9, 4), m(2, 9)], 1).len(), 1);
    }
}
