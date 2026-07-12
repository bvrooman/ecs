"""Chart geometry.

Each builder returns (context, payload): the context is plain data --
coordinates, path strings, labels -- rendered into SVG by the matching
Jinja template under templates/charts/ (line.svg.j2, histogram.svg.j2,
hbars.svg.j2), and the payload drives the hover layer (static/report.js).
Python owns the math (scales, tick placement, binning, rounded-end path
strings); the templates own every element, class, and attribute, so the
markup is editable without touching code.

Marks follow the report's viz spec: 2px lines, hairline solid gridlines,
bars <= 24px thick with a rounded data-end and a square baseline, 2px
surface gaps between touching fills.
"""

from __future__ import annotations

import math

from .trace import fmt_us, nice_ticks

W, H = 640, 200          # line plot area
PAD_L, PAD_R, PAD_T, PAD_B = 58, 12, 8, 22


def _frame(w, h):
    """The shared plot-frame numbers every chart context carries."""
    return dict(w=w, h=h, padl=PAD_L, padr=PAD_R, padt=PAD_T, padb=PAD_B)


def _gridlines(vmax, h, fmt):
    """Hairline y-gridline positions with tick labels on clean values."""
    rows = []
    for tv in nice_ticks(0, vmax):
        y = PAD_T + (h - PAD_T - PAD_B) * (1 - tv / vmax)
        rows.append(dict(y=f"{y:.1f}", ty=f"{y + 3:.1f}", label=fmt(tv)))
    return rows


def _rounded_bar_d(x, y, w, h, r, vertical):
    """Path for a bar with a rounded DATA end and a square baseline end.

    vertical: column growing up from y+h (rounded top); otherwise a
    horizontal bar growing right from x (rounded right end)."""
    if vertical:
        base = y + h
        return (f"M{x:.1f},{base:.1f} V{y + r:.1f} "
                f"Q{x:.1f},{y:.1f} {x + r:.1f},{y:.1f} "
                f"H{x + w - r:.1f} Q{x + w:.1f},{y:.1f} "
                f"{x + w:.1f},{y + r:.1f} V{base:.1f} Z")
    return (f"M{x:.1f},{y:.1f} H{x + w - r:.1f} "
            f"Q{x + w:.1f},{y:.1f} {x + w:.1f},{y + r:.1f} "
            f"V{y + h - r:.1f} Q{x + w:.1f},{y + h:.1f} "
            f"{x + w - r:.1f},{y + h:.1f} H{x:.1f} Z")


def line_chart(xs, ys, lo, hi, chart_id):
    """Single-series line, optional min..max wash, crosshair hover layer."""
    ymax = max(hi) if hi else max(ys)
    ymax = ymax * 1.05 or 1
    x0, x1 = xs[0], xs[-1]
    span = (x1 - x0) or 1

    def px(x):
        return PAD_L + (W - PAD_L - PAD_R) * (x - x0) / span

    def py(v):
        return PAD_T + (H - PAD_T - PAD_B) * (1 - v / ymax)

    band = None
    if lo:  # downsample band: the spread the mean line averages over
        pts = [f"{px(x):.1f},{py(v):.1f}" for x, v in zip(xs, hi)]
        pts += [f"{px(x):.1f},{py(v):.1f}"
                for x, v in zip(reversed(xs), reversed(lo))]
        band = " ".join(pts)
    ctx = dict(_frame(W, H),
               id=chart_id,
               grid=_gridlines(ymax, H, fmt_us),
               band=band,
               line=" ".join(f"{px(x):.1f},{py(v):.1f}"
                             for x, v in zip(xs, ys)),
               x_first=f"tick {x0}",
               x_last=str(x1))
    payload = dict(xs=xs, ys=[round(v, 2) for v in ys],
                   lo=[round(v, 2) for v in lo] if lo else None,
                   hi=[round(v, 2) for v in hi] if hi else None,
                   x0=x0, span=span, ymax=ymax,
                   padl=PAD_L, padr=PAD_R, padt=PAD_T, padb=PAD_B, w=W, h=H)
    return ctx, payload


