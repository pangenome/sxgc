//! Read-only census/distribution access-pattern probe (not a full index build).
#[path="../scan_order.rs"] mod scan_order;
use std::{fs, path::Path, io::Write, time::Instant};
fn map(d:&Path,n:&str)->memmap2::Mmap {unsafe{memmap2::Mmap::map(&fs::File::open(d.join(n)).unwrap()).unwrap()}}
fn slice<T>(m:&memmap2::Mmap)->&[T]{assert_eq!(m.len()%size_of::<T>(),0);unsafe{std::slice::from_raw_parts(m.as_ptr().cast(),m.len()/size_of::<T>())}}
fn io_read()->u64 {fs::read_to_string("/proc/self/io").unwrap().lines().find_map(|l| l.strip_prefix("read_bytes: ")).unwrap().parse().unwrap()}
fn main(){let a:Vec<_>=std::env::args().collect();let d=Path::new(&a[1]);let physical=a[2]=="physical";
 let t=map(d,"corpus");let r=map(d,"dict_refpos.bin");let l=map(d,"dict_len.bin");let p=map(d,"pa.bin");let s=map(d,"pstart.bin");
 let cg=std::path::PathBuf::from("/sys/fs/cgroup").join(fs::read_to_string("/proc/self/cgroup").unwrap().trim().strip_prefix("0::/").unwrap());
 for pass in 0..2 {let mut hist=vec![0u64;65536];let mut hash=vec![0u64;65536];let ts=Instant::now();let rb=io_read();
 scan_order::phrases(physical,t.len(),slice(&r),slice(&l),slice(&p),slice(&s),|id,pos,len|{let b=&t[pos as usize..pos as usize+len as usize];for i in 0..b.len(){let cell=b[i] as usize*256+b.get(i+1).copied().unwrap_or(0) as usize;hist[cell]+=1;hash[cell]=hash[cell].wrapping_add(((id as u64)<<32 | i as u64).wrapping_mul(11400714819323198485).rotate_left(23));}});
 println!("scan={} pass={} seconds={:.3} attributed_read_bytes={} memory_peak={} {}",a[2],pass,ts.elapsed().as_secs_f64(),io_read()-rb,fs::read_to_string(cg.join("memory.peak")).unwrap().trim(),fs::read_to_string(cg.join("memory.stat")).unwrap().lines().filter(|l|l.starts_with("workingset_refault_file ")||l.starts_with("pgmajfault ")).collect::<Vec<_>>().join(" "));
 let mut out=std::io::BufWriter::new(fs::File::create(format!("{}.{}",a[3],pass)).unwrap());for v in hist.iter().chain(hash.iter()){out.write_all(&v.to_le_bytes()).unwrap();}out.flush().unwrap();}}
