// Subprocess fixture for gates/sxls_reclaim_study.py (including fatal exits).
// A command publishes the next unique slice and runs the real scratch gate.
use super::{emit_lcp_slice, Laws};
use std::io::{self, BufRead, Write};

#[test]
#[ignore]
fn private_sink() {
    let dir = std::env::var("MARKER_STUDY_DIR").unwrap();
    let laws = Laws::new(format!("{dir}/../stop"), 0, 1024, 0,
        format!("{dir}/../modeb"), format!("{dir}/../merged"), dir.clone());
    let mut live = std::collections::VecDeque::new();
    let window = crate::laws::env_u64("SS_LCP_SLICE_WINDOW", 8);
    let mut gate_ns = 0;
    for (u, line) in io::stdin().lock().lines().enumerate() {
        assert_eq!(line.unwrap(), "publish");
        emit_lcp_slice(&dir, u as u64, 2, &[1, 300], 2*u as u64, &mut live, window, &laws);
        let t = std::time::Instant::now();
        laws.scratch_gate("marker-study");
        gate_ns += t.elapsed().as_nanos();
        println!("published {u}");
        io::stdout().flush().unwrap();
    }
    eprintln!("scratch_gate_ns {gate_ns}");
}
