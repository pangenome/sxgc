// Built beside an unmodified copy of the fixture overlap constructor.
mod overlap;
use std::io::{BufWriter, Write};
fn main() {
    let f = std::fs::File::open(std::env::args().nth(1).unwrap()).unwrap();
    assert!(f.metadata().unwrap().len() <= 8 << 20);
    let text = unsafe { memmap2::Mmap::map(&f) }.unwrap();
    let mut out = BufWriter::new(std::io::stdout().lock());
    overlap::construct(&text, |_| {}, |p, h, b| {
        for x in [p as u32, h as u32, if p == 0 {256} else {b as u32}] {
            out.write_all(&x.to_le_bytes()).unwrap();
        }
    });
    out.flush().unwrap();
}
