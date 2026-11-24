//! Parallel emission from disjoint lexical intervals of the resident dictionary SA.
//! Partition after ordering, not by text ID, and never cut an equal finite tail.
//! The join consumes pre-encoded blocks. No row comparison, merge, or LCP fix at join.
use super::*;
use std::{
    mem::size_of,
    os::unix::fs::FileTypeExt,
    sync::mpsc::{sync_channel, Receiver, SyncSender},
    thread,
};

pub(super) enum Owners {
    Dense(Vec<u32>),
    Compact(RankStarts),
}
pub(super) struct Sweep<'a, C> {
    pub dict: &'a Dict,
    pub sa: &'a Suffixes,
    pub owner: &'a Owners,
    pub lists: &'a Lists<C>,
    pub flag: &'a Path,
}
#[derive(Clone, Copy, Default)]
struct Tail {
    at: usize,
    len: usize,
}
#[derive(Clone, Copy, Default)]
struct Fence {
    r: usize,
    key: Tail,
}
#[derive(Clone, Copy, Default)]
struct Probe {
    first: Fence,
    second_key: Fence,
    last: Fence,
}
#[derive(Clone, Copy)]
struct Tile {
    lo: usize,
    hi: usize,
    prev: Tail,
}
#[derive(Default)]
pub(super) struct Counts {
    pub seen: usize,
    pub groups: usize,
    pub max_group: usize,
}
impl Counts {
    fn add(&mut self, c: Self) {
        self.seen += c.seen;
        self.groups += c.groups;
        self.max_group = self.max_group.max(c.max_group);
    }
}
const CREDITS: usize = 2;
#[derive(Clone, Copy)]
pub(super) struct Mode {
    lanes: usize,
    step: usize,
    pub async_audit: bool,
}
impl Mode {
    pub fn from_env(n: usize, audit: Option<&fs::File>) -> Self {
        let number = |var, dflt| {
            std::env::var(var)
                .map(|x| x.parse::<usize>().expect("radix integer option"))
                .unwrap_or(dflt)
        };
        let lanes = number("XSA_RESIDENT_RADIX_LANES", 1);
        let step = number("XSA_RESIDENT_RADIX_STEP", 16384);
        assert!(
            (1..=8).contains(&lanes)
                && (1..=1 << 20).contains(&step)
                && (n <= 1_000_000 || step >= 2048),
            "radix lane/step outside grant"
        );
        let async_audit = cfg!(all(target_os = "linux", target_arch = "x86_64"))
            && lanes > 1
            && std::env::var("XSA_RESIDENT_ASYNC_AUDIT").as_deref() == Ok("1")
            && audit.is_some_and(|f| f.metadata().expect("audit fd").file_type().is_fifo());
        Self {
            lanes,
            step,
            async_audit,
        }
    }
    fn blocks(self) -> usize {
        self.lanes * CREDITS * BLOCK_BYTES
    }
    pub fn extra_bytes(self, d: usize) -> usize {
        if self.lanes == 1 {
            return 0;
        }
        // Probes and final cut list coexist in planning. Recycled packets,
        // explicit 2 MiB stacks + channel/control slack, extra small heap slack.
        d.div_ceil(self.step) * (size_of::<Probe>() + size_of::<Tile>())
            + self.blocks()
            + self.lanes * (3 << 20)
            + (1 << 20)
            + usize::from(self.async_audit) * audit_tee::RESERVE
    }
}

enum Packet {
    Data(Vec<u8>),
    Done(usize, Counts),
}
struct Lane<'a> {
    free: Receiver<Vec<u8>>,
    ready: SyncSender<Packet>,
    block: Option<Vec<u8>>,
    flag: &'a Path,
}
impl Lane<'_> {
    fn publish(&mut self) {
        if let Some(b) = self.block.take() {
            stop(self.flag);
            self.ready
                .send(Packet::Data(b))
                .expect("ordered reader gone");
        }
    }
}
impl RowTarget for Lane<'_> {
    #[inline]
    fn push<C: Coord>(&mut self, p: usize, h: usize, b: u32) {
        let buf = self
            .block
            .get_or_insert_with(|| self.free.recv().expect("block credit lost"));
        encode::<C>(buf, p, h, b);
        if buf.len() == BLOCK_BYTES {
            self.publish();
        }
    }
}

