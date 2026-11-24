//! Fixed 100 MB v8 only. Hold one checked u16 per LCP, not both a u64
//! slice and a geometrically growing escape list. The frozen reader needs the
//! u8 markers BEFORE the rank/value escape pairs. Stream those two sections
//! through the same page. No file of rows and no change to SXLS v1.
use super::v8_piece_pool::{Files, ROWS, SLICE_EXTENT, SLICE_ROWS as S, SLICE_SLOTS};
use std::fs::{self, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::{AsFd, AsRawFd};
use std::os::unix::fs::{FileTypeExt, MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::time::Duration;

const PAGE: usize = 65_536;
const WINDOW: u64 = SLICE_SLOTS as u64;
const HEADER: u64 = 48;
#[derive(Clone, Copy)]
struct Shape {
    rows: u64,
    escapes: u64,
    high: u16,
    extent: u64,
}
const REAL: Shape = Shape {
    rows: ROWS,
    escapes: 1_129_147,
    high: 34_067,
    extent: SLICE_EXTENT,
};
fn need(ok: bool, msg: &'static str) -> io::Result<()> {
    if ok {
        Ok(())
    } else {
        Err(io::Error::other(msg))
    }
}
struct Packed {
    v: Box<[u16]>,
    page: Box<[u8]>,
    n: usize,
    escapes: usize,
    max: u16,
}
impl Packed {
    fn new() -> Self {
        Self {
            v: vec![0; S].into_boxed_slice(),
            page: vec![0; PAGE].into_boxed_slice(),
            n: 0,
            escapes: 0,
            max: 0,
        }
    }
    fn size(&self) -> u64 {
        HEADER + self.n as u64 + 16 * self.escapes as u64
    }
    fn push(&mut self, h: u64, shape: Shape) -> io::Result<()> {
        // Test BEFORE narrowing and before extending even an unfinished slice.
        need(
            self.n < S && h <= u64::from(shape.high),
            "fixed SXLS row/LCP bound",
        )?;
        let esc = usize::from(h >= 255);
        need(
            self.size() + 1 + 16 * esc as u64 <= shape.extent,
            "fixed SXLS extent",
        )?;
        self.v[self.n] = h as u16;
        self.n += 1;
        self.escapes += esc;
        self.max = self.max.max(h as u16);
        Ok(())
    }
    fn encode(&mut self, out: &mut dyn Write, base: u64) -> io::Result<()> {
        need(
            self.n > 0 && base <= ROWS - self.n as u64,
            "fixed SXLS nonempty rank span",
        )?;
        let mut head = [0u8; 48];
        head[..8].copy_from_slice(b"SXLS\x01\x00\x00\x00");
        for (dst, x) in head[8..].chunks_exact_mut(8).zip([
            self.n as u64,
            self.escapes as u64,
            if self.escapes == 0 {
                0
            } else {
                u64::from(self.max)
            },
            base,
            1,
        ]) {
            dst.copy_from_slice(&x.to_le_bytes());
        }
        out.write_all(&head)?;
        for part in self.v[..self.n].chunks(PAGE) {
            for (dst, &h) in self.page.iter_mut().zip(part) {
                *dst = h.min(255) as u8;
            }
            out.write_all(&self.page[..part.len()])?;
        }
        let mut used = 0;
        if self.escapes != 0 {
            for (i, &h) in self.v[..self.n].iter().enumerate() {
                if h < 255 {
                    continue;
                }
                self.page[used..used + 8].copy_from_slice(&(base + i as u64).to_le_bytes());
                self.page[used + 8..used + 16].copy_from_slice(&u64::from(h).to_le_bytes());
                used += 16;
                if used == PAGE {
                    out.write_all(&self.page)?;
                    used = 0;
                }
            }
            if used > 0 {
                out.write_all(&self.page[..used])?;
            }
        }
        Ok(())
    }
    fn clear(&mut self) {
        self.n = 0;
        self.escapes = 0;
        self.max = 0;
    }
}
fn absent(path: &Path) -> io::Result<bool> {
    match fs::symlink_metadata(path) {
        Ok(_) => Ok(false),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(true),
        Err(e) => Err(e),
    }
}
fn empty_marker(path: &Path) -> io::Result<()> {
    let st = fs::symlink_metadata(path)?;
    need(
        st.is_file() && st.nlink() == 1 && st.len() == 0,
        "fixed SXLS marker identity",
    )
}
pub(crate) struct Slices {
    shape: Shape,
    dir: PathBuf,
    pool: Files,
    buf: Packed,
    seq: u64,
    base: u64,
    escapes: u64,
    peak: u64,
    high: u16,
}
impl Slices {
    pub(crate) fn fixed(dir: &Path) -> io::Result<Self> {
        Self::new(dir, REAL)
    }
    fn new(dir: &Path, shape: Shape) -> io::Result<Self> {
        need(
            shape.rows > 0
                && shape.rows <= ROWS
                && shape.extent >= HEADER + 1
                && shape.extent <= REAL.extent
                && shape.high <= REAL.high
                && shape.escapes <= shape.rows,
            "fixed SXLS dimensions",
        )?;
        need(
            fs::symlink_metadata(dir)?.is_dir() && dir.read_dir()?.next().is_none(),
            "fixed SXLS requires fresh empty bands",
        )?;
        let pool = Files::slices(dir, shape.rows.div_ceil(S as u64))?;
        Ok(Self {
            shape,
            dir: dir.to_owned(),
            pool,
            buf: Packed::new(),
            seq: 0,
            base: 0,
            escapes: 0,
            peak: 0,
            high: 0,
        })
    }
    pub(crate) fn push(&mut self, h: u64, tick: impl Fn() -> io::Result<()>) -> io::Result<()> {
        need(
            self.base + (self.buf.n as u64) < self.shape.rows
                && self.escapes + u64::from(h >= 255) <= self.shape.escapes,
            "fixed SXLS row/escape count",
        )?;
        self.buf.push(h, self.shape)?;
        self.escapes += u64::from(h >= 255);
        self.high = self.high.max(h as u16);
        // Withhold the last image (including an exact full one) until finish().
        // finish() runs AFTER verified input EOF and BEFORE cutter.finish/BPT1.
        if self.buf.n == S && self.base + (S as u64) < self.shape.rows {
            self.publish(tick)?;
        }
        Ok(())
    }
    fn publish(&mut self, tick: impl Fn() -> io::Result<()>) -> io::Result<()> {
        tick()?;
        let marker = |seq, suffix: &str| self.dir.join(format!("band_{seq}.lcp{suffix}"));
        if self.seq >= WINDOW {
            let old = self.seq - WINDOW;
            let consumed = marker(old, ".consumed");
            while absent(&consumed)? {
                tick()?;
                std::thread::sleep(Duration::from_millis(10));
            }
            empty_marker(&consumed)?;
            empty_marker(&marker(old, ".done"))?;
            // Read credit alone is insufficient. The Files pool ALSO tests its
            // held FD/private name and nlink==1 before truncating the previous slot.
            need(
                absent(&marker(old, ""))?,
                "fixed SXLS credit before payload unlink",
            )?;
        }
        let done = marker(self.seq, ".done");
        need(
            absent(&done)? && absent(&marker(self.seq, ".consumed"))?,
            "fixed SXLS publication exists",
        )?;
        let size = self.buf.size();
        let base = self.base;
        self.pool
            .publish(self.seq, size, |f| self.buf.encode(f, base))?;
        // Same read barrier as before. At this point the canonical hard link
        // names the complete fsynced v1 image. Never replace an old marker.
        drop(
            OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(done)?,
        );
        self.peak = self.peak.max(size);
        self.seq += 1;
        self.base += self.buf.n as u64;
        self.buf.clear();
        Ok(())
    }
    pub(crate) fn finish(mut self, tick: impl Fn() -> io::Result<()>) -> io::Result<()> {
        need(
            self.buf.n > 0
                && self.base + self.buf.n as u64 == self.shape.rows
                && self.escapes == self.shape.escapes
                && self.high == self.shape.high
                && self.peak.max(self.buf.size()) == self.shape.extent,
            "fixed SXLS final geometry",
        )?;
        self.publish(tick)?;
        self.pool.retire()?;
        eprintln!("SS_V8_PACKED_SXLS rows={} slices={} escapes={} max_lcp={} peak_extent={} codec_array_bytes={}",self.base,self.seq,self.escapes,self.high,self.peak,2*S+PAGE);
        Ok(())
    }
}

// Linux pipe ABI (as in overlap_audit.rs). Only the fixed private sink owns
// this read end. Check STOP even with a writer stalled before EOF.
#[repr(C)]
struct PollFd {
    fd: i32,
    events: i16,
    revents: i16,
}
extern "C" {
    fn fcntl(fd: i32, cmd: i32, ...) -> i32;
    fn poll(p: *mut PollFd, n: usize, timeout: i32) -> i32;
}
const PACKET: usize = 8192;
fn pipe() -> io::Result<fs::File> {
    let f = fs::File::from(std::io::stdin().as_fd().try_clone_to_owned()?);
    let fd = f.as_raw_fd();
    need(
        f.metadata()?.file_type().is_fifo(),
        "fixed SXLS requires a narrow pipe",
    )?;
    // F_GETFL, F_GETPIPE_SZ, F_SETFL and O_NONBLOCK for this Linux x86-64 gate.
    let flags = unsafe { fcntl(fd, 3) };
    let size = unsafe { fcntl(fd, 1032) };
    need(
        flags >= 0
            && (1..=512 * 1024).contains(&size)
            && unsafe { fcntl(fd, 4, flags | 0o4000) } == 0,
        "fixed SXLS pipe size/flags",
    )?;
    Ok(f)
}
fn receive(f: &mut fs::File, mut dst: &mut [u8], laws: &crate::laws::Laws) -> io::Result<usize> {
    let wanted = dst.len();
    let mut p = PollFd {
        fd: f.as_raw_fd(),
        events: 1,
        revents: 0,
    };
    while !dst.is_empty() {
        laws.stop_check("fixed-sxls-input");
        match f.read(dst) {
            Ok(0) => break,
            Ok(n) => dst = &mut dst[n..],
            Err(e) if e.kind() == io::ErrorKind::Interrupted => (),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                if unsafe { poll(&mut p, 1, 50) } < 0 {
                    let e = io::Error::last_os_error();
                    if e.kind() != io::ErrorKind::Interrupted {
                        return Err(e);
                    }
                }
            }
            Err(e) => return Err(e),
        }
    }
    Ok(wanted - dst.len())
}
pub(crate) fn narrow(
    dir: &Path,
    file_n: u64,
    remap: &[u8],
    mut cutter: super::PieceCutter,
    laws: &crate::laws::Laws,
) -> io::Result<()> {
    need(
        file_n == ROWS - 1
            && remap.len() == 256
            && cutter.pool.is_some()
            && laws.scratch_budget == 2 * file_n,
        "fixed SXLS profile",
    )?;
    let mut f = pipe()?;
    let mut slices = Slices::fixed(dir)?;
    let mut packet = vec![0; 12 * PACKET].into_boxed_slice();
    let (mut rank, mut ends) = (0, 0);
    let tick = || {
        laws.stop_check("fixed-sxls-credit");
        Ok(())
    };
    while rank < ROWS {
        let bytes = 12 * PACKET.min((ROWS - rank) as usize);
        need(
            receive(&mut f, &mut packet[..bytes], laws)? == bytes,
            "truncated fixed SXLS narrow input",
        )?;
        for row in packet[..bytes].chunks_exact(12) {
            let col = |i| u64::from(u32::from_le_bytes(row[i..i + 4].try_into().unwrap()));
            let (p, h, b) = (col(0), col(4), col(8));
            need(
                p <= file_n
                    && (rank == 0) == (p == file_n)
                    && h <= file_n - p
                    && h <= u64::from(REAL.high),
                "fixed SXLS narrow position/LCP",
            )?;
            let sym = if p == 0 {
                need(b == 256 && ends == 0, "fixed SXLS END")?;
                ends += 1;
                0
            } else {
                need(b < 256 && remap[b as usize] != 0, "fixed SXLS predecessor")?;
                remap[b as usize]
            };
            cutter.feed_row(p, sym);
            slices.push(h, tick)?;
            rank += 1;
            if rank % S as u64 == 0 {
                laws.scratch_gate("fixed-sxls-partial-flat-sample");
            }
        }
    }
    need(
        receive(&mut f, &mut packet[..1], laws)? == 0 && ends == 1,
        "fixed SXLS EOF/END",
    )?;
    slices.finish(tick)?;
    cutter.finish(); // retire private FDs/names of BOTH pools before BPT1
    fs::write(dir.join("producer.done"), b"")?;
    laws.scratch_gate("fixed-sxls-done-partial-flat-sample");
    Ok(())
}

#[cfg(test)]
#[path = "v8_sxls_test.rs"]
mod tests;
