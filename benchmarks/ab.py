#!/usr/bin/env python3
"""Interleaved A/B benchmark driver: is this change a regression?

Builds the BASE ref into a git worktree and the working tree as HEAD, then
runs the suites round-robin (A B A B ..., order flipped each round to cancel
drift) with ECS_BENCH_OUT capturing every number. Per (suite, name, metric,
entities, lanes) key it compares the R paired samples: a verdict needs the
mean of the per-round differences to clear 3x its standard error AND the
relative threshold. Wall-clock comparisons are only trustworthy like this --
same machine, same session, interleaved; never against stored numbers from
another host.

    python3 benchmarks/ab.py --base main
    python3 benchmarks/ab.py --base HEAD~1 --rounds 7 --threshold 3 \
        --suites soa,churn,sort --env ECS_ENTITIES=100000 \
        --cmake-arg -DCMAKE_CXX_COMPILER=clang++ --fail-on-regression

Requires a clean-enough tree to build (uncommitted changes ARE the head
side). stdlib only.
"""

from __future__ import annotations

import argparse
import collections
import csv
import math
import os
import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# suite -> executable relative to the build dir. mist is the macro workload
# (examples/mist-headless); the rest are benchmarks/.
SUITES = {
    "soa": "benchmarks/soa_bench",
    "gather": "benchmarks/gather_bench",
    "scale": "benchmarks/scale_bench",
    "schedule": "benchmarks/schedule_bench",
    "churn": "benchmarks/churn_bench",
    "primitives": "benchmarks/primitives_bench",
    "sort": "benchmarks/sort_bench",
    "access": "benchmarks/access_bench",
    "mist": "examples/mist-headless",
}
DEFAULT_SUITES = "soa,schedule,churn,primitives,sort,access"

# Fast-but-meaningful defaults for A/B rounds; override with --env.
FAST_ENV = {
    "soa": {"ECS_ENTITIES": "300000", "ECS_ITERS": "20", "ECS_REPEATS": "3"},
    "gather": {"ECS_ENTITIES": "100000", "ECS_REPEATS": "60"},
    "scale": {},
    "schedule": {"ECS_TICKS": "800"},
    "churn": {"ECS_ENTITIES": "50000", "ECS_REPEATS": "3", "ECS_TICKS": "500"},
    "primitives": {"ECS_ENTITIES": "100000", "ECS_TICKS": "120"},
    "sort": {"ECS_ENTITIES": "100000", "ECS_REPEATS": "3"},
    "access": {"ECS_ENTITIES": "100000", "ECS_REPEATS": "10"},
    "mist": {"ECS_PARTICLES": "20000", "ECS_TICKS": "150"},
}


def sh(cmd, cwd=None, env=None):
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: {' '.join(map(str, cmd))}\n{r.stdout}\n{r.stderr}")
    return r.stdout.strip()


def build_side(tree: pathlib.Path, cmake_args, suites):
    bdir = tree / "build-ab"
    cfg = ["cmake", "-S", str(tree), "-B", str(bdir),
           "-DCMAKE_BUILD_TYPE=Release", *cmake_args]
    if not (bdir / "CMakeCache.txt").exists():
        sh(cfg)
    targets = [pathlib.Path(SUITES[s]).name for s in suites]
    sh(["cmake", "--build", str(bdir), "--target", *targets,
        "-j", str(os.cpu_count() or 2)])
    return bdir


def run_suite(bdir, suite, out_csv, commit, extra_env):
    exe = bdir / SUITES[suite]
    env = {**os.environ, "ECS_BENCH_OUT": str(out_csv),
           "ECS_GIT_COMMIT": commit, **FAST_ENV.get(suite, {}), **extra_env}
    r = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: {exe} exited {r.returncode}\n{r.stdout}\n{r.stderr}")


def load_samples(path):
    """(suite,name,metric,entities,lanes) -> {'values': [...], 'better': str}"""
    out = collections.defaultdict(lambda: {"values": [], "better": "lower"})
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            key = (row["suite"], row["name"], row["metric"],
                   row["entities"], row["lanes"])
            out[key]["values"].append(float(row["value"]))
            out[key]["better"] = row["better"]
    return out


# Two-sided 99% Student-t quantiles by df (n-1 paired diffs): with 3-5
# rounds a fixed "3 sigma" is wildly anti-conservative and flags noise.
T99 = {2: 9.92, 3: 5.84, 4: 4.60, 5: 4.03, 6: 3.71, 7: 3.50, 8: 3.36, 9: 3.25}


def verdict(a, b, better, threshold_pct):
    """Paired t-comparison of per-round samples a vs b."""
    n = min(len(a), len(b))
    ma = sum(a[:n]) / n
    mb = sum(b[:n]) / n
    if ma == 0:
        return "unchanged", 0.0
    pct = 100.0 * (mb - ma) / ma
    if n < 3:
        return "unchanged", pct  # too few rounds for any claim
    diffs = [y - x for x, y in zip(a[:n], b[:n])]
    md = sum(diffs) / n
    var = sum((d - md) ** 2 for d in diffs) / (n - 1)
    se = math.sqrt(var / n)
    t = T99.get(n - 1, 3.0)
    if abs(pct) < threshold_pct or abs(md) <= t * se:
        return "unchanged", pct
    worse = md > 0 if better == "lower" else md < 0
    return ("regression" if worse else "improvement"), pct


