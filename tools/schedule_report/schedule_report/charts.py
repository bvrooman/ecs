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


def line_chart(xs, ys, lo, hi, chart_id, xs2=None, ys2=None, ymax_cap=None):
    """Single-series line, optional min..max wash, crosshair hover layer.

    xs2/ys2: an optional SECOND series (drawn under the first in the soft
    accent step -- base vs head in the compare view); the shared x/y scales
    cover both. ymax_cap clips the y scale (a lone 20x scheduler-noise spike
    would flatten everything else); capped points draw off the top and the
    tooltip still reports their true values."""
    ymax = max(hi) if hi else max(ys)
    if ys2:
        ymax = max(ymax, max(ys2))
    if ymax_cap is not None:
        ymax = min(ymax, ymax_cap)
    ymax = ymax * 1.05 or 1
    x0 = min(xs[0], xs2[0]) if xs2 else xs[0]
    x1 = max(xs[-1], xs2[-1]) if xs2 else xs[-1]
    span = (x1 - x0) or 1

    def px(x):
        return PAD_L + (W - PAD_L - PAD_R) * (x - x0) / span

    def py(v):
        # clamp to the plot top: values past a ymax_cap pin to the edge
        return max(PAD_T, PAD_T + (H - PAD_T - PAD_B) * (1 - v / ymax))

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
               line2=" ".join(f"{px(x):.1f},{py(v):.1f}"
                              for x, v in zip(xs2, ys2)) if xs2 else None,
               x_first=f"tick {x0}",
               x_last=str(x1))
    payload = dict(xs=xs, ys=[round(v, 2) for v in ys],
                   lo=[round(v, 2) for v in lo] if lo else None,
                   hi=[round(v, 2) for v in hi] if hi else None,
                   x0=x0, span=span, ymax=ymax,
                   padl=PAD_L, padr=PAD_R, padt=PAD_T, padb=PAD_B, w=W, h=H)
    if xs2:
        payload["xs2"] = xs2
        payload["ys2"] = [round(v, 2) for v in ys2]
    return ctx, payload


