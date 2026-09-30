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
            .unwrap_or_else(|| die("SXI: missing member"))
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
        need((&b[..4]==b"SXI1" && version==1) || (&b[..4]==b"SXI2" && (version==2||version==3)), "unsupported version");
        let n = u64at(&b, 8);
        let k = u64at(&b, 16);
        let r = u64at(&b, 24);
        let count = u32at(&b, 32);
        let hs = u32at(&b, 36);
        let flags = u64at(&b, 48);
        need(
            n > 0 && n < u64::MAX && r > 0 && r <= n && k <= n && r <= u32::MAX as u64,
            "invalid n/k/r",
        );
        need(
            (if version==1 {(5..=7).contains(&count)} else {(7..=9).contains(&count)})
                && hs == 64 + 40 * count
                && u64at(&b, 40) == length
                && (flags <= 1 || (8..=15).contains(&flags) || (24..=31).contains(&flags))
                && u32at(&b, 60) == 0,
            "invalid header",
        );
        let expected = u32at(&b, 56);
        b[56..60].fill(0);
        let mut hc = crc(!0, &b);
        let mut members = Vec::new();
        let mut end = hs as u64;
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
            let expected_id=if version==1 {
                if i==5 && count==6 && m.id==7 {7} else {i+1}
            } else if version==2 {
                let optional=count-7;
                if i<5 {i+1} else if i<5+optional {
                    if optional==1 && m.id==7 {7} else {i+1}
                } else {8+i-(5+optional)}
            } else {
                let optional=count-7;
                if i<3 {[1,4,5][i as usize]} else if i<3+optional {
                    if optional==1 && flags&16!=0 {7} else {6+i-3}
                } else {8+i-(3+optional)}
            };
            let expected_codec=if version==3 {match m.id {1=>101,5=>105,8=>118,9=>109,10=>110,11=>111,_=>m.id}}
                else if version==2 {match m.id {1=>101,5=>105,8=>108,9=>109,_=>m.id}} else {m.id};
            need(m.id==expected_id && m.codec==expected_codec && u32at(&d,36)==0,
                "unsupported/duplicate member");
            let aligned = end
                .checked_add(7)
                .unwrap_or_else(|| die("SXI: offset overflow"))
                / 8
                * 8;
            need(
                m.offset == aligned && m.offset <= length && m.bytes <= length - m.offset,
                "overlapping/out-of-bounds member",
            );
            end = m.offset + m.bytes;
            members.push(m);
        }
        need(
            !hc == expected && end == length,
            "header CRC or trailing bytes",
        );
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
                && (version==3 || (c.member(2).count == r
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
            need(c.sigma[30] == 30, "remap moves separator");
        }
        let mut w=0;
        if version!=3 {
            let tail = c.member(2);
            at(&mut f, tail.offset);
            let mut hb = [0; 9];rd(&mut f, &mut hb);
            let bits = u64at(&hb, 0);w = hb[8] as u64;
            need((1..=64).contains(&w) && bits == r * w
                && tail.bytes == 9 + ((bits + 63) / 64) * 8,"packed tail size");
        }
        // Validate every independent member even on the header-only sweep path.
        if version>=2 {
            let (frequencies,heads,lengths)=super::sxi2::runs(path,c.member(1),n,r);
            super::sxi2::Phi::load(path,&c,&frequencies);
            if version==3 {super::sxi2::LfMap::load(path,&c,&heads,&lengths,&frequencies);}
        } else {
            at(&mut f, c.member(1).offset);
            let mut cb = [0; 2048];rd(&mut f,&mut cb);
            let mut chars=BufReader::new(File::open(path).unwrap());at(&mut chars,c.member(1).offset+2048);
            at(&mut f,c.member(1).offset+2048+r);
            let mut totals=[0u64;256];let mut sum=0u64;
            for _ in 0..r {let mut ch=[0];let mut len=[0;4];rd(&mut chars,&mut ch);rd(&mut f,&mut len);
                let len=u32at(&len,0) as u64;need(len>0&&len<=n-sum,"run length");sum+=len;totals[ch[0] as usize]+=len;}
            need(sum==n,"run sum");sum=0;for(i,t)in totals.iter().enumerate(){need(u64at(&cb,8*i)==sum,"C table");sum+=t;}
        }
        if version!=3 {
            at(&mut f, c.member(3).offset);
            let mut x = [0; 8];
            for _ in 0..r {rd(&mut f, &mut x);need(u64at(&x, 0) < n, "head range");}
            // Packed words are streamed with a 128-bit reservoir, not expanded.
            at(&mut f, c.member(2).offset + 9);
            let mut reservoir = 0u128;let mut available = 0u32;
            for _ in 0..r {
                if available < w as u32 {rd(&mut f, &mut x);reservoir |= (u64at(&x, 0) as u128) << available;available += 64;}
                let value = reservoir & ((1u128 << w) - 1);
                need(value < (n as u128), "tail range");reservoir >>= w;available -= w as u32;
            }
        }
        if c.members.iter().any(|m| m.id == 6) {
            let m = c.member(6);
            at(&mut f, m.offset);
            let mut bytes = vec![0; m.bytes as usize]; rd(&mut f, &mut bytes);
            let records = super::product::names(&bytes, n).unwrap_or_else(|e| die(&e));
            need(flags & 8 == 0 || records.len() as u64 == k, "record count mismatch");
            c.k = records.len() as u64;
        }
        c.anchors(path);
        c.write_chi(path, &mut std::io::sink());
        Some(c)
    }
    pub fn anchors(&self, path: &str) -> super::Anchors {
        let mut f = BufReader::new(File::open(path).unwrap());
        at(&mut f, self.member(4).offset);
        let mut b = [0; 12];
        rd(&mut f, &mut b);
        need(
            u32at(&b, 0) == 0x434e4158 && u64at(&b, 4) == self.member(4).count,
            "anchor header",
        );
        let mut rows = Vec::new();
        let mut s = Vec::new();
        for _ in 0..self.member(4).count {
            let mut e = [0; 16];
            rd(&mut f, &mut e);
            let row = u64at(&e, 0);
            let v = u64at(&e, 8);
            need(
                row < self.n && v < self.n && rows.last().map_or(true, |&p| p < row),
                "anchor value/order",
            );
            rows.push(row);
            s.push(v);
        }
        super::Anchors { rows, s }
    }
    pub fn write_chi(&self, path: &str, out: &mut impl Write) {
        let m = self.member(5);
        if self.version>=2 {super::sxi2::chi(path,m,self.n,out);return;}
        let mut f = BufReader::new(File::open(path).unwrap());
        at(&mut f, m.offset);
        let mut used = 0;
        let mut prev = 0u64;
        for i in 0..m.count {
            let mut d = 0u64;
            let mut shift = 0;
            loop {
                need(used < m.bytes, "truncated chi varint");
                let mut b = [0];
                rd(&mut f, &mut b);
                used += 1;
                let payload = (b[0] & 127) as u64;
                need(
                    shift < 64 && (shift < 63 || payload <= 1),
                    "chi varint overflow",
                );
                d |= payload << shift;
                if b[0] & 128 == 0 {
                    need(shift == 0 || payload != 0, "noncanonical chi varint");
                    break;
                }
                shift += 7;
            }
            need(i == 0 || d > 0, "duplicate chi");
            let value = prev
                .checked_add(d)
                .unwrap_or_else(|| die("SXI: chi overflow"));
            need(value <= self.n, "chi range");
            out.write_all(&value.to_le_bytes())
                .unwrap_or_else(|e| die(&format!("write chi: {e}")));
            prev = value;
        }
        need(used == m.bytes, "trailing chi bytes");
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
