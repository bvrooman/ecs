"""Load and summarize a ScheduleTrace CSV (see tools/schedule_trace.hpp).

Columns:
    tick,wave,system,busy_us,prepare_us,finish_us,items,wave_us,flush_us,tick_us,
    maint_us
`maint_us` is optional -- traces written before maintenance observability
existed lack it, and load treats it as 0 so old traces still render."""

from __future__ import annotations

import csv
import math
import sys
from collections import Counter

# Required columns; maint_us is optional (added with maintenance observability)
# so pre-existing traces still load -- absent, it reads as 0.
COLUMNS = {"tick", "wave", "system", "busy_us", "prepare_us",
           "finish_us", "items", "wave_us", "flush_us", "tick_us"}


def load(path, warmup=0):
    """Parse the trace into per-system, per-tick, per-wave, and per-tick
    maintenance series.

    warmup: drop the first `warmup` ticks (by tick index) from everything --
    the profiler convention of discarding cold-start iterations so the
    distributions reflect steady state, not the first frame's cache/frequency
    warm-up.

    Returns (systems, ticks, waves, maint) where `ticks` maps tick -> tick_us
    (systems only) and `maint` maps tick -> maint_us (0 on ticks with no
    maintenance). The true frame wall of a tick is ticks[t] + maint[t]."""
    systems = {}   # name -> dict(busy=[], prep=[], fin=[], items, wave, ticks=[])
    ticks = {}     # tick -> tick_us (systems only)
    waves = {}     # wave -> {tick: (wave_us, flush_us)}
    maint = {}     # tick -> maint_us (0 when no maintenance ran)
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        missing = COLUMNS - set(reader.fieldnames or [])
        if missing:
            sys.exit(f"error: {path} is missing columns {sorted(missing)} -- "
                     "is this a tools/schedule_trace.hpp CSV?")
        for row in reader:
            tick = int(row["tick"])
            if tick < warmup:
                continue
            wave = int(row["wave"])
            name = row["system"]
            s = systems.setdefault(name, {
                "busy": [], "prep": [], "fin": [], "ticks": [],
                "items": 0, "waves": Counter(),
            })
            s["busy"].append(float(row["busy_us"]))
            s["prep"].append(float(row["prepare_us"]))
            s["fin"].append(float(row["finish_us"]))
            s["ticks"].append(tick)
            s["items"] = max(s["items"], int(row["items"]))
            s["waves"][wave] += 1  # one-shot phases shift placements
            ticks[tick] = float(row["tick_us"])
            maint[tick] = float(row.get("maint_us") or 0.0)
            waves.setdefault(wave, {})[tick] = (float(row["wave_us"]),
                                                float(row["flush_us"]))
    if not ticks:
        sys.exit(f"error: no rows in {path}"
                 + (f" after dropping {warmup} warmup ticks" if warmup else ""))
    return systems, ticks, waves, maint


def stats(xs):
    """n / mean / sd / p50 / p99 / max of a sample list."""
    n = len(xs)
    if n == 0:
        return dict(n=0, mean=0, sd=0, p50=0, p99=0, mx=0)
    ordered = sorted(xs)
    mean = sum(xs) / n
    var = sum((x - mean) ** 2 for x in xs) / n

    def pct(p):
        return ordered[min(n - 1, int(p * n))]

    return dict(n=n, mean=mean, sd=math.sqrt(var),
                p50=pct(0.50), p99=pct(0.99), mx=ordered[-1])


def fmt_us(v):
    """Compact microsecond value: 12.3 µs / 4.56 ms / 1.20 s."""
    if v >= 1e6:
        return f"{v / 1e6:,.2f} s"
    if v >= 1e3:
        return f"{v / 1e3:,.2f} ms"
    return f"{v:,.1f} µs"


def nice_ticks(lo, hi, n=4):
    """Clean axis tick values covering [lo, hi]."""
    if hi <= lo:
        hi = lo + 1
    raw = (hi - lo) / n
    mag = 10 ** math.floor(math.log10(raw))
    step = next(s * mag for s in (1, 2, 2.5, 5, 10) if s * mag >= raw)
    first = math.ceil(lo / step) * step
    out = []
    v = first
    while v <= hi + 1e-9:
        out.append(round(v, 10))
        v += step
    return out


def downsample(xs, values, buckets=700):
    """Per-bucket mean plus min..max band when the series outgrows the plot."""
    n = len(values)
    if n <= buckets:
        return xs, values, None, None
    bx, mean, lo, hi = [], [], [], []
    for b in range(buckets):
        i0 = b * n // buckets
        i1 = max(i0 + 1, (b + 1) * n // buckets)
        chunk = values[i0:i1]
        bx.append(xs[(i0 + i1 - 1) // 2])
        mean.append(sum(chunk) / len(chunk))
        lo.append(min(chunk))
        hi.append(max(chunk))
    return bx, mean, lo, hi
