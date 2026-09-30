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
pub fn runs(path: &str, m: &sxi::Member, n: u64, r: u64) -> (Vec<u64>, Vec<u8>, Vec<u32>) {
    let mut f = BufReader::new(File::open(path).unwrap());
    f.seek(SeekFrom::Start(m.offset)).unwrap();
    let mut c = vec![0u64; 256];
    for x in &mut c { *x = get_u64(&mut f); }
    let mut lens = [0u8; 256]; f.read_exact(&mut lens).unwrap();
    let hbits = get_u64(&mut f); let lbits = get_u64(&mut f);
    let hbytes = hbits.checked_add(7).unwrap_or_else(|| die("SXI2: head bits overflow")) / 8;
    let lbytes = lbits.checked_add(7).unwrap_or_else(|| die("SXI2: len bits overflow")) / 8;
    need(m.bytes == 2320 + hbytes + lbytes && hbits >= r && lbits >= r, "run member size");
    let mut hd = vec![0; hbytes as usize]; let mut ld = vec![0; lbytes as usize];
    f.read_exact(&mut hd).unwrap(); f.read_exact(&mut ld).unwrap();
    padding(&hd, hbits); padding(&ld, lbits);
    let mut order: Vec<(u8,u8)> = lens.iter().enumerate().filter(|(_, &v)| v != 0)
        .map(|(i,&v)| (v,i as u8)).collect();
    order.sort_unstable();
    need(!order.is_empty() && order.last().unwrap().0 <= 63, "Huffman lengths");
    let mut nodes = vec![[None::<usize>; 2]];
    let mut symbols: Vec<Option<u8>> = vec![None];
    let mut code = 0u64; let mut prev = 0;
    for (width, sym) in order {
        code = code.checked_shl((width-prev) as u32).unwrap();
        need(code < (1u64 << width), "Huffman code overflow");
        let mut node = 0usize;
        for j in (0..width).rev() {
            need(symbols[node].is_none(), "Huffman prefix conflict");
            let side = ((code >> j) & 1) as usize;
            if nodes[node][side].is_none() {
                nodes[node][side] = Some(nodes.len()); nodes.push([None; 2]); symbols.push(None);
            }
            node = nodes[node][side].unwrap();
        }
        need(symbols[node].is_none() && nodes[node] == [None;2], "Huffman collision");
        symbols[node] = Some(sym); code += 1; prev = width;
    }
    let mut heads = Vec::with_capacity(r as usize); let mut lengths = Vec::with_capacity(r as usize);
    let mut hp = 0u64; let mut lp = 0u64; let mut counts = [0u64;256]; let mut sum = 0u64;
    for _ in 0..r {
        let mut node = 0;
        loop {
            need(hp < hbits, "truncated Huffman");
            node = nodes[node][bit(&hd,hp) as usize].unwrap_or_else(|| die("SXI2: invalid Huffman path"));
            hp += 1;
            if let Some(sym) = symbols[node] { heads.push(sym); break; }
        }
        let mut zeros = 0u32;
        while lp < lbits && bit(&ld,lp)==0 { zeros += 1; lp += 1; need(zeros < 32, "gamma length overflow"); }
        need(lp + (zeros as u64) < lbits, "truncated gamma");
        let mut value = 0u64;
        for _ in 0..=zeros { value = (value << 1) | bit(&ld,lp) as u64; lp += 1; }
        need(value > 0 && value <= u32::MAX as u64 && value <= n - sum, "run length");
        sum += value; counts[*heads.last().unwrap() as usize] += value;
        lengths.push(value as u32);
    }
    need(hp==hbits && lp==lbits && sum==n, "run bit counts/sum");
    let mut acc=0;for i in 0..256 { need(c[i]==acc,"C table");acc+=counts[i]; }
    (c,heads,lengths)
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

#[derive(Clone)]
pub struct Phi {
    map: Arc<memmap2::Mmap>,
    off: usize,
    compact: bool,
    u_low_off: usize,
    u_high_off: usize,
    v_off: usize,
    run_off: usize,
    u_low_width: u32,
    u_high_bits: u64,
    v_width: u32,
    run_width: u32,
    select: Vec<u64>,
    inverse: Vec<u32>,
    r: u64,
    n: u64,
    escape: Vec<(u64,u64,u64,u64)>, // run, domain start, length, bit offset
    payload: usize,
    width: u32,
}
impl Phi {
    fn packed(&self, off: usize, pos: u64, width: u32) -> u64 {
        if width==0 {return 0;}
        let p=off+(pos/8) as usize;let shift=pos%8;
        let bytes=((shift+width as u64+7)/8) as usize;
        let mut raw=0u128;
        for k in 0..bytes {raw|=(self.map[p+k] as u128)<<(8*k);}
        ((raw>>shift)&((1u128<<width)-1)) as u64
    }
    fn edge(&self, i: u64) -> (u64,u64,u64) {
        if self.compact {
            let sample=(i/64) as usize;
            let mut pos=self.select[sample];
            for _ in 0..i%64 {
                pos+=1;while bit(&self.map[self.u_high_off..],pos)==0 {pos+=1;}
            }
            let u=((pos-i)<<self.u_low_width)
                |self.packed(self.u_low_off,i*self.u_low_width as u64,self.u_low_width);
            let v=self.packed(self.v_off,i*self.v_width as u64,self.v_width);
            let run=self.packed(self.run_off,i*self.run_width as u64,self.run_width);
            return (u,v,run);
        }
        let p=self.off+i as usize*24;let b=&self.map[p..p+24];
        (u64le(&b[..8]),u64le(&b[8..16]),u32::from_le_bytes(b[16..20].try_into().unwrap()) as u64)
    }
    pub fn tail(&self, run: u64) -> u64 {
        need(self.compact&&run<self.r,"phi tail lookup");
        self.edge(self.inverse[run as usize] as u64).0
    }
    pub fn head(&self, run: u64) -> u64 {
        need(self.compact&&run<self.r,"phi head lookup");
        let previous=if run==0 {self.r-1}else{run-1};
        self.edge(self.inverse[previous as usize] as u64).1
    }
    pub fn load(path: &str, c: &sxi::Container, frequencies: &[u64]) -> Self {
        let m=c.member(8);let e=c.member(9);
        need(m.count==c.r,"phi member count");
        let file=File::open(path).unwrap();
        let map=Arc::new(unsafe{memmap2::Mmap::map(&file).unwrap()});
        let mut phi=Self{map,off:m.offset as usize,compact:c.version==3,
            u_low_off:0,u_high_off:0,v_off:0,run_off:0,u_low_width:0,u_high_bits:0,v_width:0,run_width:0,
            select:Vec::new(),inverse:Vec::new(),r:c.r,n:c.n,escape:Vec::new(),payload:0,width:0};
        if phi.compact {
            need(m.bytes>=40,"compact phi header");
            let b=&phi.map[phi.off..phi.off+40];
            let vw=u64le(&b[0..8]);let rw=u64le(&b[8..16]);let ul=u64le(&b[16..24]);
            let lb=u64le(&b[24..32]);let hb=u64le(&b[32..40]);
            let expected_v=(64-c.n.saturating_sub(1).leading_zeros()).max(1) as u64;
            let expected_r=(64-c.r.saturating_sub(1).leading_zeros()).max(1) as u64;
            need(vw==expected_v&&rw==expected_r&&ul<=63&&lb==c.r*ul
                &&hb==(c.n>>ul)+c.r+1,"compact phi widths");
            let low_bytes=(lb+7)/8;let high_bytes=(hb+7)/8;
            let v_bytes=(c.r*vw+7)/8;let run_bytes=(c.r*rw+7)/8;
            need(m.bytes==40+low_bytes+high_bytes+v_bytes+run_bytes,"compact phi size");
            phi.u_low_width=ul as u32;phi.u_high_bits=hb;phi.v_width=vw as u32;phi.run_width=rw as u32;
            phi.u_low_off=phi.off+40;
            phi.u_high_off=phi.u_low_off+low_bytes as usize;
            phi.v_off=phi.u_high_off+high_bytes as usize;
            phi.run_off=phi.v_off+v_bytes as usize;
            padding(&phi.map[phi.u_low_off..phi.u_high_off],lb);
            padding(&phi.map[phi.u_high_off..phi.v_off],hb);
            padding(&phi.map[phi.v_off..phi.run_off],c.r*vw);
            padding(&phi.map[phi.run_off..phi.run_off+run_bytes as usize],c.r*rw);
            let mut ones=0u64;
            for pos in 0..hb {
                if bit(&phi.map[phi.u_high_off..],pos)!=0 {
                    if ones%64==0 {phi.select.push(pos);}
                    ones+=1;
                }
            }
            need(ones==c.r,"compact phi EF count");
            phi.inverse=vec![u32::MAX;c.r as usize];
        } else {need(m.bytes==24*c.r,"phi member size");}
        let mut gcd=0u64;
        for i in 0..256 {let next=if i==255{c.n}else{frequencies[i+1]};
            let v=next.checked_sub(frequencies[i]).unwrap_or_else(||die("SXI2: C table order"));
            if v!=0 {gcd=gcd_u64(gcd,v);}
        }
        let mut seen=if phi.compact {Vec::new()} else {vec![0u8;((c.r+7)/8) as usize]};
        let mut failed=Vec::new();
        let mut highpos=0u64;
        let mut previous_u=None;
        for i in 0..c.r {
            let (u,v,run)=if phi.compact {
                while highpos<phi.u_high_bits&&bit(&phi.map[phi.u_high_off..],highpos)==0 {highpos+=1;}
                need(highpos<phi.u_high_bits,"compact phi EF truncated");
                let u=((highpos-i)<<phi.u_low_width)
                    |phi.packed(phi.u_low_off,i*phi.u_low_width as u64,phi.u_low_width);
                highpos+=1;
                let v=phi.packed(phi.v_off,i*phi.v_width as u64,phi.v_width);
                let run=phi.packed(phi.run_off,i*phi.run_width as u64,phi.run_width);
                (u,v,run)
            } else {phi.edge(i)};
            need(u<c.n&&v<c.n&&run<c.r,"phi edge range");
            need(previous_u.is_none_or(|old|old<u),"phi edge order");previous_u=Some(u);
            if phi.compact {
                need(phi.inverse[run as usize]==u32::MAX,"duplicate phi run");
                phi.inverse[run as usize]=i as u32;
            } else {
                let s=&mut seen[(run/8) as usize];need(*s&(1<<(run%8))==0,"duplicate phi run");*s|=1<<(run%8);
            }
        }
        if phi.compact {
            let sparse=c.member(11);
            need(sparse.count==(c.r+1023)/1024&&sparse.bytes==16+8*sparse.count,"sparse anchor size");
            let b=&phi.map[sparse.offset as usize..(sparse.offset+sparse.bytes) as usize];
            need(u64le(&b[0..8])==10&&u64le(&b[8..16])==sparse.count,"sparse anchor header");
            for j in 0..sparse.count {
                let v=u64le(&b[(16+8*j) as usize..(24+8*j) as usize]);
                need(v==phi.tail(j*1024),"sparse anchor value");
            }
        }
        if gcd!=1 {for i in 0..c.r {
            let (u,_,run)=phi.edge(i);let next=phi.edge((i+1)%c.r).0;
            let size=if next>u {next-u} else {c.n-u+next};
            need(size>0,"empty phi domain");
            if size!=1 {failed.push((run,u,size));}
        }}
        need(e.count==failed.len() as u64,"escape count/criterion");
        if failed.is_empty() {need(e.bytes==0,"unexpected escape payload");return phi;}
        need(e.bytes>=40+32*e.count,"escape size");
        let b=&phi.map[e.offset as usize..(e.offset+e.bytes) as usize];
        need(&b[..8]==b"SXESC3\0\0"&&u64le(&b[8..16])==c.n&&u64le(&b[16..24])==c.r
            &&u64le(&b[24..32])==e.count,"escape header");
        let width=b[32] as u32;
        need(width==(64-c.n.saturating_sub(1).leading_zeros()).max(1)
            &&b[33..40].iter().all(|&v|v==0),"escape width/reserved");
        phi.width=width.max(1);
        phi.payload=e.offset as usize+40+32*e.count as usize;
        let mut end=0u64;
        for i in 0..e.count as usize {
            let p=40+32*i;let run=u64le(&b[p..p+8]);let start=u64le(&b[p+8..p+16]);
            let len=u64le(&b[p+16..p+24]);let offset=u64le(&b[p+24..p+32]);
            need(run<c.r&&start<c.n&&len>0&&offset==end
                &&phi.escape.last().is_none_or(|x|x.0<run),"escape directory");
            end=end.checked_add(len.checked_mul(phi.width as u64).unwrap_or_else(||die("SXI2: escape overflow")))
                .unwrap_or_else(||die("SXI2: escape overflow"));
            phi.escape.push((run,start,len,offset));
        }
        need((end+7)/8==e.bytes-40-32*e.count,"escape payload size");
        if end%8!=0 {need(b[b.len()-1]>>(end%8)==0,"escape padding");}
        failed.sort_unstable();
        need(failed.iter().zip(&phi.escape).all(|(&(r,u,l),&(er,eu,el,_))|r==er&&u==eu&&l==el),
            "escape domain mismatch");
        phi
    }
    pub fn successor(&self, value: u64) -> u64 {
        need(value<self.n,"phi value range");
        let mut lo=0u64;let mut hi=self.r;
        while lo<hi {let mid=(lo+hi)/2;if self.edge(mid).0<=value {lo=mid+1;}else{hi=mid;}}
        let i=if lo==0 {self.r-1}else{lo-1};
        let (u,v,run)=self.edge(i);
        let delta=if value>=u {value-u}else{self.n-u+value};
        if let Ok(j)=self.escape.binary_search_by_key(&run,|x|x.0) {
            let (_,start,len,off)=self.escape[j];
            let d=if value>=start{value-start}else{self.n-start+value};
            need(d<len,"escaped value outside domain");
            let bit=off+d*self.width as u64;let p=self.payload+(bit/8) as usize;
            let mut raw=0u128;
            let bytes=((bit%8+self.width as u64+7)/8) as usize;
            for k in 0..bytes {raw|=(self.map[p+k] as u128)<<(8*k);}
            let mask=(1u128<<self.width)-1;
            let result=((raw>>(bit%8))&mask) as u64;
            need(result<self.n,"escape value range");return result;
        }
        if v>=self.n-delta {v-(self.n-delta)} else {v+delta}
    }
}
fn gcd_u64(mut a:u64,mut b:u64)->u64{while b!=0{let t=a%b;a=b;b=t;}a}

#[derive(Clone)]
pub struct LfMap { map: Arc<memmap2::Mmap>, off: usize, width: u32, r: u64 }
impl LfMap {
    pub fn load(path:&str,c:&sxi::Container,heads:&[u8],lengths:&[u32],freq:&[u64])->Self {
        let m=c.member(10);
        let width=(64-c.n.saturating_sub(1).leading_zeros()).max(1);
        need(m.count==c.r&&m.bytes==8+(c.r*width as u64+7)/8,"LF map size");
        let file=File::open(path).unwrap();let map=Arc::new(unsafe{memmap2::Mmap::map(&file).unwrap()});
        let off=m.offset as usize+8;
        need(u64le(&map[m.offset as usize..off])==width as u64,"LF map width");
        padding(&map[off..(m.offset+m.bytes) as usize],c.r*width as u64);
        let lf=Self{map,off,width,r:c.r};
        let mut seen=[0u64;256];
        for i in 0..c.r as usize {
            let ch=heads[i] as usize;let value=freq[ch]+seen[ch];
            need(lf.start(i as u64)==value&&value<c.n,"LF map interval");
            seen[ch]+=lengths[i] as u64;
        }
        lf
    }
    pub fn start(&self,run:u64)->u64 {
        need(run<self.r,"LF run range");
        let pos=run*self.width as u64;let p=self.off+(pos/8) as usize;
        let shift=pos%8;let bytes=((shift+self.width as u64+7)/8) as usize;
        let mut raw=0u128;for k in 0..bytes {raw|=(self.map[p+k] as u128)<<(8*k);}
        ((raw>>shift)&((1u128<<self.width)-1)) as u64
    }
}
