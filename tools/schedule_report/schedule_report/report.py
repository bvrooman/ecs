"""Assemble the HTML report: geometry from charts.py, layout in templates/.

Every piece of markup -- the page AND the chart SVG -- is a Jinja2 template;
charts.py hands each template a plain-data context (coordinates, path
strings, labels). The emitted page stays a single self-contained file: the
stylesheet (static/report.css) and hover script (static/report.js) are read
at render time and inlined, and the hover payloads are embedded as one JSON
block.
"""

from __future__ import annotations

import json
import pathlib

from jinja2 import Environment, FileSystemLoader
from markupsafe import Markup

from . import charts
from . import compare as cmp
from .trace import downsample, fmt_us, load, stats

_PKG = pathlib.Path(__file__).resolve().parent


def _env():
    return Environment(loader=FileSystemLoader(_PKG / "templates"),
                       autoescape=True)


def render(trace_path, out_path, warmup=0):
    env = _env()

    def chart(kind, ctx):
        return Markup(env.get_template(f"charts/{kind}.svg.j2").render(ctx))

    systems, ticks, waves, maint = load(trace_path, warmup)
    maint_per_tick = maint["per_tick"]                  # tick -> phase wall
    tick_ids = sorted(ticks)
    tick_vals = [ticks[t] for t in tick_ids]            # systems only
    maint_vals = [maint_per_tick.get(t, 0.0) for t in tick_ids]  # maint phase
    # The frame wall the headline reports: systems + maintenance, so a tick's
    # tile value is the whole picture even though the two are internally split.
    total_vals = [c + m for c, m in zip(tick_vals, maint_vals)]
    ts = stats(total_vals)          # headline distribution (whole frame)
    cs = stats(tick_vals)           # systems-only (per-system share denominator)
    n_ticks = len(tick_ids)

    # Maintenance amortized across every tick (sum / n_ticks) plus its
    # per-occurrence profile, shown only when the workload has maintenance.
    maint_ran = [m for m in maint_vals if m > 0.0]
    maint_total = sum(maint_vals)
    has_maint = maint_total > 0.0
    maint_amortized = maint_total / n_ticks if n_ticks else 0.0

    payload = {"lines": {}, "hists": {}, "hbars": {}}

    def line_section(xs, ys, lo, hi, chart_id, label):
        ctx, data = charts.line_chart(xs, ys, lo, hi, chart_id)
        data["label"] = label
        payload["lines"][chart_id] = data
        return chart("line", ctx), lo is not None

    # headline ---------------------------------------------------------------
    # mean/p50/p99 are the whole frame (systems + maintenance); ticks/s follows
    # from the same mean, so the rate matches the reported tick cost.
    tiles = [
        ("mean tick", fmt_us(ts["mean"])),
        ("p50 tick", fmt_us(ts["p50"])),
        ("p99 tick", fmt_us(ts["p99"])),
        ("ticks / s", f"{1e6 / ts['mean']:,.0f}" if ts["mean"] else "0"),
    ]

    # Frame breakdown: how the mean tick splits into systems vs maintenance,
    # so the headline total is decomposable at a glance. Only when maintenance
    # actually ran -- otherwise the total IS the systems time.
    breakdown = None
    if has_maint:
        ms = stats(maint_ran)
        breakdown = dict(
            total=fmt_us(ts["mean"]),
            systems=fmt_us(cs["mean"]),
            maint=fmt_us(maint_amortized),
            maint_pct=f"{100.0 * maint_amortized / ts['mean']:.1f}"
                      if ts["mean"] else "0.0",
            ran=len(maint_ran), of=n_ticks,
            cadence=round(n_ticks / len(maint_ran)) if maint_ran else 0,
            per_run=fmt_us(ms["mean"]), per_run_max=fmt_us(ms["mx"]),
        )

    # tick wall time: the whole frame (systems + maintenance), so the spikes on
    # maintenance ticks are visible rather than hidden under a systems-only line.
    tick_svg, tick_banded = line_section(
        *downsample(tick_ids, total_vals), "tick", "frame wall")

    # frame phases: the serial maintenance phase (if any) first, then the
    # parallel waves. Each bar is mean wall over the ticks it RAN (not amortized
    # over all ticks), so the maintenance phase shows its per-occurrence spike
    # with a "ran n of N" count -- the same machinery one-shot waves already use.
    phase_rows, phase_ran = [], []
    if has_maint:
        ran_ticks = [t for t in tick_ids if maint_per_tick.get(t, 0.0) > 0.0]
        phase_wall = sum(maint_per_tick[t] for t in ran_ticks) / len(ran_ticks)
        phase_rows.append(("maintenance", phase_wall, 0.0, "maint"))
        phase_ran.append(len(ran_ticks))
    for w in sorted(waves):
        per_tick = waves[w].values()
        wall = sum(v[0] for v in per_tick) / len(per_tick)
        flush = sum(v[1] for v in per_tick) / len(per_tick)
        phase_rows.append((f"wave {w}", max(0.0, wall - flush), flush))
        phase_ran.append(len(per_tick))
    ctx, data = charts.hbars(phase_rows, "phases", stacked=True)
    for d, n in zip(data, phase_ran):
        d["ran"], d["of"] = n, n_ticks  # tooltip: "ran n of N ticks"
    phases_partial = any(n < n_ticks for n in phase_ran)
    phases_svg = chart("hbars", ctx)
    payload["hbars"]["phases"] = data

    # Unified entries: real systems AND maintenance hooks (pseudo-systems), so
    # both rank and chart together. Maintenance rows amortize the same way
    # (sum / all ticks), so a rare hook ranks by its true per-tick budget; the
    # card carries the per-occurrence story its distribution and series show.
    entries = [dict(kind="system", name=n, busy=systems[n]["busy"],
                    prep=systems[n]["prep"], fin=systems[n]["fin"],
                    items=systems[n]["items"], ticks=systems[n]["ticks"],
                    waves=systems[n]["waves"]) for n in systems]
    entries += [dict(kind="maint", name=n, busy=h["busy"], ticks=h["ticks"])
                for n, h in maint["hooks"].items()]
    entries.sort(key=lambda e: -sum(e["busy"]))

    # systems overview: amortized over the whole run (sum / ticks), so a
    # one-shot setup system -- or a periodic maintenance hook -- compares
    # honestly against every-tick systems.
    rows = [(e["name"], sum(e["busy"]) / n_ticks,
             "maint" if e["kind"] == "maint" else None) for e in entries]
    ctx, data = charts.hbars(rows, "sys")
    overview_svg = chart("hbars", ctx)
    payload["hbars"]["sys"] = data

    # per-entry cards ------------------------------------------------------------
    cards = []
    table = []
    for idx, e in enumerate(entries):
        st = stats(e["busy"])
        ctx_h, hdata = charts.histogram(e["busy"], f"h-s{idx}")
        hdata["label"] = e["name"]
        payload["hists"][f"h-s{idx}"] = hdata
        series_svg, _ = line_section(*downsample(e["ticks"], e["busy"]),
                                     f"l-s{idx}", f"{e['name']} busy")
        base_stats = [("mean", fmt_us(st["mean"])), ("sd", fmt_us(st["sd"])),
                      ("p50", fmt_us(st["p50"])), ("p99", fmt_us(st["p99"])),
                      ("max", fmt_us(st["mx"]))]
        common = dict(name=e["name"], n=st["n"], ran_partial=st["n"] < n_ticks,
                      hist=chart("histogram", ctx_h), series=series_svg)
        if e["kind"] == "maint":
            # A serial hook: no lanes, no barrier hooks, no wave. Lead with its
            # per-occurrence distribution; annotate the amortized per-tick cost
            # as a share of the mean frame -- comparable to the systems' "% of
            # tick" rather than an absolute that says nothing about proportion.
            amort = sum(e["busy"]) / n_ticks
            amort_pct = 100.0 * amort / ts["mean"] if ts["mean"] else 0.0
            cards.append(dict(common, maint=True, amortized=fmt_us(amort),
                              amortized_pct=f"{amort_pct:.1f}", stats=base_stats))
            table.append([e["name"], "maint", "—", st["n"],
                          fmt_us(st["mean"]), fmt_us(st["sd"]),
                          fmt_us(st["p50"]), fmt_us(st["p99"]), fmt_us(st["mx"]),
                          "—", "—", f"{amort_pct:.1f}%"])
            continue
        prep = sum(e["prep"]) / len(e["prep"])
        fin = sum(e["fin"]) / len(e["fin"])
        # busy vs the systems' tick wall (cs, maintenance excluded): CPU summed
        # across lanes, so a well-parallelized system legitimately exceeds 100%
        # (the hover on the value explains this in the page).
        share = 100.0 * st["mean"] / cs["mean"] if cs["mean"] else 0
        # Dominant wave placement; one-shot phases shift indices on the ticks
        # they run (mist's scatter bumps every wave by one on tick 0), and
        # the Frame phases note + run-count tooltips carry that story.
        wave_mode, _ = e["waves"].most_common(1)[0]
        cards.append(dict(common, maint=False, wave=wave_mode,
                          n_items=e["items"], share=f"{share:.1f}",
                          stats=base_stats + [("prepare", fmt_us(prep)),
                                              ("finish", fmt_us(fin))]))
        table.append([e["name"], wave_mode, e["items"], st["n"],
                      fmt_us(st["mean"]), fmt_us(st["sd"]), fmt_us(st["p50"]),
                      fmt_us(st["p99"]), fmt_us(st["mx"]), fmt_us(prep),
                      fmt_us(fin), f"{share:.1f}%"])

    doc = env.get_template("report.html.j2").render(
        source=str(trace_path),
        n_ticks=n_ticks,
        n_systems=len(systems),
        n_waves=len(waves),
        n_maint=len(maint["hooks"]),
        has_maint=has_maint,
        tiles=tiles,
        breakdown=breakdown,
        tick_svg=tick_svg,
        tick_banded=tick_banded,
        phases_svg=phases_svg,
        phases_partial=phases_partial,
        overview_svg=overview_svg,
        cards=cards,
        table=table,
        css=Markup((_PKG / "static" / "report.css").read_text()),
        js=Markup((_PKG / "static" / "report.js").read_text()),
        payload=Markup(json.dumps(payload)),
    )
    with open(out_path, "w") as f:
        f.write(doc)
    return dict(bytes=len(doc), ticks=n_ticks, systems=len(systems))


