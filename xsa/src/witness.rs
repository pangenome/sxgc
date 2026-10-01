//! A two-slot-per-run witness bitmap. Slot 2r is the head of run r and
//! slot 2r+1 its tail. The bitmap is tied to the chi and phi member CRCs.
use super::{sxi, sxi2};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::Path;

const MAGIC: &[u8; 8] = b"SXIWIT1\0";
const HEADER: usize = 64;

fn bit(b: &[u8], i: u64) -> bool { b[(i / 8) as usize] & (1 << (i % 8)) != 0 }
fn set(b: &mut [u8], i: u64) { b[(i / 8) as usize] |= 1 << (i % 8); }
fn clear(b: &mut [u8], i: u64) { b[(i / 8) as usize] &= !(1 << (i % 8)); }

pub struct Witness {
    bits: Vec<u8>,
    // Number of one bits before each 512-bit block.
    rank: Vec<u64>,
    next_block: Vec<u64>,
    count: u64,
    slots: u64,
}
impl Witness {
    pub fn load(path: &str, c: &sxi::Container) -> Result<Self, String> {
        let mut f = File::open(path).map_err(|e| e.to_string())?;
        let mut h = [0; HEADER];
        f.read_exact(&mut h).map_err(|e| e.to_string())?;
        let get = |at: usize| u64::from_le_bytes(h[at..at+8].try_into().unwrap());
        if &h[..8] != MAGIC || get(8) != c.n || get(16) != c.r
            || get(24) != c.member(5).count || get(32) != c.member(5).checksum as u64
            || get(40) != c.member(8).checksum as u64 || get(56) != 0 {
            return Err("witness index does not match SXI2 artifact".into());
        }
        let slots = c.r.checked_mul(2).ok_or("witness slots overflow")?;
        let bytes = ((slots + 7) / 8) as usize;
        if f.metadata().map_err(|e| e.to_string())?.len() != HEADER as u64 + bytes as u64 {
            return Err("invalid witness index length".into());
        }
        let mut bits = vec![0; bytes];
        f.read_exact(&mut bits).map_err(|e| e.to_string())?;
        if slots % 8 != 0 && bits.last().unwrap() >> (slots % 8) != 0 {
            return Err("nonzero witness padding".into());
        }
        let mut rank = Vec::with_capacity((bytes + 63) / 64 + 1);
        rank.push(0);
        let mut count = 0;
        for block in bits.chunks(64) {
            count += block.iter().map(|v| v.count_ones() as u64).sum::<u64>();
            rank.push(count);
        }
        if count != get(48) || count != c.member(5).count {
            return Err("witness count differs from chi".into());
        }
        let blocks = rank.len() - 1;
        let mut next_block = vec![u64::MAX; blocks + 1];
        for block in (0..blocks).rev() {
            next_block[block] = if rank[block + 1] > rank[block] {
                block as u64
            } else { next_block[block + 1] };
        }
        Ok(Self {bits, rank, next_block, count, slots})
    }
    fn rank(&self, slot: u64) -> u64 {
        let byte = (slot / 8) as usize;
        let block = byte / 64;
        let mut result = self.rank[block];
        for &v in &self.bits[block*64..byte] { result += v.count_ones() as u64; }
        if slot % 8 != 0 && byte < self.bits.len() {
            result += (self.bits[byte] & ((1 << (slot % 8)) - 1)).count_ones() as u64;
        }
        result
    }
    pub fn first_at_or_after(&self, slot: u64) -> Option<u64> {
        if slot >= self.slots || self.rank(slot) == self.count { return None; }
        let first_byte = (slot / 8) as usize;
        let block = first_byte / 64;
        let mut word = self.bits[first_byte] & (u8::MAX << (slot % 8));
        if word != 0 { return Some(first_byte as u64 * 8 + word.trailing_zeros() as u64); }
        for byte in first_byte + 1..((block + 1) * 64).min(self.bits.len()) {
            word = self.bits[byte];
            if word != 0 { return Some(byte as u64 * 8 + word.trailing_zeros() as u64); }
        }
        let next = *self.next_block.get(block + 1)?;
        if next == u64::MAX { return None; }
        for byte in next as usize * 64..((next as usize + 1) * 64).min(self.bits.len()) {
            word = self.bits[byte];
            if word != 0 { return Some(byte as u64 * 8 + word.trailing_zeros() as u64); }
        }
        None
    }
}

struct ChiBits { bits: Vec<u8>, count: u64, n: u64 }
impl Write for ChiBits {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        if buf.len() != 8 { return Err(std::io::Error::other("chi decode width")); }
        let x = u64::from_le_bytes(buf.try_into().unwrap());
        if x > self.n || bit(&self.bits, x) {
            return Err(std::io::Error::other("duplicate/out-of-range chi"));
        }
        set(&mut self.bits, x); self.count += 1; Ok(8)
    }
    fn flush(&mut self) -> std::io::Result<()> { Ok(()) }
}

/// Read only the retained SXI2. The new sidecar is written under `output`.
pub fn build(path: &str, output: &str, c: &sxi::Container, phi: &sxi2::Phi) -> Result<(), String> {
    if c.version < 3 || !c.complete { return Err("need complete compact SXI2".into()); }
    if Path::new(output).exists() { return Err("witness output already exists".into()); }
    let mut chi = ChiBits { bits: vec![0; ((c.n + 8) / 8) as usize], count: 0, n: c.n };
    sxi2::chi(path, c.member(5), c.n, &mut chi);
    if chi.count != c.member(5).count { return Err("chi count mismatch".into()); }
    let slots = c.r.checked_mul(2).ok_or("witness slots overflow")?;
    let mut bits = vec![0; ((slots + 7) / 8) as usize];
    let mut mapped = 0u64;
    phi.for_each_edge(|u, v, run| {
        for (sa, slot) in [(u, 2*run+1), (v, 2*((run+1)%c.r))] {
            // Artifact chi uses cyclic end coordinates: SA 0 maps to chi 0.
            let x = if sa == 0 { 0 } else { c.n - sa };
            if bit(&chi.bits, x) {
                clear(&mut chi.bits, x);
                set(&mut bits, slot);
                mapped += 1;
            }
        }
    });
    if mapped != chi.count { return Err(format!("{}/{} chi positions mapped to BWT edges", mapped, chi.count)); }
    let mut h = [0u8; HEADER];
    h[..8].copy_from_slice(MAGIC);
    for (at, value) in [(8,c.n),(16,c.r),(24,chi.count),
        (32,c.member(5).checksum as u64),(40,c.member(8).checksum as u64),(48,mapped)] {
        h[at..at+8].copy_from_slice(&value.to_le_bytes());
    }
    let temporary = format!("{output}.tmp-{}", std::process::id());
    let result = (|| -> Result<(), String> {
        let mut out = OpenOptions::new().write(true).create_new(true).open(&temporary).map_err(|e| e.to_string())?;
        out.write_all(&h).and_then(|_| out.write_all(&bits)).and_then(|_| out.sync_all())
            .map_err(|e| e.to_string())?;
        fs::rename(&temporary, output).map_err(|e| e.to_string())
    })();
    if result.is_err() { let _ = fs::remove_file(&temporary); }
    result
}
