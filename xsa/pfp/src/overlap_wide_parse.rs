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

/// Logical bytes for the IDs, inverse (overwritten by PLCP), and compact SA.
pub fn plcp_array_bytes(m: u64) -> Result<u64, &'static str> {
    if usize::BITS != 64 || m == 0 || m >= u64::from(u32::MAX) {
        return Err("unsupported PLCP extent");
    }
    Ok(12 * m)
}

pub struct ParsePlcp<'a> {
    ids: &'a [u32],
    plcp: Vec<u32>,
    order: ParseOrder,
}

pub struct ParseOrder(Vec<u32>);

impl ParseOrder {
    /// (occurrence position, continuation rank), in list insertion order.
    pub fn continuations(&self) -> impl Iterator<Item = (u32, u32)> + '_ {
        std::iter::once((self.0.len() as u32 - 1, 0)).chain(
            self.0
                .iter()
                .enumerate()
                .filter_map(|(r, &p)| p.checked_sub(1).map(|j| (j, r as u32 + 1))),
        )
    }
}

impl<'a> ParsePlcp<'a> {
    pub fn build(ids: &'a [u32], mut inverse: Vec<u32>, grant: u64) -> Result<Self, &'static str> {
        let m = ids.len();
        if plcp_array_bytes(m as u64)? > grant || inverse.len() != m {
            return Err("PLCP extent/grant mismatch");
        }
        let mut order = reserve(m)?;
        order.resize(m, u32::MAX);
        for (p, &r) in inverse.iter().enumerate() {
            let slot = r
                .checked_sub(1)
                .and_then(|i| order.get_mut(i as usize))
                .ok_or("invalid rank")?;
            if *slot != u32::MAX {
                return Err("duplicate rank");
            }
            *slot = p as u32;
        }
        let key = |p: u32| {
            (
                ids[p as usize],
                inverse.get(p as usize + 1).copied().unwrap_or(0),
            )
        };
        if !order.windows(2).all(|w| key(w[0]) < key(w[1])) {
            return Err("invalid rank recurrence");
        }
        // Only inverse[p] is needed once position p is reached; overwrite in text order.
        let mut z = 0;
        for (p, slot) in inverse.iter_mut().enumerate() {
            if *slot == 1 {
                z = 0;
            } else {
                let q = order[*slot as usize - 2] as usize;
                while p + z < m && q + z < m && ids[p + z] == ids[q + z] {
                    z += 1;
                }
            }
            *slot = z as u32;
            z = z.saturating_sub(1);
        }
        Ok(Self {
            ids,
            plcp: inverse,
            order: ParseOrder(order),
        })
    }

    pub fn text_plcp(&self) -> &[u32] {
        &self.plcp
    }

    /// Adjacent suffix positions and their common token count, starting at rank 2.
    pub fn predecessor_pairs(&self) -> impl Iterator<Item = (u32, u32, u32)> + '_ {
        self.order
            .0
            .windows(2)
            .map(|w| (w[0], w[1], self.plcp[w[1] as usize]))
    }

    /// Starts and first-unequal-word common byte lengths come from the verified packed reader.
    /// Write directly into the caller's aligned/RMQ input. Discard output on error.
    pub fn fill_aligned(
        &self,
        starts: &[u64],
        eof: u32,
        mut common: impl FnMut(u32, u32) -> u64,
        out: &mut [u64],
    ) -> Result<(), &'static str> {
        let m = self.ids.len();
        if starts.len() != m + 1
            || out.len() != m
            || starts[0] != 0
            || !starts.windows(2).all(|w| w[0] < w[1])
        {
            return Err("invalid aligned extent");
        }
        if self.ids[m - 1] != eof || self.ids[..m - 1].contains(&eof) {
            return Err("EOF must occur only at end");
        }
        out[0] = 0;
        for (slot, (i, j, z)) in out[1..].iter_mut().zip(self.predecessor_pairs()) {
            let (i, j, z) = (i as usize, j as usize, z as usize);
            // Unique final ID precludes any proper-prefix suffix pair.
            let (x, y) = (self.ids[i + z], self.ids[j + z]);
            if x >= y || starts[i + z] - starts[i] != starts[j + z] - starts[j] {
                return Err("inconsistent aligned prefix");
            }
            let h = (starts[i + z] - starts[i])
                .checked_add(common(x, y))
                .ok_or("aligned LCP overflow")?;
            if h > starts[m] - starts[i.max(j)] {
                return Err("aligned LCP outside text");
            }
            *slot = h;
        }
        Ok(())
    }

    /// Drop PLCP before constructing occurrence lists; keep only the compact SA.
    pub fn into_order(self) -> ParseOrder {
        self.order
    }
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
