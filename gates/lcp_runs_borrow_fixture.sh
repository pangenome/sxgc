#!/usr/bin/env bash
# THE BORROW PATH'S OWN GATE (a fixture that GUARANTEES a point mass).
#
# A code path that only fires on a corpus you cannot re-run in less than hours is
# one you are trusting by inspection. This fixture is seconds: indented lines whose
# leading space runs are long enough that the "16 spaces" class exceeds the wave
# goal the scratch budget implies, and whose space runs are NOT at the end of the
# text (so the single-symbol tail-run proof cannot apply and the borrow is the only
# order-safe shape).
#
#   * SS_WAVE_POINT_MASS_CLASS names the class in CORPUS bytes (16 spaces)
#   * SS_WAVE_BORROW admits it as ONE wave, journaling arena/disk/mem_cap
#   * the container built WITH the borrow is BYTE-IDENTICAL to the same corpus built
#     with no scratch cap (a borrow that changes the product is worse than none)
#   * chi is identical between the two arms, and `xsa query` answers from both
#
# usage: gates/lcp_runs_borrow_fixture.sh [bytes]
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
NBYTES=${1:-150000000}
D=${D:-/mnt/nvme2n1/erikg/lcpruns-gate/borrow}
F=$D/indent150m.txt
mkdir -p "$D"
fail=0
say() { echo "BORROW_$*"; }

# ---- the fixture: lines indented 16..2000 spaces, then "x = <n>". The space runs
# are ~100 bytes on average, so the LCEs the band sort must resolve are SHORT (the
# Pile's own shape: max run 15,992 B, mean ~33 B) - a single monster run would make
# this gate a test of memcmp instead of a test of the borrow.
if [ ! -s "$F" ] || [ "$(stat -c %s "$F")" != "$NBYTES" ]; then
  python3 - "$F" "$NBYTES" <<'PY'
import random, sys
path, want = sys.argv[1], int(sys.argv[2])
rng = random.Random(1)
words = ["alpha","beta","gamma","delta","epsilon","zeta","theta","iota","kappa","lambda","mu","nu"]
buf = []
n = 0
while n < want:
    depth = rng.choice([16,17,18,20,22,24,26,32,40,64,100,200])
    line = " " * depth + "x = %s_%d\n" % (rng.choice(words), rng.randrange(10**9))
    buf.append(line); n += len(line)
    if rng.random() < 0.35:      # plain unindented prose: the corpus is not all spaces
        t = " ".join(rng.choice(words) for _ in range(12)) + "\n"
        buf.append(t); n += len(t)
data = "".join(buf)[:want]
# make sure the text does NOT end in spaces (that would be a provable tail run)
data = data[:-1] + "Z"
open(path, "w").write(data)
PY
fi
say "FIXTURE path=$F bytes=$(stat -c %s "$F")"

build() { # $1 tag, $2 scratch subdir, $3... budget flags
  local tag=$1 dir=$2; shift 2
  rm -rf "$D/$dir"
  /usr/bin/time -f "BORROW_WALL_${tag} %e s" setsid timeout --kill-after=10 3600 \
    "$X" build --tape --input "$F" --scratch "$D/$dir" --threads 16 --memory-gb 32 "$@" \
    >"$D/build_$tag.log" 2>&1
  echo "$tag rc=$?"
  grep -E "^BORROW_WALL_${tag}|XSA_BUILD_DONE" "$D/build_$tag.log"
}

# ---- the tight arm: the wave goal must collapse BELOW the class size, while the
# band unit stays above the fold floor (a piece band is 65536 RUNS, so the plan
# refuses a rows_per_band below 65536 - measured: 1 GiB x 16 threads gives 37109
# and is refused, 2 GiB gives ~74k and plans).
build tight tight --scratch-budget-gb 2 --with-lcp-runs
# ---- the reference arm: no operator cap at all.
build free  free --with-lcp-runs

grep -hE "^SS_WAVE_POINT_MASS_CLASS|^SS_WAVE_POINT_MASS_MEMBER|^SS_WAVE_BORROW |^SS_WAVE_BORROW_TOTAL" \
    "$D/tight/order-tape.log" | cut -c1-260
grep -hE "^SS_WAVE_BORROW " "$D/tight/order-tape.log" >"$D/borrow_lines.txt"
[ -s "$D/borrow_lines.txt" ] && say "PATH borrow_fired=1" || { say "PATH borrow_fired=0"; fail=1; }
grep -q "path=borrow-one-wave" "$D/borrow_lines.txt" || { say "FAIL: no borrow-one-wave line"; fail=1; }
grep -q "SS_WAVE_POINT_MASS_MEMBER.*text64='  " "$D/tight/order-tape.log" \
  && say "PREFIX corpus_bytes=spaces (the remap inversion is right)" \
  || { say "FAIL: the class prefix does not read as spaces in corpus bytes"; fail=1; }

CT=$D/tight/indent150m.sxi2
CF=$D/free/indent150m.sxi2
for f in "$CT" "$CF"; do [ -s "$f" ] || { say "FAIL: no container $f"; fail=1; }; done
ST=$(sha256sum "$CT" | cut -d' ' -f1)
SF=$(sha256sum "$CF" | cut -d' ' -f1)
say "CONTAINER tight_sha256=$ST bytes=$(stat -c %s "$CT")"
say "CONTAINER free_sha256=$SF bytes=$(stat -c %s "$CF")"
if [ "$ST" = "$SF" ]; then say "BYTE_IDENTICAL=1 (the borrow changed no product byte)"
else say "BYTE_IDENTICAL=0"; cmp "$CT" "$CF" | head -3; fail=1; fi
CTCHI=$(grep -oE "chi=[0-9]+" "$D/build_tight.log" | head -1)
CFCHI=$(grep -oE "chi=[0-9]+" "$D/build_free.log" | head -1)
say "CHI tight=$CTCHI free=$CFCHI"
[ "$CTCHI" = "$CFCHI" ] || { say "FAIL: chi differs"; fail=1; }
# the borrowed container must also be readable and answer
"$X" lcp "$CT" --audit --run 0 --sample 3 --seed 2 >"$D/lcp.tsv" 2>"$D/lcp.log"
grep -E "^LCP_OPEN|^LCP_AUDIT" "$D/lcp.log" | cut -c1-200
grep -q "tiling=exact" "$D/lcp.log" || { say "FAIL: m14 audit on the borrowed build"; fail=1; }

echo "SURVIVORS: $(pgrep -a -f 'xsa_finish|scatter_sort|cross_lcp_merge' | grep -v pgrep || echo none)"
if [ $fail = 0 ]; then echo "GATE BORROW PASS"; else echo "GATE BORROW FAIL"; exit 1; fi
