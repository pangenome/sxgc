#!/usr/bin/env bash
# Rehydrate the verified native bundle instead of rebuilding or substituting stages.
set -eu -o pipefail
repo=$(git rev-parse --show-toplevel); cd "$repo"
mkdir "$1"; D=$(realpath "$1"); G=$repo/gates
ref=6b87aa1620505cff02bc93e6c008483d0d895a83
base=5a1bb5679088214b8a3f1ecc1c2961bded7feb07
git archive "$ref" xsa | tar x -C "$D"
mkdir "$D/base"; git archive "$base" xsa | tar x -C "$D/base"
export CARGO_TARGET_DIR=$D/target
cargo build --offline --locked --release -j2 --manifest-path "$D/base/xsa/Cargo.toml" -p pfp --bin pfp_sort_r
cd "$D/xsa"
B=$HOME/.cache/xsa/13524d6bbedad508fdd8e30e7ab709dbf62ae5db64eb0cd8864dd79efa5b7b4c
python3 - "$B" "$G" <<'PY'
import hashlib,json,pathlib,sys
P=pathlib.Path;b=P(sys.argv[1]);g=P(sys.argv[2]);m=b/'MANIFEST.sha256'
assert hashlib.sha256(m.read_bytes()).hexdigest()==b.name
entries=[]
for k,v in (json.loads(m.read_bytes())['files']|{'MANIFEST.sha256':b.name}).items():
    assert hashlib.sha256((b/k).read_bytes()).hexdigest()==v
    entries.append(f'({json.dumps(k)},include_bytes!({json.dumps(str(b/k))}),"{v}"),')
P('bundled_tools.rs').write_text('pub const FILES:&[(&str,&[u8],&str)]=&['+'\n'.join(entries)+f'];pub const ID:&str="{b.name}";')
P('build.rs').write_text('fn main(){std::fs::copy("bundled_tools.rs",std::path::Path::new(&std::env::var("OUT_DIR").unwrap()).join("bundled_tools.rs")).unwrap();}')
p=P('src/main.rs');p.write_text(p.read_text().replace('fn main() {',(g/'overlap_v10_fixture_join.rs').read_text()+'\nfn main() {\nif std::env::args().nth(1).as_deref()==Some("fixture-join"){fixture_join(&std::env::args().collect::<Vec<_>>());return;}'))
PY
cargo build --offline --locked --release -j2
cd "$D/base/xsa/pfp"; cp "$G/overlap_v10_fixture_rows.rs" src/fixture_rows.rs
printf '\n[[bin]]\nname="fixture_rows"\npath="src/fixture_rows.rs"\n' >> Cargo.toml
cargo build --offline --locked --release -j2 --bin fixture_rows
cd "$D/target/release"
sha256sum pfp_sort_r fixture_rows xsa | tee fixture-tools.sha256
sha256sum "$B/"{scatter_sort,xsa_finish,cross_lcp_merge}
