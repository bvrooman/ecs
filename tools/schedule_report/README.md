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

Add `--warmup N` to drop the first `N` ticks from the stats and charts — the
profiler convention of discarding cold-start iterations so the distributions
reflect steady state rather than the first frame's cache/CPU-frequency
warm-up:

```sh
schedule-report trace.csv -o report.html --warmup 8
```

Producing a trace: attach the observer in any harness —

```cpp
std::ofstream csv("trace.csv");
ecs::diag::ScheduleTrace trace(ecs::diag::to_stream(csv));
sched.events().add(std::ref(trace));
```

— or run the mist demo with `ECS_TRACE=trace.csv`.

## Comparing two runs

```sh
schedule-report compare base.csv head.csv -o compare.html \
    --labels main change [--threshold PCT] [--csv deltas.csv] \
    [--fail-on-regression]
```

Diffs two traces of the *same workload* (same seed, same machine — e.g. two
`ECS_TRACE=... mist-headless` runs, one on the base commit and one on the
change), keyed on system **name** (wave indices shift with one-shot phases).
The HTML report pairs base/head per system with the tick series overlaid;
the console table and `--csv` carry the same verdicts for CI, and
`--fail-on-regression` exits 3 when any significant regression survives.

A delta is *significant* only when the 5%-trimmed mean shift clears a Welch
3×SE test **and** the relative `--threshold` (default 2%) **and** an
absolute 1 µs floor — so scheduler-noise tails, one-shot systems (n<5), and
timer-granularity wiggles on sub-µs systems never flag. **Calibrate the
threshold to your machine**: run a same-vs-same compare first (two traces of
the *identical* binary); if it reports changes, raise `--threshold` above
the drift you see. Back-to-back process runs drift a few percent on busy
machines — the fix is methodological (quiet machine, interleaved runs), not
statistical.

## Benchmark scaling curves

```sh
schedule-report bench results.csv -o scaling.html
```

Renders an `ECS_BENCH_OUT` results CSV (see `benchmarks/`) into
speedup-vs-lanes curves — one line per benchmark, per suite, over the lane
sweep, against a dashed ideal-linear reference. Where a curve peels away from
ideal and turns back down is where extra pool lanes stop paying (dispatch +
cache traffic, and past the physical-core count, oversubscription); ★ marks
the best lane per benchmark. Needs a lane sweep in the CSV — run a
pool-scaling suite with `ECS_LANES` set (`ECS_LANES="1,2,4,8" ./scale_bench`).

## Layout

| file | role |
|---|---|
| `schedule_report/trace.py` | trace CSV loading, summary stats, downsampling |
| `schedule_report/compare.py` | two-trace delta model + significance test |
| `schedule_report/bench.py` | results-CSV pivot into per-lane speedup series |
| `schedule_report/charts.py` | SVG chart builders (lines, histograms, bars, pairs, multiline) |
| `schedule_report/report.py` | section assembly + Jinja2 rendering (all views) |
| `schedule_report/templates/*.html.j2` | page layouts (report / compare / bench) |
| `schedule_report/static/report.css` / `report.js` | styling + hover layer, inlined at render |

`busy_us` semantics: the sum of a system's work-item durations across all
lanes (CPU time), so a fanned wave's systems legitimately sum past the wave's
wall time. `prepare_us`/`finish_us` bracket the single-threaded barrier hooks
(Reduce folds, Extract pre-sizing); `wave_us` includes the command flush,
reported separately as `flush_us`.

A system's headline cost is its **amortized busy** — the sum of its busy time
over *all* ticks, shown as a share of the mean tick — so a per-N-tick system
(Schedule `every`, e.g. a re-sort every 64 ticks) ranks by its true per-tick
budget rather than its per-run spike; the spike still shows in that card's
distribution and time series, and its wave carries a "ran n of N" count.
(Structural upkeep recorded via `Commands::sort` lands in that wave's
`flush_us`, so the whole cost is visible in the Waves split and in `tick_us`.)
