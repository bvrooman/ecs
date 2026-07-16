"""Compare two ScheduleTrace CSVs: per-system deltas with noise awareness.

The comparison is keyed on system NAME (never wave index -- one-shot phases
shift wave placements), and every delta carries a significance verdict so a
regression is a claim, not a wiggle. The test runs on 5%-trimmed samples --
tick wall times have heavy scheduler-noise tails (a single descheduled tick
can be 20x the mean and would dominate a plain variance) -- and a change is
significant only when the trimmed-mean delta clears a Welch standard-error
test (|delta| > 3*SE, conservative because many systems are tested at
once), a relative threshold (default 2%), AND a 1 us absolute floor (sub-us
systems wiggle by timer granularity). Fewer than MIN_N
samples on either side (one-shot setup systems) can never be significant:
with n=1 the variance is unknowable, not zero. Displayed means/percentiles
stay untrimmed -- only the verdict uses the trimmed view.

Traces should come from reproducible runs of the same workload on the same
machine (e.g. `ECS_TRACE=... mist-headless` at a fixed seed), interleaved
with the change under test.
"""

from __future__ import annotations

import math

from .trace import load, stats

# Verdicts, in the order sections list them.
REGRESSION = "regression"
IMPROVEMENT = "improvement"
UNCHANGED = "unchanged"
ADDED = "added"
REMOVED = "removed"

MIN_N = 5          # fewer samples than this on either side: never significant
TRIM = 0.05        # fraction trimmed from EACH tail before the test
MIN_ABS_US = 1.0   # deltas under this are timer granularity, never significant


def _trimmed_mean_sd(xs):
    xs = sorted(xs)
    k = int(len(xs) * TRIM)
    if len(xs) > 2 * k:
        xs = xs[k:len(xs) - k]
    mean = sum(xs) / len(xs)
    var = sum((x - mean) ** 2 for x in xs) / len(xs)
    return mean, math.sqrt(var), len(xs)


def significant(a_samples, b_samples, threshold_pct):
    """Is the (trimmed) mean shift from a_samples to b_samples a real change?"""
    if len(a_samples) < MIN_N or len(b_samples) < MIN_N:
        return False
    ma, sda, na = _trimmed_mean_sd(a_samples)
    mb, sdb, nb = _trimmed_mean_sd(b_samples)
    if ma == 0:
        return False
    delta = mb - ma
    if abs(delta) < MIN_ABS_US or abs(delta) < ma * threshold_pct / 100.0:
        return False
    se = math.sqrt(sda ** 2 / na + sdb ** 2 / nb)
    return abs(delta) > 3 * se


def _verdict(a_samples, b_samples, threshold_pct):
    if not significant(a_samples, b_samples, threshold_pct):
        return UNCHANGED
    ma, _, _ = _trimmed_mean_sd(a_samples)
    mb, _, _ = _trimmed_mean_sd(b_samples)
    return REGRESSION if mb > ma else IMPROVEMENT


def _delta_pct(base, head):
    return 100.0 * (head - base) / base if base else 0.0


def compare(base_path, head_path, threshold_pct=2.0):
    """Load both traces and reduce them to a delta model.

    Returns a dict:
      ticks:   {base, head, mean/p50/p99 deltas, verdict}
      systems: [ {name, verdict, base/head stats, busy/prepare/finish deltas} ]
               significant changes first (largest |mean busy delta| first),
               then unchanged by head busy, then added/removed.
    """
    sys_a, ticks_a, _, maint_a = load(base_path)
    sys_b, ticks_b, _, maint_b = load(head_path)

    # Compare the true frame wall (systems + maintenance), so a regression in a
    # maintenance hook's amortized cost shows up in the tick verdict too.
    pt_a, pt_b = maint_a["per_tick"], maint_b["per_tick"]
    tick_vals_a = [ticks_a[t] + pt_a.get(t, 0.0) for t in ticks_a]
    tick_vals_b = [ticks_b[t] + pt_b.get(t, 0.0) for t in ticks_b]
    ta = stats(tick_vals_a)
    tb = stats(tick_vals_b)
    ticks = dict(
        base=ta, head=tb,
        mean_delta_pct=_delta_pct(ta["mean"], tb["mean"]),
        p50_delta_pct=_delta_pct(ta["p50"], tb["p50"]),
        p99_delta_pct=_delta_pct(ta["p99"], tb["p99"]),
        verdict=_verdict(tick_vals_a, tick_vals_b, threshold_pct),
    )

    systems = []
    for name in sorted(set(sys_a) | set(sys_b)):
        a, b = sys_a.get(name), sys_b.get(name)
        row = dict(name=name)
        if a is None or b is None:
            s = stats((b or a)["busy"])
            row.update(verdict=ADDED if a is None else REMOVED,
                       base=None if a is None else s,
                       head=s if a is None else None,
                       delta_us=0.0, delta_pct=0.0,
                       prepare=(0.0, 0.0), finish=(0.0, 0.0))
            systems.append(row)
            continue
        sa, sb = stats(a["busy"]), stats(b["busy"])
        hooks = {k: (sum(a[k]) / len(a[k]), sum(b[k]) / len(b[k]))
                 for k in ("prep", "fin")}
        row.update(
            verdict=_verdict(a["busy"], b["busy"], threshold_pct),
            base=sa, head=sb,
            delta_us=sb["mean"] - sa["mean"],
            delta_pct=_delta_pct(sa["mean"], sb["mean"]),
            prepare=hooks["prep"], finish=hooks["fin"],
            items=(a["items"], b["items"]),
        )
        systems.append(row)

    order = {REGRESSION: 0, IMPROVEMENT: 0, UNCHANGED: 1, ADDED: 2, REMOVED: 2}
    systems.sort(key=lambda r: (
        order[r["verdict"]],
        -abs(r["delta_us"]) if order[r["verdict"]] == 0
        else -(r["head"] or r["base"])["mean"],
    ))
    return dict(ticks=ticks, systems=systems,
                base_ticks=list(ticks_a.values()),
                head_ticks=list(ticks_b.values()),
                base_tick_ids=sorted(ticks_a),
                head_tick_ids=sorted(ticks_b),
                threshold_pct=threshold_pct)


def delta_rows(model):
    """Flatten the model to (scope, name, metric, base, head, delta_pct,
    verdict) tuples -- the CSV/console shape."""
    t = model["ticks"]
    rows = [
        ("tick", "wall", "mean_us", t["base"]["mean"], t["head"]["mean"],
         t["mean_delta_pct"], t["verdict"]),
        ("tick", "wall", "p50_us", t["base"]["p50"], t["head"]["p50"],
         t["p50_delta_pct"], "-"),
        ("tick", "wall", "p99_us", t["base"]["p99"], t["head"]["p99"],
         t["p99_delta_pct"], "-"),
    ]
    for s in model["systems"]:
        if s["verdict"] in (ADDED, REMOVED):
            side = s["head"] or s["base"]
            rows.append(("system", s["name"], "mean_busy_us",
                         s["base"]["mean"] if s["base"] else "",
                         s["head"]["mean"] if s["head"] else "",
                         "", s["verdict"]))
            continue
        rows.append(("system", s["name"], "mean_busy_us", s["base"]["mean"],
                     s["head"]["mean"], s["delta_pct"], s["verdict"]))
    return rows
