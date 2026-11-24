// Tests of the actual sink codec. The ignored study receives a certified row
// stream on stdin; it never saves rows, SA, or a full LCP file.
use super::{emit_lcp_slice_mode, Laws};
use std::{
    collections::VecDeque,
    fs,
    io::{self, Read},
    os::unix::fs::MetadataExt,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};
fn file(d: &Path, u: u64) -> PathBuf {
    d.join(format!("band_{u}.lcp"))
}
fn law(d: &Path) -> Laws {
    let p = d.to_str().unwrap();
    Laws::new(
        d.join("stop").display().to_string(),
        0,
        0,
        0,
        p.into(),
        p.into(),
        p.into(),
    )
}
fn write(d: &Path, u: u64, v: &[u64], base: u64, batch: bool, law: &Laws) {
    let mut live = VecDeque::new();
    emit_lcp_slice_mode(
        d.to_str().unwrap(),
        u,
        v.len(),
        v,
        base,
        &mut live,
        u + 1,
        law,
        batch,
    );
    assert_eq!(live[0] + 48, fs::metadata(file(d, u)).unwrap().len());
}
fn verify(blob: &[u8], v: &[u64], base: u64) -> usize {
    let le = |i: usize| u64::from_le_bytes(blob[i..i + 8].try_into().unwrap());
    assert_eq!(&blob[..8], b"SXLS\x01\0\0\0");
    assert_eq!(le(8), v.len() as u64);
    assert_eq!((le(32), le(40)), (base, 1));
    let (mut pos, mut count, mut max) = (48 + v.len(), 0, 0);
    for (i, &h) in v.iter().enumerate() {
        assert_eq!(blob[48 + i], h.min(255) as u8);
        if h >= 255 {
            assert_eq!((le(pos), le(pos + 8)), (base + i as u64, h));
            pos += 16;
            count += 1;
            max = max.max(h);
        }
    }
    assert_eq!((le(16), le(24), pos), (count as u64, max, blob.len()));
    count
}
fn retire(d: &Path, u: u64) {
    fs::remove_file(file(d, u)).unwrap();
    fs::remove_file(d.join(format!("band_{u}.lcp.done"))).unwrap();
}
#[test]
fn escape_publications_are_byte_identical() {
    let d = std::env::temp_dir().join(format!("sxls-batch-{}", std::process::id()));
    fs::create_dir(&d).unwrap();
    let law = law(&d);
    for v in [
        vec![0, 1, 254],
        vec![255],
        vec![0, 255, 256, 1, (1 << 32) + 1, 65535],
    ] {
        let mut control = Vec::new();
        for b in [false, true] {
            write(&d, 0, &v, 1 << 40, b, &law);
            let bytes = fs::read(file(&d, 0)).unwrap();
            verify(&bytes, &v, 1 << 40);
            if b {
                assert_eq!(bytes, control);
            } else {
                control = bytes;
            }
            retire(&d, 0);
        }
    }
    fs::remove_dir(&d).unwrap();
}
fn counters() -> [u64; 3] {
    let s = fs::read_to_string("/proc/self/io").unwrap();
    ["syscw: ", "read_bytes: ", "write_bytes: "].map(|k| {
        s.lines()
            .find_map(|l| l.strip_prefix(k))
            .unwrap()
            .parse()
            .unwrap()
    })
}
#[test]
#[ignore = "explicit tmpfs/cgroup gate; needs the 100 MB row stream on stdin"]
fn paired_live_slices() {
    let root = PathBuf::from(std::env::var_os("XSA_SXLS_LAYOUT_ROOT").unwrap());
    assert!(root.starts_with("/dev/shm") && fs::read_dir(&root).unwrap().next().is_none());
    let dirs = [root.join("scalar"), root.join("batch")];
    for d in &dirs {
        fs::create_dir(d).unwrap();
    }
    let law = law(&root);
    let (mut elapsed, mut ios) = ([Duration::ZERO; 2], [[0u64; 3]; 2]);
    let (mut base, mut u, mut escapes, mut nonempty) = (0, 0, 0usize, 0usize);
    let (mut peak, mut alloc, mut escaped_max) = (0, 0, 0);
    let mut input = io::stdin().lock();
    const ROWS: u64 = 100_000_504;
    let mut wire = vec![0u8; 262144 * 12];
    let mut lcps = Vec::with_capacity(262144);
    while base < ROWS {
        law.stop_check("sxls-pair");
        let take = (ROWS - base).min(262144) as usize;
        let raw = &mut wire[..take * 12];
        input.read_exact(raw).unwrap();
        lcps.clear();
        lcps.extend(
            raw.chunks_exact(12)
                .map(|x| u32::from_le_bytes(x[4..8].try_into().unwrap()) as u64),
        );
        // Alternate execution order in every pair. Timed: generate SXLS,
        // write, sync, rename, done marker. Comparison and retirement outside.
        for j in [0, 1] {
            let b = j ^ (u as usize % 2);
            let before = counters();
            let t = Instant::now();
            write(&dirs[b], u, &lcps, base, b == 1, &law);
            elapsed[b] += t.elapsed();
            for (i, now) in counters().into_iter().enumerate() {
                ios[b][i] += now - before[i];
            }
        }
        let a = fs::read(file(&dirs[0], u)).unwrap();
        let b = fs::read(file(&dirs[1], u)).unwrap();
        assert!(a == b, "SXLS mismatch at slice {u}");
        let e = verify(&a, &lcps, base);
        escapes += e;
        nonempty += usize::from(e > 0);
        escaped_max = escaped_max.max(e);
        peak = peak.max(2 * a.len());
        alloc = alloc.max(
            dirs.iter()
                .map(|d| fs::metadata(file(d, u)).unwrap().blocks() * 512)
                .sum::<u64>(),
        );
        for d in &dirs {
            retire(d, u);
        }
        base += take as u64;
        u += 1;
    }
    assert_eq!(input.read(&mut wire[..1]).unwrap(), 0);
    for d in &dirs {
        fs::remove_dir(d).unwrap();
    }
    eprintln!("SXLS_TRIAL rows={base} slices={u} escapes={escapes} nonempty={nonempty} max_slice_escapes={escaped_max} peak_pair_logical={peak} peak_pair_allocated={alloc} scalar_s={:.6} batch_s={:.6} scalar_syscw={} batch_syscw={} scalar_reads={} batch_reads={} scalar_writes={} batch_writes={} exact_bytes=1",elapsed[0].as_secs_f64(),elapsed[1].as_secs_f64(),ios[0][0],ios[1][0],ios[0][1],ios[1][1],ios[0][2],ios[1][2]);
    // One ordinary write per 8-byte field vs one per nonempty escape
    // column. Allow short writes, but demand the predicted order of saving.
    assert!(
        ios[0][0] >= ios[1][0]
            && ios[0][0] - ios[1][0] >= (2 * escapes - nonempty) as u64 * 99 / 100
    );
}
