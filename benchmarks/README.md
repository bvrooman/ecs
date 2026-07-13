# benchmarks

Four suites, each answering one question. All build by default
(`ECS_BUILD_BENCHMARKS=ON`) with `-O3 -DNDEBUG -march=native` forced, so the
numbers are meaningful regardless of the tree's `CMAKE_BUILD_TYPE`.

| suite | question | knobs |
|---|---|---|
| `soa_bench` | SoA storage vs AoS baseline; `for_each_serial` vs `for_each_chunk` | `[entities] [iters] [repeats]` |
| `gather_bench` | `for_each_parallel`'s gather/scatter cost vs component width (contrast a portable vs P2996 build) | — |
| `scale_bench` | per-tick cost vs population size; where the pool pays off | — |
| `schedule_bench` | tick distribution + allocs/tick of the particle schedule; lane sweep; imperative vs kernel registration | `[measured_ticks]` |

`examples/mist-headless` doubles as the macro benchmark: the deterministic
fixed-seed flock at 1 and 4 lanes, with bitwise-reproducibility checks.

## Machine-readable results (`ECS_BENCH_OUT`)

Every suite (and `mist-headless`) also appends its numbers to a CSV when
`ECS_BENCH_OUT=<path>` is set — one row per (benchmark, metric), wrapped in a
run-metadata envelope so a row is a reproducible claim rather than a bare
number:

```
suite,name,metric,value,unit,better,entities,lanes,samples,
timestamp,commit,build,compiler,cpu,threads_hw
```

- `better` — which direction improves (`lower`/`higher`), so comparison
  tooling needs no per-metric knowledge.
- `entities`/`lanes`/`samples` — the configuration the value was measured
  under (`0` where not meaningful).
- envelope — identical for the whole process, repeated on every row
  (denormalized like the `ScheduleTrace` CSV: offline tools need no joins).
  `commit` is read from `ECS_GIT_COMMIT` (or `GITHUB_SHA`); a process cannot
  know its own checkout, so runners should export it:

```sh
export ECS_GIT_COMMIT=$(git rev-parse --short HEAD)
ECS_BENCH_OUT=results.csv ./build/benchmarks/soa_bench
ECS_BENCH_OUT=results.csv ./build/benchmarks/schedule_bench   # appends
```

The file is append-mode with a header only when it starts empty, so one
results CSV can collect a whole run of suites — or accumulate runs across
commits for trend analysis.

## Comparing runs

Wall-clock benchmarks are only trustworthy compared *within one machine and
session*. To A/B a change: run the suites on the base commit and on your
change **interleaved** (base, head, base, head — not all-base then all-head),
each appending to its own `ECS_BENCH_OUT` file, and compare per
(suite,name,metric,entities,lanes) key. For schedule-level attribution
(*which system* regressed), trace a deterministic run instead and diff the
traces:

```sh
ECS_TRACE=base.csv ./build/examples/mist-headless        # on the base commit
ECS_TRACE=head.csv ./build/examples/mist-headless        # on your change
schedule-report compare base.csv head.csv -o compare.html
```

(see `tools/schedule_report/README.md`).
