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
from .trace import downsample, fmt_us, load, stats

_PKG = pathlib.Path(__file__).resolve().parent


def _env():
    return Environment(loader=FileSystemLoader(_PKG / "templates"),
                       autoescape=True)


def render(trace_path, out_path):
    env = _env()

    def chart(kind, ctx):
        return Markup(env.get_template(f"charts/{kind}.svg.j2").render(ctx))

    systems, ticks, waves = load(trace_path)
    tick_ids = sorted(ticks)
    tick_vals = [ticks[t] for t in tick_ids]
    ts = stats(tick_vals)
    n_ticks = len(tick_ids)

    payload = {"lines": {}, "hists": {}, "hbars": {}}

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

    # tick wall time ----------------------------------------------------------
    tick_svg, tick_banded = line_section(
        *downsample(tick_ids, tick_vals), "tick", "tick wall")

    # waves: wall vs flush. One-shot phases (a setup system pruned after its
    # tick) shift every wave index on the ticks they run, so a wave may exist
    # for only a few ticks -- its row says so rather than leaving a wave no
    # system claims.
    rows = []
    ran = []
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

    # systems overview: amortized over the whole run (sum / ticks), so a
    # one-shot setup system compares honestly against every-tick systems.
    order = sorted(systems, key=lambda n: -sum(systems[n]["busy"]))
    rows = [(name, sum(systems[name]["busy"]) / n_ticks) for name in order]
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
        # busy vs the tick's WALL time: CPU summed across lanes, so a
        # well-parallelized system legitimately exceeds 100% (the hover on
        # the value explains this in the page).
        share = 100.0 * st["mean"] / ts["mean"] if ts["mean"] else 0
        # Dominant wave placement; one-shot phases shift indices on the ticks
        # they run (mist's scatter bumps every wave by one on tick 0), and
        # the Waves section's note + run-count tooltips carry that story.
        wave_mode, _ = s["waves"].most_common(1)[0]
        ctx, hdata = charts.histogram(s["busy"], f"h-s{idx}")
        hdata["label"] = name
        payload["hists"][f"h-s{idx}"] = hdata
        series_svg, _ = line_section(*downsample(s["ticks"], s["busy"]),
                                     f"l-s{idx}", f"{name} busy")
        cards.append(dict(
            name=name, wave=wave_mode,
            n_items=s["items"], n=st["n"],
            share=f"{share:.1f}",
            ran_partial=st["n"] < n_ticks,
            stats=[("mean", fmt_us(st["mean"])), ("sd", fmt_us(st["sd"])),
                   ("p50", fmt_us(st["p50"])), ("p99", fmt_us(st["p99"])),
                   ("max", fmt_us(st["mx"])), ("prepare", fmt_us(prep)),
                   ("finish", fmt_us(fin))],
            hist=chart("histogram", ctx), series=series_svg,
        ))
        table.append([name, wave_mode, s["items"], st["n"],
                      fmt_us(st["mean"]), fmt_us(st["sd"]), fmt_us(st["p50"]),
                      fmt_us(st["p99"]), fmt_us(st["mx"]), fmt_us(prep),
                      fmt_us(fin), f"{share:.1f}%"])

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
