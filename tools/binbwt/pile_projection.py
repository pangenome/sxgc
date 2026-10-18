#!/usr/bin/env python3
"""bin-bwt v1 pile projection from the measured 1GB rates. Every term is a
measured rate times a byte count: nothing is inferred from a wall alone."""
import json
n1=1000000665          # 1GB fixture bytes (measured)
npile=1310000000000    # pile.txt bytes
R={
 # 1GB, threads=96, windows=1 (binbwt-gate-1b-t96)
 'count_s':1.36,       # parallel first-byte count pass over n1 bytes
 'fill_s':2.87,        # single-threaded placing pass over n1 bytes (writes 8*n1)
 'build_crit_s':30.434,# critical-path build wall (slowest thread), 96 threads
 'build_cpu_s':2779.790,
 'emit_s':172.599,     # serial row publication, unchanged sink
 'sweep_s':4.226,'total_s':201.758,'wall_s':203.0,'total_finish_s':308.7,
 'comparisons':3612105756,'symbols':112713502363,
 'noop_wall_s':33.498, # same run with a no-op emitter: construction only
 'construct_ns_per_byte':33.498e9/n1,
 'emit_ns_per_row':172.599e9/n1,
 'sink_only_ns_per_row':(201.758-33.498)*1e9/n1,
 'build_ns_per_symbol':5.460,      # 1-thread, uncontended
 'build_ns_per_comparison':170.382,
}
def h(s): return s/3600.0
rows=[]
# construction scales with bytes; publication scales with rows (=bytes)
count=npile/n1*R['count_s']
build=npile/n1*R['build_crit_s']
emit=npile/n1*R['emit_s']
def window(cap):
    w=-(-npile//cap)
    return w, w*R['fill_s']*npile/n1
for cap,label in [(20000000000,'20G slots = 320GB arena pair'),
                  (65000000000,'65G slots = 1040GB arena pair'),
                  (131000000000,'131G slots = 2096GB arena pair')]:
    w,fill=window(cap)
    rows.append(dict(cap=cap,label=label,windows=w,count_h=h(count),fill_h=h(fill),
                     build_h=h(build),emit_h=h(emit),
                     serial_h=h(count+fill+emit),
                     total_h=h(count+fill+emit+build)))
print(f"measured 1GB (n={n1}, threads=96): count {R['count_s']}s fill {R['fill_s']}s "
      f"build_crit {R['build_crit_s']}s build_cpu {R['build_cpu_s']}s emit {R['emit_s']}s "
      f"total {R['total_s']}s wall {R['wall_s']}s with-finish {R['total_finish_s']}s")
print(f"construction only (no-op emitter): {R['noop_wall_s']}s = "
      f"{R['construct_ns_per_byte']:.2f} ns/byte; publication = "
      f"{R['emit_ns_per_row']:.1f} ns/row measured on the emitter thread, "
      f"{R['sink_only_ns_per_row']:.1f} ns/row by difference")
print(f"build unit rates (1 thread, uncontended): {R['build_ns_per_symbol']} ns/symbol-thread, "
      f"{R['build_ns_per_comparison']} ns/comparison-thread; "
      f"{R['symbols']} symbols / {R['comparisons']} comparisons at 1GB = "
      f"{R['symbols']/n1:.1f} symbols/byte")
print()
print(f"{'window cap':44s} {'win':>5s} {'count':>8s} {'fill':>8s} {'build':>8s} {'emit':>8s} {'serial':>8s} {'total':>8s}")
for r in rows:
    print(f"{r['label']:44s} {r['windows']:5d} {r['count_h']:8.2f} {r['fill_h']:8.2f} "
          f"{r['build_h']:8.2f} {r['emit_h']:8.2f} {r['serial_h']:8.2f} {r['total_h']:8.2f}")
print(f"\nv1 as landed needs cap >= the largest bin: the 1GB fixture's space bin is "
      f"{167155270/n1:.4f} of the corpus, so the pile projection of that bin is "
      f"{167155270/n1*npile/1e9:.1f}G slots = {167155270/n1*npile*16/1e12:.2f} TB of arena "
      f"pair. Every row above therefore already assumes the operator's general rule - "
      f"count and materialise SUB-bins below the top level - not v1's fixed depth 1.")
print("\nhours above are pile hours; 'serial' = count+fill+emit (they run on one thread in this shape),")
print("'total' adds the 96-thread build critical path. Retired merge pipeline at 1GB: "
      "chunk 27.6 + merge 291 + finish 432 = 751s -> x1310 = "
      f"{h(751*1310):.1f} h serial-phase sum (not a wall, not remeasured here).")
print("kway-256 merge walk alone is reported at 40+ h; bin concat has NO merge walk: "
      "ordering work is the per-bin build term only.")
json.dump(dict(measured=R,projection=rows),open('/mnt/nvme2n1/erikg/binbwt-build/pile-projection.json','w'),indent=2)
