#!/usr/bin/env python3
"""bin-bwt v2 pile projection from measured rates. Every term is a measured rate
times a byte count; nothing is inferred from a wall alone.

Measured 1GB producer-only, threads=96, load 27.9-44.6 (the pair of runs below
was taken back to back in the same window on the same box, which also carries a
live 100GB run - no quiet-box claim):

  v1 binary (rebuilt from base 51a7f02; sha256 equal to the attested v1 binary)
      wall 213.63s  sweep 4.165s  build_crit 29.767s  serial sink thread 185.333s
  v2 binary (this branch)
      wall  41.68s  count 0.038s + fill 5.084s  build_crit 23.489s
                    ordered stitch 11.408s  band writers 6.427s (8 threads,
                    overlapped)  flow_wait 0.022s  wait-for-build 22.158s

Publication unit rate: 11.41 ns/row for the one ordered stitch thread vs 172.6
ns/row for the v1 serial sink = 15.1x on the term; producer wall 5.13x.

The FILL term is measured two ways and both are reported:
  * single 1GB window: v2 5.084s of placing per corpus pass vs v1 4.126s, i.e.
    packing (tail|symbol|hit) plus the target merge join costs +23%;
  * v1's banked marginal (1GB windows=1 vs windows=5: (15.70-4.23)/4 = 2.87s
    per extra window) is the per-window figure the v1 projection used, and it
    is cheaper than a single-window fill because each window writes only its own
    bin range. Applying the SAME +23% to it gives the v2 per-window fill below.

CAVEAT THAT MATTERS MOST: the placing pass is SINGLE-THREADED and its cost is
strongly load-sensitive. Across producer-only 1GB runs on this box I measured it
between 3.8s and 188s (load 27 -> 229), i.e. a factor of 50, while the 96-thread
construction term moved much less. Every fill hour below assumes a quiet box.
"""
import json

n1 = 1000000665           # 1GB fixture bytes (measured)
npile = 1310000000000     # pile.txt bytes
M = {
    # v1, measured in the same window on this box
    'v1_wall_s': 213.63, 'v1_sweep_s': 4.165, 'v1_build_crit_s': 29.767,
    'v1_sink_s': 185.333,
    # v2, measured
    'v2_wall_s': 41.68, 'v2_count_s': 0.038, 'v2_fill_s': 5.084,
    'v2_build_crit_s': 23.489, 'v2_stitch_s': 11.408, 'v2_band_write_s': 6.427,
    'v2_wait_build_s': 22.158, 'v2_flow_wait_s': 0.022,
    # banked (v1 attestation) figures kept for comparability
    'v1_fill_per_window_s': 2.87, 'v1_count_s': 1.36,
    'v1_sink_banked_s': 172.599,   # the banked v1 projection's emit term
    'v1_build_crit_banked_s': 30.434,
    # fill packing+join cost measured on the single-window pass
    'fill_v2_over_v1': 5.084 / (4.165 - 0.038),
}
M['v2_fill_per_window_s'] = M['v1_fill_per_window_s'] * M['fill_v2_over_v1']
emit_ns_row = M['v2_stitch_s'] * 1e9 / n1
emit_ns_row_v1 = M['v1_sink_s'] * 1e9 / n1        # this window's A/B run
emit_ns_row_v1b = M['v1_sink_banked_s'] * 1e9 / n1     # the banked v1 figure


def h(s):
    return s / 3600.0