def histogram(values, chart_id, bins_cap=40):
    """Distribution of one measure: columns, rounded caps, 2px gaps."""
    n = len(values)
    lo, hi = min(values), max(values)
    if hi <= lo:
        hi = lo + 1
    bins = max(10, min(bins_cap, int(math.sqrt(n))))
    counts = [0] * bins
    width = (hi - lo) / bins
    for v in values:
        counts[min(bins - 1, int((v - lo) / width))] += 1
    cmax = max(counts) or 1
    h = 120
    plot_w = W - PAD_L - PAD_R
    slot = plot_w / bins
    bw = min(24.0, max(3.0, slot - 2))  # 2px surface gap, <=24px thick
    base = h - PAD_B
    bars = []
    for i, c in enumerate(counts):
        if c == 0:
            continue
        x = PAD_L + i * slot + (slot - bw) / 2
        y = PAD_T + (base - PAD_T) * (1 - c / cmax)
        r = min(4, base - y, bw / 2)  # rounded cap, square baseline
        bars.append(dict(i=i, d=_rounded_bar_d(x, y, bw, base - y, r,
                                               vertical=True)))
    ctx = dict(_frame(W, h),
               id=chart_id,
               grid=_hist_grid(cmax, h),
               bars=bars,
               x_first=fmt_us(lo),
               x_last=fmt_us(hi))
    edges = [lo + i * width for i in range(bins + 1)]
    return ctx, dict(counts=counts, edges=[round(e, 2) for e in edges])


def _hist_grid(cmax, h):
    rows = []
    for tv in nice_ticks(0, cmax, 2):
        y = PAD_T + (h - PAD_T - PAD_B) * (1 - tv / cmax)
        rows.append(dict(y=f"{y:.1f}", ty=f"{y + 3:.1f}", label=f"{tv:g}"))
    return rows


def hbars(rows, chart_id, stacked=False):
    """Horizontal bars: magnitude comparison (one hue), or a two-segment
    work/flush composition (ordinal steps of the same hue, 2px gaps).

    rows: (label, value) or (label, work, flush)."""
    label_w = 150
    row_h, bar_h = 26, 16
    h = PAD_T + len(rows) * row_h + 8
    vmax = max((r[1] + (r[2] if stacked else 0)) for r in rows) or 1
    plot_w = W - label_w - PAD_R - 60
    out_rows = []
    for i, r in enumerate(rows):
        y = PAD_T + i * row_h + (row_h - bar_h) / 2
        total = r[1] + (r[2] if stacked else 0)
        w1 = plot_w * r[1] / vmax
        segs = [(label_w, w1, "bar")]
        if stacked and r[2] > 0:
            w2 = max(0.0, plot_w * r[2] / vmax - 2)  # 2px surface gap
            segs.append((label_w + w1 + 2, w2, "bar2"))
        seg_ctx = []
        for j, (x, w, cls) in enumerate(segs):
            if w <= 0:
                continue
            last = j == len(segs) - 1
            r_ = min(4.0, w, bar_h / 2) if last else 0  # data-end rounded only
            seg_ctx.append(dict(cls=cls, i=i,
                                d=_rounded_bar_d(x, y, w, bar_h, r_,
                                                 vertical=False)))
        out_rows.append(dict(label=r[0],
                             ly=f"{y + bar_h - 4:.1f}",
                             segs=seg_ctx,
                             vx=f"{label_w + plot_w * total / vmax + 6:.1f}",
                             value=fmt_us(total)))
    ctx = dict(_frame(W, h), id=chart_id, label_w=label_w, rows=out_rows)
    payload = [dict(label=r[0], v=r[1], flush=(r[2] if stacked else None))
               for r in rows]
    return ctx, payload