impl<C: Coord> Sweep<'_, C> {
    #[inline]
    fn entry(&self, r: usize) -> Option<(usize, Tail)> {
        let at = self.sa.at(r);
        let v = match self.owner {
            Owners::Compact(s) => s.word(at),
            Owners::Dense(a) => {
                let v = a[at];
                if v == u32::MAX {
                    return None;
                }
                v as usize
            }
        };
        let len = self.dict.start[v + 1] as usize - 1 - at;
        (len > if v == self.dict.eof_id { 1 } else { W }).then_some((v, Tail { at, len }))
    }
    #[inline]
    fn key(&self, t: Tail) -> &[u8] {
        &self.dict.bytes[t.at..t.at + t.len]
    }
    fn probe(&self, lo: usize, hi: usize) -> Probe {
        let fence = |r| self.entry(r).map(|(_, key)| Fence { r, key });
        let Some(first) = (lo..hi).find_map(fence) else {
            return Probe::default();
        };
        let last = (first.r..hi).rev().find_map(fence).unwrap();
        // Equal finite tails are consecutive in the projected (owned) SA.
        // Never chase a huge tied/empty interval repeatedly across raw tile boundaries.
        let second_key = if self.key(first.key) == self.key(last.key) {
            Fence::default()
        } else {
            (first.r + 1..=last.r)
                .filter_map(fence)
                .find(|f| self.key(f.key) != self.key(first.key))
                .unwrap()
        };
        Probe {
            first,
            second_key,
            last,
        }
    }
    fn tiles(&self, mode: Mode) -> Vec<Tile> {
        let d = self.dict.bytes.len();
        let mut probes = vec![Probe::default(); d.div_ceil(mode.step)];
        let stride = probes.len().div_ceil(mode.lanes);
        let phase = Instant::now();
        thread::scope(|s| {
            for (j, chunk) in probes.chunks_mut(stride).enumerate() {
                thread::Builder::new()
                    .stack_size(2 << 20)
                    .spawn_scoped(s, move || {
                        for (i, p) in chunk.iter_mut().enumerate() {
                            let lo = (j * stride + i) * mode.step;
                            stop(self.flag);
                            *p = self.probe(lo, d.min(lo + mode.step));
                        }
                    })
                    .expect("radix planner thread");
            }
        });
        let mut tiles: Vec<Tile> = Vec::with_capacity(probes.len());
        let mut prior = Fence::default();
        let (mut adjusted, mut absorbed, mut empty) = (0, 0, 0);
        for p in &probes {
            if p.first.key.len == 0 {
                empty += 1;
                continue;
            }
            let cut = if prior.key.len == 0 || self.key(prior.key) != self.key(p.first.key) {
                Some((p.first.r, prior.r + 1, prior.key))
            } else if p.second_key.key.len != 0 {
                adjusted += 1;
                Some((p.second_key.r, p.second_key.r, p.first.key))
            } else {
                absorbed += 1;
                None
            };
            if let Some((lo, end_old, prev)) = cut {
                if let Some(old) = tiles.last_mut() {
                    old.hi = end_old;
                    assert!(old.lo < old.hi && old.hi <= lo);
                }
                tiles.push(Tile { lo, hi: 0, prev });
            }
            prior = p.last;
        }
        tiles.last_mut().expect("no owned suffixes").hi = prior.r + 1;
        // A heap needs <= one cursor/word and <= one cursor/raw rank in this
        // interval. Bound concurrent capacities (incl. allocation doubling),
        // not just the largest individual heap. Refuse BEFORE launching lanes.
        let k = self.dict.start.len() - 1;
        let mut largest = vec![0usize; mode.lanes];
        for (i, t) in tiles.iter().enumerate() {
            let cap = k.min(t.hi - t.lo).max(4);
            let j = i % mode.lanes;
            largest[j] = largest[j].max(cap);
        }
        // Allow old and new allocations to coexist across a moving realloc.
        let heaps = 3 * size_of::<Cursor>() * largest.iter().sum::<usize>();
        assert!(
            heaps <= 40 * k + (1 << 20),
            "radix concurrent heaps exceed reserved bytes"
        );
        eprintln!("RESIDENT_RADIX_PLAN lanes={} sa_ranks_per_tile={} raw_tiles={} ranges={} empty={} adjusted={} absorbed={} heap_bound={} block_bytes={} row_queue_reserved={} plan_s={:.6}",mode.lanes,mode.step,probes.len(),tiles.len(),empty,adjusted,absorbed,heaps,BLOCK_BYTES,mode.blocks(),phase.elapsed().as_secs_f64());
        tiles
    }
    fn scan(&self, t: Tile, q: &mut BinaryHeap<Cursor>, out: &mut impl RowTarget) -> Counts {
        debug_assert!(q.is_empty());
        let mut prev = t.prev;
        let mut c = Counts::default();
        let mut head = 0;
        for r in t.lo..t.hi {
            if r % (1 << 22) == 0 {
                stop(self.flag);
            }
            let Some((v, key)) = self.entry(r) else {
                continue;
            };
            let h = common(self.key(prev), self.key(key));
            if c.seen == 0 || h < prev.len.min(key.len) {
                assert!(
                    prev.len == 0 || h < prev.len.min(key.len),
                    "range cut inside equal/prefixed finite keys"
                );
                flush(q, self.lists, prev.len, head, out);
                c.groups += 1;
                head = h as u32;
            } else {
                assert_eq!(prev.len, key.len, "proper prefix among representatives");
            }
            let off = local_offset(key.at, self.dict.start[v]);
            let idx = self.lists.loc[v];
            let b = if off == 0 {
                0
            } else {
                self.dict.decode[self.dict.bytes[key.at - 1] as usize] as u32
            };
            q.push(Reverse((
                self.lists.next[idx as usize],
                idx,
                self.lists.loc[v + 1],
                off,
                b,
            )));
            c.max_group = c.max_group.max(q.len());
            prev = key;
            c.seen += 1;
        }
        flush(q, self.lists, prev.len, head, out);
        c
    }
    pub fn emit(&self, mode: Mode, out: &mut Output) -> Counts {
        let d = self.dict.bytes.len();
        if mode.lanes == 1 {
            let t = Tile {
                lo: 0,
                hi: d,
                prev: Tail::default(),
            };
            let c = self.scan(t, &mut BinaryHeap::new(), out);
            out.send();
            return c;
        }
        let tiles = self.tiles(mode);
        // Publish the global END to both branches before starting any data writer.
        let pipe = if mode.async_audit {
            Some(audit_tee::send_end(out))
        } else {
            out.send();
            None
        };
        let mut sum = Counts::default();
        let mut wait = Duration::ZERO;
        let (mut max_rows, mut blocks) = (0, 0u64);
        let mut rows_per_lane = vec![0u64; mode.lanes];
        thread::scope(|s| {
            let mut pipes = Vec::new();
            for j in 0..mode.lanes {
                let (credit, free) = sync_channel(CREDITS);
                let (ready, read) = sync_channel(CREDITS + 1);
                for _ in 0..CREDITS {
                    credit.send(Vec::with_capacity(BLOCK_BYTES)).unwrap();
                }
                pipes.push((read, credit));
                let tiles = &tiles;
                thread::Builder::new()
                    .stack_size(2 << 20)
                    .spawn_scoped(s, move || {
                        let mut lane = Lane {
                            free,
                            ready,
                            block: None,
                            flag: self.flag,
                        };
                        let mut q = BinaryHeap::new();
                        for i in (j..tiles.len()).step_by(mode.lanes) {
                            stop(self.flag);
                            let c = self.scan(tiles[i], &mut q, &mut lane);
                            lane.publish();
                            lane.ready
                                .send(Packet::Done(i, c))
                                .expect("range reader gone");
                        }
                    })
                    .expect("radix worker thread");
            }
            audit_tee::ordered(pipe, self.flag, mode.lanes * CREDITS, |mut tee| {
                // No per-row decode or comparison here. Per-lane credits mean a
                // later interval can never consume the earliest interval's buffer.
                for i in 0..tiles.len() {
                    let j = i % mode.lanes;
                    let (read, credit) = &pipes[j];
                    let mut rows = 0;
                    loop {
                        let t = Instant::now();
                        let packet = read.recv().expect("range worker gone");
                        wait += t.elapsed();
                        match packet {
                            Packet::Data(mut bytes) => {
                                assert!(
                                    bytes.len() % 12 == 0
                                        && !bytes.is_empty()
                                        && bytes.len() <= BLOCK_BYTES
                                );
                                let n = (bytes.len() / 12) as u64;
                                rows += n;
                                out.rows += n;
                                if let Some(writer) = tee.as_deref_mut() {
                                    writer.submit(bytes, credit, out);
                                } else {
                                    out.write_block(&bytes);
                                    bytes.clear();
                                    let _ = credit.send(bytes);
                                }
                                blocks += 1;
                            }
                            Packet::Done(id, c) => {
                                assert_eq!(id, i);
                                assert!(rows > 0);
                                sum.add(c);
                                break;
                            }
                        }
                    }
                    max_rows = max_rows.max(rows);
                    rows_per_lane[j] += rows;
                }
            })
        });
        eprintln!("RESIDENT_RADIX_JOIN lanes={} ranges={} blocks={} max_range_rows={} order_wait_s={:.6} lane_rows={:?} serial_row_comparisons=0",mode.lanes,tiles.len(),blocks,max_rows,wait.as_secs_f64(),rows_per_lane);
        sum
    }
}
