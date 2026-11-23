// Request: little-endian u32 payload length, u8 count, then repeated
// little-endian u16 length + original bytes. Malformed input is terminal.
fn positional_patterns(data: &[u8]) -> std::io::Result<Vec<&[u8]>> {
    let bad = || std::io::Error::other("invalid positional frame");
    let (&count, mut tail) = data.split_first().ok_or_else(bad)?;
    if !(1..=16).contains(&count) {
        return Err(bad());
    }
    let mut patterns = Vec::new();
    for _ in 0..count {
        let n = u16::from_le_bytes(tail.get(..2).ok_or_else(bad)?.try_into().unwrap()) as usize;
        if !(1..=256).contains(&n) {
            return Err(bad());
        }
        patterns.push(tail.get(2..2 + n).ok_or_else(bad)?);
        tail = &tail[2 + n..];
    }
    if !tail.is_empty() {
        return Err(bad());
    }
    Ok(patterns)
}

pub fn positional_worker(args: &[std::ffi::OsString]) -> std::io::Result<()> {
    use serde_json::json;
    use std::io::{Error, Read, Write};
    use std::os::unix::fs::FileExt;
    fn ns(t: Instant) -> u64 {
        t.elapsed().as_nanos() as u64
    }
    let [path, corpus] = args else {
        return Err(Error::other("positional-worker INDEX CORPUS"));
    };
    let now = Instant::now();
    let reader = Reader::open(path.to_str().ok_or_else(|| Error::other("index path"))?)
        .map_err(Error::other)?;
    let index = Index::open(reader, 1).map_err(Error::other)?;
    let index_ns = ns(now);
    let t = Instant::now();
    let docs = crate::docstarts::DocStarts::from_container(&index.reader);
    let file = std::fs::File::open(corpus)?;
    if file.metadata()?.len() != docs.text_len {
        return Err(Error::other("corpus length"));
    }
    let verifier_ns = ns(t);
    let t = Instant::now();
    let mut cx = index.ctx(); // Index owns the CLI's bounded decoded-block arena.
    let ctx_ns = ns(t);
    let mut out = std::io::BufWriter::new(std::io::stdout().lock());
    writeln!(
        out,
        "{}",
        json!({"ready":1,"index_ns":index_ns,"verifier_ns":verifier_ns,"ctx_ns":ctx_ns,"arena_bytes":index.cache_bytes()})
    )?;
    out.flush()?;
    let mut input = std::io::stdin().lock();
    loop {
        let mut header = [0; 4];
        if input.read(&mut header[..1])? == 0 {
            return Ok(());
        }
        let t = Instant::now(); // idle wait for byte 1 excluded
        input.read_exact(&mut header[1..])?;
        let size = u32::from_le_bytes(header) as usize;
        if !(4..=1 + 16 * 258).contains(&size) {
            return Err(Error::other("positional frame size"));
        }
        let mut data = vec![0; size];
        input.read_exact(&mut data)?;
        let patterns = positional_patterns(&data)?;
        let receive_ns = ns(t);
        let mut rows = Vec::new();
        for p in patterns {
            let cache = index.cache_stats();
            let before = cx.diag();
            let walls = index.wall_stats();
            let t = Instant::now();
            let work: Vec<_> = p
                .iter()
                .rev()
                .map(|&b| index.reader.sigma[b as usize])
                .collect();
            let (l, r) = if work.contains(&0) {
                (0, 0)
            } else {
                cx.search(&work)
            };
            let search_ns = ns(t);
            let ranks = cx.diag().3 - before.3;
            let mut hits = Vec::new();
            let mut lf = Vec::new();
            let (mut locate_ns, mut bounds_ns, mut verify_ns) = (0, 0, 0);
            if r - l <= 8 {
                for row in l..r {
                    let t = Instant::now();
                    let (pos, steps) = cx.locate_steps(row);
                    locate_ns += ns(t);
                    lf.push(steps);
                    let t = Instant::now();
                    let last = pos
                        .checked_add(p.len() as u64 - 1)
                        .filter(|&v| v < docs.text_len)
                        .ok_or_else(|| Error::other("invalid position"))?;
                    let (doc, base) = docs.rank_start(last);
                    bounds_ns += ns(t);
                    let t = Instant::now();
                    let mut buf = [0; 256];
                    file.read_exact_at(&mut buf[..p.len()], pos)?;
                    if &buf[..p.len()] != p {
                        return Err(Error::other("byte verification failed"));
                    }
                    verify_ns += ns(t);
                    if base <= pos {
                        hits.push([pos, doc, pos - base]);
                    }
                }
            }
            let after = index.cache_stats();
            let done = cx.diag();
            let (decode, drop, _) = index.wall_stats();
            rows.push(json!({"status":if r-l>8 {"overcap"} else {"complete"},"raw":r-l,"hits":hits,"lf":lf,"ranks":ranks,"search_ns":search_ns,"locate_ns":locate_ns,"bounds_ns":bounds_ns,"verify_ns":verify_ns,"block_lookups":done.4-before.4,"scan_of":done.0-before.0,"scan_cum":done.1-before.1,"decode_ns":decode-walls.0,"drop_ns":drop-walls.1,"cache_hits":after.0-cache.0,"cache_misses":after.1-cache.1}));
        }
        let t = Instant::now();
        writeln!(out, "{}", json!({"rows":rows,"receive_ns":receive_ns}))?;
        out.flush()?;
        // Second line fences actual serialization + flush of the answer.
        writeln!(out, "{}", json!({"send_ns":ns(t)}))?;
        out.flush()?;
    }
}
