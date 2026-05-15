#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$ROOT/bench/results"
mkdir -p "$OUT_DIR"

BIN="$ROOT/build/core/matching_core/matching_core_phase1_bench"
PERF_CSV="$OUT_DIR/phase1_perf_raw.csv"

echo "scenario,orders,levels,iters,metric,value" > "$PERF_CSV"

SCENARIOS=("lmt_rest" "lmt_cross_deep" "mkt_sweep_deep" "cxl_miss" "dup_reject")
ORDERS=(1000 10000 100000)
LEVELS=(10 100 1000)
ITERS=2000
EVENTS="cycles,instructions,cache-misses,branches,branch-misses,LLC-load-misses,LLC-store-misses"

for s in "${SCENARIOS[@]}"; do
  for o in "${ORDERS[@]}"; do
    for l in "${LEVELS[@]}"; do
      if (( l > o )); then
        continue
      fi
      TMP="$(mktemp)"
      perf stat -x, -e "$EVENTS" \
        "$BIN" --scenario "$s" --orders "$o" --levels "$l" --iters "$ITERS" >/dev/null 2>"$TMP" || true

      while IFS=, read -r val unit metric rest; do
        # perf CSV line pattern is kernel/version dependent; filter numeric+metric lines
        [[ -z "${metric:-}" ]] && continue
        [[ "$val" =~ ^[0-9]+([.][0-9]+)?$ ]] || continue
        echo "$s,$o,$l,$ITERS,$metric,$val" >> "$PERF_CSV"
      done < "$TMP"
      rm -f "$TMP"
    done
  done
done

echo "perf raw saved: $PERF_CSV"