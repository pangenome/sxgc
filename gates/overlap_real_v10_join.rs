// Compiled only with the pinned 6b87aa1 v10 Reader/append/embed, in a private
// tools tree. No generic fixture-size override, container rewrite, or copy path.
fn real_v10_join() {
    use sha2::{Digest, Sha256};
    use std::os::unix::fs::MetadataExt;
    assert_eq!(std::env::args().count(), 2);
    let cwd = std::env::current_dir().unwrap();
    assert_eq!(cwd.canonicalize().unwrap(), cwd);
    assert_eq!(cwd.metadata().unwrap().mode() & 0o777, 0o700);
    assert!(!cwd.join("stop.requested").exists());
    let index = "work.sxi2";
    let check = |name, limit| { let s = std::fs::symlink_metadata(name).unwrap();
        assert!(s.is_file() && s.nlink()==1 && s.len()>0 && s.len()<=limit); };
    let m20 = if std::env::args().nth(1).as_deref()==Some("real-v10-quota-join") { "quota/m20" } else { "m20" };
    check(index, 128 << 20); check(m20, 16 << 20); check("in/map/seps.bin", 68_344);
    let map = std::fs::read("in/map/seps.bin").unwrap();
    assert_eq!(format!("{:x}", Sha256::digest(&map)), "63bb635e128e00d06037eaef5ce9e83d343cc622509b155d010789e4a20654ac");
    let base = qlite::Reader::open(index).unwrap();
    assert_eq!((base.version,base.n), (7,100_000_504));
    assert_eq!(base.members.iter().map(|m| m.id).collect::<Vec<_>>(), [1,4,5,7,10,11,12,13]);
    drop(base);
    let trie = std::fs::read(m20).unwrap();
    let word = |i: usize| u64::from_le_bytes(trie[i*8..(i+1)*8].try_into().unwrap());
    assert_eq!([word(0),word(1),word(2),word(3),word(4),word(7),word(19),word(20),word(21)],
               [3,8,1000,100_000_504,100_000_503,1000,3,0,1]);
    assert_eq!(ctx::append_m20(index, &trie, 100_000_503), trie.len() as u64);
    drop(trie);
    let s = docstarts::embed(index, &docstarts::Source::DocsMap("in/map".into()), docstarts::SAMPLE_STRIDE, None);
    assert!(s.in_place && (s.docs,s.text_len,s.corpus_bytes)==(16_556,100_000_503,100_000_503));
    // Keep the side payload: its full reservation includes this lifetime.
    println!("REAL_V10_JOIN docs={} m15_bytes={} in_place=1",s.docs,s.member_bytes);
}
