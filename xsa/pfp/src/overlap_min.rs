// Exact byte-LCP RMQ over adjacent ranks. Keep the u32 leaves, but index
// only minima of 16 consecutive ranks. between(x,y) takes min(x,y].
pub struct BlockMin {
    leaves: Vec<u32>,
    tree: Vec<u32>,
    blocks: usize,
}
impl BlockMin {
    const B: usize = 16;
    pub fn new(leaves: Vec<u32>) -> Self {
        let blocks = leaves.len().div_ceil(Self::B);
        let mut tree = vec![u32::MAX; 2 * blocks];
        for (i, b) in leaves.chunks(Self::B).enumerate() {
            tree[blocks + i] = *b.iter().min().unwrap();
        }
        for i in (1..blocks).rev() {
            tree[i] = tree[i * 2].min(tree[i * 2 + 1]);
        }
        Self {
            leaves,
            tree,
            blocks,
        }
    }
    pub fn between(&self, x: usize, y: usize) -> usize {
        assert!(x < y && y < self.leaves.len());
        let (lo, hi) = (x + 1, y + 1);
        let (a, b) = (lo.div_ceil(Self::B), hi / Self::B);
        if a >= b {
            return *self.leaves[lo..hi].iter().min().unwrap() as usize;
        }
        let mut v = u32::MAX;
        for &t in self.leaves[lo..a * Self::B]
            .iter()
            .chain(&self.leaves[b * Self::B..hi])
        {
            v = v.min(t);
        }
        let (mut l, mut r) = (a + self.blocks, b + self.blocks);
        while l < r {
            if l % 2 == 1 {
                v = v.min(self.tree[l]);
                l += 1;
            }
            if r % 2 == 1 {
                r -= 1;
                v = v.min(self.tree[r]);
            }
            l /= 2;
            r /= 2;
        }
        v as usize
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn every_interval() {
        for n in 1..99 {
            let t = BlockMin::new((0..n).map(|i| ((i * 37 + n) % 53) as u32).collect());
            for x in 0..n {
                for y in x + 1..n {
                    assert_eq!(
                        t.between(x, y),
                        *t.leaves[x + 1..=y].iter().min().unwrap() as usize
                    );
                }
            }
        }
    }
}
