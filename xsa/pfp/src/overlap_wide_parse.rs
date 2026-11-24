//! Isolated packed-parse suffix ranks. Not used by the resident or recursive producer.
#[link(name = "libsais_r", kind = "static")]
extern "C" {
    fn libsais64_long(t: *mut i64, sa: *mut i64, n: i64, k: i64, fs: i64) -> i64;
}

#[derive(Clone, Copy, Debug)]
pub enum Workspace {
    FourK,
    SixK,
}

#[derive(Debug, PartialEq)]
pub struct Plan {
    pub fs: usize,
    pub sa_slots: usize,
    pub peak_array_bytes: u64,
}

/// Includes the original u32 slice; excludes allocator overhead and kernel internals.
pub fn plan(m: u64, k: u64, workspace: Workspace, grant: u64) -> Result<Plan, &'static str> {
    if usize::BITS != 64 || m == 0 || m > u64::from(u32::MAX) || k == 0 || k > m {
        return Err("unsupported packed parse dimensions");
    }
    // m,k <= U32_MAX, hence all products, sums, byte sizes and FFI casts fit i64.
    let fs = k * match workspace {
        Workspace::FourK => 4,
        Workspace::SixK => 6,
    };
    let sa_slots = m + fs;
    let peak_array_bytes = 12 * m + 8 * sa_slots;
    if peak_array_bytes > grant {
        return Err("parse array grant exceeded");
    }
    Ok(Plan {
        fs: fs as usize,
        sa_slots: sa_slots as usize,
        peak_array_bytes,
    })
}

fn reserve<T>(n: usize) -> Result<Vec<T>, &'static str> {
    let mut v = Vec::new();
    v.try_reserve_exact(n)
        .map_err(|_| "parse allocation failed")?;
    Ok(v)
}

pub fn wide_parse_ranks(
    ids: &[u32],
    k: u64,
    workspace: Workspace,
    grant: u64,
) -> Result<Vec<u32>, &'static str> {
    let m = ids.len();
    let p = plan(m as u64, k, workspace, grant)?;
    if ids.iter().any(|&x| u64::from(x) >= k) {
        return Err("parse ID outside alphabet");
    }
    let mut text = reserve(m)?;
    text.extend(ids.iter().map(|&x| i64::from(x)));
    let mut sa = reserve(p.sa_slots)?;
    sa.resize(p.sa_slots, 0);
    // Nonaliasing writable buffers, m input symbols in [0,k), m+fs SA slots.
    let rc = unsafe {
        libsais64_long(
            text.as_mut_ptr(),
            sa.as_mut_ptr(),
            m as i64,
            k as i64,
            p.fs as i64,
        )
    };
    if rc != 0 {
        return Err("libsais64_long failed");
    }
    if !text.iter().copied().eq(ids.iter().copied().map(i64::from)) {
        return Err("kernel did not restore parse");
    }
    drop(text);
    certified_ranks(ids, &sa[..m])
}

fn certified_ranks(ids: &[u32], sa: &[i64]) -> Result<Vec<u32>, &'static str> {
    let m = ids.len();
    if sa.len() != m || m > u32::MAX as usize {
        return Err("rank extent mismatch");
    }
    let mut ranks = reserve(m)?;
    ranks.resize(m, 0);
    for (r, &j) in sa.iter().enumerate() {
        let slot = usize::try_from(j)
            .ok()
            .and_then(|i| ranks.get_mut(i))
            .ok_or("invalid suffix index")?;
        if *slot != 0 {
            return Err("duplicate suffix index");
        }
        *slot = (r + 1) as u32;
    }
    // Length plus uniqueness implies coverage. Rank zero belongs only to END.
    let key = |p: i64| {
        let i = p as usize;
        (ids[i], ranks.get(i + 1).copied().unwrap_or(0))
    };
    if !sa.windows(2).all(|w| key(w[0]) < key(w[1])) {
        return Err("suffix recurrence mismatch");
    }
    Ok(ranks)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_bad_certificates() {
        for sa in [&[0][..], &[-1, 0], &[0, 2], &[1, 1], &[0, 1]] {
            assert!(certified_ranks(&[7, 7], sa).is_err());
        }
    }
}
