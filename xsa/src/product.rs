//! Annotated, deterministic JSONL queries over a shared immutable SXI index.
use super::{die, ms_vector, sxi, Ri4};
use flate2::read::MultiGzDecoder;
use rayon::prelude::*;
use serde_json::{json, Value};
use std::fs::File;
use std::io::{self, BufRead, BufReader, BufWriter, Read, Write};

const MAX_READ: usize = 65536;
const BATCH_BYTES: usize = 1 << 20;
const HTTP_BYTES: usize = 2 << 20;

#[derive(Debug)]
pub struct Record {
    pub name: String,
    pub start: u64,
    pub len: u64,
}
/// Sidecar fstart is in mirrored coordinates. Transform it back to stream
/// coordinates; record ends are precisely the sorted separator positions.
pub fn names(bytes: &[u8], n: u64) -> Result<Vec<Record>, String> {
    let text = std::str::from_utf8(bytes).map_err(|_| "names must be UTF-8")?;
    let mut records = Vec::new();
    let mut next = 0;
    for line in text.lines() {
        let fields: Vec<_> = line.split('\t').collect();
        if fields.len() != 3 || fields[0].is_empty() {
            return Err("invalid names TSV".into());
        }
        let fs: u64 = fields[1].parse().map_err(|_| "invalid names start")?;
        let len: u64 = fields[2].parse().map_err(|_| "invalid names length")?;
        let start = n
            .checked_sub(1)
            .and_then(|v| v.checked_sub(fs))
            .and_then(|v| v.checked_sub(len))
            .ok_or("names coordinates outside text")?;
        if start != next {
            return Err("names must partition text in stream order".into());
        }
        next = start
            .checked_add(len)
            .and_then(|x| x.checked_add(1))
            .ok_or("names overflow")?;
        records.push(Record {
            name: fields[0].into(),
            start,
            len,
        });
    }
    if records.is_empty() || next != n {
        return Err("names lengths disagree with n".into());
    }
    Ok(records)
}

