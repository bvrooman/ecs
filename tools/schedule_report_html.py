#!/usr/bin/env python3
"""Render a ScheduleTrace CSV into a self-contained HTML report.

Usage: schedule_report_html.py trace.csv [out.html]

Input is the CSV ScheduleTrace writes (see tools/schedule_trace.hpp):

    tick,wave,system,busy_us,prepare_us,finish_us,items,wave_us,flush_us,tick_us

Output is one HTML file with no external requests (inline CSS/JS/SVG):

    * headline stat tiles (mean / p50 / p99 tick, ticks per second)
    * tick wall time over the run (line + min..max band when downsampled)
    * per-wave wall time split into work vs command flush (stacked bars)
    * per-system busy comparison, then a card per system with its
      distribution histogram, busy-over-time line, and barrier-hook costs
    * the full numbers as a table (everything a chart shows is in the table)

Stdlib only; charts are hand-built SVG. Hover layers (crosshair on lines,
per-bar tooltips) are inline JS reading the same embedded data.
"""

from __future__ import annotations

import csv
import html
import json
import math
import sys
from collections import defaultdict


# --------------------------------------------------------------------------
# data
# --------------------------------------------------------------------------

def load(path):
    """Parse the trace into per-tick and per-system series."""
    systems = {}   # name -> dict(busy=[], prep=[], fin=[], items, wave, ticks=[])
    ticks = {}     # tick -> tick_us
    waves = {}     # wave -> {tick: (wave_us, flush_us)}
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        need = {"tick", "wave", "system", "busy_us", "prepare_us",
                "finish_us", "items", "wave_us", "flush_us", "tick_us"}
        missing = need - set(reader.fieldnames or [])
        if missing:
            sys.exit(f"error: {path} is missing columns {sorted(missing)} -- "
                     "is this a tools/schedule_trace.hpp CSV?")
        for row in reader:
            tick = int(row["tick"])
            wave = int(row["wave"])
            name = row["system"]
            s = systems.setdefault(name, {
                "busy": [], "prep": [], "fin": [], "ticks": [],
                "items": 0, "wave": wave,
            })
            s["busy"].append(float(row["busy_us"]))
            s["prep"].append(float(row["prepare_us"]))
            s["fin"].append(float(row["finish_us"]))
            s["ticks"].append(tick)
            s["items"] = max(s["items"], int(row["items"]))
            s["wave"] = wave  # last seen wave placement
            ticks[tick] = float(row["tick_us"])
            waves.setdefault(wave, {})[tick] = (float(row["wave_us"]),
                                                float(row["flush_us"]))
    if not ticks:
        sys.exit(f"error: no rows in {path}")
    return systems, ticks, waves


def stats(xs):
    n = len(xs)
    if n == 0:
        return dict(n=0, mean=0, sd=0, p50=0, p99=0, mx=0)
    ordered = sorted(xs)
    mean = sum(xs) / n
    var = sum((x - mean) ** 2 for x in xs) / n
    pct = lambda p: ordered[min(n - 1, int(p * n))]
    return dict(n=n, mean=mean, sd=math.sqrt(var),
                p50=pct(0.50), p99=pct(0.99), mx=ordered[-1])


def fmt_us(v):
    """Compact microsecond value: 12.3 us / 4.56 ms / 1.20 s."""
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


