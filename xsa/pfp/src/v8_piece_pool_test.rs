use super::{v8_piece_pool::{self as v8, Pool}, PieceCutter};
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::Read;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

struct Dir(PathBuf);
impl Dir {
    fn new() -> Self {
        static ID: AtomicUsize = AtomicUsize::new(0);
        let p = std::env::temp_dir().join(format!("v8-slot-{}-{}", std::process::id(), ID.fetch_add(1, Ordering::Relaxed)));
        fs::create_dir(&p).unwrap(); Self(p)
    }
}
impl Drop for Dir { fn drop(&mut self) { if !std::thread::panicking() { fs::remove_dir_all(&self.0).unwrap(); } } }
fn image(p: &mut Pool, i: u64, bytes: &[u8]) -> std::io::Result<()> {
    p.publish(i,bytes.len() as u64,|f| f.write_all(bytes))
}
fn files(d: &Path, into: &mut BTreeMap<(u64,u64),u64>) {
    for e in d.read_dir().unwrap() { let p=e.unwrap().path(); let m=fs::symlink_metadata(&p).unwrap();
        if m.is_dir() { files(&p,into); } else { into.insert((m.dev(),m.ino()),m.len()); }
    }
}
#[test]
fn open_handles_are_not_extra_extents_and_ack_is_not_unlink() {
    let d=Dir::new(); let orig=d.0.join("p-0-0"); let mut pool=Pool::new(&d.0,1,31,2,1).unwrap();
    image(&mut pool,0,&[7;31]).unwrap(); let st=fs::metadata(&orig).unwrap(); assert_eq!(st.nlink(),2);
    fs::write(d.0.join("frag.runcols.consumed"),b"0\n").unwrap();
    assert!(image(&mut pool,1,b"x").unwrap_err().to_string().contains("still linked"));
    assert_eq!(fs::read(&orig).unwrap(),vec![7;31]); assert!(!d.0.join("p-1-0").exists());
    fs::remove_file(&orig).unwrap(); assert!(image(&mut pool,1,b"x").is_err()); // failed owner cannot retry
    let e=Dir::new(); let mut pool=Pool::new(&e.0,1,31,3,1).unwrap();
    let mut fd: Option<File>=None;
    for (i,blob) in [&[9u8;31][..],&[8u8;31][..],b"x"].iter().enumerate() {
        image(&mut pool,i as u64,blob).unwrap(); let p=e.0.join(format!("p-{i}-0"));
        let id=p.metadata().unwrap(); if i==0 { fd=Some(File::open(&p).unwrap()); }
        let kept=fd.as_ref().unwrap().metadata().unwrap(); assert_eq!((id.dev(),id.ino()),(kept.dev(),kept.ino()));
        let mut inodes=BTreeMap::new(); files(&e.0,&mut inodes); inodes.insert((kept.dev(),kept.ino()),kept.len());
        assert_eq!(inodes.len(),1); assert_eq!(inodes.values().sum::<u64>(),blob.len() as u64);
        assert_eq!(fs::read(&p).unwrap(),*blob); fs::remove_file(p).unwrap();
    }
    // Deliberately HOLD a reader beyond unlink in this test. Its extent remains
    // countable, NOT automatically zero at unlink. Production finish closes first.
    pool.retire().unwrap(); let mut f=fd.unwrap(); assert_eq!(f.metadata().unwrap().nlink(),0);
    let mut x=Vec::new(); f.read_to_end(&mut x).unwrap(); assert_eq!(x,b"x");
}
#[test]
fn failures_precede_growth_or_publication_and_retain_evidence() {
    for (seq,size,body,actual) in [(0,32,32,None),(1,1,1,None),(0,5,6,Some(0)),(0,5,4,Some(4))] {
        let d=Dir::new(); let mut p=Pool::new(&d.0,2,31,3,1).unwrap();
        assert!(p.publish(seq,size,|f| f.write_all(&vec![0;body])).is_err());
        assert_eq!(fs::metadata(d.0.join("payload-slots/0")).ok().map(|m| m.len()),actual);
        assert!(!d.0.join(format!("p-{seq}-0")).exists()); assert!(p.retire().is_err());
    }
    for which in ["existing","symlink","substitute","premature-close"] {
        let d=Dir::new(); let mut pool=Pool::new(&d.0,1,31,2,1).unwrap(); assert!(Pool::fixed(&d.0).is_err());
        image(&mut pool,0,b"previous").unwrap(); fs::remove_file(d.0.join("p-0-0")).unwrap();
        let slot=d.0.join("payload-slots/0"); let dest=d.0.join("p-1-0");
        match which {
            "existing"=>fs::write(&dest,b"original").unwrap(),
            "symlink"=>std::os::unix::fs::symlink(&slot,&dest).unwrap(),
            "substitute"=>{fs::rename(&slot,d.0.join("moved")).unwrap(); fs::write(&slot,b"original").unwrap();},
            _=>{assert!(pool.retire().is_err()); assert_eq!(fs::read(&slot).unwrap(),b"previous");continue;}
        }
        assert!(image(&mut pool,1,b"replacement").is_err());
        if which=="substitute" {assert_eq!(fs::read(&slot).unwrap(),b"original"); assert_eq!(fs::read(d.0.join("moved")).unwrap(),b"previous");}
        else {assert_eq!(fs::read(&slot).unwrap(),b"previous");if which=="existing" {assert_eq!(fs::read(&dest).unwrap(),b"original");}}
    }
}
// Independent wire, including padding. No packing routine from the cutter.
fn packed(values:impl Iterator<Item=u64>, count:usize, width:usize)->Vec<u8> {
    let mut out=vec![0;8*(count*width).div_ceil(64)];
    for (j,v) in values.enumerate() { for k in 0..width { let b=j*width+k; out[b/8]|=(((v>>k)&1) as u8)<<(b%8); } } out
}
type Run=(u8,u32,u64,u64);
fn expected(seq:usize, first:usize, w:usize, r:&[Run])->Vec<u8> {
    let mut b=b"PRC1".to_vec(); b.extend_from_slice(&(seq as u64).to_le_bytes()); b.extend_from_slice(&0u32.to_le_bytes());
    for x in [first,r.len()] { b.extend_from_slice(&(x as u64).to_le_bytes()); } b.extend_from_slice(&(w as u32).to_le_bytes()); b.extend_from_slice(&[0;8]);
    b.extend(r.iter().map(|x|x.0)); for x in r {b.extend_from_slice(&x.1.to_le_bytes());}
    b.extend(packed(r.iter().map(|x|x.2),r.len(),w)); b.extend(packed(r.iter().map(|x|x.3),r.len(),w)); b
}
#[test]
fn cutter_generated_stream_matches_wire_across_reuse_and_terminal() {
    assert_eq!(v8::piece_bytes(27,65536).unwrap(),v8::FULL);assert_eq!(v8::FULL*78,60_067_176);
    for (band,count,slots) in [(2,67,3),(31,124,2),(64,301,1),(1024,8195,5),(65536,65539,1)] {
        let dir=Dir::new(); let prefix=dir.0.join("frag"); let pieces=dir.0.join("frag.pieces");
        let lens:Vec<u32>=(0..count).map(|i|(1+(i*7)%19) as u32).collect(); let n=lens.iter().map(|&l|u64::from(l)).sum::<u64>();
        let w=(64-n.leading_zeros()) as usize;let cap=v8::piece_bytes(w as u32,band as u64).unwrap();
        let mut c=PieceCutter::new(prefix.to_str().unwrap(),n,(count+1) as u64); c.band_size=band as u64;
        c.pool=Some(Pool::new(&pieces,slots,cap,count as u64,band as u64).unwrap());
        let mut runs=Vec::new(); let pos=|i:u64|n-1-i; let mut at=0u64;let mut ids=BTreeSet::new();
        for (i,&len) in lens.iter().enumerate() {
            let ch=(i%251) as u8; runs.push((ch,len,pos(at),pos(at+u64::from(len)-1)));
            for r in at..at+u64::from(len) {c.feed_row(pos(r),ch);} at+=u64::from(len);
            if i!=0 && i%band==0 {
                let s=i/band-1;let p=pieces.join(format!("p-{s}-0"));
                assert_eq!(expected(s,i-band,w,&runs[i-band..i]),fs::read(&p).unwrap());
                let st=fs::metadata(&p).unwrap();ids.insert((st.dev(),st.ino()));assert!(ids.len()<=slots && st.nlink()==2);
                fs::remove_file(p).unwrap(); // model only the post-durable unlink, NOT a finish proof
            }
        }
        let s=(count-1)/band;c.finish();let p=pieces.join(format!("p-{s}-0"));
        assert_eq!(fs::read(&p).unwrap(),expected(s,s*band,w,&runs[s*band..]));assert_eq!(p.metadata().unwrap().nlink(),1);
        assert!(!pieces.join("payload-slots").exists());let term=fs::read(pieces.join("terminal")).unwrap();assert_eq!(&term[..4],b"BPT1");
        assert_eq!(u64::from_le_bytes(term[12..20].try_into().unwrap()),count as u64);assert_eq!(dir.0.join("frag.runexc").metadata().unwrap().len(),16);
    }
}
