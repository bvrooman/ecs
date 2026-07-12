# schedule-report

Renders a `ScheduleTrace` CSV (see `tools/schedule_trace.hpp`) into a
self-contained HTML report — headline tick stats, per-wave wall/flush split,
systems ranked by amortized busy time, and a card per system with its
busy-time distribution, time series, and barrier-hook costs. The output is a
single file with no external requests (CSS/JS/SVG inlined); charts carry
crosshair and per-mark tooltips, and light/dark render from validated color
tokens.

## Usage

From the repo, without installing (only needs `jinja2`):

```sh
PYTHONPATH=tools/schedule_report python3 -m schedule_report trace.csv -o report.html
```

Or install it (editable), which provides the `schedule-report` command:

```sh
pip install -e tools/schedule_report
schedule-report trace.csv -o report.html
```

Producing a trace: attach the observer in any harness —

```cpp
std::ofstream csv("trace.csv");
ecs::diag::ScheduleTrace trace(ecs::diag::to_stream(csv));
sched.events().add(std::ref(trace));
```

— or run the mist demo with `ECS_TRACE=trace.csv`.

## Layout

| file | role |
|---|---|
| `schedule_report/trace.py` | CSV loading, summary stats, downsampling |
| `schedule_report/charts.py` | SVG chart builders (lines, histograms, bars) |
| `schedule_report/report.py` | section assembly + Jinja2 rendering |
| `schedule_report/templates/report.html.j2` | page layout |
| `schedule_report/static/report.css` / `report.js` | styling + hover layer, inlined at render |

`busy_us` semantics: the sum of a system's work-item durations across all
lanes (CPU time), so a fanned wave's systems legitimately sum past the wave's
wall time. `prepare_us`/`finish_us` bracket the single-threaded barrier hooks
(Reduce folds, Extract pre-sizing); `wave_us` includes the command flush,
reported separately as `flush_us`.
