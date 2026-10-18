#!/usr/bin/env python3
"""bin-bwt v2 pile projection from measured 1GB rates. Every term is a measured
rate times a byte count; nothing is inferred from a wall alone.

What changed vs pile_projection.py (v1), and why each term is what it is:

  count   UNCHANGED pass, unchanged rate (1.36 ns/byte, banked v1 measurement).
  fill    SAME single-threaded placing pass, now also packing the arena word
          (tail|symbol|hit) and merge-joining the capture-target list. Measured
          1GB sweep 5.122s - 1.36s count = 3.762s per window (v1: 2.87s), i.e.
          the fill got 31% dearer and the emit got 16x cheaper.
  build   96-thread critical path, unchanged work (construction counters are
          bit-identical between v1 and v2 at 1GB: comparisons 3,612,105,756 /
          symbols 112,713,502,363 / level_elems 10,179,725,481 /
          leaf_sorts 127,119,193 / leaf_elems 925,152,140). Measured 23.489s in
          the v2 producer-only 1GB run at load 27.9-44.6; the v1 banked number
          was 30.434s at load 50.6-59.9. Both are shown for the build term so
          the box-load difference stays visible.
  emit    the serial row publication, measured v2: 11.408s at 1GB = 11.41
          ns/row on the ONE ordered stitch thread (v1: 172.599s = 172.6
          ns/row). The band writers are 6.427s of writer-thread CPU over 8
          threads at 1GB, fully overlapped: max(stitch, writers) = stitch.

Producer-only walls are the honest 1GB A/B: with the finish running and the
transport's default window (CROSS_V2_PIECE_WINDOW=1100) the producer is
FLOW-LIMITED by pile_finish's band consumption (flow_wait_s=147.874 in
binbwt-gate-1b-v2), so that harness reports the finish's rate, not this sink's.
"""
import json

n1 = 1000000665           # 1GB fixture bytes (measured)
npile = 1310000000000     # pile.txt bytes
R = {
    'count_s': 1.360,       # parallel first-byte count pass, unchanged pass
    'fill_s': 3.762,        # v2 measured: 1GB sweep 5.122 - count 1.360
    'fill_s_v1': 2.870,
    'build_crit_s': 23.489,     # v2 producer-only 1GB/96t, load 27.9-44.6
    'build_crit_s_v1bf': 30.434,  # v1 banked, load 50.6-59.9
    'emit_s': 11.408,       # v2 ordered stitch thread, 1GB
    'emit_s_v1': 172.599,   # v1 serial sink thread, 1GB
    'band_write_s': 6.427,  # 8 writer threads, summed; overlapped
    'flow_wait_s': 0.022,   # producer-only (no consumer backpressure)
    'wall_s': 41.68,        # /usr/bin/time -v, producer only
    'wall_s_v1': 213.63,    # same binary pair, producer only, same box window
    'windows': 1,
}
emit_ns_per_row = R['emit_s'] * 1e9 / n1
emit_ns_per_row_v1 = R['emit_s_v1'] * 1e9 / n1


def h(s):
    return s / 3600.0


rows = []
count = npile / n1 * R['count_s']
build = npile / n1 * R['build_crit_s']
build_v1bf = npile / n1 * R['build_crit_s_v1bf']
emit = npile / n1 * R['emit_s']
emit_v1 = npile / n1 * R['emit_s_v1']

