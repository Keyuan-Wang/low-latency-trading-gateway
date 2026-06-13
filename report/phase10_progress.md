# Phase 10 Progress Report

## Summary

Phase 10 continues from the Phase 9 conclusion: the remaining per-scenario p99 tail is no longer dominated by removable Linux noise, so the next productive direction is **matching-engine internals**, especially the `add_rest_new_level` path.

Work so far falls into three tracks:

1. **Engine micro-optimizations** on the array-side book hot path (`PriceLevel` layout, level prefetch).
2. **Benchmark correctness fixes** so multi-trial per-scenario results pool distinct replays instead of repeating one stream.
3. **Bitmap attribution profiling** in the per-scenario benchmark, to explain *why* `add_rest_new_level` tails without reading engine internals inside the timed window.

Phase 10 is still in progress. The first attribution run (`28eed17`, 50 trials) confirms that tail latency inside `add_rest_new_level` is not uniform: it splits cleanly by occupancy-bitmap set path and by level-reuse history. The engine-side prefetch/cache-line changes show mixed results on p99 so far.

## Starting Point (Phase 9 Handoff)

Phase 9 established:

- a tuned remote per-scenario runner on Hetzner CCX23 with `nohz_full` on CPU2–3;
- three measured scenario buckets: `add_rest_existing_level`, `add_rest_new_level`, `cancel_order`;
- a stable baseline at roughly **p99 = 154 cycles / 80 ns** for `add_rest_new_level` under tuned 10-trial runs.

Phase 9's final recommendation was to stop investing in guest-VM Linux hygiene and return to instruction reduction in the add-new-level / occupancy-bitmap path.

## Measurement Environment

All Phase 10 results below use the same remote environment as Phase 9 unless noted otherwise.

| Field | Value |
|---|---|
| provider / machine | Hetzner Cloud CCX23 |
| benchmark CPU | CPU2 (`nohz_full=2-3` active) |
| run prefix | `chrt -f 95 numactl --physcpubind=2 --membind=0` |
| per-scenario settings | trials=50, seed=42, orders=100k, batch=100k, focus=all |
| script | `benchmark/scripts/run_remote_hft_macro_scenarios_tuned.sh` |

Primary artifact for the attribution milestone:

```text
server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260613_162525/
```

## Commit Timeline (master since phase9)

| Commit | Description |
|---|---|
| `d894232` | Add `ArraySideBook::prefetch_level()` before limit-order matching |
| `ffbf3dc` | Try alternate prefetch target (last byte of `PriceLevel`) |
| `443145c` | Fix RNG: mix `trial_id` into SplitMix64 seeds |
| `7456828` | Remove unused `PriceLevel::size_`; shrink object to 16 bytes (one cache line for head/tail) |
| `c96fbbc` | Restore standard prefetch of `&levels_[idx]` (revert `ffbf3dc` tail-byte strategy) |
| `28eed17` | Add benchmark-side bitmap attribution (`ShadowOccupancyTree`, CSV columns, analysis script) |

Branch reference: `phase9` @ `1d3383d`; Phase 10 work lives on `master`.

---

## Track 1: Engine Hot-Path Changes

### 1.1 PriceLevel cache-line alignment (`7456828`)

`PriceLevel` previously carried an unused `size_` field. After removal, the object is two pointers (`head_`, `tail_`) — **16 bytes**, fitting one cache line.

This targets the array-side book design from Phase 8: each price index maps to a dense `std::vector<PriceLevel>`, so smaller `PriceLevel` objects improve cache utilization when activating or reusing levels.

### 1.2 Level prefetch (`d894232`, `ffbf3dc`, `c96fbbc`)

Before the matching loop in `add_limit_order`, the book now prefetches the resting-side `PriceLevel` for the order's limit price:

```cpp
bids_.prefetch_level(price);   // or asks_
out.remaining_quantity = matching_engine_limit<...>(...);
```

Implementation in `ArraySideBook`:

```cpp
__builtin_prefetch(&levels_[idx], 1, 3);  // write intent, high locality
```

`ffbf3dc` experimented with prefetching the last byte of the struct instead; measured p99 regressed, so `c96fbbc` restored the direct level prefetch.

### 1.3 Measured effect on per-scenario latency

50-trial tuned runs on the same VM, comparing `add_rest_new_level` cycles:

| Commit | Change | p50 | p99 | p999 |
|---|---|---:|---:|---:|
| `443145c` | RNG fix baseline | 44 | 154 | 264 |
| `7456828` | `size_` removed, prefetch dropped in same window | 44 | **176** | 286 |
| `c96fbbc` | prefetch restored | 44 | **154** | 286 |
| `28eed17` | + attribution instrumentation | 44 | **176** | 286 |

Observations:

