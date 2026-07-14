# benchmarks

Eight suites, each answering one question. All build by default
(`ECS_BUILD_BENCHMARKS=ON`) with `-O3 -DNDEBUG -march=native` forced, so the
numbers are meaningful regardless of the tree's `CMAKE_BUILD_TYPE`. Every
suite reads its knobs from env vars (`ECS_ENTITIES`, `ECS_TICKS`,
`ECS_ITERS`, `ECS_REPEATS`; historical positional args still win), so sweep
scripts and the A/B driver configure them uniformly.

| suite | question |
|---|---|
| `soa_bench` | SoA storage vs AoS baseline; `for_each_serial` vs `for_each_chunk` |
| `gather_bench` | `for_each_parallel`'s gather/scatter cost vs component width (contrast a portable vs P2996 build) |
| `scale_bench` | per-tick cost vs population size; where the pool pays off |
| `schedule_bench` | tick distribution + allocs/tick of the particle schedule; lane sweep; imperative vs kernel registration |
| `churn_bench` | structural throughput: spawn/despawn/add/remove ns/op + an emitter/reaper steady state with allocs/tick |
| `primitives_bench` | Reduce/Extract/Collect/Bin/Events vs hand-rolled imperative baselines, at 1 lane (overhead) and hw lanes (payoff) |
| `sort_bench` | `sort_rows` mechanics: early exit, full counting sort, `min_disorder` skip under 5% churn drift |
| `access_bench` | `get<C>(entity)` sequential vs random, `alive`, query over 1 vs 64 archetypes (fragmentation) |

`examples/mist-headless` doubles as the macro benchmark: the deterministic
fixed-seed flock at 1 and 4 lanes, with bitwise-reproducibility checks.
CI runs a tiny-size smoke of every suite (rows emitted, envelope intact —
never gating on shared-runner timings).

## Pool-width sweep (`ECS_LANES`)

`scale_bench`, `primitives_bench`, and `schedule_bench` sweep the WorkerPool
lane count so the throughput turnover is visible rather than hidden behind a
single point. `hardware_concurrency()` counts logical (SMT) cores and the
dispatch thread is extra, so timing only at `hw` oversubscribes — peak
throughput is usually back at the physical-core count. `ECS_LANES="1,2,4,8"`
sets the sweep; the default is a power-of-two ladder up to `hw` (plus `hw`),
so the sweep reaches the turnover but a default run never overshoots it.

```sh
export ECS_LANES="1,2,4,8,12,16" ECS_BENCH_OUT=results.csv
./build/benchmarks/scale_bench        # lanes x N matrix, best lane marked
schedule-report bench results.csv -o scaling.html   # speedup-vs-lanes curves
```

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

## Comparing runs: the A/B driver

Wall-clock benchmarks are only trustworthy compared *within one machine and
session*, interleaved. `ab.py` does the whole workflow — builds the base ref
into a git worktree and the working tree as head, runs the suites
round-robin (A B A B …, order flipped each round), and compares the paired
per-round samples per (suite, name, metric, entities, lanes) key with a
paired t-test (99%) plus a relative threshold:

```sh
python3 benchmarks/ab.py --base main --fail-on-regression
python3 benchmarks/ab.py --base HEAD~1 --rounds 7 --suites churn,sort \
    --env ECS_ENTITIES=100000 --cmake-arg=-DCMAKE_CXX_COMPILER=clang++
```

(`--cmake-arg` values that start with `-D` need the `=` form.) Calibrate on
your machine first with a same-vs-same run — `--base HEAD` on a clean tree —
and raise `--threshold` (default 5%) if it flags anything: micro ns/op
numbers carry a few percent of code-layout variance even between identical
sources, and only interleaving + rounds beats scheduler drift, not
statistics.

For schedule-level attribution (*which system* regressed), trace a
deterministic run and diff the traces instead:

```sh
ECS_TRACE=base.csv ./build/examples/mist-headless        # on the base commit
ECS_TRACE=head.csv ./build/examples/mist-headless        # on your change
schedule-report compare base.csv head.csv -o compare.html
```

(see `tools/schedule_report/README.md`).
