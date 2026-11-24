#!/usr/bin/env bash
# Build only in a fresh fixture archive. Never patch a scale gate in-place.
set -euo pipefail
G=$(cd "$(dirname "$0")" && pwd); root=$(git -C "$G" rev-parse --show-toplevel)
mkdir -m700 "$1"; D=$(realpath "$1"); cd "$D"
git -C "$root" archive b9f60c1b2a8d5ee46687b41b6b5631a1dd509a0e xsa | tar xf -
cd xsa/pfp
python3 - <<'PY'
from pathlib import Path
p=Path('src/main.rs'); s=p.read_text(); key='let triples = resident_sink || external;'; assert s.count(key)==1
s=s.replace(key,key+'''\n    assert!(external && !resident_sink && !fixture && file_n == 6_311_445, "exact tagged v10 fixture only");''');key='if ten {let(p,h,b)=overlap_row43::decode'; assert s.count(key)==1
p.write_text(s.replace(key,'if external {let(p,h,b)=overlap_row43::decode'))
PY
# Same constructor revision as the narrow frozen fixture; emit the tagged ABI.
git -C "$root" show 5a1bb5679088214b8a3f1ecc1c2961bded7feb07:xsa/pfp/src/overlap.rs > src/v10_overlap.rs
cat >src/v10_rows.rs <<'RS'
mod overlap_row43;
mod v10_overlap;
use std::io::{BufWriter, Write};
fn main() {
    let file = std::fs::File::open(std::env::args_os().nth(1).unwrap()).unwrap();
    assert_eq!(file.metadata().unwrap().len(), 6_311_445);
    let text = unsafe { memmap2::Mmap::map(&file) }.unwrap();
    let mut out = BufWriter::new(std::io::stdout().lock());
    v10_overlap::construct(&text, |_| {}, |p,h,b| {
        out.write_all(&overlap_row43::encode(p as u64,h as u64,if p==0 {256} else {b as u32})).unwrap();
    });
    out.flush().unwrap();
}
RS
printf '\n[[bin]]\nname="v10_rows"\npath="src/v10_rows.rs"\n' >> Cargo.toml
CARGO_TARGET_DIR="$D/target" cargo build -j2 --offline --locked --release --bin v10_rows --bin pfp_sort_r
cd "$D"; cp target/release/{v10_rows,pfp_sort_r} .
c++ -std=c++17 -O2 -Wall -Wextra -Werror "$G/overlap_v10_relay.cpp" -o relay
sha256sum v10_rows pfp_sort_r relay > native-tools.sha256