def multiline_chart(series, lane_labels, chart_id, ideal=None):
    """Speedup-vs-lanes scaling curves: one line per series over evenly-spaced
    lane ticks, per-point dots, and an optional dashed ideal-linear reference.

    series: list of dict(label=, ys=[speedup per lane], slot=int categorical).
    lane_labels: the lane counts, one per point (the x tick labels).
    ideal: optional [speedup per lane] drawn muted+dashed (perfect scaling)."""
    n = len(lane_labels)
    h = 240
    # Scale to the REAL series only; the ideal diagonal is a reference, so let
    # it run off the top (clipped by the viewBox) rather than compressing the
    # measured curves -- with heavy oversubscription the real peak is a small
    # fraction of ideal-at-max-lanes.
    allys = [v for s in series for v in s["ys"]]
    ymax = (max(allys) if allys else 1) * 1.15 or 1
    plot_w = W - PAD_L - PAD_R

    def px(i):
        return PAD_L + (plot_w * i / (n - 1) if n > 1 else plot_w / 2)

    def py(v):
        return PAD_T + (h - PAD_T - PAD_B) * (1 - v / ymax)

    lines, dots = [], []
    for s in series:
        lines.append(dict(slot=s["slot"], pts=" ".join(
            f"{px(i):.1f},{py(v):.1f}" for i, v in enumerate(s["ys"]))))
        for i, v in enumerate(s["ys"]):
            dots.append(dict(slot=s["slot"], cx=f"{px(i):.1f}",
                             cy=f"{py(v):.1f}", label=s["label"],
                             lane=lane_labels[i], spd=f"{v:.2f}"))
    ctx = dict(_frame(W, h), id=chart_id,
               grid=_gridlines(ymax, h, lambda v: f"{v:g}x"),
               xticks=[dict(x=f"{px(i):.1f}", label=str(l))
                       for i, l in enumerate(lane_labels)],
               ideal=" ".join(f"{px(i):.1f},{py(v):.1f}"
                              for i, v in enumerate(ideal)) if ideal else None,
               lines=lines, dots=dots,
               legend=[dict(label=s["label"], slot=s["slot"]) for s in series])
    return ctx, None


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

    rows: stacked -> (label, work, flush[, kind]); plain -> (label, value[, kind]).
    An optional trailing `kind` string recolors the bar's data segment via the
    class `bar-<kind>` (e.g. "maint" for the serial maintenance phase); the flush
    segment always keeps the soft step."""
    label_w = 150
    row_h, bar_h = 26, 16
    h = PAD_T + len(rows) * row_h + 8

    def work(r):
        return r[1]

    def flush(r):
        return r[2] if stacked else 0.0

    def kind(r):
        k = (r[3] if len(r) > 3 else None) if stacked \
            else (r[2] if len(r) > 2 else None)
        return k if isinstance(k, str) else None

    vmax = max((work(r) + flush(r)) for r in rows) or 1
    plot_w = W - label_w - PAD_R - 60
    out_rows = []
    for i, r in enumerate(rows):
        y = PAD_T + i * row_h + (row_h - bar_h) / 2
        total = work(r) + flush(r)
        w1 = plot_w * work(r) / vmax
        base_cls = f"bar-{kind(r)}" if kind(r) else "bar"
        segs = [(label_w, w1, base_cls)]
        if stacked and flush(r) > 0:
            w2 = max(0.0, plot_w * flush(r) / vmax - 2)  # 2px surface gap
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
    # A maintenance bar is a single serial magnitude -- no work/flush split --
    # so it reports flush=None (a plain bar) and carries a `maint` note.
    payload = [dict(label=r[0], v=work(r),
                    flush=(flush(r) if (stacked and kind(r) != "maint")
                           else None),
                    maint=(kind(r) == "maint"))
               for r in rows]
    return ctx, payload


def paired_hbars(rows, chart_id):
    """Base-vs-head magnitude comparison: two thin bars per row (base in the
    soft accent step above, head in the full accent below), one shared scale,
    the head bar annotated with its delta.

    rows: (label, base_value, head_value, delta_text, delta_cls) --
    delta_cls is the CSS class for the annotation ("up"/"down"/"flat")."""
    label_w = 150
    bar_h, gap = 9, 2
    row_h = 2 * bar_h + gap + 10
    h = PAD_T + len(rows) * row_h + 8
    vmax = max(max(r[1], r[2]) for r in rows) or 1
    plot_w = W - label_w - PAD_R - 96  # room for value + delta annotations
    out_rows = []
    for i, r in enumerate(rows):
        y = PAD_T + i * row_h + 4
        bars = []
        for j, (v, cls) in enumerate(((r[1], "bar2"), (r[2], "bar"))):
            w = plot_w * v / vmax
            if w <= 0:
                continue
            by = y + j * (bar_h + gap)
            r_ = min(3.0, w, bar_h / 2)
            bars.append(dict(cls=cls, i=i,
                             d=_rounded_bar_d(label_w, by, w, bar_h, r_,
                                              vertical=False)))
        vx = label_w + plot_w * max(r[1], r[2]) / vmax + 6
        out_rows.append(dict(label=r[0],
                             ly=f"{y + bar_h + gap + 4:.1f}",
                             bars=bars,
                             vx=f"{vx:.1f}",
                             vy=f"{y + bar_h + gap + 4:.1f}",
                             value=fmt_us(r[2]),
                             delta=r[3], delta_cls=r[4]))
    ctx = dict(_frame(W, h), id=chart_id, label_w=label_w, rows=out_rows)
    payload = [dict(label=r[0], base=round(r[1], 2), head=round(r[2], 2),
                    delta=r[3]) for r in rows]
    return ctx, payload
