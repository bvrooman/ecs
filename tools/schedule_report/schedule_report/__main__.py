"""CLI.

    schedule-report trace.csv [-o report.html]              # single-run report
    schedule-report compare base.csv head.csv [-o out.html]
                    [--labels A B] [--threshold PCT] [--csv deltas.csv]
                    [--fail-on-regression]
    schedule-report bench results.csv [-o scaling.html]     # lane-sweep curves

(`render` also works as an explicit first word for the single-run form.)
"""

from __future__ import annotations

import argparse
import csv
import sys

from .compare import REGRESSION, delta_rows
from .report import render, render_bench, render_compare


def _render_main(argv):
    p = argparse.ArgumentParser(
        prog="schedule-report",
        description="Render a tools/schedule_trace.hpp CSV into a "
                    "self-contained HTML report.")
    p.add_argument("trace", help="ScheduleTrace CSV file")
    p.add_argument("-o", "--out",
                   help="output HTML path (default: <trace>.html)")
    p.add_argument("--warmup", type=int, default=0, metavar="N",
                   help="drop the first N ticks (cold-start warm-up) from the "
                        "stats and charts, so they reflect steady state -- the "
                        "profiler convention of discarding warm-up iterations")
    args = p.parse_args(argv)
    out = args.out or args.trace.rsplit(".", 1)[0] + ".html"
    info = render(args.trace, out, warmup=args.warmup)
    print(f"wrote {out} ({info['bytes'] / 1024:.0f} KB): "
          f"{info['ticks']:,} ticks, {info['systems']} systems"
          + (f" (dropped {args.warmup} warmup)" if args.warmup else ""))


def _compare_main(argv):
    p = argparse.ArgumentParser(
        prog="schedule-report compare",
        description="Diff two ScheduleTrace CSVs from reproducible runs of "
                    "the same workload: per-system busy deltas with "
                    "noise-aware significance.")
    p.add_argument("base", help="baseline trace CSV")
    p.add_argument("head", help="trace CSV of the change under test")
    p.add_argument("-o", "--out",
                   help="output HTML path (default: compare.html)")
    p.add_argument("--labels", nargs=2, metavar=("A", "B"),
                   default=("base", "head"), help="series names in the report")
    p.add_argument("--threshold", type=float, default=2.0, metavar="PCT",
                   help="relative change below this is never significant "
                        "(default 2%%)")
    p.add_argument("--csv", metavar="PATH",
                   help="also write the deltas as CSV (for CI gating)")
    p.add_argument("--fail-on-regression", action="store_true",
                   help="exit 3 if any significant regression (tick wall or "
                        "any system's busy)")
    args = p.parse_args(argv)
    out = args.out or "compare.html"

    model = render_compare(args.base, args.head, out,
                           label_a=args.labels[0], label_b=args.labels[1],
                           threshold_pct=args.threshold)
    rows = delta_rows(model)

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(("scope", "name", "metric", args.labels[0],
                        args.labels[1], "delta_pct", "verdict"))
            w.writerows(rows)

    def num(v):
        return f"{v:12.1f}" if isinstance(v, float) else f"{v:>12}"

    print(f"{'scope':7} {'name':24} {'metric':14} {args.labels[0]:>12} "
          f"{args.labels[1]:>12} {'delta':>8}  verdict")
    for scope, name, metric, base, head, pct, verdict in rows:
        delta = f"{pct:+7.1f}%" if pct != "" else " " * 8
        print(f"{scope:7} {name[:24]:24} {metric:14} {num(base)} {num(head)} "
              f"{delta}  {verdict}")
    print(f"wrote {out}")

    regressed = model["ticks"]["verdict"] == REGRESSION or any(
        s["verdict"] == REGRESSION for s in model["systems"])
    if args.fail_on_regression and regressed:
        sys.exit(3)


def _bench_main(argv):
    p = argparse.ArgumentParser(
        prog="schedule-report bench",
        description="Render an ECS_BENCH_OUT results CSV into speedup-vs-lanes "
                    "scaling curves (needs a lane sweep -- ECS_LANES).")
    p.add_argument("results", help="benchmark results CSV (ECS_BENCH_OUT)")
    p.add_argument("-o", "--out", help="output HTML path (default: scaling.html)")
    args = p.parse_args(argv)
    out = args.out or "scaling.html"
    info = render_bench(args.results, out)
    print(f"wrote {out}: {info['suites']} suite(s) with a lane sweep")


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "compare":
        _compare_main(argv[1:])
    elif argv and argv[0] == "bench":
        _bench_main(argv[1:])
    elif argv and argv[0] == "render":
        _render_main(argv[1:])
    else:
        _render_main(argv)


if __name__ == "__main__":
    main()
