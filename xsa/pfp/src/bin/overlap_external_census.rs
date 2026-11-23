//! Exact phrase census ONLY. Hash selects a bucket; actual finite bytes decide equality.
use std::{
    fs::{self, File},
    io::{BufWriter, Write},
    path::PathBuf,
};
fn hash(b: &[u8]) -> u64 {
    b.iter().fold(1469598103934665603, |h, &x| {
        (h ^ u64::from(x)).wrapping_mul(1099511628211)
    })
}
fn reads() -> u64 {
    fs::read_to_string("/proc/self/io")
        .unwrap()
        .lines()
        .find_map(|l| l.strip_prefix("read_bytes: "))
        .unwrap()
        .parse()
        .unwrap()
}
fn main() {
    let mut args = std::env::args().skip(1);
    let input = File::open(args.next().unwrap()).unwrap();
    let dir = PathBuf::from(args.next().unwrap());
    fs::create_dir(&dir).unwrap();
    let path = |b| dir.join(format!("p{b:02x}"));
    let n = input.metadata().unwrap().len();
    assert!(n > 0 && n <= 1_010_000_000);
    let t = unsafe { memmap2::Mmap::map(&input).unwrap() };
    t.advise(memmap2::Advice::Sequential).unwrap();
    let mut out: Vec<_> = (0..256)
        .map(|i| BufWriter::new(File::create(path(i)).unwrap()))
        .collect();
    let (mut scratch, mut m, mut longest) = (0u64, 0u64, 0usize);
    let mut sizes = [0usize; 256];
    let mut counts = [0usize; 256];
    let mut record = |raw: &[u8], eof: bool| {
        assert!(raw.len() < 1 << 31);
        let bucket = (hash(raw) >> 56) as usize;
        let add = 4 + raw.len();
        scratch += add as u64;
        sizes[bucket] += add;
        counts[bucket] += 1;
        assert!(
            scratch <= 2 * n && sizes[bucket] <= 64 << 20 && counts[bucket] <= 4_000_000,
            "scratch or bucket RAM guard"
        );
        let hdr = raw.len() as u32 | ((eof as u32) << 31);
        out[bucket].write_all(&hdr.to_le_bytes()).unwrap();
        out[bucket].write_all(raw).unwrap();
        m += 1;
        longest = longest.max(raw.len());
    };
    let mut start = 0;
    for (i, w) in t.windows(8).enumerate().skip(1) {
        if i % 1048576 == 0 {
            assert!(reads() <= 4 * n, "read guard");
        }
        if hash(w) % 31 == 0 {
            record(&t[start..i + 8], false);
            start = i;
        }
    }
    record(&t[start..], true);
    for f in &mut out {
        f.flush().unwrap();
    }
    drop(out);
    drop(t);
    let (mut k, mut reps) = (0u64, 0u64);
    for b in 0..256 {
        assert!(reads() <= 4 * n, "read guard");
        let data = fs::read(path(b)).unwrap();
        assert_eq!(data.len(), sizes[b]);
        let mut ids = Vec::with_capacity(counts[b]);
        let mut i = 0;
        while i < data.len() {
            let hdr = u32::from_le_bytes(data[i..i + 4].try_into().unwrap());
            ids.push((i as u32 + 4, hdr));
            i += 4 + (hdr & 0x7fff_ffff) as usize;
        }
        assert_eq!(ids.len(), counts[b]);
        let key = |(off, h): (u32, u32)| {
            (
                &data[off as usize..off as usize + (h & 0x7fff_ffff) as usize],
                h >> 31 != 0,
            )
        };
        ids.sort_unstable_by(|&a, &b| key(a).cmp(&key(b)));
        let mut prev = None;
        for id in ids {
            let v = key(id);
            if prev != Some(v) {
                k += 1;
                reps += (v.0.len() - if v.1 { 0 } else { 8 }) as u64;
                prev = Some(v);
            }
        }
        fs::remove_file(path(b)).unwrap();
    }
    assert!(reads() <= 4 * n);
    println!("CENSUS n={n} phrases={m} distinct={k} descriptors={reps} longest_raw={longest} counted_bucket_bytes={scratch} max_bucket_bytes={} max_bucket_records={} reads={} private_input_bytes={n}",sizes.iter().max().unwrap(),counts.iter().max().unwrap(),reads());
}
