//! Component-only study on the producer's in-memory dictionary. No row transport.
use super::{counter, index::DictSort, io, stop, Suffixes};
use sha2::{Digest, Sha256};
use std::{fs, path::Path, time::Instant};

fn process_cpu_s() -> f64 {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    assert_eq!(
        unsafe { libc::clock_gettime(libc::CLOCK_PROCESS_CPUTIME_ID, &mut ts) },
        0
    );
    ts.tv_sec as f64 + ts.tv_nsec as f64 * 1e-9
}

pub fn run(dict: &[u8], flag: &Path) {
    let mut previous = None;
    for requested in [1, 4, 2] {
        stop(flag);
        if std::env::var_os("XSA_GATE_PACKED_10G").is_some() {
            use std::time::{SystemTime, UNIX_EPOCH};
            let deadline: u64 = std::env::var("XSA_GRID_DEADLINE").unwrap().parse().unwrap();
            let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs();
            if deadline.saturating_sub(now) < 1200 {
                fs::write(flag, b"insufficient time for another native dictionary sort\n").unwrap();
                eprintln!("DICT_GRID_STOP insufficient_native_window=1");
                std::process::exit(75);
            }
        }
        let sort = DictSort::requested(true, requested);
        let threads = sort.selected_threads();
        assert_eq!(threads, requested);
        fs::write("/proc/self/clear_refs", b"5").expect("reset sort RSS high-water mark");
        let before = io();
        let cpu = process_cpu_s();
        let start = Instant::now();
        let Suffixes::Wide(sa) = sort.sort(dict) else {
            unreachable!()
        };
        let wall_s = start.elapsed().as_secs_f64();
        let cpu_s = process_cpu_s() - cpu;
        let after = io();
        let peak = counter(&fs::read_to_string("/proc/self/status").unwrap(), "VmHWM:") * 1024;
        let hash_start = Instant::now();
        let mut sha = Sha256::new();
        let mut block = Vec::with_capacity(1 << 20);
        for chunk in sa.chunks(1 << 17) {
            stop(flag);
            block.clear();
            for &v in chunk {
                block.extend_from_slice(&v.to_le_bytes());
            }
            sha.update(&block);
        }
        let hash = sha.finalize();
        eprintln!("DICT_GRID threads={threads} entries={} wall_s={wall_s:.9} cpu_s={cpu_s:.9} peak_rss={peak} read_bytes={} write_bytes={} sha256={hash:x} hash_s={:.6}", sa.len(), after.0-before.0, after.1-before.1, hash_start.elapsed().as_secs_f64());
        if let Some(serial) = previous {
            assert_eq!(serial, hash, "dictionary grid SA mismatch");
        }
        previous = Some(hash);
    }
}
