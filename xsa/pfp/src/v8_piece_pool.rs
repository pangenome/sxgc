//! Fixed-rung v8 handoff. A read credit is not a deletion credit. Keep one
//! private name/FD per slot and link the fsynced image under its PRC1 name.
//! Only the existing finish removes that link, after closing its reader and
//! making the covering m1 bytes durable. No overwrite, spill, retry or resume.
use std::fs::{self, DirBuilder, File, OpenOptions};
use std::io::{self, Seek, Write};
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};

pub const ROWS: u64 = 100_000_504;
pub const RUNS: u64 = 38_647_521;
pub const SLOTS: usize = 78;
pub const BAND: u64 = 65_536;
pub const FULL: u64 = 770_092;
pub fn piece_bytes(width: u32, count: u64) -> io::Result<u64> {
    check((1..64).contains(&width) && (1..=BAND).contains(&count), "PRC1 dimensions")?;
    Ok(44 + 5 * count + 16 * (count * u64::from(width)).div_ceil(64))
}
fn check(ok: bool, why: &'static str) -> io::Result<()> {
    if ok { Ok(()) } else { Err(io::Error::other(why)) }
}
struct Slot { name: PathBuf, fd: File, extent: u64 }
impl Slot {
    fn identity(&self) -> io::Result<u64> {
        let fd = self.fd.metadata()?;
        let name = fs::symlink_metadata(&self.name)?;
        check(name.is_file() && name.dev() == fd.dev() && name.ino() == fd.ino()
              && fd.len() == self.extent, "private PRC1 slot identity or extent")?;
        Ok(fd.nlink())
    }
}
/// The serializer cannot seek, expose the FD, or extend even a partial image
/// past its declared wire extent. On error the owner preserves partial evidence.
struct Limited<'a> { fd: &'a mut File, left: u64 }
impl Write for Limited<'_> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        check(buf.len() as u64 <= self.left, "PRC1 write exceeds reserved extent")?;
        let n = self.fd.write(buf)?; self.left -= n as u64; Ok(n)
    }
    fn flush(&mut self) -> io::Result<()> { self.fd.flush() }
}
pub struct Pool {
    dir: PathBuf, slots: Vec<Slot>, count: usize, cap: u64, limit: u64, pub(super) runs: u64,
    next: u64, poisoned: bool,
}
impl Pool {
    pub fn fixed(dir: &Path) -> io::Result<Self> {
        check(piece_bytes(27, BAND)? == FULL && RUNS.div_ceil(BAND) == 590, "fixed PRC1 dimensions")?;
        Self::new(dir, SLOTS, FULL, RUNS, BAND)
    }
    pub(super) fn new(dir: &Path, count: usize, cap: u64, runs: u64, band: u64) -> io::Result<Self> {
        check(count > 0 && count <= SLOTS && cap > 0 && cap <= FULL && runs > 0 && band > 0, "PRC1 pool dimensions")?;
        let limit = runs.div_ceil(band);
        let dir = dir.join("payload-slots");
        DirBuilder::new().mode(0o700).create(&dir)?; // refuse pre-existing private names
        Ok(Self { dir, slots: Vec::with_capacity(count), count, cap, limit, runs, next: 0, poisoned: false })
    }
    pub fn publish(&mut self, seq: u64, bytes: u64, body: impl FnOnce(&mut dyn Write) -> io::Result<()>) -> io::Result<()> {
        check(!self.poisoned, "failed PRC1 pool cannot resume")?; self.poisoned = true;
        check(seq == self.next && seq < self.limit && bytes > 0 && bytes <= self.cap, "PRC1 image reservation")?;
        let target = self.dir.parent().unwrap().join(format!("p-{seq}-0"));
        match fs::symlink_metadata(&target) {
            Err(e) if e.kind() == io::ErrorKind::NotFound => (),
            Err(e) => return Err(e), Ok(_) => return Err(io::Error::other("PRC1 publication exists")),
        }
        let idx = (seq % self.count as u64) as usize;
        if idx == self.slots.len() {
            let name = self.dir.join(idx.to_string());
            let fd = OpenOptions::new().write(true).create_new(true).mode(0o600).open(&name)?;
            self.slots.push(Slot { name, fd, extent: 0 });
        }
        let s = &mut self.slots[idx];
        // In particular, a .runcols.consumed marker cannot satisfy this test.
        check(s.identity()? == 1, "PRC1 slot still linked; refuse before truncate")?;
        s.fd.set_len(0)?; s.fd.rewind()?;
        let mut writer = Limited { fd: &mut s.fd, left: bytes };
        body(&mut writer)?; check(writer.left == 0, "short PRC1 image")?;
        s.extent = bytes; s.fd.sync_all()?;
        check(s.identity()? == 1, "unexpected PRC1 slot link before publish")?;
        fs::hard_link(&s.name, target)?; // no replacement; finish opens only after BPG1 publication
        self.next += 1; self.poisoned = false; Ok(())
    }
    /// Remove *only our slot names* and drop *all* writer FDs BEFORE terminal.
    /// Unconsumed published names (possibly including the last short piece)
    /// stay intact for the finish. Never unlink a published PRC1 here.
    pub fn retire(self) -> io::Result<()> {
        check(!self.poisoned && self.next == self.limit, "incomplete PRC1 pool")?;
        for s in &self.slots { check((1..=2).contains(&s.identity()?), "extra PRC1 links")?; }
        for s in self.slots { fs::remove_file(&s.name)?; drop(s); }
        fs::remove_dir(self.dir)?;
        eprintln!("SS_V8_PIECE_POOL_RETIRED slots={} per_slot={} capacity={} before_terminal=1", self.count, self.cap, self.count as u64 * self.cap);
        Ok(())
    }
}
