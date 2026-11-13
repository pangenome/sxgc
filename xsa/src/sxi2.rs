//! SXI2 member codecs. All bit counts are exact; unused high bits are zero.
use super::{die, sxi};
use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom, Write};
use std::sync::Arc;

fn need(ok: bool, msg: &str) {
    if !ok { die(&format!("SXI2: {msg}")); }
}
fn u64le(b: &[u8]) -> u64 { u64::from_le_bytes(b.try_into().unwrap()) }
fn get_u64(f: &mut impl Read) -> u64 {
    let mut b = [0; 8]; f.read_exact(&mut b).unwrap_or_else(|e| die(&e.to_string())); u64le(&b)
}
fn bit(data: &[u8], i: u64) -> u8 { (data[(i / 8) as usize] >> (i % 8)) & 1 }
fn padding(data: &[u8], bits: u64) {
    if bits % 8 != 0 && !data.is_empty() {
        need(data[data.len() - 1] >> (bits % 8) == 0, "nonzero bit padding");
    }
}
fn packed_bits(data: &[u8], pos: u64, width: u32) -> u64 {
    if width==0 {return 0;}
    let p=(pos/8) as usize;let shift=pos%8;
    let bytes=((shift+width as u64+7)/8) as usize;
    let mut raw=0u128;
    for k in 0..bytes {raw|=(data[p+k] as u128)<<(8*k);}
    ((raw>>shift)&((1u128<<width)-1)) as u64
}
struct DiskBits { f: BufReader<File>, bits: u64, pos: u64, byte: u8 }
impl DiskBits {
    fn new(path: &str, off: u64, bits: u64) -> Self {
        let mut f=BufReader::new(File::open(path).unwrap());
        f.seek(SeekFrom::Start(off)).unwrap();
        Self{f,bits,pos:0,byte:0}
    }
    fn next(&mut self) -> u8 {
        need(self.pos<self.bits,"bitstream underflow");
        if self.pos%8==0 { let mut b=[0];self.f.read_exact(&mut b).unwrap();self.byte=b[0]; }
        let x=(self.byte>>(self.pos%8))&1;self.pos+=1;x
    }
    fn value(&mut self, width: u32) -> u64 {
        let mut v=0;for i in 0..width {v|=(self.next() as u64)<<i;}v
    }
    fn end(&mut self) {
        need(self.pos==self.bits,"bitstream trailing bits");
        if self.bits%8!=0 { need(self.byte>>(self.bits%8)==0,"nonzero bit padding"); }
    }
}
pub fn chi(path: &str, m: &sxi::Member, n: u64, out: &mut impl Write) {
    let mut f=BufReader::new(File::open(path).unwrap());f.seek(SeekFrom::Start(m.offset)).unwrap();
    let width=get_u64(&mut f);let low_bits=get_u64(&mut f);let high_bits=get_u64(&mut f);
    need(width<=63 && low_bits==m.count*width && high_bits==if m.count==0 {0} else {(n>>width)+m.count+1},
        "EF dimensions");
    need(m.bytes==24+(low_bits+7)/8+(high_bits+7)/8,"EF size");
    let mut low=DiskBits::new(path,m.offset+24,low_bits);
    let mut high=DiskBits::new(path,m.offset+24+(low_bits+7)/8,high_bits);
    let mut ones=0u64;let mut previous=0u64;
    for pos in 0..high_bits {
        if high.next()==1 {
            need(ones<m.count && pos>=ones,"EF upper order");
            let value=((pos-ones)<<width)|low.value(width as u32);
            need(value<=n && (ones==0||value>previous),"EF value order/range");
            out.write_all(&value.to_le_bytes()).unwrap_or_else(|e| die(&e.to_string()));
            previous=value;ones+=1;
        }
    }
    need(ones==m.count,"EF count");low.end();high.end();
}

// The phi, escape, LF-start and lite-anchor codecs that used to live here are
// GONE with the shapes that carried their members (m2/m3 phi+tails, m8/m9, the
// packed head/tail samples). The supported shape's run stream is decoded by the
// query kernel's block cache (qlite::Index), and its locate anchors are m11,
// read cell by cell where they are needed. Nothing in this file reads an
// artifact that is not part of SXI2 v10.
