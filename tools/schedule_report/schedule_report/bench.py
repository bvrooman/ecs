"""Load an ECS_BENCH_OUT results CSV (see benchmarks/bench.hpp) and pivot the
pool-scaling suites into per-suite speedup-vs-lanes series.

The results schema carries a `lanes` column, so a lane sweep is already a
matrix: for each (suite, name, metric, entities) we collect lane -> value.
The throughput series for a benchmark is its representative time-cost metric
(the `_PREF` order) across lanes, turned into speedup vs the smallest lane --
so the scaling curve, and where it turns over past the physical-core count,
is what the report draws.
"""

from __future__ import annotations

import csv
from collections import defaultdict

# The one lower-is-better time metric that represents a benchmark's throughput,
# most-specific first. A suite emits several per-lane metrics (mean/p50/p99);
# we plot one line per benchmark, not one per metric.
_PREF = ["us_per_tick", "kernel_us_per_tick", "mean_us_per_tick",
         "ns_per_op", "ns_per_row", "ns_per_entity"]


def _fmt_count(n):
    if n >= 1_000_000:
        return f"{n / 1e6:g}M"
    if n >= 1_000:
        return f"{n / 1e3:g}k"
    return str(n)


def load_bench(path):
    """-> dict(meta=..., suites=[{suite, lanes, series, table}]).

    series: [{label, slot, ys(speedup per lane), us(raw per lane)}] for the
    charts; table: flat rows for the numbers block. Only suites with a
    benchmark measured at >= 2 lanes appear (a lane sweep is the point)."""
    rows = list(csv.DictReader(open(path, newline="")))
    if not rows:
        raise SystemExit(f"error: no rows in {path}")

    # (suite, name, metric, entities) -> {lane: value}, plus the 'better' dir
    cells = defaultdict(dict)
    better = {}
    for r in rows:
        key = (r["suite"], r["name"], r["metric"], int(r["entities"]))
        cells[key][int(r["lanes"])] = float(r["value"])
        better[key] = r["better"]

    # pick the representative throughput metric for each (suite, name, entities)
    by_bench = {}  # (suite, name, entities) -> (metric, {lane: value})
    for (suite, name, metric, ent), lanevals in cells.items():
        if better[(suite, name, metric, ent)] != "lower":
            continue
        if metric not in _PREF or len(lanevals) < 2:
            continue
        cur = by_bench.get((suite, name, ent))
        if cur is None or _PREF.index(metric) < _PREF.index(cur[0]):
            by_bench[(suite, name, ent)] = (metric, lanevals)

    # group benches by suite
    suites = defaultdict(list)
    for (suite, name, ent), (metric, lanevals) in by_bench.items():
        suites[suite].append((name, ent, lanevals))

    out = []
    for suite in sorted(suites):
        benches = suites[suite]
        # Align on the RICHEST lane set in the suite (its full sweep). A
        # benchmark measured at only a couple of lanes -- e.g. an
        # imperative-vs-kernel spot check at {serial, hw} -- is not a scaling
        # curve; keep only series that cover the full sweep so a short one
        # can't truncate the real curves.
        target = max((frozenset(lv) for _, _, lv in benches), key=len)
        if len(target) < 2:
            continue
        lanes = sorted(target)
        kept = [(name, ent, lv) for name, ent, lv in benches
                if target <= set(lv)]
        dropped = len(benches) - len(kept)
        if dropped:
            print(f"note: {suite}: {dropped} series measured at fewer lanes "
                  f"than the {len(lanes)}-point sweep -- omitted from the curve")
        benches = kept
        names = {b[0] for b in benches}
        ents = {b[1] for b in benches}

        def label(name, ent):
            parts = []
            if len(names) > 1:
                parts.append(name)
            if len(ents) > 1:
                parts.append(_fmt_count(ent))
            return " ".join(parts) or name

        series, table = [], []
        for slot, (name, ent, lv) in enumerate(
                sorted(benches, key=lambda b: (b[0], b[1]))):
            us = [lv[l] for l in lanes]  # target <= set(lv), so all present
            base = us[0]
            spd = [base / v if v else 0.0 for v in us]
            best = max(range(len(lanes)), key=lambda i: spd[i])
            series.append(dict(label=label(name, ent), slot=slot % 8,
                               ys=[round(v, 3) for v in spd]))
            table.append(dict(label=label(name, ent),
                              us=[round(v, 1) for v in us],
                              spd=[round(v, 2) for v in spd],
                              best=lanes[best]))
        # perfect scaling relative to the smallest lane
        ideal = [l / lanes[0] for l in lanes]
        out.append(dict(suite=suite, lanes=lanes, series=series, table=table,
                        ideal=ideal))

    meta = rows[0]
    return dict(
        meta=dict(commit=meta.get("commit", "?"), build=meta.get("build", "?"),
                  cpu=meta.get("cpu", "?"), threads_hw=meta.get("threads_hw", "?")),
        suites=out,
    )
