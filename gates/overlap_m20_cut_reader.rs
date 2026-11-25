//! One-shot observer of selected shallow histograms. `frozen.rs` is assembled
//! from the exact historical ctx.rs by the tool qualifier; no producer calls it.
#[allow(dead_code)] #[path="frozen.rs"] mod frozen;
use std::collections::BTreeSet;
use std::ffi::c_void;
use std::fs::{self, File};
use std::io::{BufWriter,Write};
use std::os::fd::{FromRawFd,AsRawFd};
use std::os::unix::fs::MetadataExt;
use std::path::{Path,PathBuf};
use std::time::{Duration,SystemTime,UNIX_EPOCH};
use frozen::{Builder,ChildView,Trie};
fn die(s:&str)->! {panic!("{s}")}
const SEP:u8=30;const SLOTS:u64=192000000;
extern "C" {fn mmap(p:*mut c_void,n:usize,prot:i32,flags:i32,fd:i32,off:i64)->*mut c_void; fn munmap(p:*mut c_void,n:usize)->i32; fn madvise(p:*mut c_void,n:usize,adv:i32)->i32;fn getpagesize()->i32;}
fn now()->u64{SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs()}
fn guard(end:u64){if now()>=end || !matches!(Path::new("stop.requested").try_exists(),Ok(false)){eprintln!("STOP_OBSERVED phase=m20-shallow-cuts");std::process::exit(75);}}
fn scope()->PathBuf{let p=fs::read_to_string("/proc/self/cgroup").unwrap();let cg=PathBuf::from(format!("/sys/fs/cgroup{}",p.trim().strip_prefix("0::").unwrap()));for (f,v) in [("memory.max","268435456"),("cpu.max","100000 100000"),("memory.swap.max","0")]{assert_eq!(fs::read_to_string(cg.join(f)).unwrap().trim(),v);}let s=fs::read_to_string("/proc/self/status").unwrap();let cores=s.lines().find_map(|x|x.strip_prefix("Cpus_allowed_list:")).unwrap().trim();assert!(cores.parse::<u32>().is_ok());let d=std::env::current_dir().unwrap();assert_eq!(d.canonicalize().unwrap(),d);assert_eq!(d.metadata().unwrap().mode()&0o777,0o700);d}
struct Dump<W:Write>{out:W,nodes:usize,edges:usize,bytes:usize,stop:u64}
impl<W:Write> Dump<W>{
 fn line(&mut self,s:&str){self.bytes+=s.len()+1;assert!(self.bytes<=8<<20);writeln!(self.out,"{s}").unwrap();}
 fn node(&mut self,t:&Trie,p:&[u8],id:u32)->Vec<ChildView>{guard(self.stop);assert_eq!(t.node_depth(id),p.len() as u64);let e=t.children(id);let n=t.count(id);let mut k=String::new();for x in p{use std::fmt::Write;write!(&mut k,"{x:02x}").unwrap();}if k.is_empty(){k.push('-');}let mut line=format!("NODE {} {} {} {} {}",k,id,p.len(),n,e.len());let mut sum=0u64;let mut last=None;
  for x in &e{use std::fmt::Write;assert!(x.code<=256 && last.is_none_or(|prev|prev<x.code) && x.count>0);last=Some(x.code);sum=sum.checked_add(x.count).unwrap();write!(&mut line," {}:{}:{}",x.code,x.count,x.node.map(i64::from).unwrap_or(-1)).unwrap();}
  assert_eq!(n,sum);self.nodes+=1;self.edges+=e.len();assert!(self.nodes<=4096 && self.edges<=524288);self.line(&line);e}
 fn walk(&mut self,t:&Trie,p:&mut Vec<u8>,id:u32,slots:u64){assert!((2..=8).contains(&p.len()) && !p.contains(&SEP));let children=self.node(t,p,id);for e in children{if e.code!=0 && e.count>slots && p.len()<8{p.push((e.code-1) as u8);self.walk(t,p,e.node.unwrap(),slots);p.pop();}}}
}
fn observe(payload:&[u8],target:bool,end:u64){let t=Trie::load(payload);let slots=if target{SLOTS}else{80};assert_eq!((t.depth,t.boundary_aware,t.source,t.source_mem),(8,1,3,0));if target{assert_eq!((t.corpus_len,t.n,t.nodes,t.edges,t.min_support,t.docs_min_support),(100000000000,100000000001,21707038,551286102,1000,1000));}else{assert!(t.corpus_len<=16384 && t.min_support==2);}
 let text=fs::read_to_string("requests").unwrap();assert!(text.len()<=4096);let mut req=BTreeSet::new();for s in text.lines(){let k:u32=s.parse().unwrap();assert!(k<256*257 && k%257>0 && k/257!=SEP as u32 && k%257!=1+SEP as u32 && req.insert(k));}assert!(!req.is_empty() && req.len()<=256 && (!target || req.len()==115));let mut bytes:BTreeSet<u8>=req.iter().map(|k|(k/257)as u8).collect();bytes.insert(SEP);
 let mut d=Dump{out:BufWriter::new(std::io::stdout().lock()),nodes:0,edges:0,bytes:0,stop:end};d.line(&format!("M20 version=3 depth={} aware={} support={} rows={} corpus={} nodes={} edges={}",t.depth,t.boundary_aware,t.min_support,t.n,t.corpus_len,t.nodes,t.edges));assert_eq!(t.root() as u64+1,t.nodes);let root=d.node(&t,&[],t.root());for x in bytes{let id=root.iter().find(|e|e.code==1+x as u16).unwrap().node.unwrap();let c=d.node(&t,&[x],id);for &k in req.range(x as u32*257..(1+x as u32)*257){let y=(k%257)as u16;let child=c.iter().find(|e|e.code==y).unwrap();assert!(child.count>slots);d.walk(&t,&mut vec![x,(y-1)as u8],child.node.unwrap(),slots);}}
 d.line(&format!("DONE {} {}",d.nodes,d.edges));d.out.flush().unwrap();}
