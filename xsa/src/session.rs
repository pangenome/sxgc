//! Causal branch queries: immutable m20 plus an immediately readable append tail.
use crate::ctx::{Builder, OwnedMember};
use std::io::{self, BufRead, Write};

// 0..255 = known content, 256 = confirmed document END. An open EOF is neither.
type Histogram = [u64; 257];
trait BranchRead {
    fn branches(&self, p: &[u8]) -> Histogram;
}
struct Sealed {
    member: Option<OwnedMember>,
    positions: Vec<usize>, // seal's depth-limited suffix ordering (includes EOF)
    open_suffix: Vec<u8>,
}
impl BranchRead for Sealed {
    fn branches(&self, p: &[u8]) -> Histogram {
        let mut h = [0; 257];
        if let Some(member) = &self.member {
            let trie = member.view();
            if let Some(n) = trie.find(p) {
                for c in trie.children(n) {
                    h[c.byte().map_or(256, usize::from)] = c.count;
                }
            }
            // m20 represents physical EOF as END. Only confirmed document ends are evidence.
            if self.open_suffix.ends_with(p) {
                h[256] -= 1;
            }
        }
        h
    }
}
struct Live {
    depth: usize,
    text: Vec<u8>,
    ends: Vec<usize>, // exclusive end offsets, strictly increasing
    cut: usize,
    sealed_ends: usize,
    base: Sealed,
}
impl Live {
    fn new(depth: usize) -> Self {
        Self {
            depth,
            text: vec![],
            ends: vec![],
            cut: 0,
            sealed_ends: 0,
            base: Sealed {
                member: None,
                positions: Vec::new(),
                open_suffix: vec![],
            },
        }
    }
    // Positions are logical byte offsets. Restrict the WHOLE occurrence to [lo,hi).
    fn locate(
        &self,
        p: &[u8],
        lo: usize,
        hi: usize,
        cap: usize,
    ) -> Result<(Vec<usize>, usize, usize), String> {
        if p.is_empty() || p.len() > self.depth || lo > hi || hi > self.text.len() || cap > 65536 {
            return Err("invalid locate pattern/range/cap".into());
        }
        let cmp = |i: usize| self.text[i..self.cut.min(i + p.len())].cmp(p);
        let sa = &self.base.positions;
        let a = sa.partition_point(|&i| cmp(i).is_lt());
        let b = sa.partition_point(|&i| !cmp(i).is_gt());
        let mut out = Vec::new();
        let mut push = |i: usize| -> Result<(), String> {
            let doc = self.ends.partition_point(|&e| e <= i);
            if i >= lo
                && i + p.len() <= hi
                && self.ends.get(doc).is_none_or(|&end| i + p.len() <= end)
            {
                if out.len() == cap {
                    return Err("locate result cap exceeded".into());
                }
                out.push(i);
            }
            Ok(())
        };
        for &i in &sa[a..b] {
            push(i)?;
        }
        let start = lo.max(self.cut.saturating_sub(p.len() - 1));
        let mut scanned = 0;
        if hi >= p.len() {
            for i in start..=hi - p.len() {
                scanned += 1;
                if &self.text[i..i + p.len()] == p {
                    push(i)?;
                }
            }
        }
        out.sort_unstable();
        Ok((out, b - a, scanned))
    }
    fn end(&mut self) -> Result<(), String> {
        if self.ends.last().copied().unwrap_or(0) == self.text.len() {
            return Err("empty document".into());
        }
        self.ends.push(self.text.len());
        Ok(())
    }
    fn seal(&mut self) {
        let t = &self.text;
        let mut builder = Builder::new(self.depth, 1, 1);
        builder.set_boundary_aware();
        // m20 only reads depth+1 bytes per suffix. Equal prefixes can tie.
        let prefix = |i: usize| &t[i..t.len().min(i + self.depth + 1)];
        let mut positions: Vec<_> = (0..=t.len()).collect();
        positions.sort_unstable_by(|&a, &b| prefix(a).cmp(prefix(b)));
        for &i in &positions {
            let doc = self.ends.partition_point(|&e| e <= i);
            let end = self.ends.get(doc).copied().unwrap_or(t.len());
            builder.feed(
                prefix(i),
                (t.len() - i) as u64,
                (end - i) as u64,
                doc as u32,
            );
        }
        let (member, _) = builder.finish(t.len() as u64 + 1, t.len() as u64);
        let start = self
            .ends
            .last()
            .copied()
            .unwrap_or(0)
            .max(t.len().saturating_sub(self.depth));
        self.base = Sealed {
            member: Some(OwnedMember::new(member)),
            positions,
            open_suffix: t[start..].to_vec(),
        };
        self.cut = t.len();
        self.sealed_ends = self.ends.len();
    }
}
impl BranchRead for Live {
    fn branches(&self, p: &[u8]) -> Histogram {
        let mut h = self.base.branches(p);
        // Assign each event by continuation position, not by match start: the latter misses joins.
        let mut start = self
            .ends
            .get(self.sealed_ends.wrapping_sub(1))
            .copied()
            .unwrap_or(0);
        let mut q = self.cut;
        for end in self.ends[self.sealed_ends..]
            .iter()
            .copied()
            .map(Some)
            .chain(std::iter::once(None))
        {
            let limit = end.unwrap_or(self.text.len());
            for i in q.max(start + p.len())..limit {
                if self.text[i - p.len()..i] == *p {
                    h[self.text[i] as usize] += 1;
                }
            }
            if end.is_some() && self.text[start..limit].ends_with(p) {
                h[256] += 1;
            }
            start = limit;
            q = limit;
        }
        h
    }
}
fn unhex(s: &str) -> Result<Vec<u8>, String> {
    if !s.is_ascii() || s.len() % 2 != 0 {
        return Err("expected even-length hex".into());
    }
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).map_err(|_| "invalid hex".into()))
        .collect()
}
// Worker owns a consistent copy of the prefix and END events at submission.
// Publication moves only the derived base and its two watermarks; appends and
// END events on the serving thread are never replaced by an older snapshot.
struct Pending(std::thread::JoinHandle<(Sealed, usize, usize)>);
impl Pending {
    fn start(s: &Live) -> Self {
        let mut snapshot = Live::new(s.depth);
        snapshot.text = s.text.clone();
        snapshot.ends = s.ends.clone();
        Self(std::thread::spawn(move || {
            snapshot.seal();
            // Build the generation's lazy directory off the request thread.
            snapshot.base.branches(b"a");
            (snapshot.base, snapshot.cut, snapshot.sealed_ends)
        }))
    }
    fn publish(self, s: &mut Live) -> Result<(), String> {
        let (base, cut, sealed_ends) = self.0.join().map_err(|_| "background seal failed")?;
        s.base = base;
        s.cut = cut;
        s.sealed_ends = sealed_ends;
        Ok(())
    }
}
// Persistent line protocol for correctness testing, not a throughput benchmark.
pub fn command(args: &[String]) -> Result<(), String> {
    let depth = match args {
        [d] => d.parse::<usize>().ok(),
        _ => None,
    }
    .filter(|n| (1..=64).contains(n))
    .ok_or("usage: xsa session DEPTH (1..64)")?;
    let mut session = Live::new(depth);
    let mut pending: Option<Pending> = None;
    let mut out = io::stdout().lock();
    for line in io::stdin().lock().lines() {
        let line = line.map_err(|e| e.to_string())?;
        let fields: Vec<_> = line.split_whitespace().collect();
        if pending.as_ref().is_some_and(|p| p.0.is_finished()) {
            pending.take().unwrap().publish(&mut session)?;
        }
        let reply = match fields.as_slice() {
            ["seal_async"] => {
                if pending.is_some() {
                    return Err("seal already pending".into());
                }
                pending = Some(Pending::start(&session));
                serde_json::json!({"submitted":session.text.len()})
            }
            ["status"] => {
                serde_json::json!({"pending":pending.is_some(), "cut":session.cut, "visible":session.text.len()})
            }
            ["append", s] => {
                session.text.extend(unhex(s)?);
                serde_json::json!({"visible":session.text.len()})
            }
            ["end"] => {
                session.end()?;
                serde_json::json!({"ended":true})
            }
            ["seal"] => {
                if pending.is_some() {
                    return Err("seal already pending".into());
                }
                session.seal();
                serde_json::json!({"sealed":session.cut})
            }
            ["locate", hex, lo, hi, cap] => {
                let num = |s: &str| s.parse::<usize>().map_err(|_| "invalid unsigned offset");
                let (positions, sealed_rows, tail_starts) =
                    session.locate(&unhex(hex)?, num(lo)?, num(hi)?, num(cap)?)?;
                serde_json::json!({"positions":positions,"sealed_rows":sealed_rows,"tail_starts":tail_starts})
            }
            ["byte", pos] => {
                let pos = pos
                    .parse::<usize>()
                    .map_err(|_| "invalid unsigned offset")?;
                let b = session
                    .text
                    .get(pos)
                    .ok_or("offset outside visible prefix")?;
                serde_json::json!({"byte":b})
            }
            ["branch", s] => {
                let p = unhex(s)?;
                if p.is_empty() || p.len() > depth {
                    return Err("query length outside session depth".into());
                }
                let h = session.branches(&p);
                serde_json::json!({"counts":&h[..256],"end":h[256]})
            }
            _ => {
                return Err(
                    "expected append HEX, branch HEX, end, seal, seal_async or status".into(),
                )
            }
        };
        writeln!(out, "{}", reply)
            .and_then(|_| out.flush())
            .map_err(|e| e.to_string())?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn oracle(docs: &[Vec<u8>], p: &[u8]) -> Histogram {
        let mut h = [0; 257];
        for (n, d) in docs.iter().enumerate() {
            for (i, w) in d.windows(p.len()).enumerate() {
                if w == p {
                    match d.get(i + p.len()) {
                        Some(&b) => h[usize::from(b)] += 1,
                        None if n + 1 < docs.len() => h[256] += 1,
                        _ => (),
                    }
                }
            }
        }
        h
    }
    #[test]
    fn locate_ranges_cuts_and_boundaries() {
        for cut in 0..=7 {
            let mut s = Live::new(3);
            for (i, b) in b"aaa\x00aaa".iter().enumerate() {
                if i == cut {
                    s.seal();
                }
                if i == 5 {
                    s.end().unwrap();
                }
                s.text.push(*b);
            }
            if cut == 7 {
                s.seal();
            }
            for lo in 0..=7 {
                for hi in lo..=7 {
                    for p in [b"a".as_slice(), b"aa", b"aaa", b"\x00a"] {
                        let want = (lo..hi)
                            .filter(|&i| {
                                i + p.len() <= hi
                                    && !(i < 5 && i + p.len() > 5)
                                    && s.text[i..].starts_with(p)
                            })
                            .collect::<Vec<_>>();
                        assert_eq!(s.locate(p, lo, hi, 32).unwrap().0, want);
                    }
                }
            }
            assert!(s.locate(b"a", 0, 7, 1).is_err());
        }
    }
    #[test]
    fn pending_generation_preserves_new_bytes_and_ends() {
        let mut s = Live::new(4);
        s.text.extend(b"abcXab");
        let pending = Pending::start(&s);
        s.text.extend(b"cYabc");
        s.end().unwrap();
        s.text.extend(b"abcZ");
        let expected = oracle(&[b"abcXabcYabc".to_vec(), b"abcZ".to_vec()], b"abc");
        assert_eq!(s.branches(b"abc"), expected);
        pending.publish(&mut s).unwrap();
        assert_eq!(s.cut, 6);
        assert_eq!(s.branches(b"abc"), expected);
        let pending = Pending::start(&s);
        s.end().unwrap(); // same visible length, additional confirmed END
        pending.publish(&mut s).unwrap();
        assert_eq!(s.branches(b"Z")[256], 1);
    }

    #[test]
    fn prefix_seal_and_boundary_parity() {
        let fixture: &[&[u8]] = &[b"aaaa\x00\x1eaaa", b"a\xffaa\x1e", b"aa"];
        let mut queries = vec![vec![99]];
        for d in fixture {
            for k in 1..=4 {
                queries.extend(d.windows(k).map(Vec::from));
            }
        }
        queries.sort();
        queries.dedup();
        for stride in [1, 2, 3, 7, 100] {
            let mut s = Live::new(4);
            let mut docs = vec![vec![]];
            s.seal();
            for d in fixture {
                for &b in *d {
                    s.text.push(b);
                    docs.last_mut().unwrap().push(b);
                    for p in &queries {
                        assert_eq!(s.branches(p), oracle(&docs, p));
                    }
                    if s.text.len() % stride == 0 {
                        s.seal();
                        for p in &queries {
                            assert_eq!(s.base.branches(p), oracle(&docs, p));
                        }
                    }
                }
                s.end().unwrap();
                docs.push(vec![]);
                for p in &queries {
                    assert_eq!(s.branches(p), oracle(&docs, p));
                }
            }
            s.seal();
            for p in &queries {
                assert_eq!(s.base.branches(p), oracle(&docs, p));
            }
        }
    }
}
