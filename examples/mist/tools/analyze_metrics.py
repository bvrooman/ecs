#!/usr/bin/env python3
"""Summarise a mist ECS_METRICS csv: overall flock stats + per-step follow response.

Usage: analyze_metrics.py [metrics.csv]   (default /tmp/metrics.csv)

Best paired with ECS_CURSOR_PATH=step (the cursor jumps between quadrants every
4s): each segment then shows how close the flock gets to a freshly-placed cursor
and whether it settles on it or wheels away.
"""
import csv
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/metrics.csv"
rows = list(csv.DictReader(open(path)))
if not rows:
    sys.exit("no rows in " + path)
col = lambda k: [float(r[k]) for r in rows]
t, fe, sp, co, gx, gy = (col(k) for k in
                         ("t", "follow_err", "spread", "coherence", "goal_sx", "goal_sy"))


def stat(name, x):
    print(f"  {name:11s} min={min(x):.3f}  max={max(x):.3f}  mean={sum(x) / len(x):.3f}")


print(f"== {path}  ({len(rows)} ticks, {t[-1]:.0f}s sim) ==")
stat("follow_err", fe)
stat("spread", sp)
stat("coherence", co)

print("  step response (each cursor hold):  err@jump -> closest -> err@end")
s = settles = wheels = loose = 0
for i in range(1, len(rows) + 1):
    if i == len(rows) or gx[i] != gx[s] or gy[i] != gy[s]:
        if i - s > 10:  # ignore tiny segments
            seg = slice(s, i)
            es, emin, ee = fe[s], min(fe[seg]), fe[i - 1]
            v = ("settles" if ee < 0.25 else
                 "wheels-away" if ee > emin + 0.15 else "holds-loose")
            settles += v == "settles"
            wheels += v == "wheels-away"
            loose += v == "holds-loose"
            print(f"    t={t[s]:5.1f}  {es:.2f} -> {emin:.2f} -> {ee:.2f}   {v}")
        s = i
print(f"  -> settles:{settles}  holds-loose:{loose}  wheels-away:{wheels}")
