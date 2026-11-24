// Emit each dictionary phrase once. Physical order walks three sequential streams
// (parse IDs, adjacent starts, corpus); one bit per distinct phrase suppresses repeats.
#[inline]
pub fn phrases(physical: bool, text_len: usize, refs: &[u64], lens: &[u32],
               pa: &[u32], starts: &[u64], mut visit: impl FnMut(usize,u64,u32)) {
    if !physical {
        for (p, (&pos, &len)) in refs.iter().zip(lens).enumerate() { visit(p,pos,len); }
        return;
    }
    assert_eq!(pa.len(), starts.len());
    let mut seen = vec![0u64; (refs.len()+63)/64];
    for (i, &p) in pa.iter().enumerate() {
        let p = p as usize;
        let word = &mut seen[p/64]; let mask = 1u64 << (p%64);
        if *word & mask != 0 { continue; }
        *word |= mask;
        let pos = starts[i]; let end = starts.get(i+1).copied().unwrap_or(text_len as u64);
        visit(p,pos,u32::try_from(end-pos).expect("phrase length exceeds u32"));
    }
}
