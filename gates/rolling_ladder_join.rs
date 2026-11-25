// Small CLI binding for the frozen v10 in-place joins, not an index converter.
fn rolling_join(){
    use std::os::unix::fs::MetadataExt;
    let cwd=std::env::current_dir().unwrap();assert_eq!(cwd.canonicalize().unwrap(),cwd);
    assert_eq!(cwd.metadata().unwrap().mode()&0o777,0o700);assert!(!cwd.join("stop.requested").exists());
    let raw=std::fs::metadata("in/raw").unwrap().len();let r=match raw{1_000_000_665=>"1g",10_000_000_000=>"10g",100_000_000_000=>"100g",_=>panic!("wrong rung")};
    assert!(cwd.file_name().unwrap().to_str().unwrap().starts_with(&format!("rolling-build-{r}-")));
    let p="work.sxi2";let base=qlite::Reader::open(p).unwrap();assert_eq!((base.n,base.version),(raw+1,7));
    assert_eq!(base.members.iter().map(|m|m.id).collect::<Vec<_>>(),[1,4,5,7,10,11,12,13]);drop(base);
    let t="quota/m20";let st=std::fs::symlink_metadata(t).unwrap();assert!(st.is_file() && st.nlink()==1 && st.len()<=raw/8);
    let b=std::fs::read(t).unwrap();let w=|j:usize|u64::from_le_bytes(b[8*j..8*j+8].try_into().unwrap());
    assert_eq!([w(0),w(1),w(2),w(3),w(4),w(7),w(19),w(20),w(21)],[3,8,1000,raw+1,raw,1000,3,0,1]);
    assert_eq!(ctx::append_m20(p,&b,raw),st.len());drop(b);
    let d=docstarts::embed(p,&docstarts::Source::DocsMap("in/map".into()),docstarts::SAMPLE_STRIDE,None);
    assert!(d.in_place && d.text_len==raw && d.corpus_bytes==raw);println!("ROLLING_JOIN n={raw} docs={} m20={} m15={}",d.docs,st.len(),d.member_bytes);
}