# --- compare view ------------------------------------------------------------

# The delta annotation always carries its sign, so color (status green/red)
# is never the only channel. "lower is better" throughout: busy and tick wall
# are costs.
def _delta_txt(pct):
    return f"{pct:+.1f}%"


def _delta_cls(verdict):
    return {cmp.REGRESSION: "up", cmp.IMPROVEMENT: "down"}.get(verdict, "flat")


MAX_PAIR_ROWS = 40  # keep the paired chart readable; the table has everything


def render_compare(base_path, head_path, out_path, label_a="base",
                   label_b="head", threshold_pct=2.0):
    env = _env()

    def chart(kind, ctx):
        return Markup(env.get_template(f"charts/{kind}.svg.j2").render(ctx))

    model = cmp.compare(base_path, head_path, threshold_pct)
    t = model["ticks"]
    payload = {"lines": {}, "hists": {}, "hbars": {}, "pairs": {}}

    # headline tiles: head value large, base -> delta beneath
    def tile(label, key, pct):
        return dict(label=label, head=fmt_us(t["head"][key]),
                    base=fmt_us(t["base"][key]), delta=_delta_txt(pct),
                    cls="up" if pct > threshold_pct
                        else "down" if pct < -threshold_pct else "flat")

    tiles = [
        tile("mean tick", "mean", t["mean_delta_pct"]),
        tile("p50 tick", "p50", t["p50_delta_pct"]),
        tile("p99 tick", "p99", t["p99_delta_pct"]),
    ]

    counts = {}
    for s in model["systems"]:
        counts[s["verdict"]] = counts.get(s["verdict"], 0) + 1
    verdicts = [
        dict(cls={"regression": "bad", "improvement": "good"}.get(v, "flat"),
             text=f"{n} {v}{'s' if n != 1 and v not in ('unchanged',) else ''}")
        for v, n in counts.items()
    ]

    # overlaid tick series: head over base, shared scales. The y scale clips
    # at the joint p99.5 -- one 20x descheduled tick would otherwise flatten
    # both lines to the baseline (tooltips still report true values).
    both = sorted(model["base_ticks"] + model["head_ticks"])
    cap = both[min(len(both) - 1, int(0.995 * len(both)))]
    bx, by, _, _ = downsample(model["base_tick_ids"], model["base_ticks"])
    hx, hy, _, _ = downsample(model["head_tick_ids"], model["head_ticks"])
    ctx, data = charts.line_chart(hx, hy, None, None, "tick", xs2=bx, ys2=by,
                                  ymax_cap=cap)
    data["label"] = f"{label_b} tick wall"
    data["label2"] = f"{label_a} tick wall"
    payload["lines"]["tick"] = data
    tick_svg = chart("line", ctx)

    # paired bars, model order (significant first). One-shot systems (fewer
    # than MIN_N samples a side) sit out: they can never be significant, and
    # a setup system's one 18ms tick would crush every per-tick bar.
    rows = []
    skipped = []
    for s in model["systems"]:
        if s["verdict"] in (cmp.ADDED, cmp.REMOVED):
            continue
        if s["base"]["n"] < cmp.MIN_N or s["head"]["n"] < cmp.MIN_N:
            skipped.append(s["name"])
            continue
        rows.append((s["name"], s["base"]["mean"], s["head"]["mean"],
                     _delta_txt(s["delta_pct"]) if s["verdict"] != cmp.UNCHANGED
                     else "·",
                     _delta_cls(s["verdict"])))
    if len(rows) > MAX_PAIR_ROWS:
        print(f"note: paired chart shows the top {MAX_PAIR_ROWS} of "
              f"{len(rows)} systems; the table has all of them")
        rows = rows[:MAX_PAIR_ROWS]
    ctx, data = charts.paired_hbars(rows, "sys")
    for d in data:
        d["label_a"], d["label_b"] = label_a, label_b
    payload["pairs"]["sys"] = data
    pairs_svg = chart("pairs", ctx)

    table = []
    for s in model["systems"]:
        a, b = s["base"], s["head"]
        prep_d = s["prepare"][1] - s["prepare"][0]
        fin_d = s["finish"][1] - s["finish"][0]
        table.append(dict(
            name=s["name"], verdict=s["verdict"],
            cls={"regression": "up", "improvement": "down",
                 "added": "up", "removed": "flat"}.get(s["verdict"], "flat"),
            row_cls="dim" if s["verdict"] == cmp.UNCHANGED else "",
            base_mean=fmt_us(a["mean"]) if a else "—",
            head_mean=fmt_us(b["mean"]) if b else "—",
            delta=_delta_txt(s["delta_pct"])
                  if s["verdict"] in (cmp.REGRESSION, cmp.IMPROVEMENT,
                                      cmp.UNCHANGED) else "—",
            base_p99=fmt_us(a["p99"]) if a else "—",
            head_p99=fmt_us(b["p99"]) if b else "—",
            prep=f"{prep_d:+.2f} µs" if a and b else "—",
            fin=f"{fin_d:+.2f} µs" if a and b else "—",
        ))

    doc = env.get_template("compare.html.j2").render(
        source_a=str(base_path), source_b=str(head_path),
        label_a=label_a, label_b=label_b,
        n_ticks_a=t["base"]["n"], n_ticks_b=t["head"]["n"],
        threshold=f"{threshold_pct:g}", pairs_skipped=skipped,
        tiles=tiles, verdicts=verdicts,
        tick_svg=tick_svg, pairs_svg=pairs_svg, table=table,
        css=Markup((_PKG / "static" / "report.css").read_text()),
        js=Markup((_PKG / "static" / "report.js").read_text()),
        payload=Markup(json.dumps(payload)),
    )
    with open(out_path, "w") as f:
        f.write(doc)
    return model


# --- bench scaling view ------------------------------------------------------

def render_bench(bench_csv, out_path):
    from . import bench as bench_load
    env = _env()

    data = bench_load.load_bench(bench_csv)
    suites = []
    for s in data["suites"]:
        ctx, _ = charts.multiline_chart(s["series"], s["lanes"],
                                        f"ml-{s['suite']}", ideal=s["ideal"])
        suites.append(dict(suite=s["suite"], lanes=s["lanes"],
                           table=s["table"], legend=ctx["legend"],
                           svg=Markup(env.get_template(
                               "charts/multiline.svg.j2").render(ctx))))
    if not suites:
        raise SystemExit(
            f"error: {bench_csv} has no lane sweep (a benchmark measured at "
            ">= 2 lane counts). Run a pool-scaling suite with ECS_LANES set.")

    doc = env.get_template("bench.html.j2").render(
        source=str(bench_csv), meta=data["meta"], suites=suites,
        css=Markup((_PKG / "static" / "report.css").read_text()),
        js=Markup((_PKG / "static" / "report.js").read_text()),
    )
    with open(out_path, "w") as f:
        f.write(doc)
    return dict(suites=len(suites))