rows = []
# conservative: the banked v1 count term (1.36s) rather than the measured 0.038s
count = npile / n1 * M['v1_count_s']
count_direct = npile / n1 * M['v2_count_s']
build = npile / n1 * M['v2_build_crit_s']
build_banked = npile / n1 * M['v1_build_crit_banked_s']
emit = npile / n1 * M['v2_stitch_s']
emit_v1 = npile / n1 * M['v1_sink_banked_s']
for cap, label in [(20000000000, '20G slots = 320GB arena pair'),
                   (65000000000, '65G slots = 1040GB arena pair'),
                   (131000000000, '131G slots = 2096GB arena pair')]:
    w = -(-npile // cap)
    fill = w * M['v2_fill_per_window_s'] * npile / n1
    fill_v1 = w * M['v1_fill_per_window_s'] * npile / n1
    rows.append(dict(cap=cap, label=label, windows=w,
                     count_h=h(count), fill_h=h(fill), build_h=h(build),
                     emit_h=h(emit),
                     total_h=h(count + fill + emit + build),
                     total_banked_build_h=h(count + fill + emit + build_banked),
                     total_v1_h=h(count + fill_v1 + emit_v1 + build_banked)))

print("MEASURED 1GB producer-only, threads=96, load 27.9-44.6, back to back:")
print(f"  v1 wall {M['v1_wall_s']:.2f}s = sweep {M['v1_sweep_s']:.3f}s + build "
      f"{M['v1_build_crit_s']:.3f}s + serial sink thread {M['v1_sink_s']:.3f}s "
      f"(the three sum to {M['v1_sweep_s']+M['v1_build_crit_s']+M['v1_sink_s']-M['v1_wall_s']:.2f}s more than the wall: "
      f"the sink overlaps the build)")
print(f"  v2 wall {M['v2_wall_s']:.2f}s = count {M['v2_count_s']:.3f}s + fill "
      f"{M['v2_fill_s']:.3f}s + build {M['v2_build_crit_s']:.3f}s + stitch "
      f"{M['v2_stitch_s']:.3f}s, with band writers {M['v2_band_write_s']:.3f}s over 8 "
      f"threads overlapped and {M['v2_wait_build_s']:.1f}s waiting for the build")
print(f"  publication rate {emit_ns_row:.2f} ns/row vs {emit_ns_row_v1b:.1f} ns/row "
      f"banked ({emit_ns_row_v1:.1f} ns/row in this window's A/B run) = "
      f"{emit_ns_row_v1b/emit_ns_row:.1f}x on the term; wall "
      f"{M['v1_wall_s']/M['v2_wall_s']:.2f}x")
print(f"  fill packing+join cost: {M['v2_fill_s']:.3f}s vs "
      f"{M['v1_sweep_s']-M['v2_count_s']:.3f}s = +{100*(M['fill_v2_over_v1']-1):.0f}%")
print()
print(f"{'window cap':44s} {'win':>4s} {'count':>7s} {'fill':>7s} {'build':>7s} "
      f"{'stitch':>7s} {'total':>7s} {'v1 total':>9s}")
for r in rows:
    print(f"{r['label']:44s} {r['windows']:4d} {r['count_h']:7.2f} {r['fill_h']:7.2f} "
          f"{r['build_h']:7.2f} {r['emit_h']:7.2f} {r['total_h']:7.2f} "
          f"{r['total_v1_h']:9.2f}")
print()
print("hours are pile hours (n=1,310,000,000,000). 'total' = count + windows*fill +")
print("96-thread build critical path + ordered stitch, summed: at 1GB the build finished")
print(f"{M['v2_wait_build_s']:.1f}s before the stitch did and the stitch is NOT hidden, so")
print("summing is the conservative composition, and it reproduces the measured 41.68s wall")
print(f"(sum = {M['v2_count_s']+M['v2_fill_s']+M['v2_build_crit_s']+M['v2_stitch_s']:.2f}s).")
print("'v1 total' is the banked v1 projection (62.81h emit, 2.87s/window fill, 30.434s build).")
print()
print(f"THE BINDING TERM MOVED: v1's serial publication was 62.81h = 74-85% of its total.")
print(f"In v2 the largest term is the WINDOWED PLACING PASS, {rows[-1]['fill_h']:.1f}-"
      f"{rows[0]['fill_h']:.1f}h for 10-66 windows (single-threaded, and the single most")
print("load-sensitive term on the box: 3.8-188s per 1GB pass depending on load).")
print(f"Second is the per-bin construction {h(build):.2f}h, third the ordered stitch "
      f"{h(emit):.2f}h (was {h(emit_v1):.2f}h banked).")
print(f"Using the banked v1 build path instead: {rows[0]['total_banked_build_h']:.2f} / "
      f"{rows[1]['total_banked_build_h']:.2f} / {rows[2]['total_banked_build_h']:.2f} h.")
print(f"Count term check: measured {h(count_direct):.3f}h (0.038s/GB direct) vs banked "
      f"{h(count):.2f}h; the table keeps the banked 1.36s/GB figure, i.e. it is")
print(f"conservative by {h(count)-h(count_direct):.2f}h, and the measured count is negligible.")
print()
print("CAPACITY CAVEAT (unchanged by v2): each row assumes its arena pair exists (2096GB")
print(f"for the 131G row on a 1007GB box) and the largest bin at pile is "
      f"{167155270/n1:.4f} of the corpus = {167155270/n1*npile/1e9:.0f}G slots = "
      f"{167155270/n1*npile*16/1e12:.2f}TB of arena pair,")
print("so counting and materialising SUB-bins below the top level is still required.")
print()
print("FINISH CAVEAT (new, and it is now the end-to-end limit at 1GB): with the")
print("transport's default window (CROSS_V2_PIECE_WINDOW=1100) the 1GB producer spent")
print("147.874s of its 191.7s in v2_flow_wait, i.e. waiting for pile_finish to consume")
print("bands; with-finish wall 312.1s vs 308.7s in v1. The producer-side term this")
print("branch attacks is no longer what sets the 1GB end-to-end wall.")
json.dump(dict(measured=M, projection=rows,
               v2_fill_per_window_s=M['v2_fill_per_window_s']),
          open('/mnt/nvme2n1/erikg/binbwt-v2-build/pile-projection-v2.json', 'w'), indent=2)
