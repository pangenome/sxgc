// AMENDMENT R: the tape-reader producer - the column IO (the only
// corpus-proportional arrays are mapped disc columns; no text is
// ever written to disc - the corpus file is the only text).
use std::fs;
use std::io::{Read, Write};
use std::os::unix::fs::FileExt;
use std::path::Path;

pub fn die(msg: &str) -> ! {
    eprintln!("pfp_sort_r: {}", msg);
    std::process::exit(2);
}

// an append-only column builder
pub struct ColWriter {
    f: fs::File,
    pub path: String,
    buf: Vec<u8>,
}

impl ColWriter {
    pub fn open(path: &str) -> ColWriter {
        if let Some(d) = Path::new(path).parent() {
            let _ = std::fs::create_dir_all(d);
        }
        let f = fs::File::create(path).unwrap_or_else(|_| die("open column"));
        ColWriter { f, path: path.to_string(), buf: Vec::with_capacity(1 << 20) }
    }
    pub fn put_u32(&mut self, v: u32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
        if self.buf.len() >= (1 << 20) { self.flush_buf(); }
    }
    pub fn put_u64(&mut self, v: u64) {
        self.buf.extend_from_slice(&v.to_le_bytes());
        if self.buf.len() >= (1 << 20) { self.flush_buf(); }
    }
    fn flush_buf(&mut self) {
        if !self.buf.is_empty() {
            self.f.write_all(&self.buf)
                .unwrap_or_else(|_| die("write column"));
            self.buf.clear();
        }
    }
    pub fn close(mut self) {
        self.flush_buf();
        self.f.sync_all().unwrap_or_else(|_| die("sync column"));
    }
}

// a mapped column (read-only or read-write)
pub struct ColMap {
    m: Option<memmap2::MmapMut>,
    ro: Option<memmap2::Mmap>,
    pub ptr: *mut u8,
    pub len: u64,
}

unsafe impl Send for ColMap {}
unsafe impl Sync for ColMap {}

fn col_bytes(count: u64, elem: u64) -> u64 {
    die_if(elem == 0 || elem > 8, "elem size");
    count * elem
}

fn die_if(b: bool, m: &str) {
    if b { die(m); }
}

impl ColMap {
    pub fn map_ro(path: &str, count: u64, elem: u64) -> ColMap {
        let bytes = col_bytes(count, elem);
        if bytes == 0 {
            return ColMap { m: None, ro: None, ptr: std::ptr::null_mut(), len: 0 };
        }
        let f = fs::File::open(path).unwrap_or_else(|_| die("open mapped column"));
        let m = unsafe { memmap2::MmapOptions::new()
            .len(bytes as usize).map(&f)
            .unwrap_or_else(|_| die("mmap column")) };
        let ptr = m.as_ptr() as *mut u8;
        ColMap { m: None, ro: Some(m), ptr, len: count }
    }
    pub fn map_rw(path: &str, count: u64, elem: u64) -> ColMap {
        let bytes = col_bytes(count, elem);
        if let Some(d) = Path::new(path).parent() {
            let _ = std::fs::create_dir_all(d);
        }
        let f = fs::OpenOptions::new()
            .read(true).write(true).create(true).truncate(true)
            .open(path).unwrap_or_else(|_| die("create mapped column"));
        f.set_len(bytes).unwrap_or_else(|_| die("ftruncate column"));
        if bytes == 0 {
            return ColMap { m: None, ro: None, ptr: std::ptr::null_mut(), len: 0 };
        }
        let m = unsafe { memmap2::MmapOptions::new()
            .len(bytes as usize).map_mut(&f)
            .unwrap_or_else(|_| die("mmap rw column")) };
        let mut m = m;
        let ptr = m.as_mut_ptr();
        ColMap { m: Some(m), ro: None, ptr, len: count }
    }
    pub fn u32s(&self) -> &[u32] {
        unsafe { std::slice::from_raw_parts(self.ptr as *const u32, self.len as usize) }
    }
    pub fn u32s_mut(&mut self) -> &mut [u32] {
        unsafe { std::slice::from_raw_parts_mut(self.ptr as *mut u32, self.len as usize) }
    }
    pub fn u64s(&self) -> &[u64] {
        unsafe { std::slice::from_raw_parts(self.ptr as *const u64, self.len as usize) }
    }
    pub fn u64s_mut(&mut self) -> &mut [u64] {
        unsafe { std::slice::from_raw_parts_mut(self.ptr as *mut u64, self.len as usize) }
    }
    pub fn u8s(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.ptr, self.len as usize) }
    }
    pub fn close(self) {}
}

// a windowed read of a file (the corpus reads; never the whole file
// in RAM unless the window asks)
pub fn read_window(mut f: &fs::File, at: u64, want: u64, out: &mut Vec<u8>) {
    out.clear();
    out.resize(want as usize, 0);
    let mut done = 0usize;
    while done < want as usize {
        let n = f.read_at(&mut out[done..], at + done as u64)
            .unwrap_or_else(|_| die("corpus read"));
        if n == 0 { die("corpus short read (EOF inside a window)"); }
        done += n;
    }
    let _ = done;
}