fn main(){let a:Vec<String>=std::env::args().collect();assert!(a.len()==3 || a.len()==4);let mode=&a[1];let end=a[2].parse::<u64>().unwrap();assert!(end<=now()+180);guard(end);let d=scope();let _watch=std::thread::spawn(move||loop{guard(end);std::thread::sleep(Duration::from_millis(50));});
 if mode=="fixture"{assert!(a.len()==3 && d.parent().unwrap().parent()==Some(Path::new("/tmp")) && d.parent().unwrap().file_name().unwrap().to_str().unwrap().starts_with("m20-cut-tools-") && d.file_name().unwrap()=="fixture");let raw=fs::read("toy.raw").unwrap();let starts:Vec<usize>=fs::read_to_string("toy.starts").unwrap().lines().map(|x|x.parse().unwrap()).collect();assert!(raw.len()<=16384 && !raw.is_empty() && !starts.is_empty() && starts[0]==0 && *starts.last().unwrap()<raw.len() && starts.windows(2).all(|z|z[0]<z[1]));let mut sa:Vec<usize>=(0..=raw.len()).collect();sa.sort_by(|&a,&b|raw[a..].cmp(&raw[b..]));let mut b=Builder::new(8,2,2);b.set_boundary_aware();b.set_source(3,0);for p in sa{let doc=starts.partition_point(|&s|s<=p)-1;let rem=raw.len()-p;let avail=starts.get(doc+1).copied().unwrap_or(raw.len())-p;b.feed(&raw[p..p+rem.min(9)],rem as u64,avail as u64,doc as u32);}let (m,_)=b.finish(raw.len() as u64+1,raw.len() as u64);observe(&m,false,end);
 }else{assert!(mode=="archive" && a.len()==4 && d.parent()==Some(Path::new("/mnt/nvme2n1/erikg/TAPEREADER/sandbox")) && d.file_name().unwrap().to_str().unwrap().starts_with("m20-target-cuts-"));let fd:i32=a[3].parse().unwrap();assert!(fd>=3);let f=unsafe{File::from_raw_fd(fd)};let st=f.metadata().unwrap();assert!(st.is_file() && st.dev()==66308 && st.ino()==32215060707 && st.len()==65533578480);assert_eq!(fs::read_link(format!("/proc/self/fd/{fd}")).unwrap(),Path::new("/mnt/nvme2n1/erikg/PILE100-REHEARSAL/pile-head100.sxi2"));let (off,len)=(63163489376usize,2333923624usize);assert_eq!(unsafe{getpagesize()},4096);let delta=off%4096;let size=len+delta;let ptr=unsafe{mmap(std::ptr::null_mut(),size,1,2,f.as_raw_fd(),(off-delta)as i64)};assert_ne!(ptr as isize,-1);assert_eq!(unsafe{madvise(ptr,size,1)},0);let payload=unsafe{std::slice::from_raw_parts((ptr as *const u8).add(delta),len)};observe(payload,true,end);assert_eq!(unsafe{munmap(ptr,size)},0);}
 guard(end);
}