- Removing prefetch (`7456828` window) hurt p99 (154 → 176 cycles).
- Restoring prefetch recovered p99 to 154 cycles; p999 stayed slightly higher than the earliest baseline.
- The `28eed17` attribution build adds benchmark-side shadow-tree work in Setup (untimed), but the measured p99 moved again to 176 cycles — worth re-checking for interaction or run-to-run noise before treating it as attribution overhead.

`add_rest_existing_level` and `cancel_order` remained stable at p50=22, p99≈44 across these runs.

**Conclusion so far:** prefetch before the limit-add path is useful on this VM; cache-line shrink alone did not yet show a clear p99 win in per-scenario data. End-to-end macro benchmark / PMC confirmation is still pending for Phase 10.

---

## Track 2: Benchmark Correctness (`443145c`)

### Problem

Before `443145c`, `HftMacroWorkload::Setup()` seeded RNG with `seed + iter_idx` only. Every trial replayed nearly the same event sequence. Pooled histograms looked like one trial copied 50×: bar heights scaled, shape identical.

### Fix

```cpp
event_rng_ = SplitMix64(args.seed + args.trial_id * 1000003ULL + iter_idx * 9973ULL);
param_rng_ = SplitMix64(args.seed * 1337ULL + args.trial_id * 500009ULL + iter_idx * 331ULL);
```

Verification: trial 1 vs trial 50 now produce different price/qty sequences; pooled percentiles aggregate distinct replays.

This fix is prerequisite for all subsequent Phase 10 attribution analysis.

---

## Track 3: Bitmap Attribution Profiling (`28eed17`)

### Motivation

Phase 9 showed that `add_rest_new_level` owns the tail, but not *which internal branch* of level activation drives it:

- occupancy bitmap already warm vs cold;
- L1-only set vs propagate to L2/L3;
- first use of a price index vs reuse after cancels/matches.

Phase 10 adds a **benchmark-side shadow occupancy tree** that mirrors the engine's three-level bitmap without reading engine state during timed samples.

### Design

`HftMacroWorkload` is now a template:

| Instantiation | Used in | Attribution |
|---|---|---|
| `HftMacroWorkload<false>` | `bench_hft_macro`, per-scenario warmup | off |
| `HftMacroWorkload<true>` | per-scenario measured replay | on |

During **untimed pre-generation** (Setup), for each pending add the shadow tree records:

| Field | Meaning |
|---|---|
| `occupancy_set_path` | Which bitmap path `set(bit)` would take *before* the add |
| `occupancy_l1_popcount_before` | Popcount of the target L1 word before set |
| `price_mod8` | `price & 7` (cache-layout proxy) |
| `level_reuse_distance_ops` | Ops since this side+price was last touched in the pregen window (`-1` = first touch in window) |

Timed `Execute()` only replays the pre-generated op. Attribution is attached **after** the full replay loop, so measurement isolation from Phase 9 is preserved.

New artifacts:

```text
benchmark/scripts/analyze_hft_macro_attribution.py
```

Remote runner invokes it after plotting; outputs in the results directory:

```text
attribution_set_path.csv
attribution_reuse_distance.csv
attribution_price_mod8.csv
attribution_correlations.csv
```

### Important naming caveat: `target_already_set` ≠ `add_rest_existing_level`

These are **two different classification axes**:

| Axis | Rule | Example |
|---|---|---|
| Scenario tag | Any resting order at this price before add? | `add_rest_existing_level` vs `add_rest_new_level` |
| `occupancy_set_path` | Occupancy bitmap bit already 1 before `set()`? | `target_already_set` vs `l1_only` / `reached_l2` / `reached_l3` |

They diverge because **`cancel_order` does not clear the occupancy bitmap** when the last order at a non-best price is removed. The bit can remain set on an empty `PriceLevel` (ghost level). Tracking maps say "no live orders" → scenario = `add_rest_new_level`, while shadow tree says "bit already hot" → `target_already_set`.

So `target_already_set` inside `add_rest_new_level` means **ghost bitmap reuse**, not a mis-tagged existing-level add.

### First 50-trial attribution results (`162525`, commit `28eed17`)

Pooled scenario percentiles (cycles):

| Scenario | p50 | p99 | p999 |
|---|---:|---:|---:|
| `add_rest_existing_level` | 22 | 44 | 154 |
| `add_rest_new_level` | 44 | 176 | 286 |
| `cancel_order` | 22 | 44 | 44 |

#### `add_rest_new_level` by occupancy set path

| Path | Share | p50 | p99 | p999 | Interpretation |
|---|---:|---:|---:|---:|---|
| `target_already_set` | 63.7% | 44 | 66 | 110 | Ghost bit warm; no L2/L3 propagate |
| `l1_only` | 33.1% | 44 | 198 | 308 | Cold bit, set stays in L1 word |
| `reached_l2` | 2.0% | 88 | 330 | 550 | Propagate into L2 |
| `reached_l3` | 1.2% | 44 | 264 | 738 | Propagate into L3 |

