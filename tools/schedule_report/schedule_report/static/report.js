// Hover layer for the schedule report: crosshair tooltips on line
// charts, per-mark tooltips on bars. Reads the JSON payload embedded
// by templates/report.html.j2; names are inserted via textContent.

(function () {
    // The bench-scaling page carries no JSON payload (its dots hold their
    // own data attributes); the trace/compare pages embed one.
    const dataEl = document.getElementById('data');
    const D = dataEl ? JSON.parse(dataEl.textContent)
        : {lines: {}, hists: {}, hbars: {}, pairs: {}};
    const tip = document.getElementById('tip');

    function fmt(v) {
        if (v >= 1e6) return (v / 1e6).toFixed(2) + ' s';
        if (v >= 1e3) return (v / 1e3).toFixed(2) + ' ms';
        return v.toFixed(1) + ' \u00b5s';
    }

    function show(evt, rows) {
        tip.replaceChildren();
        rows.forEach(([k, v]) => {
            const d = document.createElement('div');
            const b = document.createElement('span');
            b.className = 'tv';
            b.textContent = v;          // values lead
            const s = document.createElement('span');
            s.className = 'tk';
            s.textContent = '  ' + k;   // labels follow
            d.append(b, s);
            tip.append(d);
        });
        tip.style.display = 'block';
        const pad = 14, tw = tip.offsetWidth, th = tip.offsetHeight;
        let x = evt.clientX + pad, y = evt.clientY + pad;
        if (x + tw > innerWidth - 8) x = evt.clientX - tw - pad;
        if (y + th > innerHeight - 8) y = evt.clientY - th - pad;
        tip.style.left = x + 'px';
        tip.style.top = y + 'px';
    }

    const hide = () => {
        tip.style.display = 'none';
    };
    
    // Plain-text hints (e.g. the busy-value explanation) ride the same styled
    // tooltip layer as the charts -- native title tooltips are delayed and
    // inconsistent across browsers.
    function showText(evt, text) {
        tip.replaceChildren();
        const d = document.createElement('div');
        d.className = 'tk';
        d.textContent = text;
        tip.append(d);
        tip.style.display = 'block';
        const pad = 14, tw = tip.offsetWidth, th = tip.offsetHeight;
        let x = evt.clientX + pad, y = evt.clientY + pad;
        if (x + tw > innerWidth - 8) x = evt.clientX - tw - pad;
        if (y + th > innerHeight - 8) y = evt.clientY - th - pad;
        tip.style.left = x + 'px';
        tip.style.top = y + 'px';
    }

    document.querySelectorAll('.hint[data-hint]').forEach(el => {
        el.addEventListener('pointermove', evt => showText(evt, el.dataset.hint));
        el.addEventListener('pointerleave', hide);
    });

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
                const e = Math.abs(x - xval);
                if (e < err) {
                    err = e;
                    best = i;
                }
            });
            const px = d.padl + (d.xs[best] - d.x0) / d.span * plotW;
            const py = d.padt + (d.h - d.padt - d.padb) * (1 - d.ys[best] / d.ymax);
            xh.setAttribute('x1', px);
            xh.setAttribute('x2', px);
            xh.removeAttribute('visibility');
            dot.setAttribute('cx', px);
            dot.setAttribute('cy', py);
            dot.removeAttribute('visibility');
            const rows = [[d.label + ' \u2014 tick ' + d.xs[best], fmt(d.ys[best])]];
            if (d.lo) rows.push(['min\u2026max in bucket',
                fmt(d.lo[best]) + ' \u2026 ' + fmt(d.hi[best])]);
            if (d.xs2) {
                // second (base) series: snap to ITS nearest x -- the two
                // series may have different tick counts
                let b2 = 0, e2 = Infinity;
                d.xs2.forEach((x, i) => {
                    const e = Math.abs(x - d.xs[best]);
                    if (e < e2) {
                        e2 = e;
                        b2 = i;
                    }
                });
                rows.push([d.label2 + ' \u2014 tick ' + d.xs2[b2], fmt(d.ys2[b2])]);
            }
            show(evt, rows);
        });
        r.addEventListener('pointerleave', () => {
            hide();
            xh.setAttribute('visibility', 'hidden');
            dot.setAttribute('visibility', 'hidden');
        });
    });

    document.querySelectorAll('[data-hist]').forEach(b => {
        const d = D.hists[b.dataset.hist], i = +b.dataset.i;
        b.addEventListener('pointermove', evt => show(evt, [
            [d.label, d.counts[i] + ' ticks'],
            ['range', fmt(d.edges[i]) + ' \u2013 ' + fmt(d.edges[i + 1])],
        ]));
        b.addEventListener('pointerleave', hide);
    });

    document.querySelectorAll('[data-pair]').forEach(b => {
        const d = D.pairs[b.dataset.pair][+b.dataset.i];
        b.addEventListener('pointermove', evt => show(evt, [
            [d.label, d.delta],
            [d.label_a, fmt(d.base)],
            [d.label_b, fmt(d.head)],
        ]));
        b.addEventListener('pointerleave', hide);
    });

    document.querySelectorAll('circle[data-ml]').forEach(c => {
        c.addEventListener('pointermove', evt => show(evt, [
            [c.dataset.label, c.dataset.spd + '× speedup'],
            ['lanes', c.dataset.lane],
        ]));
        c.addEventListener('pointerleave', hide);
    });

    document.querySelectorAll('[data-hbar]').forEach(b => {
        const d = D.hbars[b.dataset.hbar][+b.dataset.i];
        b.addEventListener('pointermove', evt => {
            const rows = [[d.label, fmt(d.v + (d.flush || 0))]];
            if (d.flush != null) {
                rows.push(['work', fmt(d.v)]);
                rows.push(['command flush', fmt(d.flush)]);
            }
            if (d.maint) rows.push(['phase', 'serial maintenance']);
            if (d.ran != null) rows.push(['ticks ran', d.ran + ' of ' + d.of]);
            show(evt, rows);
        });
        b.addEventListener('pointerleave', hide);
    });
})();
