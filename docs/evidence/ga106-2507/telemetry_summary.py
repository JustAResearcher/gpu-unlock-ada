#!/usr/bin/env python3
"""Summarise a sampled.sh telemetry CSV: samples under load (util>=50%)."""
import csv, sys
for path in sys.argv[1:]:
    rows = list(csv.reader(open(path), skipinitialspace=True))[1:]
    load = [r for r in rows if len(r) > 13 and r[6].rstrip(' %').isdigit() and int(r[6].rstrip(' %')) >= 50]
    if not load:
        print(f"{path}: no loaded samples ({len(rows)} total)"); continue
    p = [float(r[1].split()[0]) for r in load]; c = [int(r[3].split()[0]) for r in load]
    t = [int(r[5]) for r in load]; lim = sorted({r[2] for r in load})
    flags = {k: sum(r[i] == 'Active' for r in load) for i, k in
             [(9, 'sw_power_cap'), (10, 'hw_slowdown'), (11, 'sw_thermal'), (12, 'hw_thermal'), (13, 'hw_power_brake')]}
    print(f"{path}: n={len(load)}/{len(rows)} limit={lim} power W min/med/max={min(p):.1f}/{sorted(p)[len(p)//2]:.1f}/{max(p):.1f} "
          f"sm MHz min/med/max={min(c)}/{sorted(c)[len(c)//2]}/{max(c)} temp max={max(t)} throttle_samples={flags}")
