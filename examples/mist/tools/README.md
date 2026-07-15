# mist — behaviour review harness

Tools for reviewing the murmuration's *time-varying* behaviour headless and
quantitatively, instead of eyeballing single screenshots.

## mist-headless (no GLFW)

`headless.cpp` builds as the `mist-headless` target on every platform (the
OpenGL demo needs macOS + GLFW). It runs the simulation at 1 lane and 4 lanes
with the same seed, verifies the flock evolves **bitwise identically** (the
kernel schedule's lane-count-invariance guarantee), and reports tick timing:

```sh
cmake --build build --target mist-headless
./build/examples/mist-headless                    # 40k birds, 240 ticks
ECS_PARTICLES=85000 ECS_TICKS=600 ./build/examples/mist-headless
ECS_METRICS=/tmp/m.csv ./build/examples/mist-headless   # + metrics CSV
ECS_TRACE=/tmp/t.csv ./build/examples/mist-headless     # + schedule trace CSV
ECS_BENCH_OUT=/tmp/b.csv ./build/examples/mist-headless # + benchmark results rows
```

It honours `ECS_METRICS`, `ECS_TRACE` (per-system schedule trace, written by
the last — 4-lane — run; feed it to `schedule-report`, or diff two runs with
`schedule-report compare`), `ECS_BENCH_OUT` (per-lane us/tick + speedup rows
in the `benchmarks/bench.hpp` CSV schema), and the steering-weight env vars
below; the cursor stays inactive (`ECS_CURSOR_PATH` needs the GL build), and
the seed is fixed (determinism is the point).

## GL demo hooks

The demo (`examples/mist`) exposes these env-gated hooks (zero cost when unset):

| env var | effect |
|---|---|
| `ECS_SEED=<n>` | deterministic scatter — reproducible runs |
| `ECS_METRICS=<csv>` | log whole-flock metrics every tick (see columns below) |
| `ECS_CURSOR_PATH=circle\|step\|sweep` | drive the cursor along a path over time (headless follow tests) |
| `ECS_FILMSTRIP=<dir>` | grab frames into `<dir>/NN.bmp`; `ECS_FILMSTRIP_EVERY=<s>` (2.5), `ECS_FILMSTRIP_N=<n>` (12) |
| `ECS_CAPTURE=<bmp>` / `ECS_CAPTURE_AT=<s>` | grab one frame at a time |
| steering weights | `ECS_CRUISE ECS_GOALSEEK ECS_COHESION ECS_ALIGN ECS_SEPARATION ECS_SOFTCAP ECS_WANDER` — override at runtime, no recompile |

For **schedule timing** (as opposed to flock behaviour): run the GL demo with
`ECS_TRACE=<csv>` (or attach `ecs::diag::ScheduleTrace` in any harness), then

```sh
PYTHONPATH=tools/schedule_report python3 -m schedule_report trace.csv -o report.html
```

renders a self-contained HTML report — headline tick stats, per-wave wall/flush
split, and a card per system with its busy-time distribution, time series, and
barrier-hook costs (`busy` = work-item time summed across lanes).

Metrics columns: `t, com_sx, com_sy, com_z, goal_sx, goal_sy, follow_err, spread, coherence`
where **follow_err** = screen distance from the flock centre to the cursor/goal
(0 = on it; a non-decaying oscillation = the undamped wheel), **spread** = RMS
world radius (compact vs diffuse), **coherence** = `|mean velocity|/mean speed`
(1 = flying together, ~0 = wheeling/scrambled).

## Loop

```sh
# one run: metrics + a moving cursor + a filmstrip
ECS_SEED=7 ECS_CURSOR_PATH=step ECS_METRICS=/tmp/m.csv \
  ECS_FILMSTRIP=/tmp/strip timeout 35 ./build-host/examples/mist

python3 examples/mist/tools/analyze_metrics.py /tmp/m.csv   # quantitative summary
python3 examples/mist/tools/filmstrip.py /tmp/strip /tmp/strip.png   # visual montage

# sweep one weight and compare follow quality across values:
examples/mist/tools/sweep.sh ECS_GOALSEEK 0.5 1.0 1.5 2.0 3.0
```

`analyze_metrics.py` reports overall stats plus, for each cursor jump, how close
the flock gets and whether it **settles** on the cursor or **wheels away**.