for cap, label in [(20000000000, '20G slots = 320GB arena pair'),
                   (65000000000, '65G slots = 1040GB arena pair'),
                   (131000000000, '131G slots = 2096GB arena pair')]:
    w = -(-npile // cap)
    fill = w * R['fill_s'] * npile / n1
    fill_v1 = w * R['fill_s_v1'] * npile / n1
    rows.append(dict(cap=cap, label=label, windows=w,
                     count_h=h(count), fill_h=h(fill), build_h=h(build),
                     emit_h=h(emit),
                     serial_h=h(count + fill + emit),
                     total_h=h(count + fill + emit + build),
                     total_v1_h=h(count + fill_v1 + emit_v1 + build_v1bf),
                     total_v2bf_h=h(count + fill + emit + build_v1bf)))

print(f"measured 1GB producer-only (n={n1}, threads=96, load 27.9-44.6): "
      f"sweep {R['count_s'] + R['fill_s']:.3f}s (count {R['count_s']}s + fill "
      f"{R['fill_s']}s), build_crit {R['build_crit_s']}s, stitch {R['emit_s']}s, "
      f"band_writers {R['band_write_s']}s (8 threads, overlapped), "
      f"flow_wait {R['flow_wait_s']}s, wall {R['wall_s']}s")
print(f"v1 same box window: sweep 4.165s, build_crit 29.767s, sink thread "
      f"{R['emit_s_v1']}s, wall {R['wall_s_v1']}s")
print(f"publication unit rate: {emit_ns_per_row:.2f} ns/row (v2 ordered thread) "
      f"vs {emit_ns_per_row_v1:.1f} ns/row (v1 serial sink) = "
      f"{emit_ns_per_row_v1/emit_ns_per_row:.1f}x on the term")
print(f"producer wall: {R['wall_s']:.1f}s vs {R['wall_s_v1']:.1f}s = "
      f"{R['wall_s_v1']/R['wall_s']:.2f}x (construction counters unchanged)")
print()
print(f"{'window cap':44s} {'win':>4s} {'count':>7s} {'fill':>7s} {'build':>7s} "
      f"{'emit':>7s} {'total':>7s} {'v1 total':>9s}")
for r in rows:
    print(f"{r['label']:44s} {r['windows']:4d} {r['count_h']:7.2f} {r['fill_h']:7.2f} "
          f"{r['build_h']:7.2f} {r['emit_h']:7.2f} {r['total_h']:7.2f} "
          f"{r['total_v1_h']:9.2f}")
print()
print("hours above are pile hours. 'total' = count + fill + emit + build critical path,")
print("summed conservatively because at 1GB the build finished before the ordered stitch")
print("did (wait_s 22.158s of build -> 11.408s of stitch, so the stitch is NOT hidden).")
print("'v1 total' = the banked v1 projection (emit 62.81h, fill 2.87 ns/byte, build 30.434s).")
print()
print("THE BINDING TERM MOVED. In v1 the serial row publication was 62.81h = 74-85% of the")
print("total. In v2 the largest term is the WINDOWED FILL PASS at "
      f"{rows[-1]['fill_h']:.1f}-{rows[0]['fill_h']:.1f}h for 10-66 windows:")
print("one thread rewriting 8 bytes+1 packed word per position once per window, which is")
print("what the window count multiplies. Second is the per-bin construction "
      f"{h(build):.2f}h, third the ordered stitch {h(emit):.2f}h.")
print()
print("Capacity caveat, unchanged from v1 and now MORE binding: every row above assumes")
print(f"the arena pair for the cap row actually exists (2096GB for the 131G row on a")
print("1007GB box), and the largest bin at pile is "
      f"{167155270/n1:.4f} of the corpus = {167155270/n1*npile/1e9:.0f}G slots = "
      f"{167155270/n1*npile*16/1e12:.2f}TB of arena pair, so sub-bins below the top")
print("level are still required (v2 does not change the arena shape).")
print()
print("Finish caveat: with the transport's default window the 1GB gate producer spent")
print("147.874s of its 191.7s waiting for pile_finish to consume bands, i.e. the FINISH")
print("is now the end-to-end bottleneck at 1GB (with-finish wall 312.1s vs 308.7s in v1).")
print("At pile the launch table is set by whichever of (v2 producer, pile_finish) is slower.")
json.dump(dict(measured=R, projection=rows),
          open('/mnt/nvme2n1/erikg/binbwt-v2-build/pile-projection-v2.json', 'w'), indent=2)
