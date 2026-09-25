#!/usr/bin/env python3
"""c_probe_gate.py — validate the C-lane baseline machinery.

(1) numpy SA == gated pure-python SA (done in c_probe_common self-test).
(2) MODEL v3 exactness: chi_events_v3 == chi_full on the fmTexts battery
    (must be 124/124 with direct-law pieces + both extremes), and on the
    aligned-text battery (5 parse_probe texts + random-bin-20k).
(3) Report r, iv (pieces), P (cells), chi for every battery text — the
    scale context for the C-term measurements.
"""
import sys
import time

sys.path.insert(0, "tools")
from c_probe_common import battery_texts, build, chi_events_v3, chi_full, fm_texts_battery


def main():
    t0 = time.time()
    ok = tot = 0
    fails = []
    for x in fm_texts_battery():
        S = build(x)
        f = chi_full(S)
        ev = chi_events_v3(S)
        tot += 1
        if f == ev:
            ok += 1
        elif len(fails) < 6:
            fails.append((list(x), f, ev))
    print(f"MODEL v3 gate (fmTexts battery): chi_events_v3 == chi_full on {ok}/{tot}")
    for t in fails:
        print("   FAIL:", t)
    print(f"  [{time.time()-t0:.1f}s]")
    print()
    print(f"{'text':16s} {'n':>7s} {'r':>7s} {'iv':>6s} {'P':>7s} {'P/r':>5s} {'chi':>7s} {'chi/r':>6s} {'P/chi':>6s} gate")
    for name, x in battery_texts():
        t1 = time.time()
        S = build(x)
        f = chi_full(S)
        ev = chi_events_v3(S)
        r = len(S["runs"])
        iv = S["n_pieces"]
        P = len(S["cell_max_pos"])
        print(f"{name:16s} {S['N']:7d} {r:7d} {iv:6d} {P:7d} {P/r:5.2f} {f:7d} {f/r:6.3f} {P/max(f,1):6.2f} "
              f"{'PASS' if f == ev else 'FAIL'} [{time.time()-t1:.1f}s]")
    print("GATE DONE")


if __name__ == "__main__":
    main()
