//! Three explicit real product rungs. Input: bounded tagged row43 pipe. The
//! ordinary frozen PRC1 and SXLS wires are unchanged. The launcher confines ALL
//! temporary files to a private block image; neither a row spool nor m14 here.
use super::{laws::Laws, overlap_row43, unit};
use std::{collections::VecDeque, fs, io::{Read,Write}, os::unix::fs::{FileTypeExt,PermissionsExt}, path::Path};

pub fn setup(rung:&str,n:u64,prefix:&str,bands:&str,raw:&str,cap:u64) {
    let root=Path::new(bands).parent().unwrap();let d=root.parent().unwrap();
    assert_eq!(root.canonicalize().unwrap(),root);assert_eq!(root.file_name().unwrap(),"quota");
    assert!(d.file_name().unwrap().to_str().unwrap().starts_with(&format!("rolling-build-{rung}-")));
    assert_eq!(d.metadata().unwrap().permissions().mode()&0o777,0o700);
    assert_eq!(Path::new(raw),d.join("in/raw"));assert_eq!(Path::new(prefix),root.join("merged/frag"));assert_eq!(Path::new(bands),root.join("bands"));
    let bytes=match rung {"1g"=>1_000_000_665,"10g"=>10_000_000_000,"100g"=>100_000_000_000,_=>panic!("unknown ladder rung")};
    assert_eq!((n,cap),(bytes,2*bytes));assert!(fs::metadata("/proc/self/fd/0").unwrap().file_type().is_fifo());
    assert!(!d.join("stop.requested").exists());assert!(root.join("inverse.u64").is_file());
    for p in [root.join("merged"),root.join("merged/modeb"),root.join("bands")] {fs::create_dir(p).expect("fresh ladder handoff");}
}

pub fn run(rung:&str,raw:&[u8],remap:&[u8],prefix:&str,bands:&str,laws:&Laws) {
    let n=raw.len() as u64;let win=super::laws::env_u64("CROSS_V2_PIECE_WINDOW",0);let sw=super::laws::env_u64("SS_LCP_SLICE_WINDOW",0);
    assert!((6..=128).contains(&win) && (1..=512).contains(&sw) && remap.len()==256);
    assert_eq!(super::laws::env_u64("XSA_BATCH_SXLS_ESCAPES",0),1);assert!(!Path::new(&format!("{prefix}.pieces")).exists());
    let mut cutter=unit::PieceCutter::new(prefix,n+1,win);
    let mut lcps=Vec::with_capacity(262144);let mut live=VecDeque::new();let mut seq=0;let mut base=0;
    let mut publish=|v:&mut Vec<u64>|{unit::emit_lcp_slice(bands,seq,v.len(),v,base,&mut live,sw,laws);seq+=1;base+=v.len() as u64;v.clear();};
    // The two smaller rungs have verified v8 payloads but no pinned complete
    // v10 product. Check raw rows once in the existing sink. At 100 GB use the
    // independent historical whole-v10 SHA; do not repeat the expensive check.
    let first=rung!="100g";let mut prev=n;let(mut ends,mut high,mut maxh,mut runs,mut last)=(0u64,0u64,0u64,0u64,257u32);
    let mut input=std::io::stdin().lock();let mut buf=vec![0u8;32768*12];let mut rank=0u64;
    while rank<=n {
        laws.stop_check("rolling-row43");let rows=(n+1-rank).min(32768) as usize;let packet=&mut buf[..12*rows];input.read_exact(packet).expect("short row43 packet");
        // Withhold the final packet until exact EOF; no false producer.done.
        if rank+rows as u64==n+1 {assert_eq!(input.read(&mut [0]).unwrap(),0,"extra row43 bytes");}
        for frame in packet.chunks_exact(12){let(p,h,b)=overlap_row43::decode(frame.try_into().unwrap());
            assert!(p<=n && (p==n)==(rank==0) && h<=n-prev.max(p) && (b==256)==(p==0),"row geometry");
            if first {assert_eq!(b,if p==0{256}else{u32::from(raw[p as usize-1])},"raw BWT");let(p,q,l)=(p as usize,prev as usize,h as usize);
                assert!(raw[p..p+l]==raw[q..q+l],"false LCP");assert!(rank==0 || (p!=q && (q+l==raw.len() || (p+l<raw.len() && raw[q+l]<raw[p+l]))),"suffix order or LCP");}
            let c=if p==0{0}else{remap[b as usize]};assert_eq!(c==0,p==0,"invalid remap");ends+=u64::from(p==0);high+=u64::from(p>u32::MAX as u64);maxh=maxh.max(h);
            if b!=last {runs+=1;last=b;}prev=p;cutter.feed_row(p,c);lcps.push(h);rank+=1;
            if lcps.len()==262144 && rank<n+1 {publish(&mut lcps);}
        }
    }
    assert_eq!((ends,high,rank),(1,n.saturating_sub(u32::MAX as u64),n+1));
    if first {let tail=&raw[prev as usize..];let mut pos=0usize;for chunk in raw.chunks(65536){laws.stop_check("rolling-final-rank");for (j,&x) in chunk.iter().enumerate(){if x>=tail[0]{assert!(x==tail[0] && (pos+j==prev as usize || raw[pos+j..]<=*tail),"not maximum raw suffix");}}pos+=chunk.len();}}
    if !lcps.is_empty(){publish(&mut lcps);}assert_eq!(base,n+1);cutter.finish();
    fs::File::create_new(Path::new(bands).join("producer.done")).unwrap().flush().unwrap();
    eprintln!("ROLLING_V10_SINK rung={rung} rows={rank} raw_edges_checked={first} final_pos={prev} final_rank_checked={first} above_u32={high} max_lcp={maxh} runs={runs} slices={seq} terminal=1");
}
