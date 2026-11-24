//! Feed the trie from the ordered row pipe itself; pass the same bytes to the next consumer.
//! Linux pipes, not files or a whole-SA cache. The caller owns end-to-end verification.
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::os::fd::{AsFd, AsRawFd};
use std::os::unix::fs::{FileTypeExt, MetadataExt, OpenOptionsExt};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

pub(super) const ROWS: usize = 32_768;
pub(super) const LIMIT: u64 = 1 << 43;
fn invalid(msg: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg)
}

pub(super) struct Pipe {
    input: File,
    output: File,
    stop: String,
    until: u64,
}
impl Pipe {
    pub fn open(stop: String, until: u64) -> io::Result<Self> {
        let input = File::from(io::stdin().as_fd().try_clone_to_owned()?);
        let output = File::from(io::stdout().as_fd().try_clone_to_owned()?);
        let (i, o) = (input.metadata()?, output.metadata()?);
        if (i.dev(), i.ino()) == (o.dev(), o.ino()) {
            return Err(invalid("row pipe loop"));
        }
        for f in [&input, &output] {
            let fd = f.as_raw_fd();
            if !f.metadata()?.file_type().is_fifo()
                || !(1..=512 * 1024).contains(&unsafe { libc::fcntl(fd, libc::F_GETPIPE_SZ) })
            {
                return Err(invalid("row43 stdin and stdout must be bounded pipes"));
            }
            let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
            if flags < 0 || unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0
            {
                return Err(io::Error::last_os_error());
            }
        }
        let p = Self {
            input,
            output,
            stop,
            until,
        };
        p.guard()?;
        Ok(p)
    }
    pub fn guard(&self) -> io::Result<()> {
        let stopped = match fs::symlink_metadata(&self.stop) {
            Ok(_) => true,
            Err(e) if e.kind() == io::ErrorKind::NotFound => false,
            Err(e) => return Err(e),
        };
        if stopped
            || SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_err(io::Error::other)?
                .as_secs()
                >= self.until
        {
            eprintln!("STOP_OBSERVED phase=ctx-row43-pipe");
            std::process::exit(75);
        }
        Ok(())
    }
    fn transfer(&self, mut bytes: &mut [u8], writing: bool) -> io::Result<()> {
        let mut f = if writing { &self.output } else { &self.input };
        while !bytes.is_empty() {
            self.guard()?;
            match if writing {
                f.write(bytes)
            } else {
                f.read(bytes)
            } {
                Ok(0) => return Err(invalid("short row43 pipe")),
                Ok(n) => bytes = &mut bytes[n..],
                Err(e) if e.kind() == io::ErrorKind::Interrupted => (),
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => self.wait(f, writing)?,
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }
    fn wait(&self, f: &File, writing: bool) -> io::Result<()> {
        let mut p = libc::pollfd {
            fd: f.as_raw_fd(),
            events: if writing { libc::POLLOUT } else { libc::POLLIN },
            revents: 0,
        };
        if unsafe { libc::poll(&mut p, 1, 50) } < 0 {
            let e = io::Error::last_os_error();
            if e.kind() != io::ErrorKind::Interrupted {
                return Err(e);
            }
        }
        Ok(())
    }
    fn eof(&self) -> io::Result<()> {
        loop {
            self.guard()?;
            match (&self.input).read(&mut [0]) {
                Ok(0) => return Ok(()),
                Ok(_) => return Err(invalid("extra row43 bytes")),
                Err(e) if e.kind() == io::ErrorKind::Interrupted => (),
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => self.wait(&self.input, false)?,
                Err(e) => return Err(e),
            }
        }
    }
    pub fn relay(&self, n: u64, mut feed: impl FnMut(&[u64])) -> io::Result<()> {
        if n == 0 || n >= LIMIT {
            return Err(invalid("row43 corpus extent"));
        }
        let mut bytes = vec![0u8; 12 * ROWS];
        let mut positions = vec![0u64; ROWS];
        let (mut rank, mut prev, mut ends) = (0, n, 0u64);
        while rank <= n {
            let count = (n + 1 - rank).min(ROWS as u64) as usize;
            let packet = &mut bytes[..12 * count];
            self.transfer(packet, false)?;
            for (j, row) in packet.chunks_exact(12).enumerate() {
                let word = |i| u32::from_le_bytes(row[i..i + 4].try_into().unwrap());
                let tag = word(8);
                let b = tag & 511;
                let coord =
                    |lo, shift: u32| u64::from(word(lo)) | (u64::from((tag >> shift) & 2047) << 32);
                let p = coord(0, 9);
                let h = coord(4, 20);
                if tag >> 31 != 1
                    || p > n
                    || (p == n) != (rank + j as u64 == 0)
                    || h > n - p.max(prev)
                    || b > 256
                    || (b == 256) != (p == 0)
                {
                    return Err(invalid("row43 position/LCP/END geometry"));
                }
                ends += u64::from(p == 0);
                if ends > 1 {
                    return Err(invalid("duplicate END predecessor"));
                }
                positions[j] = p;
                prev = p;
            }
            rank += count as u64;
            if rank == n + 1 {
                self.eof()?;
                if ends != 1 {
                    return Err(invalid("missing END predecessor"));
                }
            }
            feed(&positions[..count]);
            self.transfer(packet, true)?;
        }
        self.guard()
    }
    pub fn publish(&self, path: &str, payload: &[u8]) -> io::Result<()> {
        self.guard()?;
        let tmp = format!("{path}.partial");
        let mut out = File::options()
            .create_new(true)
            .write(true)
            .mode(0o600)
            .open(&tmp)?;
        out.write_all(payload)?;
        out.sync_all()?;
        drop(out);
        self.guard()?;
        fs::hard_link(&tmp, path)?;
        fs::remove_file(&tmp)?;
        File::open(
            Path::new(path)
                .parent()
                .filter(|p| !p.as_os_str().is_empty())
                .unwrap_or(Path::new(".")),
        )?
        .sync_all()
    }
}
