#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark/results}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
mkdir -p "$OUT_DIR"

DRY_RUN="${DRY_RUN:-0}"
if [[ "${1:-}" == "--dry-run" ]]; then
	DRY_RUN=1
fi

# The target cloud machine is fixed, so these groups are hard-coded instead of
# discovered dynamically. They map to Linux perf_event_open generic hardware and
# cache events and are measured inside bench_hft_macro, not through perf stat.
PMC_GROUPS="${PMC_GROUPS:-core cache l1d l2 dtlb}"

TRIALS="${TRIALS:-10}"
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-100000}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ITERS="${ITERS:-1}"
SEED="${SEED:-42}"
VERSION_TAG="${VERSION_TAG:-master}"
COMMIT_SHA="${COMMIT_SHA:-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)}"
OUT_PREFIX="${OUT_PREFIX:-hft_macro_op_pmc}"

PMC_RAW_CSV="${PMC_RAW_CSV:-$OUT_DIR/${OUT_PREFIX}_raw.csv}"
WRITE_OP_LATENCY="${WRITE_OP_LATENCY:-0}"
OP_PROFILE_CSV="${OP_PROFILE_CSV:-$OUT_DIR/${OUT_PREFIX}_latency_profile_raw.csv}"
LOG_DIR="${LOG_DIR:-$OUT_DIR/${OUT_PREFIX}_logs}"
mkdir -p "$LOG_DIR"

BIN="$BUILD_DIR/benchmark/bench_hft_macro"

if (( DRY_RUN == 0 )); then
	cmake -S "$ROOT" -B "$BUILD_DIR" \
		-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
		-DLLMES_BUILD_BENCHMARKS=ON \
		-DLLMES_PROFILE_HFT_MACRO_OPS=ON \
		-DLLMES_PROFILE_HFT_MACRO_OP_PMCS=ON
	cmake --build "$BUILD_DIR" --target bench_hft_macro -j

	: > "$PMC_RAW_CSV"
	if (( WRITE_OP_LATENCY == 1 )); then
		: > "$OP_PROFILE_CSV"
	fi
fi

echo "===== hft_macro operation-scoped PMC profiling ====="
echo "  bin            : $BIN"
echo "  pmc groups     : $PMC_GROUPS"
echo "  trials         : $TRIALS"
echo "  orders/levels  : $ORDERS / $LEVELS"
echo "  batch_size     : $BATCH_SIZE"
echo "  warmup/iters   : $WARMUP_ITERS / $ITERS"
echo "  seed           : $SEED"
echo "  version_tag    : $VERSION_TAG"
echo "  commit_sha     : $COMMIT_SHA"
echo "  pmc csv        : $PMC_RAW_CSV"
echo "  latency csv    : $OP_PROFILE_CSV (WRITE_OP_LATENCY=$WRITE_OP_LATENCY)"
echo "  logs           : $LOG_DIR"

read -r -a GROUP_ARRAY <<< "$PMC_GROUPS"
for group in "${GROUP_ARRAY[@]}"; do
	for trial in $(seq 1 "$TRIALS"); do
		trial_seed=$((SEED + trial - 1))
		trial_log="$LOG_DIR/${group}_trial_${trial}.log"

		printf "[group=%-5s trial=%2d/%2d] seed=%-6d " \
			"$group" "$trial" "$TRIALS" "$trial_seed"
		if (( DRY_RUN == 1 )); then
			echo "(dry-run)"
			continue
		fi

		env_args=(
			"LLMES_HFT_MACRO_OP_PMC_GROUP=$group"
			"LLMES_HFT_MACRO_OP_PMC_OUT=$PMC_RAW_CSV"
		)
		if (( WRITE_OP_LATENCY == 1 )); then
			env_args+=("LLMES_HFT_MACRO_OP_PROFILE_OUT=$OP_PROFILE_CSV")
		fi

		if env "${env_args[@]}" \
			"$BIN" \
				--metric latency \
				--trial-id "$trial" \
				--orders "$ORDERS" \
				--levels "$LEVELS" \
				--batch-size "$BATCH_SIZE" \
				--warmup-iters "$WARMUP_ITERS" \
				--iters "$ITERS" \
				--seed "$trial_seed" \
				--version-tag "$VERSION_TAG" \
				--commit-sha "$COMMIT_SHA" \
				>"$trial_log" 2>&1; then
			echo "ok"
		else
			echo "FAIL (see $trial_log)"
			exit 1
		fi
	done
done

if (( DRY_RUN == 1 )); then
	echo "Dry-run complete."
	exit 0
fi

echo "done:"
echo "  operation PMC raw data: $PMC_RAW_CSV"
if (( WRITE_OP_LATENCY == 1 )); then
	echo "  latency profile raw   : $OP_PROFILE_CSV"
fi
echo "  trial logs            : $LOG_DIR"
