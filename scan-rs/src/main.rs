// scan-rs — Bit 2: Rust translation of the Lean-verified one-pass state machine
// (sxgc/lean/Sxgc.lean, Sxgc.scan — 511-text exhaustive gate GREEN).
//
// Input:  triples on stdin, one per line: "<bwt> <lcp> <sa>" (sa 0-based,
//         stream = reverse(text)+sentinel convention — Bit-1 contract).
// Args:   N = text length + 1 (the -n value).
// Output: emitted suffixient positions, one per line, ascending as emitted.
//
// The state machine is a faithful mirror of lean/Sxgc.lean (evalStep, upd,
// scanAux): per-char LCP maxima, eval on char change with the run-min LCP
// (including the boundary triple's lcp), candidate updates with N - sa,
// final eval(-1), sentinel char 0 never emitted.
use anyhow::Context;
use std::io::{BufRead, BufWriter, Write};

const MAXINT: i64 = i64::MAX;
const SIGMA: usize = 128;

#[derive(Clone, Copy)]
struct Cand {
    len: i64,
    pos: u64,
    active: bool,
}

fn eval_step(l: i64, r: &mut [Cand; SIGMA], out: &mut Vec<u64>) {
    // C++/Lean: for c = 1; c < sigma; ++c — sentinel char 0 never emitted
    for c in 1..SIGMA {
        if l < r[c].len {
            if r[c].active {
                out.push(r[c].pos);
            }
            r[c] = Cand { len: l, pos: 0, active: false };
        }
    }
}

fn upd(r: &mut [Cand; SIGMA], c: usize, l: i64, pos: u64) {
    if l > r[c].len {
        r[c] = Cand { len: l, pos, active: true };
    }
}

fn main() -> anyhow::Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: scan-rs <N> < triples.txt   (N = text length + 1)");
        std::process::exit(1);
    }
    let n: u64 = args[1].parse()?;
    let stdin = std::io::stdin();
    let mut r = [Cand { len: -1, pos: 0, active: false }; SIGMA];
    let mut out: Vec<u64> = Vec::new();
    let mut m: i64 = MAXINT;
    let mut p: i64 = -1; // -1 = uninitialized (first triple initializes)
    let mut p_sa: u64 = 0;

    for line in stdin.lock().lines() {
        let line = line?;
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let mut it = line.split_whitespace();
        let c: i64 = it.next().context("c")?.parse()?;
        let lcp: i64 = it.next().context("lcp")?.parse()?;
        let sa: u64 = it.next().context("sa")?.parse()?;
        if p < 0 {
            // first triple initializes (C++: i=1; ++iter; p=bwt; p_sa=sa)
            p = c;
            p_sa = sa;
            continue;
        }
        let m2 = m.min(lcp);
        if c != p {
            eval_step(m2, &mut r, &mut out);
            upd(&mut r, p as usize, lcp, n - p_sa);
            upd(&mut r, c as usize, lcp, n - sa);
            m = MAXINT;
        } else {
            m = m2;
        }
        p = c;
        p_sa = sa;
    }
    if p >= 0 {
        eval_step(-1, &mut r, &mut out);
    }

    let stdout = std::io::stdout();
    let mut w = BufWriter::new(stdout.lock());
    for x in &out {
        writeln!(w, "{}", x)?;
    }
    eprintln!("scan-rs: {} positions emitted (N={})", out.len(), n);
    Ok(())
}
