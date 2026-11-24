//! Explicit wide-text row ABI. Still 12 bytes: low position and LCP words,
//! then [tag:1 | LCP-high:11 | pos-high:11 | predecessor:9], little endian.
//! Old narrow readers reject the tag as an invalid predecessor. This is a
//! transport limit, not a claim that all other columns support 43-bit text.
pub const LIMIT: u64 = 1 << 43;
const TAG: u32 = 1 << 31;
#[inline]
pub fn encode(p: u64, h: u64, b: u32) -> [u8; 12] {
    assert!(p < LIMIT && h < LIMIT && b <= 256, "row43 extent");
    let high = TAG | (((h >> 32) as u32) << 20) | (((p >> 32) as u32) << 9) | b;
    let mut row = [0; 12];
    row[..4].copy_from_slice(&(p as u32).to_le_bytes());
    row[4..8].copy_from_slice(&(h as u32).to_le_bytes());
    row[8..].copy_from_slice(&high.to_le_bytes());
    row
}
#[inline]
pub fn decode(row: &[u8; 12]) -> (u64, u64, u32) {
    let word = |i: usize| u32::from_le_bytes(row[i..i + 4].try_into().unwrap());
    let t = word(8);
    let b = t & 511;
    assert!(t & TAG != 0 && b <= 256, "invalid row43 tag/predecessor");
    let field = |lo, hi| (word(lo) as u64) | (((hi & 2047u32) as u64) << 32);
    (field(0, t >> 9), field(4, t >> 20), b)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn widths_and_byte_order() {
        let v = [
            0,
            1,
            (1 << 32) - 1,
            1 << 32,
            (1 << 32) + 1,
            10_000_000_000,
            LIMIT - 1,
        ];
        for p in v {
            for h in v {
                for b in [0, 1, 127, 255, 256] {
                    assert_eq!(decode(&encode(p, h, b)), (p, h, b));
                }
            }
        }
        assert_eq!(
            encode(0x1_12345678, 0x2_98765432, 256),
            [0x78, 0x56, 0x34, 0x12, 0x32, 0x54, 0x76, 0x98, 0x00, 0x03, 0x20, 0x80]
        );
    }
    #[test]
    fn never_truncate_or_accept_narrow_frames() {
        for (p, h, b) in [(LIMIT, 0, 0), (0, LIMIT, 0), (0, 0, 257)] {
            assert!(std::panic::catch_unwind(|| encode(p, h, b)).is_err());
        }
        for t in [0u32, 256, TAG | 257, TAG | 511] {
            let mut r = [0; 12];
            r[8..].copy_from_slice(&t.to_le_bytes());
            assert!(std::panic::catch_unwind(|| decode(&r)).is_err());
        }
    }
}
