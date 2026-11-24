//! Producer-only diagnostic, not a product sink. Stop after a fixed number of rows.
#[path = "../overlap.rs"]
mod overlap;
use std::{
    cell::Cell,
    fs::{read_to_string, File},
    time::Instant,
};
fn read_bytes() -> u64 {
    read_to_string("/proc/self/io")
        .unwrap()
        .lines()
        .find_map(|l| l.strip_prefix("read_bytes: "))
        .unwrap()
        .parse()
        .unwrap()
}
fn main() {
    let args: Vec<_> = std::env::args().collect();
    let file = File::open(&args[1]).unwrap();
    let bytes = unsafe { memmap2::Mmap::map(&file).unwrap() };
    let limit: u64 = args[2].parse().unwrap();
    assert!(limit > 0 && limit <= bytes.len() as u64 + 1 && bytes.len() <= 101_000_000);
    let guard: u64 = args[3].parse().unwrap();
    let start = Instant::now();
    let next = Cell::new(0.0);
    let hint = std::env::var("XSA_PROBE_PHASE_ADVICE").as_deref() == Ok("1");
    let check = |stage: &str| {
        let sec = start.elapsed().as_secs_f64();
        let boundary = stage.starts_with('@');
        if hint && boundary {
            let adv = match stage {
                "@distribute" => Some(memmap2::Advice::Normal),
                "@refill" | "@exact-sort" | "@emit" => Some(memmap2::Advice::Random),
                _ => None,
            };
            if let Some(a) = adv {
                bytes.advise(a).expect("phase madvise");
            }
        }
        if boundary || sec >= next.get() {
            next.set(sec + 0.25);
            let r = read_bytes();
            eprintln!("TRACE sec={sec:.3} stage={stage} reads={r}");
            if r > guard {
                eprintln!("STOP read guard");
                std::process::exit(75)
            }
        }
    };
    let mut rows = 0;
    let mut hash = 0u64;
    overlap::construct(&bytes, &check, |p, h, raw| {
        let b = if p == 0 { 256 } else { u64::from(raw) };
        for k in [p, h, b] {
            hash = hash
                .rotate_left(19)
                .wrapping_add(k)
                .wrapping_mul(0x9e3779b97f4a7c15);
        }
        rows += 1;
        if rows % 65536 == 0 {
            check("emit");
        }
        if rows % 1_000_000 == 0 {
            eprintln!(
                "CHECKPOINT rows={rows} digest={hash:016x} reads={} wall={:.3}",
                read_bytes(),
                start.elapsed().as_secs_f64()
            );
        }
        if rows == limit {
            let reads = read_bytes();
            println!(
                "PROBE rows={rows} digest={hash:016x} reads={reads} wall={:.3}",
                start.elapsed().as_secs_f64()
            );
            if reads > guard {
                std::process::exit(75);
            }
            if limit != bytes.len() as u64 + 1 {
                std::process::exit(0);
            }
        }
    });
    assert_eq!(rows, limit);
    assert_eq!(rows, bytes.len() as u64 + 1);
    println!("FULL_STREAM rows={rows} digest={hash:016x}");
}
