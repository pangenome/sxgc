#!/usr/bin/env python3
"""Compile the actual phase owner and verify native grandchildren die on drop."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'src/chain.rs').read_text()
wrapper = re.search(r'^const PHASE_WRAPPER:.*$', source, re.M).group()
start = source.index('struct RunningPhase {')
end = source.index('/// Wait for a spawned phase', start)
harness = r'''
use std::fs::{self, File};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::os::unix::process::CommandExt;
struct Journal;
impl Journal { fn line(&mut self, _: &str) {} }
WRAPPER
OWNER
fn main() {
    let dir = PathBuf::from(std::env::args().nth(1).unwrap());
    let pidfile = dir.join("native.pid");
    let args: Vec<String> = ["900", "sh", "-c", "echo $$ > native.pid; exec sleep 600"]
        .into_iter().map(str::to_string).collect();
    let running = spawn_phase(&mut Journal, &dir, "test", 1024*1024,
                              "/usr/bin/timeout", &args, &[]).unwrap();
    for _ in 0..500 {
        if pidfile.exists() { break; }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
    let pid = fs::read_to_string(&pidfile).expect("native child started").trim().to_string();
    assert!(Path::new(&format!("/proc/{pid}")).exists());
    drop(running); // Also exercised on ? early returns during fused startup/join.
    for _ in 0..500 {
        let state = fs::read_to_string(format!("/proc/{pid}/stat")).unwrap_or_default();
        if state.is_empty() || state.split_whitespace().nth(2) == Some("Z") {
            println!("PASS phase drop killed native grandchild pid={pid}"); return;
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
    panic!("native grandchild survived phase cleanup: {pid}");
}
'''.replace('WRAPPER', wrapper).replace('OWNER', source[start:end])
with tempfile.TemporaryDirectory(prefix='phase-cleanup-') as tmp:
    path = Path(tmp)
    (path/'test.rs').write_text(harness)
    subprocess.run(['timeout', '900', 'rustc', '--edition=2021', '-Awarnings', str(path/'test.rs'),
                    '-o', str(path/'test')], check=True, stdin=subprocess.DEVNULL)
    subprocess.run(['timeout', '900', str(path/'test'), str(path)], check=True, stdin=subprocess.DEVNULL)
