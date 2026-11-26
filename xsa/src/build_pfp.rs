//! `build-pfp`: fresh construction. Rows flow through bounded Unix pipes;
//! the only bytes following the rows are the two metadata members.
use crate::{ctx, die, docstarts};
use std::fs::File;
use std::io::{self, Read, Write};
use std::os::unix::process::CommandExt;
use std::process::Command;

pub fn command(args: &[std::ffi::OsString]) {
    // The normal installed xsa bundle, not a checkout or a study's /tmp tools.
    let exe = std::env::current_exe().unwrap_or_else(|e| die(&e.to_string()));
    let dir = crate::bundle::resolve().unwrap_or_else(|e| die(&e));
    let err = Command::new("python3")
        .arg("-I")
        .arg(dir.join("tools/build_pfp.py"))
        .arg(&dir)
        .arg(exe)
        .args(args)
        .exec();
    die(&format!("build-pfp: exec python3: {err}"));
}

// One coarse entry per 16 KiB keeps the random doc lookup to a small local
// bisect. Absolute starts are authoritative; never interpret a byte as a cut.
struct Docs {
    starts: Vec<u64>, // includes the exclusive text end
    pages: Vec<u32>,
}
impl Docs {
    const PAGE: usize = 16384;
    fn load(path: &str, n: usize) -> Self {
        let source = docstarts::Source::DocsMap(path.to_owned());
        let mut starts: Vec<_> = docstarts::StartsIter::open(&source, Some(n as u64)).collect();
        if starts.first() != Some(&0) {
            die("document map must start at byte zero");
        }
        let mut pages = Vec::with_capacity(n.div_ceil(Self::PAGE) + 1);
        let mut doc = 0;
        for base in (0..n).step_by(Self::PAGE) {
            while doc + 1 < starts.len() && starts[doc + 1] <= base as u64 {
                doc += 1;
            }
            pages.push(doc as u32);
        }
        pages.push((starts.len() - 1) as u32);
        starts.push(n as u64);
        Self { starts, pages }
    }
    fn at(&self, p: usize) -> (u32, u64) {
        let page = p / Self::PAGE;
        let (lo, hi) = (self.pages[page] as usize, self.pages[page + 1] as usize);
        let doc = lo + self.starts[lo + 1..hi + 1].partition_point(|&s| s <= p as u64);
        (doc as u32, self.starts[doc + 1] - p as u64)
    }
}

pub fn context(raw: &str, map: &str) -> io::Result<()> {
    let file = File::open(raw)?;
    let text = unsafe { memmap2::Mmap::map(&file)? }; // writer has closed the corpus
    let n = text.len();
    let docs = Docs::load(map, n);
    let mut trie = ctx::Builder::new(8, 1000, 1000);
    trie.set_source(ctx::SOURCE_IN_BUILD, 0);
    trie.set_boundary_aware();
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    let mut packet = vec![0u8; 32768 * 12];
    let mut left = n + 1;
    while left > 0 {
        let take = left.min(packet.len() / 12);
        let bytes = &mut packet[..12 * take];
        input.read_exact(bytes)?;
        for r in bytes.chunks_exact(12) {
            let tag = u32::from_le_bytes(r[8..12].try_into().unwrap());
            let p = u32::from_le_bytes(r[..4].try_into().unwrap()) as usize
                | (((tag as usize >> 9) & 2047) << 32);
            if p > n {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "row position exceeds corpus",
                ));
            }
            let rem = n - p;
            let (doc, avail) = if rem == 0 { (0, u64::MAX) } else { docs.at(p) };
            trie.feed(&text[p..p + rem.min(9)], rem as u64, avail, doc);
        }
        output.write_all(bytes)?;
        left -= take;
    }
    if input.read(&mut [0])? != 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "extra ordered rows",
        ));
    }
    let (payload, _) = trie.finish(n as u64 + 1, n as u64);
    output.write_all(&(payload.len() as u64).to_le_bytes())?;
    output.write_all(&payload)?;
    drop(payload);
    let count = docs.starts.len() - 1;
    let m15 = docstarts::encode(
        &mut docs.starts[..count].iter().copied(),
        n as u64,
        docstarts::SAMPLE_STRIDE,
        Some(count as u64),
    );
    output.write_all(&m15.docs.to_le_bytes())?;
    output.write_all(&(m15.bytes.len() as u64).to_le_bytes())?;
    output.write_all(&m15.bytes)?;
    output.flush()
}