Key findings:

1. **Most "new level" adds are cheap** (`target_already_set`, p99 ≈ 66 cycles) — ghost reuse dominates volume and behaves like a warm slot, not a cold activation.
2. **True cold bitmap paths are rare but expensive** — `reached_l2` is ~2% of samples but p99 = 330 cycles, the heaviest tail bucket.
3. **`add_rest_existing_level` is 100% `target_already_set`**, as expected.

#### Reuse distance (`level_reuse_distance_ops`)

| Bin | Samples | p50 | p99 | p999 |
|---|---:|---:|---:|---:|
| 1–10 ops | 756k | 44 | 176 | 264 |
| 11–100 ops | 860k | 44 | 176 | 286 |
| 101–1000 ops | 250k | 44 | 154 | 264 |
| 1001–10000 ops | 44k | 44 | 220 | 330 |
| `first_touch` | 6.3k | 44 | **704** | **1937** |
| (Pearson/Spearman vs log2 reuse) | — | — | ~0.01 / ~0.10 | weak |

`first_touch` means **first touch of this side+price within the measured batch pregen window**, not "never used in the entire warmup history." Prices still live at pregen start are seeded in `last_level_touch`; prices used in warmup but fully cancelled before pregen can still appear as `first_touch`. That bin is small (~0.3%) but has the heaviest tail — likely true cold index activation.

Distribution plot:

```text
server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260613_162525/hft_macro_scenario_cycles_distributions.png
```

---

## Infrastructure Notes

### Remote attribution analysis gap

`analyze_hft_macro_attribution.py` uses pandas Spearman correlation, which requires `scipy`. The remote venv does not install it yet, so the tuned runner logs a traceback on the server even though the CSV is complete. Analysis was run successfully locally against the downloaded artifact.

**TODO:** add `scipy` to `requirements.txt`, or compute Spearman without scipy.

### Plot script

`run_remote_hft_macro_scenarios_tuned.sh` now passes `TRIALS=""` to the plot script so all trials are pooled by default.

---

## Current Position

Phase 10 has moved from "optimize Linux" to "explain and shrink the engine path" with measurable progress:

| Area | Status |
|---|---|
| Multi-trial statistical validity | Done (`443145c`) |
| Prefetch on limit-add path | Helpful; keep (`c96fbbc`) |
| `PriceLevel` 16-byte layout | Landed; per-scenario p99 impact unclear |
| Bitmap attribution pipeline | Landed (`28eed17`); first data collected |
| Ghost vs cold new-level taxonomy | Understood; needs clearer naming in reports/plots |
| Macro benchmark / PMC validation of engine changes | Not yet run for Phase 10 commits |
| Instruction-level perf record on L2/L3 set path | Not yet run |

The attribution data supports a refined Phase 10 target:

> Optimize the **cold occupancy propagate** path (`l1_only` / `reached_l2` / `reached_l3`, especially `reached_l2`), not the common ghost-reuse case that masquerades as `add_rest_new_level`.

Ghost reuse (`target_already_set` within new-level scenario) is already near existing-level cost and is unlikely to be the main p99 lever.

---

## Recommended Next Steps

1. **Rename attribution enums** for clarity — e.g. `bitmap_already_set` / `ghost_bit_warm` instead of `target_already_set` when paired with scenario tags; split "true cold new level" from "ghost reuse" in analysis output.
2. **Run macro benchmark + PMC** (`run_remote_compare.sh` or `run_benchmarks.sh`) across `phase9` tip vs current `master` to confirm engine changes on the primary metric.
3. **`perf record` on `--focus add_rest_new_level`** filtered to `reached_l2` samples using attribution CSV op indices (once export join is scripted).
4. **Engine experiment:** clear occupancy bit when a level becomes empty on cancel (if semantics allow), or lazy-clear ghosts — measure effect on ghost-reuse volume and tail.
5. **Fix remote analysis deps** (`scipy` in requirements).
6. **Optional:** extend `first_touch` to cover full warmup history if the goal is literal "never-used price index."

---

## References

- Phase 9 report: `report/phase9_per_scenario_benchmark.md`
- Phase 9 branch: `origin/phase9` @ `1d3383d`
- Workload / attribution code: `benchmark/src/hft/hft_macro_workload.hpp`
- Per-scenario collector: `benchmark/src/hft/bench_hft_macro_scenarios.cpp`
- Analysis script: `benchmark/scripts/analyze_hft_macro_attribution.py`
- Latest 50-trial run: `server_results/hft_macro/scenarios_tuned/hft_macro_scenarios_tuned_20260613_162525/`