pub struct Engine {
    idx: Ri4,
    records: Vec<Record>,
    boundaries: Vec<u64>,
    dna: bool,
    reversed: bool,
    chi: u64,
}
impl Engine {
    fn open(path: &str, mode: &str, orientation: Option<bool>) -> Result<Self, String> {
        let meta = sxi::Container::open(path).ok_or("need an SXI container")?;
        let flags = meta.flags;
        let chi = meta.member(5).count;
        let idx = Ri4::load_validated(path, Some(meta));
        let records = if let Some((path, offset, len)) = &idx.names {
            use std::io::{Seek, SeekFrom};
            let mut f = File::open(path).map_err(|e| e.to_string())?;
            f.seek(SeekFrom::Start(*offset))
                .map_err(|e| e.to_string())?;
            let mut data = Vec::new();
            f.take(*len)
                .read_to_end(&mut data)
                .map_err(|e| e.to_string())?;
            names(&data, idx.n)?
        } else {
            vec![Record {
                name: "text".into(),
                start: 0,
                len: idx.n,
            }]
        };
        let dna = match mode {
            "auto" => flags & 2 != 0,
            "dna" => true,
            "text" => false,
            _ => return Err("--mode must be auto, dna or text".into()),
        };
        let reversed = orientation.unwrap_or(flags & 4 != 0);
        let boundaries = records.iter().map(|r| r.start + r.len).collect();
        Ok(Self {
            idx,
            records,
            boundaries,
            dna,
            reversed,
            chi,
        })
    }
    fn annotate(&self, pos: u64, len: usize) -> Option<(usize, &Record, u64)> {
        let doc = self.boundaries.partition_point(|&end| end < pos);
        let record = self.records.get(doc)?;
        let offset = pos.checked_sub(record.start)?;
        if offset.checked_add(len as u64)? > record.len {
            return None;
        }
        let offset = if self.reversed {
            record.len - offset - len as u64
        } else {
            offset
        };
        Some((doc, record, offset))
    }
    fn validate(&self, seq: &[u8]) -> Result<(), String> {
        if seq.is_empty() || seq.len() > MAX_READ {
            return Err(format!("read length must be 1..{MAX_READ}"));
        }
        if seq.contains(&0x1e) {
            return Err("query contains reserved separator 0x1E".into());
        }
        if self.dna && seq.iter().any(|&c| complement(c).is_none()) {
            return Err("dna mode: non-IUPAC query byte".into());
        }
        Ok(())
    }
    fn orientations(&self, seq: &[u8]) -> Vec<(Vec<u8>, &'static str)> {
        let mut out = vec![(seq.to_vec(), "+")];
        if self.dna {
            out.push((
                seq.iter().rev().map(|&c| complement(c).unwrap()).collect(),
                "-",
            ));
        }
        for (seq, _) in &mut out {
            if self.reversed {
                seq.reverse();
            }
        }
        out
    }
    fn interval(&self, seq: &[u8]) -> (u64, u64) {
        self.idx
            .search(&seq.iter().rev().copied().collect::<Vec<_>>())
    }
    fn ms(&self, seq: &[u8]) -> Vec<u32> {
        let mut values = ms_vector(&self.idx, &seq.iter().rev().copied().collect::<Vec<_>>());
        values.reverse();
        values
    }
    fn position(&self, row: u64) -> u64 {
        self.idx.n - 1 - self.idx.s_at_opt(row, self.idx.embedded_anchors.as_ref())
    }
    fn hit(
        &self,
        read: &str,
        row: u64,
        len: usize,
        qstart: Option<usize>,
        strand: &str,
    ) -> Option<Value> {
        let (doc, record, offset) = self.annotate(self.position(row), len)?;
        let mut v =
            json!({"read": read, "name": record.name, "offset": offset, "len": len, "doc_id": doc});
        if let Some(qstart) = qstart {
            v["qstart"] = json!(qstart);
        }
        if self.dna {
            v["strand"] = json!(strand);
        }
        Some(v)
    }
    fn query(
        &self,
        read: &str,
        seq: &[u8],
        out: &mut impl Write,
        sample: Option<usize>,
        seed: u64,
        trace: bool,
    ) -> Result<(), String> {
        self.validate(seq)?;
        for (seq, strand) in self.orientations(seq) {
            let (l, r) = self.interval(&seq);
            if let Some(limit) = sample {
                // Floyd's O(sample) selection: never allocate the full interval.
                let k = (limit as u64).min(r.saturating_sub(l));
                let mut chosen = std::collections::BTreeSet::new();
                let mut state = seed ^ 0xa0761d6478bd642f;
                for j in (r - l - k)..(r - l) {
                    state ^= state >> 12;
                    state ^= state << 25;
                    state ^= state >> 27;
                    let pick = state.wrapping_mul(0x2545f4914f6cdd1d) % (j + 1);
                    if !chosen.insert(pick) {
                        chosen.insert(j);
                    }
                }
                for row in chosen {
                    if let Some(mut v) = self.hit(read, l + row, seq.len(), None, strand) {
                        if trace {
                            v["row"] = json!(l + row);
                        }
                        line(out, &v)?;
                    }
                }
            } else {
                for row in l..r {
                    if let Some(mut v) = self.hit(read, row, seq.len(), None, strand) {
                        if trace {
                            v["row"] = json!(row);
                        }
                        line(out, &v)?;
                    }
                }
            }
        }
        Ok(())
    }
    fn matching_statistics(
        &self,
        read: &str,
        seq: &[u8],
        out: &mut impl Write,
    ) -> Result<(), String> {
        self.validate(seq)?;
        for (seq, strand) in self.orientations(seq) {
            let mut values = self.ms(&seq);
            // MS is indexed by start in the oriented query. For reversed
            // reference storage compute forward-oriented starts explicitly.
            if self.reversed {
                values = (0..seq.len())
                    .map(|i| {
                        let end = seq.len() - i;
                        (1..=end)
                            .rev()
                            .find(|&len| {
                                let (l, r) = self.interval(&seq[end - len..end]);
                                l < r
                            })
                            .unwrap_or(0) as u32
                    })
                    .collect();
            }
            let mut v = json!({"read": read, "ms": values});
            if self.dna {
                v["strand"] = json!(strand);
            }
            line(out, &v)?;
        }
        Ok(())
    }
    fn mems(
        &self,
        read: &str,
        seq: &[u8],
        min_len: usize,
        out: &mut impl Write,
    ) -> Result<(), String> {
        self.validate(seq)?;
        for (work, strand) in self.orientations(seq) {
            let ms = self.ms(&work);
            for (start, &max) in ms.iter().enumerate() {
                // MS bounds the search. Shorter maximal matches at OTHER text
                // occurrences must also be emitted; longest-only loses MEMs.
                for len in min_len..=max as usize {
                    let (l, r) = self.interval(&work[start..start + len]);
                    let (el, er) = if start + len < work.len() {
                        self.interval(&work[start..start + len + 1])
                    } else {
                        (0, 0)
                    };
                    for row in l..r {
                        if row >= el && row < er {
                            continue;
                        } // right extendible at this occurrence
                        if start > 0
                            && self.idx.run_char[self.idx.run_of(row) as usize] == work[start - 1]
                        {
                            continue;
                        }
                        let reverse_query = self.reversed ^ (strand == "-");
                        let qstart = if reverse_query {
                            work.len() - start - len
                        } else {
                            start
                        };
                        if let Some(v) = self.hit(read, row, len, Some(qstart), strand) {
                            line(out, &v)?;
                        }
                    }
                }
            }
        }
        Ok(())
    }
    fn stats(&self) -> Value {
        json!({"n": self.idx.n, "k": self.records.len(), "runs": self.idx.r, "chi": self.chi,
               "mode": if self.dna { "dna" } else { "text" }, "reversed": self.reversed})
    }
}
fn complement(c: u8) -> Option<u8> {
    let b = match c.to_ascii_uppercase() {
        b'A' => b'T',
        b'T' => b'A',
        b'C' => b'G',
        b'G' => b'C',
        b'R' => b'Y',
        b'Y' => b'R',
        b'S' => b'S',
        b'W' => b'W',
        b'K' => b'M',
        b'M' => b'K',
        b'B' => b'V',
        b'V' => b'B',
        b'D' => b'H',
        b'H' => b'D',
        b'N' => b'N',
        _ => return None,
    };
    Some(if c.is_ascii_lowercase() {
        b.to_ascii_lowercase()
    } else {
        b
    })
}
fn line(out: &mut impl Write, value: &Value) -> Result<(), String> {
    serde_json::to_writer(&mut *out, value).map_err(|e| e.to_string())?;
    out.write_all(b"\n").map_err(|e| e.to_string())
}