def downsample(ticks_sorted, values):
    """Per-bucket mean plus min..max band when the series outgrows the plot."""
    n = len(values)
    buckets = 700
    if n <= buckets:
        return ticks_sorted, values, None, None
    xs, mean, lo, hi = [], [], [], []
    for b in range(buckets):
        i0 = b * n // buckets
        i1 = max(i0 + 1, (b + 1) * n // buckets)
        chunk = values[i0:i1]
        xs.append(ticks_sorted[(i0 + i1 - 1) // 2])
        mean.append(sum(chunk) / len(chunk))
        lo.append(min(chunk))
        hi.append(max(chunk))
    return xs, mean, lo, hi


# --------------------------------------------------------------------------
# svg builders (marks per the viz spec: 2px lines, hairline grids, thin bars
# with a rounded data-end and a square baseline, 2px surface gaps)
# --------------------------------------------------------------------------

W, H = 640, 200          # line plot area
PAD_L, PAD_R, PAD_T, PAD_B = 58, 12, 8, 22


def esc(s):
    return html.escape(str(s), quote=True)


def svg_open(w, h, cls=""):
    return (f'<svg viewBox="0 0 {w} {h}" class="{cls}" role="img" '
            f'preserveAspectRatio="none">')


def grid_and_axis(ymax, w=W, h=H):
    """Hairline horizontal gridlines with tick labels on clean values."""
    parts = []
    for tv in nice_ticks(0, ymax):
        y = PAD_T + (h - PAD_T - PAD_B) * (1 - tv / ymax)
        parts.append(f'<line class="grid" x1="{PAD_L}" y1="{y:.1f}" '
                     f'x2="{w - PAD_R}" y2="{y:.1f}"/>')
        parts.append(f'<text class="tick" x="{PAD_L - 6}" y="{y + 3:.1f}" '
                     f'text-anchor="end">{esc(fmt_us(tv))}</text>')
    return "".join(parts)


def line_chart(xs, ys, lo, hi, chart_id):
    """Single-series line, optional min..max wash, crosshair hover layer."""
    ymax = max(hi) if hi else max(ys)
    ymax = ymax * 1.05 or 1
    x0, x1 = xs[0], xs[-1]
    span = (x1 - x0) or 1
    px = lambda x: PAD_L + (W - PAD_L - PAD_R) * (x - x0) / span
    py = lambda v: PAD_T + (H - PAD_T - PAD_B) * (1 - v / ymax)
    out = [svg_open(W, H, "chart")]
    out.append(grid_and_axis(ymax))
    if lo:  # downsample band: the spread the mean line averages over
        pts = [f"{px(x):.1f},{py(v):.1f}" for x, v in zip(xs, hi)]
        pts += [f"{px(x):.1f},{py(v):.1f}" for x, v in zip(reversed(xs), reversed(lo))]
        out.append(f'<polygon class="band" points="{" ".join(pts)}"/>')
    pts = " ".join(f"{px(x):.1f},{py(v):.1f}" for x, v in zip(xs, ys))
    out.append(f'<polyline class="line" points="{pts}"/>')
    # x axis: first/last tick numbers
    out.append(f'<text class="tick" x="{PAD_L}" y="{H - 6}">tick {x0}</text>')
    out.append(f'<text class="tick" x="{W - PAD_R}" y="{H - 6}" '
               f'text-anchor="end">{x1}</text>')
    # crosshair + hover proxy (JS fills in)
    out.append(f'<line class="xhair" id="{chart_id}-xh" y1="{PAD_T}" '
               f'y2="{H - PAD_B}" visibility="hidden"/>')
    out.append(f'<circle class="xdot" id="{chart_id}-dot" r="4" '
               f'visibility="hidden"/>')
    out.append(f'<rect class="hover" data-line="{chart_id}" x="{PAD_L}" '
               f'y="{PAD_T}" width="{W - PAD_L - PAD_R}" '
               f'height="{H - PAD_T - PAD_B}" fill="none" '
               f'pointer-events="all"/>')
    out.append("</svg>")
    data = dict(xs=xs, ys=[round(v, 2) for v in ys],
                lo=[round(v, 2) for v in lo] if lo else None,
                hi=[round(v, 2) for v in hi] if hi else None,
                x0=x0, span=span, ymax=ymax,
                padl=PAD_L, padr=PAD_R, padt=PAD_T, padb=PAD_B, w=W, h=H)
    return "".join(out), data


def histogram(values, chart_id):
    """Distribution of one measure: columns, rounded caps, 2px gaps."""
    n = len(values)
    lo, hi = min(values), max(values)
    if hi <= lo:
        hi = lo + 1
    bins = max(10, min(40, int(math.sqrt(n))))
    counts = [0] * bins
    width = (hi - lo) / bins
    for v in values:
        counts[min(bins - 1, int((v - lo) / width))] += 1
    cmax = max(counts) or 1
    h = 120
    out = [svg_open(W, h, "chart")]
    for tv in nice_ticks(0, cmax, 2):
        y = PAD_T + (h - PAD_T - PAD_B) * (1 - tv / cmax)
        out.append(f'<line class="grid" x1="{PAD_L}" y1="{y:.1f}" '
                   f'x2="{W - PAD_R}" y2="{y:.1f}"/>')
        out.append(f'<text class="tick" x="{PAD_L - 6}" y="{y + 3:.1f}" '
                   f'text-anchor="end">{tv:g}</text>')
    plot_w = W - PAD_L - PAD_R
    slot = plot_w / bins
    bw = min(24.0, max(3.0, slot - 2))  # 2px surface gap, <=24px thick
    base = h - PAD_B
    for i, c in enumerate(counts):
        if c == 0:
            continue
        x = PAD_L + i * slot + (slot - bw) / 2
        y = PAD_T + (base - PAD_T) * (1 - c / cmax)
        bh = base - y
        r = min(4, bh, bw / 2)  # rounded cap, square baseline
        out.append(
            f'<path class="bar" data-hist="{chart_id}" data-i="{i}" '
            f'd="M{x:.1f},{base:.1f} V{y + r:.1f} Q{x:.1f},{y:.1f} '
            f'{x + r:.1f},{y:.1f} H{x + bw - r:.1f} Q{x + bw:.1f},{y:.1f} '
            f'{x + bw:.1f},{y + r:.1f} V{base:.1f} Z"/>')
    out.append(f'<text class="tick" x="{PAD_L}" y="{h - 6}">'
               f'{esc(fmt_us(lo))}</text>')
    out.append(f'<text class="tick" x="{W - PAD_R}" y="{h - 6}" '
               f'text-anchor="end">{esc(fmt_us(hi))}</text>')
    out.append("</svg>")
    edges = [lo + i * width for i in range(bins + 1)]
    return "".join(out), dict(counts=counts, edges=[round(e, 2) for e in edges])


def hbars(rows, chart_id, stacked=False):
    """Horizontal bars: magnitude comparison (one hue), or a two-segment
    work/flush composition (ordinal steps of the same hue, 2px gaps).
    rows: (label, value) or (label, work, flush)."""
    label_w = 150
    row_h, bar_h = 26, 16
    h = PAD_T + len(rows) * row_h + 8
    vmax = max((r[1] + (r[2] if stacked else 0)) for r in rows) or 1
    plot_w = W - label_w - PAD_R - 60
    out = [svg_open(W, h, "chart")]
    for i, r in enumerate(rows):
        y = PAD_T + i * row_h + (row_h - bar_h) / 2
        label = r[0]
        out.append(f'<text class="lab" x="{label_w - 8}" y="{y + bar_h - 4}" '
                   f'text-anchor="end">{esc(label)}</text>')
        total = r[1] + (r[2] if stacked else 0)
        w1 = plot_w * r[1] / vmax
        segs = [(label_w, w1, "bar")]
        if stacked and r[2] > 0:
            w2 = max(0.0, plot_w * r[2] / vmax - 2)  # 2px surface gap
            segs.append((label_w + w1 + 2, w2, "bar2"))
        for j, (x, w, cls) in enumerate(segs):
            if w <= 0:
                continue
            last = j == len(segs) - 1
            rr = min(4.0, w, bar_h / 2) if last else 0  # data-end rounded only
            out.append(
                f'<path class="{cls}" data-hbar="{chart_id}" data-i="{i}" '
                f'd="M{x:.1f},{y:.1f} H{x + w - rr:.1f} '
                f'Q{x + w:.1f},{y:.1f} {x + w:.1f},{y + rr:.1f} '
                f'V{y + bar_h - rr:.1f} Q{x + w:.1f},{y + bar_h:.1f} '
                f'{x + w - rr:.1f},{y + bar_h:.1f} H{x:.1f} Z"/>')
        out.append(f'<text class="val" x="{label_w + plot_w * total / vmax + 6:.1f}" '
                   f'y="{y + bar_h - 4}">{esc(fmt_us(total))}</text>')
    out.append("</svg>")
    return "".join(out), [dict(label=r[0], v=r[1],
                               flush=(r[2] if stacked else None)) for r in rows]


# --------------------------------------------------------------------------
# page
# --------------------------------------------------------------------------

CSS = """
:root {
  --surface: #fcfcfb; --card: #ffffff; --hairline: #e7e6e2;
  --ink: #0b0b0b; --ink-2: #52514e; --ink-3: #8a897f;
  --accent: #2a78d6; --accent-soft: #86b6ef; --band: rgba(42,120,214,.10);
}
@media (prefers-color-scheme: dark) {
  :root {
    --surface: #1a1a19; --card: #222221; --hairline: #383835;
    --ink: #ffffff; --ink-2: #c3c2b7; --ink-3: #8a897f;
    --accent: #3987e5; --accent-soft: #1c5cab; --band: rgba(57,135,229,.14);
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--surface); color: var(--ink);
  font: 14px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width: 960px; margin: 0 auto; padding: 24px 20px 64px; }
h1 { font-size: 20px; margin: 0 0 2px; }
h2 { font-size: 15px; margin: 28px 0 10px; }
.meta { color: var(--ink-2); margin-bottom: 18px; }
.tiles { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
  gap: 10px; }
.tile { background: var(--card); border: 1px solid var(--hairline);
  border-radius: 8px; padding: 10px 14px; }
.tile .k { color: var(--ink-2); font-size: 12px; }
.tile .v { font-size: 26px; font-weight: 600; margin-top: 2px; }
.card { background: var(--card); border: 1px solid var(--hairline);
  border-radius: 8px; padding: 14px 16px 10px; margin-bottom: 14px; }
.card h3 { margin: 0 0 2px; font-size: 14px; }
.card .sub { color: var(--ink-2); font-size: 12px; margin-bottom: 8px; }
.statrow { display: flex; flex-wrap: wrap; gap: 18px; color: var(--ink-2);
  font-size: 12px; margin: 6px 0 10px; }
.statrow b { color: var(--ink); font-weight: 600; font-size: 13px;
  font-variant-numeric: tabular-nums; }
svg.chart { width: 100%; height: auto; display: block; }
.grid { stroke: var(--hairline); stroke-width: 1; }
.tick, .lab, .val { font: 11px system-ui, sans-serif; fill: var(--ink-2); }
.val { fill: var(--ink); font-variant-numeric: tabular-nums; }
.line { fill: none; stroke: var(--accent); stroke-width: 2;
  stroke-linejoin: round; stroke-linecap: round; }
.band { fill: var(--band); }
.bar { fill: var(--accent); }
.bar2 { fill: var(--accent-soft); }
.bar:hover, .bar2:hover { filter: brightness(1.12); }
.xhair { stroke: var(--ink-3); stroke-width: 1; }
.xdot { fill: var(--accent); stroke: var(--surface); stroke-width: 2; }
.legend { display: flex; gap: 16px; font-size: 12px; color: var(--ink-2);
  margin: 4px 0 8px; }
.legend .sw { display: inline-block; width: 12px; height: 12px;
  border-radius: 3px; vertical-align: -2px; margin-right: 5px; }
#tip { position: fixed; pointer-events: none; background: var(--card);
  border: 1px solid var(--hairline); border-radius: 6px; padding: 6px 9px;
  font-size: 12px; box-shadow: 0 2px 8px rgba(0,0,0,.12); display: none;
  z-index: 9; max-width: 260px; }
#tip .tv { font-weight: 600; font-variant-numeric: tabular-nums; }
#tip .tk { color: var(--ink-2); }
table { border-collapse: collapse; width: 100%; font-size: 12.5px; }
th, td { text-align: right; padding: 5px 10px;
  border-bottom: 1px solid var(--hairline);
  font-variant-numeric: tabular-nums; }
th { color: var(--ink-2); font-weight: 500; }
th:first-child, td:first-child { text-align: left; }
"""

JS = """
(function () {
  const D = JSON.parse(document.getElementById('data').textContent);
  const tip = document.getElementById('tip');
  function fmt(v) {
    if (v >= 1e6) return (v / 1e6).toFixed(2) + ' s';
    if (v >= 1e3) return (v / 1e3).toFixed(2) + ' ms';
    return v.toFixed(1) + ' \\u00b5s';
  }
  function show(evt, rows) {
    tip.replaceChildren();
    rows.forEach(([k, v]) => {
      const d = document.createElement('div');
      const b = document.createElement('span');
      b.className = 'tv'; b.textContent = v;          // values lead
      const s = document.createElement('span');
      s.className = 'tk'; s.textContent = '  ' + k;   // labels follow
      d.append(b, s); tip.append(d);
    });
    tip.style.display = 'block';
    const pad = 14, tw = tip.offsetWidth, th = tip.offsetHeight;
    let x = evt.clientX + pad, y = evt.clientY + pad;
    if (x + tw > innerWidth - 8) x = evt.clientX - tw - pad;
    if (y + th > innerHeight - 8) y = evt.clientY - th - pad;
    tip.style.left = x + 'px'; tip.style.top = y + 'px';
  }
  const hide = () => { tip.style.display = 'none'; };

  document.querySelectorAll('rect.hover[data-line]').forEach(r => {
    const id = r.dataset.line, d = D.lines[id];
    const svg = r.closest('svg');
    const xh = document.getElementById(id + '-xh');
    const dot = document.getElementById(id + '-dot');
    const plotW = d.w - d.padl - d.padr;
    r.addEventListener('pointermove', evt => {
      const box = svg.getBoundingClientRect();
      const fx = (evt.clientX - box.left) / box.width * d.w;   // viewBox units
      const xval = d.x0 + (fx - d.padl) / plotW * d.span;
      let best = 0, err = Infinity;                            // snap to nearest X
      d.xs.forEach((x, i) => {
        const e = Math.abs(x - xval); if (e < err) { err = e; best = i; }
      });
      const px = d.padl + (d.xs[best] - d.x0) / d.span * plotW;
      const py = d.padt + (d.h - d.padt - d.padb) * (1 - d.ys[best] / d.ymax);
      xh.setAttribute('x1', px); xh.setAttribute('x2', px);
      xh.removeAttribute('visibility');
      dot.setAttribute('cx', px); dot.setAttribute('cy', py);
      dot.removeAttribute('visibility');
      const rows = [[d.label + ' \\u2014 tick ' + d.xs[best], fmt(d.ys[best])]];
      if (d.lo) rows.push(['min\\u2026max in bucket',
                           fmt(d.lo[best]) + ' \\u2026 ' + fmt(d.hi[best])]);
      show(evt, rows);
    });
    r.addEventListener('pointerleave', () => {
      hide(); xh.setAttribute('visibility', 'hidden');
      dot.setAttribute('visibility', 'hidden');
    });
  });

  document.querySelectorAll('[data-hist]').forEach(b => {
    const d = D.hists[b.dataset.hist], i = +b.dataset.i;
    b.addEventListener('pointermove', evt => show(evt, [
      [d.label, d.counts[i] + ' ticks'],
      ['range', fmt(d.edges[i]) + ' \\u2013 ' + fmt(d.edges[i + 1])],
    ]));
    b.addEventListener('pointerleave', hide);
  });

  document.querySelectorAll('[data-hbar]').forEach(b => {
    const d = D.hbars[b.dataset.hbar][+b.dataset.i];
    b.addEventListener('pointermove', evt => {
      const rows = [[d.label, fmt(d.v + (d.flush || 0))]];
      if (d.flush != null) {
        rows.push(['work', fmt(d.v)]);
        rows.push(['command flush', fmt(d.flush)]);
      }
      show(evt, rows);
    });
    b.addEventListener('pointerleave', hide);
  });
})();
"""


def build(path, out_path):
    systems, ticks, waves = load(path)
    tick_ids = sorted(ticks)
    tick_vals = [ticks[t] for t in tick_ids]
    ts = stats(tick_vals)

    lines, hists, hbars_data = {}, {}, {}
    body = []
    body.append(f"<h1>Schedule report</h1>")
    body.append(f'<div class="meta">{esc(path)} &middot; '
                f'{len(tick_ids):,} ticks &middot; {len(systems)} systems '
                f'&middot; {len(waves)} waves</div>')

    # --- headline tiles ----------------------------------------------------
    tps = 1e6 / ts["mean"] if ts["mean"] else 0
    tiles = [("mean tick", fmt_us(ts["mean"])), ("p50 tick", fmt_us(ts["p50"])),
             ("p99 tick", fmt_us(ts["p99"])), ("ticks / s", f"{tps:,.0f}")]
    body.append('<div class="tiles">' + "".join(
        f'<div class="tile"><div class="k">{esc(k)}</div>'
        f'<div class="v">{esc(v)}</div></div>' for k, v in tiles) + "</div>")

    # --- tick wall time over the run ----------------------------------------
    xs, ys, lo, hi = downsample(tick_ids, tick_vals)
    svg, data = line_chart(xs, ys, lo, hi, "tick")
    data["label"] = "tick wall"
    lines["tick"] = data
    body.append('<h2>Tick wall time</h2><div class="card">')
    if lo:
        body.append('<div class="sub">mean per bucket; the wash spans each '
                    'bucket’s min…max</div>')
    body.append(svg + "</div>")

    # --- waves: wall vs flush -----------------------------------------------
    rows = []
    for w in sorted(waves):
        per_tick = waves[w].values()
        wall = sum(v[0] for v in per_tick) / len(per_tick)
        flush = sum(v[1] for v in per_tick) / len(per_tick)
        rows.append((f"wave {w}", max(0.0, wall - flush), flush))
    svg, data = hbars(rows, "waves", stacked=True)
    hbars_data["waves"] = data
    body.append('<h2>Waves</h2><div class="card">'
                '<div class="legend">'
                '<span><span class="sw" style="background:var(--accent)"></span>'
                'work</span>'
                '<span><span class="sw" style="background:var(--accent-soft)">'
                '</span>command flush</span></div>'
                '<div class="sub">mean wall time per tick, per wave</div>'
                + svg + "</div>")

    # --- systems overview ----------------------------------------------------
    # Amortized over the whole run (sum / ticks), so a one-shot setup system
    # (n=1) compares honestly against every-tick systems; its per-sample
    # stats stay un-amortized in its card and the table.
    order = sorted(systems, key=lambda n: -sum(systems[n]["busy"]))
    rows = [(name, sum(systems[name]["busy"]) / len(tick_ids)) for name in order]
    svg, data = hbars(rows, "sys")
    hbars_data["sys"] = data
    body.append('<h2>Systems by busy time per tick</h2><div class="card">'
                '<div class="sub">total busy across the run / total ticks. '
                'busy = sum of the system’s work items across all lanes '
                '(CPU time; a fanned wave’s systems legitimately sum past '
                'the wave’s wall time)</div>'
                + svg + "</div>")

    # --- per-system cards -----------------------------------------------------
    body.append("<h2>Systems</h2>")
    for idx, name in enumerate(order):
        s = systems[name]
        st = stats(s["busy"])
        prep = sum(s["prep"]) / len(s["prep"])
        fin = sum(s["fin"]) / len(s["fin"])
        share = 100.0 * st["mean"] / ts["mean"] if ts["mean"] else 0
        cid = f"s{idx}"
        once = f' &middot; ran {st["n"]} of {len(tick_ids)} ticks' \
            if st["n"] < len(tick_ids) else ""
        body.append(f'<div class="card"><h3>{esc(name)}</h3>'
                    f'<div class="sub">wave {s["wave"]} &middot; '
                    f'{s["items"]} work item{"s" if s["items"] != 1 else ""}'
                    f' &middot; busy = {share:.1f}% of mean tick wall{once}'
                    f'</div>')
        body.append('<div class="statrow">'
                    + "".join(f'<span>{esc(k)} <b>{esc(v)}</b></span>' for k, v in [
                        ("mean", fmt_us(st["mean"])), ("sd", fmt_us(st["sd"])),
                        ("p50", fmt_us(st["p50"])), ("p99", fmt_us(st["p99"])),
                        ("max", fmt_us(st["mx"])),
                        ("prepare", fmt_us(prep)), ("finish", fmt_us(fin)),
                    ]) + "</div>")
        svg, hdata = histogram(s["busy"], f"h-{cid}")
        hdata["label"] = name
        hists[f"h-{cid}"] = hdata
        body.append(f'<div class="sub">busy time distribution</div>{svg}')
        sxs, sys_, slo, shi = downsample(s["ticks"], s["busy"])
        svg, ldata = line_chart(sxs, sys_, slo, shi, f"l-{cid}")
        ldata["label"] = name + " busy"
        lines[f"l-{cid}"] = ldata
        body.append(f'<div class="sub">busy time over the run</div>{svg}</div>')

    # --- table -----------------------------------------------------------------
    body.append("<h2>All numbers</h2><div class='card' style='overflow-x:auto'>"
                "<table><thead><tr><th>system</th><th>wave</th><th>items</th>"
                "<th>n</th><th>mean</th><th>sd</th><th>p50</th><th>p99</th>"
                "<th>max</th><th>prepare</th><th>finish</th>"
                "<th>% of tick</th></tr></thead><tbody>")
    for name in order:
        s = systems[name]
        st = stats(s["busy"])
        prep = sum(s["prep"]) / len(s["prep"])
        fin = sum(s["fin"]) / len(s["fin"])
        share = 100.0 * st["mean"] / ts["mean"] if ts["mean"] else 0
        body.append("<tr>" + "".join(f"<td>{esc(c)}</td>" for c in [
            name, s["wave"], s["items"], st["n"], fmt_us(st["mean"]),
            fmt_us(st["sd"]), fmt_us(st["p50"]), fmt_us(st["p99"]),
            fmt_us(st["mx"]), fmt_us(prep), fmt_us(fin), f"{share:.1f}%",
        ]) + "</tr>")
    body.append("</tbody></table></div>")

    payload = json.dumps(dict(lines=lines, hists=hists, hbars=hbars_data))
    doc = ("<!doctype html><html lang='en'><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           f"<title>Schedule report — {esc(path)}</title>"
           f"<style>{CSS}</style></head><body><main>"
           + "".join(body) +
           "</main><div id='tip'></div>"
           f"<script type='application/json' id='data'>{payload}</script>"
           f"<script>{JS}</script></body></html>")
    with open(out_path, "w") as f:
        f.write(doc)
    print(f"wrote {out_path} ({len(doc) / 1024:.0f} KB): "
          f"{len(tick_ids):,} ticks, {len(systems)} systems")


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        sys.exit(__doc__.strip())
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".html"
    build(src, out)


if __name__ == "__main__":
    main()
