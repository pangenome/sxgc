//! One ordered writer on a dedicated inherited Linux verifier pipe. No payload allocation.
use super::{stop, Output};
use std::{
    fs::File,
    io::{self, Write},
    os::fd::AsRawFd,
    path::Path,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed},
        mpsc::{sync_channel, RecvTimeoutError, SyncSender},
        Arc,
    },
    thread,
    time::{Duration, Instant},
};

const STACK: usize = 2 << 20;
// At most 16 Arc headers/credit handles, queue slots, counters and thread controls.
pub(super) const RESERVE: usize = STACK + (64 << 10);
const TICK: Duration = Duration::from_millis(100);

// Linux pollfd ABI; fcntl applies O_NONBLOCK to the dedicated write description.
#[repr(C)]
struct PollFd {
    fd: i32,
    events: i16,
    revents: i16,
}
const POLLOUT: i16 = 4;
const F_GETFL: i32 = 3;
const F_SETFL: i32 = 4;
const O_NONBLOCK: i32 = 0o4000; // Linux x86-64
extern "C" {
    fn poll(fds: *mut PollFd, n: usize, timeout: i32) -> i32;
    fn fcntl(fd: i32, cmd: i32, ...) -> i32;
}
fn write_pipe(
    pipe: &mut File,
    mut bytes: &[u8],
    flag: &Path,
    cancel: &AtomicBool,
) -> io::Result<()> {
    let mut fd = PollFd {
        fd: pipe.as_raw_fd(),
        events: POLLOUT,
        revents: 0,
    };
    let mut last_check = Instant::now();
    while !bytes.is_empty() {
        if last_check.elapsed() >= TICK {
            stop(flag);
            last_check = Instant::now();
        }
        if cancel.load(Relaxed) {
            return Err(io::ErrorKind::Interrupted.into());
        }
        match pipe.write(bytes) {
            Ok(0) => return Err(io::ErrorKind::WriteZero.into()),
            Ok(n) => bytes = &bytes[n..],
            Err(e) if e.kind() == io::ErrorKind::Interrupted => (),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                if unsafe { poll(&mut fd, 1, TICK.as_millis() as i32) } < 0 {
                    let e = io::Error::last_os_error();
                    if e.kind() != io::ErrorKind::Interrupted {
                        return Err(e);
                    }
                }
            }
            Err(e) => return Err(e),
        }
    }
    Ok(())
}
pub(super) fn send_end(out: &mut Output) -> File {
    let mut pipe = out.audit.take().unwrap();
    let fd = pipe.as_raw_fd();
    let flags = unsafe { fcntl(fd, F_GETFL) };
    assert!(
        flags >= 0 && unsafe { fcntl(fd, F_SETFL, flags | O_NONBLOCK) } == 0,
        "nonblocking audit pipe"
    );
    let end = std::mem::take(&mut out.bytes);
    assert_eq!(end.len(), 12);
    out.write_block(&end);
    let t = Instant::now();
    write_pipe(&mut pipe, &end, out.flag, &AtomicBool::new(false)).expect("audit END");
    out.audit_time += t.elapsed();
    pipe
}
struct Block<'a> {
    bytes: Vec<u8>,
    credit: SyncSender<Vec<u8>>,
    live: &'a AtomicUsize,
}
impl Drop for Block<'_> {
    fn drop(&mut self) {
        self.live.fetch_sub(1, Relaxed);
        self.bytes.clear();
        let _ = self.credit.send(std::mem::take(&mut self.bytes));
    }
}
pub(super) struct Tee<'a> {
    tx: SyncSender<Arc<Block<'a>>>,
    live: &'a AtomicUsize,
    peak: usize,
    enqueue: Duration,
    bytes: u64,
}
impl Tee<'_> {
    pub(super) fn submit(
        &mut self,
        bytes: Vec<u8>,
        credit: &SyncSender<Vec<u8>>,
        out: &mut Output,
    ) {
        let t = Instant::now();
        self.peak = self.peak.max(self.live.fetch_add(1, Relaxed) + 1);
        let block = Arc::new(Block {
            bytes,
            credit: credit.clone(),
            live: self.live,
        });
        self.tx.send(Arc::clone(&block)).expect("audit writer gone");
        self.enqueue += t.elapsed();
        out.write_block(&block.bytes);
        self.bytes += block.bytes.len() as u64;
    }
}
struct CancelOnDrop<'a>(&'a AtomicBool);
impl Drop for CancelOnDrop<'_> {
    fn drop(&mut self) {
        self.0.store(true, Relaxed);
    }
}
pub(super) fn ordered(
    pipe: Option<File>,
    flag: &Path,
    capacity: usize,
    emit: impl FnOnce(Option<&mut Tee>),
) {
    let Some(mut pipe) = pipe else {
        return emit(None);
    };
    let live = AtomicUsize::new(0);
    let cancelled = AtomicBool::new(false);
    thread::scope(|s| {
        let (tx, rx) = sync_channel::<Arc<Block>>(capacity);
        let cancel = &cancelled;
        let worker = thread::Builder::new()
            .stack_size(STACK)
            .spawn_scoped(s, move || {
                let started = Instant::now();
                let mut bytes = 0u64;
                let mut writes = Duration::ZERO;
                loop {
                    stop(flag);
                    match rx.recv_timeout(TICK) {
                        Ok(block) => {
                            let t = Instant::now();
                            if let Err(e) = write_pipe(&mut pipe, &block.bytes, flag, cancel) {
                                if cancel.load(Relaxed) {
                                    break;
                                }
                                // The ordered join might itself be stalled on the other consumer.
                                eprintln!("RESIDENT_AUDIT_ERROR {e}");
                                std::process::exit(1);
                            }
                            writes += t.elapsed();
                            bytes += block.bytes.len() as u64;
                        }
                        Err(RecvTimeoutError::Disconnected) => break,
                        Err(RecvTimeoutError::Timeout) => (),
                    }
                }
                (bytes, writes, started.elapsed())
            })
            .expect("audit writer");
        let _cancel = CancelOnDrop(&cancelled);
        let mut tee = Tee {
            tx,
            live: &live,
            peak: 0,
            bytes: 0,
            enqueue: Duration::ZERO,
        };
        emit(Some(&mut tee));
        let t = Instant::now();
        drop(tee.tx);
        let (bytes, writes, wall) = worker.join().expect("audit writer failed");
        assert_eq!(bytes, tee.bytes);
        assert_eq!(live.load(Relaxed), 0);
        assert!(tee.peak <= capacity);
        eprintln!("RESIDENT_ASYNC_AUDIT data_bytes={bytes} sink_bytes={} audit_bytes={} peak_inflight_blocks={} enqueue_s={:.6} worker_write_s={:.6} worker_wall_s={:.6} drain_s={:.6}", bytes+12, bytes+12, tee.peak, tee.enqueue.as_secs_f64(), writes.as_secs_f64(), wall.as_secs_f64(), t.elapsed().as_secs_f64());
    });
}