/// Bounded reader: wrapped FASTA and strict four-line FASTQ, gzip by magic.
struct Reads {
    input: Box<dyn BufRead>,
    pending: Option<Vec<u8>>,
}
impl Reads {
    fn open(path: &str) -> Result<Self, String> {
        let mut f = BufReader::new(File::open(path).map_err(|e| e.to_string())?);
        let gz = f
            .fill_buf()
            .map_err(|e| e.to_string())?
            .starts_with(&[31, 139]);
        let input: Box<dyn BufRead> = if gz {
            Box::new(BufReader::new(MultiGzDecoder::new(f)))
        } else {
            Box::new(f)
        };
        Ok(Self {
            input,
            pending: None,
        })
    }
    fn physical(&mut self) -> Result<Option<Vec<u8>>, String> {
        let mut b = Vec::new();
        self.input
            .by_ref()
            .take((MAX_READ + 2) as u64)
            .read_until(b'\n', &mut b)
            .map_err(|e| e.to_string())?;
        if b.is_empty() {
            return Ok(None);
        }
        if b.len() > MAX_READ + 1 {
            return Err("input line too long".into());
        }
        if b.last() == Some(&b'\n') {
            b.pop();
        }
        if b.last() == Some(&b'\r') {
            b.pop();
        }
        Ok(Some(b))
    }
    fn next(&mut self) -> Result<Option<(String, Vec<u8>)>, String> {
        let h = match self
            .pending
            .take()
            .map(Ok)
            .unwrap_or_else(|| self.physical().and_then(|x| x.ok_or("EOF".into())))
        {
            Ok(v) => v,
            Err(e) if e == "EOF" => return Ok(None),
            Err(e) => return Err(e),
        };
        if h.len() < 2 || !matches!(h[0], b'>' | b'@') {
            return Err("expected FASTA/FASTQ header".into());
        }
        let name = std::str::from_utf8(&h[1..])
            .map_err(|_| "header must be UTF-8")?
            .to_owned();
        let mut seq = Vec::new();
        if h[0] == b'@' {
            seq = self.physical()?.ok_or("truncated FASTQ sequence")?;
            let plus = self.physical()?.ok_or("truncated FASTQ +")?;
            let quality = self.physical()?.ok_or("truncated FASTQ quality")?;
            if !plus.starts_with(b"+")
                || quality.len() != seq.len()
                || quality.iter().any(|&x| !(33..=126).contains(&x))
            {
                return Err("malformed FASTQ +/quality".into());
            }
            if plus.len() > 1 && plus[1..] != h[1..] {
                return Err("FASTQ + identifier mismatch".into());
            }
        } else {
            while let Some(b) = self.physical()? {
                if b.starts_with(b">") {
                    self.pending = Some(b);
                    break;
                }
                seq.extend_from_slice(&b);
                if seq.len() > MAX_READ {
                    return Err("read too long".into());
                }
            }
        }
        Ok(Some((name, seq)))
    }
}
struct Options {
    path: String,
    reads: Option<String>,
    pattern: Option<String>,
    mode: String,
    orientation: Option<bool>,
    jobs: usize,
    min: usize,
    bind: String,
    ms: bool,
    output: Option<String>,
    sample: Option<usize>,
    seed: u64,
    trace: bool,
}
fn parse(args: &[String]) -> Result<Options, String> {
    let mut o = Options {
        path: String::new(),
        reads: None,
        pattern: None,
        mode: "auto".into(),
        orientation: None,
        jobs: 1,
        min: 20,
        bind: "127.0.0.1:7331".into(),
        ms: false,
        output: None,
        sample: None,
        seed: 0,
        trace: false,
    };
    let mut i = 0;
    while i < args.len() {
        let flag = &args[i];
        i += 1;
        match flag.as_str() {
            "--trace-samples" => {
                o.trace = true;
                continue;
            }
            "--ms" => {
                o.ms = true;
                continue;
            }
            "--plain" => {
                o.orientation = Some(false);
                continue;
            }
            "--revlines" => {
                o.orientation = Some(true);
                continue;
            }
            _ => {}
        }
        let v = args.get(i).ok_or_else(|| format!("{flag} needs a value"))?;
        i += 1;
        match flag.as_str() {
            "--sxi" | "--ri4" => o.path = v.clone(),
            "--reads" | "--patterns" => o.reads = Some(v.clone()),
            "--pattern" => o.pattern = Some(v.clone()),
            "--mode" => o.mode = v.clone(),
            "--bind" => o.bind = v.clone(),
            "--output" | "--ms-out" => o.output = Some(v.clone()),
            "-j" | "--threads" => o.jobs = v.parse().map_err(|_| "invalid threads")?,
            "--min-len" => o.min = v.parse().map_err(|_| "invalid min-len")?,
            "--sample" => o.sample = Some(v.parse().map_err(|_| "invalid sample")?),
            "--seed" => o.seed = v.parse().map_err(|_| "invalid seed")?,
            _ => return Err(format!("unknown option {flag}")),
        }
    }
    if o.path.is_empty() || !(1..=64).contains(&o.jobs) || o.min == 0 {
        return Err("need --sxi; threads 1..64; min-len > 0".into());
    }
    Ok(o)
}
fn process(
    e: &Engine,
    kind: &str,
    o: &Options,
    name: &str,
    seq: &[u8],
    out: &mut impl Write,
) -> Result<(), String> {
    if kind == "mems" {
        e.mems(name, seq, o.min, out)
    } else if o.ms {
        e.matching_statistics(name, seq, out)
    } else {
        e.query(name, seq, out, o.sample, o.seed, o.trace)
    }
}
fn run(kind: &str, args: &[String]) -> Result<(), String> {
    let o = parse(args)?;
    let engine = Engine::open(&o.path, &o.mode, o.orientation)?;
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(o.jobs)
        .build()
        .map_err(|e| e.to_string())?;
    if kind == "serve" {
        return serve(engine, o, pool);
    }
    let mut out: Box<dyn Write> = if let Some(path) = &o.output {
        Box::new(BufWriter::new(
            File::create(path).map_err(|e| e.to_string())?,
        ))
    } else {
        Box::new(BufWriter::new(io::stdout()))
    };
    if let Some(pattern) = &o.pattern {
        process(&engine, kind, &o, "query", pattern.as_bytes(), &mut out)?;
    } else {
        let mut reads = Reads::open(o.reads.as_deref().ok_or("need --reads or --pattern")?)?;
        loop {
            let mut batch = Vec::new();
            let mut bytes = 0;
            while bytes < BATCH_BYTES && batch.len() < 256 {
                if let Some((name, seq)) = reads.next()? {
                    engine.validate(&seq)?;
                    bytes += name.len() + seq.len();
                    batch.push((name, seq));
                } else {
                    break;
                }
            }
            if batch.is_empty() {
                break;
            }
            // Spool output per read: abundant/repetitive hits cannot inflate RAM.
            let results: Vec<Result<File, String>> = pool.install(|| {
                batch
                    .par_iter()
                    .map(|(name, seq)| {
                        let mut file = tempfile::tempfile().map_err(|e| e.to_string())?;
                        {
                            let mut w = BufWriter::new(&mut file);
                            process(&engine, kind, &o, name, seq, &mut w)?;
                            w.flush().map_err(|e| e.to_string())?;
                        }
                        Ok(file)
                    })
                    .collect()
            });
            for result in results {
                use std::io::{Seek, SeekFrom};
                let mut f = result?;
                f.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
                io::copy(&mut f, &mut out).map_err(|e| e.to_string())?;
            }
        }
    }
    out.flush().map_err(|e| e.to_string())
}
fn request(
    e: &Engine,
    o: &Options,
    method: &str,
    path: &str,
    value: Value,
    out: &mut impl Write,
) -> Result<(), String> {
    if method == "GET" && path == "/stats" {
        return line(out, &e.stats());
    }
    if method != "POST" {
        return Err("expected POST /query, /ms, /batch or GET /stats".into());
    }
    let read = |v: &Value| -> Result<(String, String), String> {
        let name = v.get("name").and_then(Value::as_str).unwrap_or("query");
        let seq = v
            .get("pattern")
            .or_else(|| v.get("read"))
            .and_then(Value::as_str)
            .ok_or("need pattern or read string")?;
        e.validate(seq.as_bytes())?;
        Ok((name.into(), seq.into()))
    };
    match path {
        "/query" => {
            let (name, seq) = read(&value)?;
            e.query(&name, seq.as_bytes(), out, o.sample, o.seed, o.trace)
        }
        "/ms" => {
            let (name, seq) = read(&value)?;
            e.matching_statistics(&name, seq.as_bytes(), out)
        }
        "/batch" => {
            let reads = value
                .get("reads")
                .and_then(Value::as_array)
                .ok_or("need reads array")?;
            let min = value
                .get("min_len")
                .map(|v| {
                    v.as_u64()
                        .filter(|&n| n > 0 && n <= MAX_READ as u64)
                        .ok_or("invalid min_len")
                })
                .transpose()?
                .unwrap_or(o.min as u64) as usize;
            if reads.len() > 256 {
                return Err("batch exceeds 256 reads".into());
            }
            let reads = reads.iter().map(read).collect::<Result<Vec<_>, _>>()?;
            for (name, seq) in reads {
                e.mems(&name, seq.as_bytes(), min, out)?;
            }
            Ok(())
        }
        _ => Err("unknown endpoint".into()),
    }
}
fn serve(engine: Engine, o: Options, pool: rayon::ThreadPool) -> Result<(), String> {
    let server = tiny_http::Server::http(&o.bind).map_err(|e| e.to_string())?;
    eprintln!("xsa serve: listening {}", server.server_addr());
    // Fixed workers pull requests directly. There is no unbounded application
    // work queue and every worker borrows the same mmap-backed index.
    pool.scope(|scope| {
        for _ in 0..o.jobs {
            let server = &server;
            let engine = &engine;
            let o = &o;
            scope.spawn(move |_| {
                while let Ok(mut req) = server.recv() {
                    let mut body = Vec::new();
                    let result: Result<File, String> = (|| {
                        req.as_reader()
                            .take((HTTP_BYTES + 1) as u64)
                            .read_to_end(&mut body)
                            .map_err(|e| e.to_string())?;
                        if body.len() > HTTP_BYTES {
                            return Err("request exceeds 2 MiB".into());
                        }
                        let value = if body.is_empty() {
                            Value::Null
                        } else {
                            serde_json::from_slice(&body).map_err(|e| e.to_string())?
                        };
                        let mut file = tempfile::tempfile().map_err(|e| e.to_string())?;
                        {
                            let mut out = BufWriter::new(&mut file);
                            request(engine, o, req.method().as_str(), req.url(), value, &mut out)?;
                            out.flush().map_err(|e| e.to_string())?;
                        }
                        use std::io::{Seek, SeekFrom};
                        file.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
                        Ok(file)
                    })();
                    let header =
                        tiny_http::Header::from_bytes("Content-Type", "application/x-ndjson")
                            .unwrap();
                    match result {
                        Ok(file) => {
                            let _ = req
                                .respond(tiny_http::Response::from_file(file).with_header(header));
                        }
                        Err(error) => {
                            let _ = req.respond(
                                tiny_http::Response::from_string(format!(
                                    "{}\n",
                                    json!({"error":error})
                                ))
                                .with_status_code(400)
                                .with_header(header),
                            );
                        }
                    }
                }
            });
        }
    });
    Ok(())
}
pub fn command(kind: &str, args: &[String]) {
    if args.iter().any(|x| x == "--help" || x == "-h") {
        println!("xsa {kind} --sxi FILE [--reads FA|FQ|GZ | --pattern STRING] [-j N] [--mode auto|dna|text]\n  [--min-len 20] [--ms] [--sample N --seed N] [--plain|--revlines] [--bind 127.0.0.1:7331]\nOutput: deterministic JSONL; coordinates zero-based; MEM qstart in original read.");
        return;
    }
    if let Err(e) = run(kind, args) {
        die(&e);
    }
}
