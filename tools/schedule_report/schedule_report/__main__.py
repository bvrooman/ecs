"""CLI: schedule-report trace.csv [-o report.html]"""

from __future__ import annotations

import argparse

from .report import render


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="schedule-report",
        description="Render a tools/schedule_trace.hpp CSV into a "
                    "self-contained HTML report.")
    p.add_argument("trace", help="ScheduleTrace CSV file")
    p.add_argument("-o", "--out",
                   help="output HTML path (default: <trace>.html)")
    args = p.parse_args(argv)
    out = args.out or args.trace.rsplit(".", 1)[0] + ".html"
    info = render(args.trace, out)
    print(f"wrote {out} ({info['bytes'] / 1024:.0f} KB): "
          f"{info['ticks']:,} ticks, {info['systems']} systems")


if __name__ == "__main__":
    main()