def main():
    p = argparse.ArgumentParser(
        description="Interleaved A/B benchmark comparison of two git refs.")
    p.add_argument("--base", required=True,
                   help="baseline git ref (the working tree is the head side)")
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--suites", default=DEFAULT_SUITES,
                   help=f"comma list from: {','.join(SUITES)} "
                        f"(default {DEFAULT_SUITES})")
    p.add_argument("--threshold", type=float, default=5.0, metavar="PCT",
                   help="relative change below this is never significant "
                        "(default 5%%; micro ns/op numbers carry a few "
                        "percent of build-layout variance even between "
                        "identical sources -- calibrate with --base HEAD)")
    p.add_argument("--env", action="append", default=[], metavar="K=V",
                   help="extra env for every suite run (repeatable)")
    p.add_argument("--cmake-arg", action="append", default=[], metavar="ARG",
                   help="extra cmake configure argument (repeatable)")
    p.add_argument("--out", default="ab-results",
                   help="directory for the two ECS_BENCH_OUT CSVs")
    p.add_argument("--fail-on-regression", action="store_true")
    args = p.parse_args()

    suites = [s.strip() for s in args.suites.split(",") if s.strip()]
    for s in suites:
        if s not in SUITES:
            sys.exit(f"error: unknown suite '{s}' (know: {','.join(SUITES)})")
    extra_env = dict(kv.split("=", 1) for kv in args.env)

    base_sha = sh(["git", "rev-parse", "--short", args.base], cwd=REPO)
    head_sha = sh(["git", "rev-parse", "--short", "HEAD"], cwd=REPO)
    dirty = sh(["git", "status", "--porcelain"], cwd=REPO) != ""
    head_label = head_sha + ("+dirty" if dirty else "")

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    a_csv, b_csv = out / "base.csv", out / "head.csv"
    for f in (a_csv, b_csv):
        f.unlink(missing_ok=True)

    # base side: a detached worktree of the ref (removed at the end)
    wt = out / f"worktree-{base_sha}"
    if not wt.exists():
        sh(["git", "worktree", "add", "--detach", str(wt), base_sha], cwd=REPO)
    try:
        print(f"building base {base_sha} and head {head_label} ...")
        bdir_a = build_side(wt, args.cmake_arg, suites)
        bdir_b = build_side(REPO, args.cmake_arg, suites)

        for r in range(args.rounds):
            # flip the order each round so slow drift hits both sides equally
            order = [("A", bdir_a, a_csv, base_sha),
                     ("B", bdir_b, b_csv, head_label)]
            if r % 2:
                order.reverse()
            for tag, bdir, csv_path, sha in order:
                for s in suites:
                    run_suite(bdir, s, csv_path, sha, extra_env)
            print(f"round {r + 1}/{args.rounds} done")
    finally:
        sh(["git", "worktree", "remove", "--force", str(wt)], cwd=REPO)

    sa, sb = load_samples(a_csv), load_samples(b_csv)
    rows = []
    for key in sorted(set(sa) | set(sb)):
        if key not in sa or key not in sb:
            continue  # a benchmark only one side has: nothing to compare
        v, pct = verdict(sa[key]["values"], sb[key]["values"],
                         sa[key]["better"], args.threshold)
        ma = sum(sa[key]["values"]) / len(sa[key]["values"])
        mb = sum(sb[key]["values"]) / len(sb[key]["values"])
        rows.append((v, pct, key, ma, mb))

    rank = {"regression": 0, "improvement": 1, "unchanged": 2}
    rows.sort(key=lambda r: (rank[r[0]], -abs(r[1])))
    n_reg = sum(1 for r in rows if r[0] == "regression")
    n_imp = sum(1 for r in rows if r[0] == "improvement")

    print(f"\nbase {base_sha} vs head {head_label}: {args.rounds} rounds, "
          f"threshold {args.threshold:g}%")
    print(f"{'verdict':12} {'suite':11} {'benchmark':30} {'metric':24} "
          f"{'ent':>8} {'ln':>3} {'base':>12} {'head':>12} {'delta':>8}")
    for v, pct, (suite, name, metric, ent, lanes), ma, mb in rows:
        if v == "unchanged" and abs(pct) < args.threshold:
            continue  # keep the table to the interesting rows
        print(f"{v:12} {suite:11} {name[:30]:30} {metric:24} {ent:>8} "
              f"{lanes:>3} {ma:12.2f} {mb:12.2f} {pct:+7.1f}%")
    print(f"\n{n_reg} regression(s), {n_imp} improvement(s), "
          f"{len(rows) - n_reg - n_imp} unchanged "
          f"({a_csv} / {b_csv} keep the raw samples)")

    if args.fail_on_regression and n_reg:
        sys.exit(3)


if __name__ == "__main__":
    main()
