#!/usr/bin/env bash
# Sweep one steering weight and report follow quality for each value, so a tuning
# choice can be made from numbers rather than eyeballed frames.
#
#   examples/mist/tools/sweep.sh ECS_GOALSEEK 0.5 1.0 1.5 2.0 3.0
#
# Runs the already-built mist binary headless with a deterministic step-cursor.
# Override the binary with MIST_BIN=..., seconds with SWEEP_SECS=... (default 30).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${MIST_BIN:-$HERE/../../../build-host/examples/mist}"
SECS="${SWEEP_SECS:-30}"
PARAM="$1"; shift

printf "%-10s %-12s %-9s %-9s %-8s %-8s\n" "$PARAM" "follow_mean" "spread" "coh" "settle" "wheel"
for v in "$@"; do
  csv="$(mktemp)"
  env ECS_SEED=7 ECS_CURSOR_PATH=step "$PARAM=$v" ECS_METRICS="$csv" \
      timeout "$SECS" "$BIN" >/dev/null 2>&1 || true
  python3 - "$csv" "$v" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
c = lambda k: [float(r[k]) for r in rows]
fe, sp, co, gx, gy = c('follow_err'), c('spread'), c('coherence'), c('goal_sx'), c('goal_sy')
s = settle = wheel = 0
for i in range(1, len(rows) + 1):
    if i == len(rows) or gx[i] != gx[s] or gy[i] != gy[s]:
        if i - s > 10:
            seg = slice(s, i); emin, ee = min(fe[seg]), fe[i - 1]
            if ee < 0.25: settle += 1
            elif ee > emin + 0.15: wheel += 1
        s = i
m = lambda x: sum(x) / len(x)
print("%-10s %-12.3f %-9.3f %-9.3f %-8d %-8d" % (sys.argv[2], m(fe), m(sp), m(co), settle, wheel))
PY
done
