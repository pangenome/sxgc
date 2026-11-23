// One-open, boundary-aware native window paging. IDs are local to this pinned mount;
// caller owns admission. Never scan the text or infer a delimiter from a byte.
fn workspace_pages(index: &str) -> std::io::Result<()> {
    use std::collections::HashMap;
    use std::io::{Read, Write};
    let reader = open_reader(index);
    let trie = load_trie(&reader);
    if trie.depth != 8 || trie.boundary_aware != 1 || trie.nodes >= u32::MAX as u64 {
        return Err(std::io::Error::other("workspace requires a depth-8 boundary-aware trie"));
    }
    let root = trie.root() + 1; // zero = unmaterialized, not a byte or END
    let mut input = std::io::BufReader::new(std::io::stdin().lock());
    let mut out = std::io::BufWriter::new(std::io::stdout().lock());
    let mut cache: HashMap<(u32,u8),u32> = HashMap::new();
    out.write_all(b"W3BP0001")?;
    out.write_all(&root.to_le_bytes())?;
    out.write_all(&(trie.nodes as u32).to_le_bytes())?;
    out.flush()?;
    loop {
        let mut op = [0];
        if input.read(&mut op)? == 0 { break; }
        let mut word = [0;4]; input.read_exact(&mut word)?;
        let n = u32::from_le_bytes(word) as usize;
        if !(1..=4096).contains(&n) { return Err(std::io::Error::other("workspace frame capacity")); }
        match op[0] {
            b'S' => {
                let mut text = vec![0;n]; input.read_exact(&mut text)?;
                let mut active = [0u32;8]; // ALWAYS reset the logical frontier at a frame boundary
                for byte in text {
                    let old = active;
                    for k in 0..8 {
                        let prev = if k == 0 { root } else { old[k-1] };
                        active[k] = if prev == 0 { 0 } else {
                            *cache.entry((prev,byte)).or_insert_with(|| {
                                let node = prev-1;
                                let want = byte as u16 + 1;
                                let (mut lo,mut hi) = (0,trie.node(node).1);
                                while lo < hi {
                                    let mid = (lo+hi)/2;
                                    let edge = trie.edge_at(node,mid);
                                    if edge.code == want { return edge.node.map_or(0,|x| x+1); }
                                    if edge.code < want {lo=mid+1;} else {hi=mid;}
                                }
                                0
                            })
                        };
                        out.write_all(&active[k].to_le_bytes())?;
                    }
                    if cache.len() > 1_000_000 { cache.clear(); }
                }
            }
            b'R' => {
                // Consume the entire input frame before writing: no pipe-buffer deadlock.
                let mut words = vec![0;n*4]; input.read_exact(&mut words)?;
                for bytes in words.chunks_exact(4) {
                    let id = u32::from_le_bytes(bytes.try_into().unwrap());
                    if id==0 || id as u64>trie.nodes { return Err(std::io::Error::other("invalid workspace page")); }
                    let mut counts = [0u64;257]; let mut children = [0u32;256];
                    for edge in trie.children(id-1) {
                        if let Some(b) = edge.byte() {
                            counts[b as usize] = edge.count;
                            children[b as usize] = edge.node.map_or(0,|x|x+1);
                        } else { counts[256] = edge.count; }
                    }
                    for v in counts {out.write_all(&v.to_le_bytes())?;}
                    for v in children {out.write_all(&v.to_le_bytes())?;}
                }
            }
            _ => return Err(std::io::Error::other("wrong workspace opcode")),
        }
        out.flush()?;
    }
    Ok(())
}
