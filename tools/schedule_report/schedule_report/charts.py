"""Hand-built SVG chart fragments.

Marks follow the report's viz spec: 2px lines, hairline solid gridlines,
bars <= 24px thick with a rounded data-end and a square baseline, 2px
surface gaps between touching fills. Each builder returns (svg_markup,
hover_payload) -- the payload is embedded as JSON and drives the tooltip
layer in static/report.js.
"""

from __future__ import annotations

import html

from .trace import fmt_us, nice_ticks

W, H = 640, 200          # line plot area
PAD_L, PAD_R, PAD_T, PAD_B = 58, 12, 8, 22


def esc(s):
    return html.escape(str(s), quote=True)


def svg_open(w, h):
    return (f'<svg viewBox="0 0 {w} {h}" class="chart" role="img" '
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

    def px(x):
        return PAD_L + (W - PAD_L - PAD_R) * (x - x0) / span

    def py(v):
        return PAD_T + (H - PAD_T - PAD_B) * (1 - v / ymax)

    out = [svg_open(W, H), grid_and_axis(ymax)]
    if lo:  # downsample band: the spread the mean line averages over
        pts = [f"{px(x):.1f},{py(v):.1f}" for x, v in zip(xs, hi)]
        pts += [f"{px(x):.1f},{py(v):.1f}"
                for x, v in zip(reversed(xs), reversed(lo))]
        out.append(f'<polygon class="band" points="{" ".join(pts)}"/>')
    pts = " ".join(f"{px(x):.1f},{py(v):.1f}" for x, v in zip(xs, ys))
    out.append(f'<polyline class="line" points="{pts}"/>')
    out.append(f'<text class="tick" x="{PAD_L}" y="{H - 6}">tick {x0}</text>')
    out.append(f'<text class="tick" x="{W - PAD_R}" y="{H - 6}" '
               f'text-anchor="end">{x1}</text>')
    # crosshair + hover proxy (static/report.js drives these)
    out.append(f'<line class="xhair" id="{chart_id}-xh" y1="{PAD_T}" '
               f'y2="{H - PAD_B}" visibility="hidden"/>')
    out.append(f'<circle class="xdot" id="{chart_id}-dot" r="4" '
               f'visibility="hidden"/>')
    out.append(f'<rect class="hover" data-line="{chart_id}" x="{PAD_L}" '
               f'y="{PAD_T}" width="{W - PAD_L - PAD_R}" '
               f'height="{H - PAD_T - PAD_B}" fill="none" '
               f'pointer-events="all"/>')
    out.append("</svg>")
    payload = dict(xs=xs, ys=[round(v, 2) for v in ys],
                   lo=[round(v, 2) for v in lo] if lo else None,
                   hi=[round(v, 2) for v in hi] if hi else None,
                   x0=x0, span=span, ymax=ymax,
                   padl=PAD_L, padr=PAD_R, padt=PAD_T, padb=PAD_B, w=W, h=H)
    return "".join(out), payload


def histogram(values, chart_id, bins_cap=40):
    """Distribution of one measure: columns, rounded caps, 2px gaps."""
    import math
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
    out = [svg_open(W, h)]
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
    out = [svg_open(W, h)]
    for i, r in enumerate(rows):
        y = PAD_T + i * row_h + (row_h - bar_h) / 2
        out.append(f'<text class="lab" x="{label_w - 8}" y="{y + bar_h - 4}" '
                   f'text-anchor="end">{esc(r[0])}</text>')
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
        out.append(f'<text class="val" '
                   f'x="{label_w + plot_w * total / vmax + 6:.1f}" '
                   f'y="{y + bar_h - 4}">{esc(fmt_us(total))}</text>')
    out.append("</svg>")
    payload = [dict(label=r[0], v=r[1], flush=(r[2] if stacked else None))
               for r in rows]
    return "".join(out), payload
