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

    systems, ticks, waves = load(trace_path, warmup)
    tick_ids = sorted(ticks)
    tick_vals = [ticks[t] for t in tick_ids]
    ts = stats(tick_vals)   # whole-tick distribution (a per-N-tick system's
    n_ticks = len(tick_ids)  # cost is inside tick_us, on the ticks it runs)

    payload = {"lines": {}, "hists": {}, "hbars": {}, "stacks": {}}

    def line_section(xs, ys, lo, hi, chart_id, label):
        ctx, data = charts.line_chart(xs, ys, lo, hi, chart_id)
        data["label"] = label
        payload["lines"][chart_id] = data
        return chart("line", ctx), lo is not None

    # headline ---------------------------------------------------------------
    tiles = [
        ("mean tick", fmt_us(ts["mean"])),
        ("p50 tick", fmt_us(ts["p50"])),
        ("p99 tick", fmt_us(ts["p99"])),
        ("ticks / s", f"{1e6 / ts['mean']:,.0f}" if ts["mean"] else "0"),
    ]

    # tick wall time -- the whole tick, so a per-N-tick system's spike (its cost
    # lands in that tick's wave/flush) shows as a periodic bump rather than being
    # averaged away.
    tick_svg, tick_banded = line_section(
        *downsample(tick_ids, tick_vals), "tick", "tick wall")

    # waves: wall vs flush, mean over the ticks each wave ran. A one-shot or
    # per-N-tick phase (e.g. a re-sort every 64 ticks) exists for only a subset
    # of ticks, so its bar carries a "ran n of N" count.
    rows, ran = [], []
    for w in sorted(waves):
        per_tick = waves[w].values()
        wall = sum(v[0] for v in per_tick) / len(per_tick)
        flush = sum(v[1] for v in per_tick) / len(per_tick)
        rows.append((f"wave {w}", max(0.0, wall - flush), flush))
        ran.append(len(per_tick))
    ctx, data = charts.hbars(rows, "waves", stacked=True)
    for d, n in zip(data, ran):
        d["ran"], d["of"] = n, n_ticks  # tooltip: "ran n of N ticks"
    waves_partial = any(n < n_ticks for n in ran)
    waves_svg = chart("hbars", ctx)
    payload["hbars"]["waves"] = data

    # wave composition: the same per-wave walls stacked over the run, so the top
    # edge traces the tick wall (waves run sequentially behind barriers, so per
    # tick they sum to it). A wave that sits out a tick contributes 0 there, so
    # a per-N-tick re-sort shows as a periodic spike rather than an averaged
    # band; the top band is the residual tick time outside any wave (barrier and
    # dispatch overhead), clamped at 0 when timing noise makes the wave walls
    # sum just past the tick's.
    wave_order = sorted(waves)
    per_wave_ys = [[waves[w].get(t, (0.0, 0.0))[0] for t in tick_ids]
                   for w in wave_order]
    overhead = [max(0.0, tick_vals[i] - sum(col[i] for col in per_wave_ys))
                for i in range(len(tick_ids))]
    # downsample every band on the shared tick axis: identical length and bucket
    # count give aligned bucket centres, and the per-bucket mean is linear, so
    # the stacked means still sum to the mean tick.
    stack_x, _, _, _ = downsample(tick_ids, tick_vals)
    bands = [dict(label=f"wave {w}", slot=w % 8,
                  ys=downsample(tick_ids, ys)[1])
             for w, ys in zip(wave_order, per_wave_ys)]
    bands.append(dict(label="scheduler overhead", slot=0, over=True,
                      ys=downsample(tick_ids, overhead)[1]))
    ctx, data = charts.stacked_area(stack_x, bands, "wavestack")
    wavestack_svg = chart("stacked", ctx)
    wavestack_legend = [dict(label=b["label"], slot=b["slot"],
                            over=b.get("over", False)) for b in bands]
    payload["stacks"]["wavestack"] = data

    # Per-system command flush, measured in the core (the cmd_us column): the
    # part of each wave's barrier flush spent applying THIS system's commands.
    # A command-recording system (e.g. a re-sort via Commands::sort) shows almost
    # no busy -- its real cost is the flush -- so crediting it here lets it rank
    # by its true budget. Exact even when several systems share a wave.
    flush_by_system = {nm: sum(systems[nm]["cmd"]) for nm in systems}
    flush_ticks = {nm: dict(zip(systems[nm]["ticks"], systems[nm]["cmd"]))
                   for nm in systems}

    # A system's cost is its amortized busy PLUS its attributed flush, both
    # spread over every tick -- so a command system ranks by its real budget.
    def amort_cost(nm):
        return (sum(systems[nm]["busy"]) + flush_by_system[nm]) / n_ticks

    # systems overview: amortized cost over the whole run, so a one-shot or
    # per-N-tick system compares honestly against every-tick ones.
    order = sorted(systems, key=lambda n: -amort_cost(n))
    rows = [(name, amort_cost(name)) for name in order]
    ctx, data = charts.hbars(rows, "sys")
    overview_svg = chart("hbars", ctx)
    payload["hbars"]["sys"] = data

    # per-system cards -----------------------------------------------------------
    cards = []
    table = []
    for idx, name in enumerate(order):
        s = systems[name]
        st = stats(s["busy"])
        prep = sum(s["prep"]) / len(s["prep"])
        fin = sum(s["fin"]) / len(s["fin"])
        ctx_h, hdata = charts.histogram(s["busy"], f"h-s{idx}")
        hdata["label"] = name
        payload["hists"][f"h-s{idx}"] = hdata
        series_svg, _ = line_section(*downsample(s["ticks"], s["busy"]),
                                     f"l-s{idx}", f"{name} busy")
        # One cost metric: amortized busy + attributed flush (sum / all ticks)
        # as a share of the mean tick, so a rarely-run system reads by its true
        # per-tick budget, not its per-run spike (which the series still shows).
        a_busy = sum(s["busy"]) / n_ticks
        a_flush = flush_by_system[name] / n_ticks
        a_total = a_busy + a_flush
        amort_pct = 100.0 * a_total / ts["mean"] if ts["mean"] else 0.0
        wave_mode, _ = s["waves"].most_common(1)[0]
        # "Meaningful" flush: a real command cost, not the ~1us empty-flush
        # overhead every lone wave's barrier carries. Gate the breakdown, the
        # flush stat, and the flush time-series on it.
        flush_big = a_flush > 1.0 and a_flush > 0.02 * a_total
        flush_svg = None
        if flush_big:
            ft = sorted(flush_ticks[name])
            flush_svg, _ = line_section(
                *downsample(ft, [flush_ticks[name][t] for t in ft]),
                f"f-s{idx}", f"{name} command flush")
        cards.append(dict(
            name=name, wave=wave_mode, n_items=s["items"], n=st["n"],
            amortized=fmt_us(a_total), amortized_pct=f"{amort_pct:.1f}",
            has_flush=flush_big, amort_busy=fmt_us(a_busy),
            amort_flush=fmt_us(a_flush), flush_series=flush_svg,
            stats=[("mean", fmt_us(st["mean"])), ("sd", fmt_us(st["sd"])),
                   ("p50", fmt_us(st["p50"])), ("p99", fmt_us(st["p99"])),
                   ("max", fmt_us(st["mx"])), ("prepare", fmt_us(prep)),
                   ("finish", fmt_us(fin))]
                  + ([("flush", fmt_us(a_flush))] if flush_big else []),
            hist=chart("histogram", ctx_h), series=series_svg,
        ))
        table.append([name, wave_mode, s["items"], st["n"],
                      fmt_us(st["mean"]), fmt_us(st["sd"]), fmt_us(st["p50"]),
                      fmt_us(st["p99"]), fmt_us(st["mx"]), fmt_us(prep),
                      fmt_us(fin), fmt_us(a_flush) if a_flush > 0 else "—",
                      f"{amort_pct:.1f}%"])

    doc = env.get_template("report.html.j2").render(
        source=str(trace_path),
        n_ticks=n_ticks,
        n_systems=len(systems),
        n_waves=len(waves),
        tiles=tiles,
        tick_svg=tick_svg,
        tick_banded=tick_banded,
        waves_svg=waves_svg,
        waves_partial=waves_partial,
        wavestack_svg=wavestack_svg,
        wavestack_legend=wavestack_legend,
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
