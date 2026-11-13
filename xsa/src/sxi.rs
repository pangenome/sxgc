//! SXI1 v1 decoder. Format contract: bit6/SXI_FORMAT.md.
use super::die;
use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom, Write};
fn need(ok: bool, msg: &str) {
    if !ok {
        die(&format!("SXI: {msg}"));
    }
}
fn rd(f: &mut impl Read, b: &mut [u8]) {
    f.read_exact(b)
        .unwrap_or_else(|e| die(&format!("SXI: read: {e}")));
}
fn at(f: &mut (impl Read + Seek), off: u64) {
    f.seek(SeekFrom::Start(off))
        .unwrap_or_else(|e| die(&format!("SXI: seek: {e}")));
}
fn u32at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}
pub(crate) fn header_run_count(b: &[u8]) -> u64 { u64at(b, 24) }
/// IEEE CRC32 of a byte slice (the container's member/header checksum).
pub fn crc32_of(b: &[u8]) -> u32 { !crc(!0, b) }
fn crc(mut v: u32, b: &[u8]) -> u32 {
    static TABLE: std::sync::OnceLock<[u32; 256]> = std::sync::OnceLock::new();
    let table = TABLE.get_or_init(|| {
        let mut t = [0; 256];
        for (i, e) in t.iter_mut().enumerate() {
            let mut x = i as u32;
            for _ in 0..8 {
                x = (x >> 1) ^ (0xedb88320u32.wrapping_mul(x & 1));
            }
            *e = x;
        }
        t
    });
    for &x in b {
        v = table[((v ^ x as u32) & 255) as usize] ^ (v >> 8);
    }
    v
}
#[derive(Clone)]
pub struct Member {
    pub id: u32,
    pub codec: u32,
    pub offset: u64,
    pub bytes: u64,
    pub count: u64,
    pub checksum: u32,
}
pub struct Container {
    pub version: u32,
    pub n: u64,
    pub k: u64,
    pub r: u64,
    pub complete: bool,
    pub flags: u64,
    pub sigma: [u8; 256],
    pub members: Vec<Member>,
}
impl Container {
    pub fn print_members(&self) {
        for m in &self.members {
            println!("member={} count={} bytes={} crc32={:08x}",
                m.id, m.count, m.bytes, m.checksum);
        }
        let m = self.member(5);
        println!("chi_complete={}", self.complete);
        println!("chi_{}_ratio={:.9}",if self.version>=2{"ef"}else{"delta"}, if m.count == 0 { 0.0 }
            else { m.bytes as f64 / (8.0 * m.count as f64) });
    }
    pub fn member(&self, id: u32) -> &Member {
        self.members
            .iter()
            .find(|m| m.id == id)
            .unwrap_or_else(|| die(&format!("SXI: missing member m{id}")))
    }
    pub fn open(path: &str) -> Option<Self> {
        let mut f =
            BufReader::new(File::open(path).unwrap_or_else(|e| die(&format!("open {path}: {e}"))));
        let length = f.get_ref().metadata().unwrap().len();
        let mut b = [0u8; 64];
        rd(&mut f, &mut b[..4]);
        if &b[..4] != b"SXI1" && &b[..4] != b"SXI2" {
            return None;
        }
        rd(&mut f, &mut b[4..]);
        let version = u32at(&b,4);
        // FORWARD-ONLY: the same one-shape gate the query kernel applies. A file
        // that is not SXI2 v10 never becomes a Container, so no caller of this
        // reader can paper over an older shape. The message is the crate's ONE
        // refusal (crate::qlite::refuse), so `stats`, `sxi-info` and every other
        // export path refuse in the same words.
        if &b[..4] != b"SXI2" || version != 10 {
            die(&crate::qlite::refuse(path, &crate::qlite::describe_format(path)));
        }
        // The member directory is a TRAILING section at length-40*count: member
        // bytes are appended in stream order and this is the only layout the
        // supported shape has.
        let tail_dir = true;
        let n = u64at(&b, 8);
        let k = u64at(&b, 16);
        let r = header_run_count(&b);
        let count = u32at(&b, 32);
        let hs = u32at(&b, 36);
        let flags = u64at(&b, 48);
        need(flags & 64 == 0, "missing member m5 (chi): this full validator/export reader requires chi");
        need(
            n > 0 && n < u64::MAX && r > 0 && r <= n && k <= n && r <= (u64::MAX - 2048) / 8,
            "invalid n/k/r",
        );
        // The supported shape's member set: m1, m4, m5, m6|m7, m10, m11, m12,
        // m13, m15, m20 and the optional m14 - 9..=11 directory slots, with the
        // two members the query surface is built on DECLARED by their flags
        // (bit 8 = m20, bit 9 = m15). Anything else is refused, not tolerated.
        need(
            (9..=11).contains(&count) && hs == 64 + 40 * count
                && u64at(&b, 40) == length
                && flags & !(1 | 8 | 16 | 32 | 128 | 256 | 512) == 0
                && flags & 256 != 0 && flags & 512 != 0
                && u32at(&b, 60) == 0,
            "invalid header (not the supported SXI2 v10 shape: m15+m20 declared, 9..=11 members)",
        );
        let expected = u32at(&b, 56);
        b[56..60].fill(0);
        let mut hc = crc(!0, &b);
        let mut members = Vec::new();
        {
            // The 40-byte entries live at the END of the file; the 64-byte header
            // keeps declaring header+directory bytes (hs), but
            // the physical position is derived from the length the header carries.
            let dir_off = u64at(&b, 40).checked_sub(40 * count as u64)
                .unwrap_or_else(|| die("SXI: v7 directory offset underflow"));
            need(dir_off >= 64 && dir_off % 8 == 0, "v7 directory offset");
            at(&mut f, dir_off);
        }
        for i in 0..count {
            let mut d = [0; 40];
            rd(&mut f, &mut d);
            hc = crc(hc, &d);
            let m = Member {
                id: u32at(&d, 0),
                codec: u32at(&d, 4),
                offset: u64at(&d, 8),
                bytes: u64at(&d, 16),
                count: u64at(&d, 24),
                checksum: u32at(&d, 32),
            };
            // THE ONE SHAPE's member order: the directory is SORTED BY ID, so
            // the extension members follow m13 in ID order - 14 (LCP over runs,
            // optional), 15 (document starts), 20 (context trie) - each present
            // iff its flag bit is set (128/512/256). The m6/m7 split is the one
            // remaining optionality and is inferred from the count.
            let expected_id = {
                let ext = u64::from(flags & 128 != 0)
                    + u64::from(flags & 256 != 0)
                    + u64::from(flags & 512 != 0);
                let opt67 = count as u64 - 7 - ext; // 10, 11, 12, 13 are 4 slots
                let mut ids: Vec<u32> = Vec::with_capacity(count as usize);
                ids.extend_from_slice(&[1, 4, 5]);
                if opt67 == 1 {
                    ids.push(if flags & 16 != 0 { 7 } else { 6 });
                } else if opt67 == 2 {
                    ids.extend_from_slice(&[6, 7]);
                }
                ids.extend_from_slice(&[10, 11, 12, 13]);
                if flags & 128 != 0 { ids.push(14); }
                if flags & 512 != 0 { ids.push(15); }
                if flags & 256 != 0 { ids.push(20); }
                need(ids.len() == count as usize, "member count vs flags");
                ids[i as usize]
            };
            // THE ONE SHAPE's codecs. A member whose codec is not this shape's is
            // a different shape, not a variant to be sniffed.
            let expected_codec = match m.id {
                1 => 116,
                5 => 105,
                10 => 110,
                11 => 111,
                12 => 112,
                13 => 115,
                _ => m.id,
            };
            need(m.id==expected_id && m.codec==expected_codec && u32at(&d,36)==0,
                &format!("unsupported/duplicate member: slot {i} has id {} codec {} (expected id {expected_id} codec {expected_codec}), version {version} count {count} flags {flags}",
                    m.id, m.codec));
            need(m.offset <= length && m.bytes <= length - m.offset,
                "overlapping/out-of-bounds member");
            members.push(m);
        }
        need(!hc == expected, "header CRC or trailing bytes");
        // The directory lists members in ID order, which is NOT the order the
        // writer had to place them in: the pile lite writer (bit6/xsa_finish.cpp
        // ContainerOut::finish) emits m5/m4/m7/m10/m11 streaming and m1 last, then
        // sorts the directory by ID. Validate the member set as a partition of
        // [header, file) by PHYSICAL offset instead of chaining the directory.
        {
            let mut spans: Vec<(u64, u64)> = members.iter().map(|m| (m.offset, m.bytes)).collect();
            spans.sort_unstable();
            // v7's trailing directory means the member region starts at the
            // fixed 64-byte header, not at header+directory (the directory is
            // at the END of the file).
            let mut prev_end = if tail_dir { 64 } else { hs as u64 };
            for &(off, bytes) in &spans {
                let aligned = prev_end
                    .checked_add(7)
                    .unwrap_or_else(|| die("SXI: offset overflow"))
                    / 8
                    * 8;
                need(off == aligned, "overlapping/out-of-bounds member");
                prev_end = off
                    .checked_add(bytes)
                    .unwrap_or_else(|| die("SXI: offset overflow"));
            }
            // v7: the member region ends where the trailing directory starts.
            let members_end = if tail_dir { length - 40 * count as u64 } else { length };
            need(prev_end == members_end, "header CRC or trailing bytes");
        }
        let mut c = Self {
            version,
            n,
            k,
            r,
            complete: flags & 1 != 0,
            flags,
            sigma: std::array::from_fn(|i| i as u8),
            members,
        };
        need(
            c.member(1).count == r
                && (version!=1 || c.member(1).bytes == 2048 + 5 * r)
                && (version>=3 || (c.member(2).count == r
                    && c.member(3).count == r && c.member(3).bytes == 8 * r)),
            "run/sample sizes",
        );
        let a = c.member(4);
        need(
            a.count <= k && a.count <= (u64::MAX - 12) / 16 && a.bytes == 12 + 16 * a.count,
            "anchor size",
        );
        let chi = c.member(5);
        need(
            chi.count <= n + 1
                && chi.count <= u64::MAX / 10
                && (version!=1 || (chi.bytes >= chi.count && chi.bytes <= 10 * chi.count)),
            "chi size",
        );
        need(
            c.complete || (chi.count == 0 && chi.bytes == 0),
            "unfinished chi",
        );
        if c.members.iter().any(|m| m.id == 6) {
            need(c.member(6).count == c.member(6).bytes, "names size");
        }
        let mut buf = vec![0; 1 << 20];
        let t_crc = std::time::Instant::now();
        for m in &c.members {
            at(&mut f, m.offset);
            let mut left = m.bytes;
            let mut v = !0;
            while left > 0 {
                let z = left.min(buf.len() as u64) as usize;
                rd(&mut f, &mut buf[..z]);
                v = crc(v, &buf[..z]);
                left -= z as u64;
            }
            need(!v == m.checksum, "member CRC mismatch");
        }
        super::trace_since("sxi.member-crc", t_crc);
        let remapped = c.members.iter().any(|m|m.id==7);
        need(remapped == (flags & 16 != 0), "remap flag/member mismatch");
        if remapped {
            let m = c.member(7);
            need(m.count == 256 && m.bytes == 256, "remap size");
            at(&mut f, m.offset);
            rd(&mut f, &mut c.sigma);
            let mut seen = [false; 256];
            for &v in &c.sigma {
                need(!seen[v as usize], "remap is not a permutation");
                seen[v as usize] = true;
            }
            // NO separator-identity check here. `sigma[0x1E] == 0x1E` is a legacy
            // CHUNK-FRONTEND contract: it kept the record separator on its own
            // symbol so that collection order coincided with byte order. The
            // END-format (:ends_in_sentinel) contract is the opposite - present
            // bytes must map to 1,2,3,... in increasing byte order, which
            // necessarily moves 0x1E. The alphabet is validated where it is
            // built (the finish asserts present -> 1..k, order preserving, and a
            // bijection); the reader decodes, it does not re-legislate it.
        }
        let mut w=0;
        if version<3 {
            let tail = c.member(2);
            at(&mut f, tail.offset);
            let mut hb = [0; 9];rd(&mut f, &mut hb);
            let bits = u64at(&hb, 0);w = hb[8] as u64;
            need((1..=64).contains(&w) && r <= u64::MAX / w && bits == r * w && bits <= u64::MAX - 63
                && tail.bytes == 9 + ((bits + 63) / 64) * 8,"packed tail size");
        }
        // The phi-era member validators (the run decode, Phi, the LF starts map,
        // the packed head/tail samples and the names record count) are GONE: they
        // decoded the whole run stream on every open (385 s on a 10 GB container,
        // tools/binbwt/RESULTS_QUERY_PROFILE.md) and none of them describes the
        // supported shape. What remains below is what the shape actually has: the
        // chi set (m5) decoded once as the law check, and the member CRCs above.
        let t_chi=std::time::Instant::now();
        c.write_chi(path, &mut std::io::sink());
        super::trace_since("sxi.chi-decode",t_chi);
        Some(c)
    }
    /// Export m5 (chi) as the u64 LE stream. One codec: the supported shape's
    /// EF-encoded set. The delta-varint arm belonged to the shapes before it.
    pub fn write_chi(&self, path: &str, out: &mut impl Write) {
        super::sxi2::chi(path, self.member(5), self.n, out);
    }
}
pub fn command(args: &[String]) {
    if args.len() != 1 && args.len() != 3 {
        die("usage: xsa sxi-info FILE [--chi-out FILE]");
    }
    let c = Container::open(&args[0]).unwrap_or_else(|| die("expected SXI1"));
        println!(
        "SXI{} version={} n={} k={} runs={} chi_complete={}",
        if c.version==1 {1}else{2},c.version,c.n, c.k, c.r, c.complete
    );
    c.print_members();
    if args.len() == 3 {
        need(args[1] == "--chi-out", "unknown option");
        need(c.complete, "chi not constructed");
        let f = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&args[2])
            .unwrap_or_else(|e| die(&format!("create chi: {e}")));
        let mut out = std::io::BufWriter::new(f);
        c.write_chi(&args[0], &mut out);
        out.flush().unwrap();
    }
}
