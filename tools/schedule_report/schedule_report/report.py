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
