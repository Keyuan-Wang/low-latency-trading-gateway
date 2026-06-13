# llmes — Low-Latency Matching & Execution Simulator

A C++20 order-matching engine evolved incrementally from a correctness-first baseline through handle-based identity, direct-addressed price-level storage, profile-guided optimization, and HFT workload measurement.

---

## Current Status

| Area | Status |
|------|--------|
| Matching core | Phase 8b array side book baseline |
| Price-level storage | `ArraySideBook`: 65536 direct-addressed levels + `OccupancyTree` |
| Order storage | Pool-backed intrusive `PriceLevel` queues |
| Order identity | Engine-issued `OrderHandle`; no id hash table on the matching hot path |
| Order types | Limit / Market / Cancel / Modify |
| Benchmark suite | Batched `hft_macro` + complete per-call scenario collector |
| Primary benchmark | `hft_macro` Zero-Intelligence model with realistic order flow |
| Current headline | **17.2 ns/op**, about **58.1M ops/s** on Hetzner CCX23 `hft_macro` |
| Diagnostic benchmark | Existing-level add, new-level add, and cancel per-call cycles/ns |
| Market Data / Execution / Risk | Not started |

---

## Historical Hash Table Engineering Journey

The cancel path is the dominant operation in realistic order-book workloads. Earlier phases optimized arbitrary external-order-id lookup inside the matching core:

| Phase | ID Index | Macro ops/s | vs 2b | Key Limitation |
|---|---|---|---|---|
| **2b** | `std::unordered_map` | 11.0M | baseline | Node-based: pointer chase, cache-unfriendly |
| 2c | Custom open-addressing + tombstones | 7.8M | −29% | Tombstone buildup degrades lookup under cancel-heavy load |
| 2d | Robin Hood + backward-shift deletion | 7.8M | −29% | Probe chains regress under HFT spatial concentration |
| **2e** | `absl::flat_hash_map` (Swiss Table) | **11.9M** | **+8%** | — |

**Phase 2e was the in-core hash-table winner** — the Swiss-table `absl::flat_hash_map` outperformed hand-rolled open-addressing by 52% and the baseline `std::unordered_map` by 8% under realistic HFT order flow.

Phase 6a later moved identity resolution out of the matching hot path entirely. The gateway owns external `client_order_id` validation and id-to-handle mapping; the matching core receives `OrderHandle` values and resolves them by direct pool index.

---

## Current Engine: Phase 8b

Phase 8 replaced the Phase 7 hot-ring/cold-map coupling with one fixed-range side-book representation:

```text
ArraySideBook<IsAsk>
├── std::vector<PriceLevel> levels_   # 65536 direct-addressed price slots
└── OccupancyTree active_tree         # three-level 64-bit bitmap hierarchy
```

Key properties:

- price-to-level lookup is direct index arithmetic;
- `OccupancyTree` finds the next active bid/ask price using bit operations instead of a linear price scan;
- tree size is fixed at compile time with `std::array`, and its three levels are explicitly unrolled;
- bid/ask direction is a template parameter, removing side-direction branches inside the side book;
- `empty()`, `best_price()`, and `best_level()` are pure read paths;
- empty best levels are retired lazily in `erase_best()`; eager retirement was tested and rejected because it added about 9.5 instructions/op.

Cloud `hft_macro` result at `orders=100000`, `levels=100`, `batch_size=100000`:

| Version | avg ns/op | ops/s | instructions/op | branch misses/op |
|---|---:|---:|---:|---:|
| Phase 7c hot ring + cold map | 19.27 | 51.9M | 137.13 | 1.496 |
| **Phase 8b array side book** | **17.21** | **58.1M** | **130.05** | **1.229** |

Phase 8b improved latency and cycles by about 10.7%, instructions by 5.2%, branch misses by 17.8%, and cache misses by 39.3% against Phase 7c.

---

## HFT Benchmarking

The current benchmark suite deliberately keeps two complementary executables:

| Benchmark | Purpose | Measurement shape |
|---|---|---|
| `bench_hft_macro` | Release-level throughput, latency, and PMC comparison | One timing/counter window around a large batch |
| `bench_hft_macro_scenarios` | Diagnose basic-operation latency inside the real macro stream | One CSV row for every focused operation call |

The macro workload uses a deterministic Zero-Intelligence-style event stream:

- 45% limit add;
- 48% cancel;
- 5% modify;
- 2% market;
- near-best spatial locality, short order lifetimes, cancel clustering, and non-flat depth.

The per-scenario collector times only clean single-operation classes:

- `add_rest_existing_level`;
- `add_rest_new_level`;
- `cancel_order`.

Crossing limit and market orders are replayed but not timed because they may perform many internal matches. Modify is omitted because it is cancel plus add. Each measured scenario receives a separate deterministic replay so instrumentation on one scenario does not perturb another.

Phase 9 found that `cancel_order` and existing-level adds are compact and stable, while `add_rest_new_level` owns the largest basic-operation tail. Full rationale and system-tuning results: `report/phase9_per_scenario_benchmark.md`.

---

## Build & Test

**Requirements:** CMake >= 3.20, C++20 (GCC 13+ or Clang 16+ recommended), Linux (for PMC benchmarks)

```bash
cmake -S . -B build -DLLMES_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Benchmark Pipeline

```bash
# Batched macro benchmark: latency + PMC
bash benchmark/scripts/run_benchmarks.sh

# Complete per-call scenario data and distribution plot
bash benchmark/scripts/run_hft_macro_scenarios.sh
python3 benchmark/scripts/plot_hft_macro_scenarios.py

# Merge macro trials, compute mean/std/CV/95% CI
python3 benchmark/scripts/merge_benchmark_metrics.py

# Parametric and version-comparison plots
python3 benchmark/scripts/plot_benchmark.py
python3 benchmark/scripts/plot_version_comparison.py
```

Override parameters via environment variables:

```bash
SCENARIOS=hft_macro METRICS=latency,pmc \
VERSION_TAG=experiment COMMIT_SHA=<sha> TRIALS=10 \
  bash benchmark/scripts/run_benchmarks.sh
```

Cloud comparison uses `benchmark/scripts/run_remote_compare.sh`. After one-time boot isolation via `benchmark/scripts/setup_remote_nohz_full.sh`, the compare runner applies the tested CPU2/NUMA binding, IRQ/workqueue migration, realtime priority, watchdog, governor, and background-noise settings uniformly to every compared version.

---

## Architecture

The batched macro benchmark uses `IBenchScenario`, while the per-scenario collector reuses the same deterministic workload generator and matching core.

```
                   benchmark_runner.cpp
                   (ParseArgs → warmup → measure → output)
                           │
                    IBenchScenario (virtual)
                     bench_hft_macro
                           │
                           ▼
                  hft_macro_workload.hpp
                           ▲
                           │
              bench_hft_macro_scenarios
```

### Measurement Modes

- **Latency** (`--metric latency`): `avg / p50 / p95 / p99` nanoseconds per op and `ops/s`, normalized per batch.
- **PMC** (`--metric pmc`): In-process hardware counters via `perf_event_open` (cycles, instructions, branches, misses, cache misses). Derived: CPI, branch miss rate.
- **Per scenario**: raw and measurement-overhead-adjusted cycles/ns for every focused call, intended for relative diagnosis rather than release-level absolute latency.

---

## Key Results

### Phase 1 → Phase 2a (Pool Allocator)

Pool-based `PriceLevel` replaces `std::list`: 22–38% fewer instructions per op. Cancel-heavy scenarios see 2× throughput, memory latency drops dramatically (CPI −59%, cache misses −87%).

### Phase 2a → Phase 2b (O(1) Cancel Index)

`std::unordered_map<id, Order*>` eliminates O(N) book scan: cancel throughput improves **300–1800×**. Cross/match scenarios regress 40–80% due to hash-map maintenance overhead, but in a realistic mixed workload the net effect is **894× overall throughput**.

### Phase 2b → 2e (Hash Table Engineering)

Under the HFT macro benchmark (48% cancel / 45% add), custom open-addressing (2c/2d) regresses 29% — tombstones and probe chains hurt cancel-heavy access. `absl::flat_hash_map` (2e) leads at 11.9M ops/s: 52% ahead of 2c/2d and 8% ahead of 2b.

### Phase 6a (Gateway-Owned Handles)

The matching core stopped resolving arbitrary external ids. Cancels and modifies now take `OrderHandle`, resolving directly into the order pool. This removed the cancel-index hash table from the measured hot path and improved `hft_macro` from 34.4 ns/op to about 30.3 ns/op on the comparable cloud run.

### Phase 7 (Hot Ring + Cold Map)

The remaining hot `std::map` price-level lookup was replaced with a near-best ring cache plus cold ordered map. Phase 7a reached about 23.2 ns/op; direct `PriceLevelPool` storage and forced inlining of tiny pool/handle helpers brought Phase 7c to about 19.3 ns/op / 51.7M ops/s.

### Phase 8 (Array Side Book)

Phase 8 unified price-level storage into a direct-addressed array and used an occupancy bitmap tree for next-best lookup. The optimized Phase 8b version reaches about 17.2 ns/op / 58.1M ops/s, with 130.05 instructions/op and 1.229 branch misses/op. The eager-clear Phase 8c experiment regressed by about 5% and was rejected.

### Phase 9 (Per-Scenario Diagnosis and System Isolation)

Phase 9 added complete per-call cycles/ns collection for existing-level add, new-level add, and cancel within the real macro workload. It then tested CPU/NUMA binding, IRQ and workqueue migration, realtime scheduling, watchdog suppression, and boot-time `nohz_full` on Hetzner CCX23.

`nohz_full` reduced CPU2 softirqs from roughly 2254 to 219 and local timer interrupts from roughly 15590 to 4695, but p99 remained essentially unchanged. System tuning is therefore considered complete for this VM; the next engine target is instruction-count reduction in `add_rest_new_level`.

Full reports:
- `report/phase1_vs_phase2_report.md` — Phase 1 → 2a → 2b comparison
- `report/phase2b_to_phase_2e_comparison.md` — Hash table engineering (2b–2e)
- `report/phase3_hft_benchmark_design.md` — HFT benchmark design
- `report/phase6_engine_handle_refactor_plan.md` — gateway-owned identity and engine handles
- `report/phase7_hot_ring_cold_map_design.md` — hot ring + cold map design
- `report/phase7_benchmark_results.md` — Phase 7 benchmark results and RingSize sweep
- `report/phase8_fixed_array_design.md` — rationale for unified array storage
- `report/phase8_array_side_book_results.md` — Phase 8a/8b/8c results and final decision
- `report/phase9_per_scenario_benchmark.md` — per-scenario design and final Linux tuning campaign

---

## Project Layout

```
llmes/
├── CMakeLists.txt
├── readme.md
├── core/matching_core/
│   ├── include/matching/
│   │   ├── order_book.hpp          # OrderBook class
│   │   ├── array_side_book.hpp     # direct-addressed bid/ask side book
│   │   ├── occupancy_tree.hpp      # hierarchical active-price bitmap
│   │   ├── price_level.hpp         # Intrusive doubly-linked list per price
│   │   ├── order_pool.hpp          # Pre-allocated order pool
│   │   └── types.hpp               # Order, Trade, AddResult, ErrorCode
│   ├── src/
│   │   ├── occupancy_tree.cpp
│   │   └── order_book.cpp
│   └── tests/
│       └── order_book_test.cpp
├── benchmark/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── benchmark_runner.hpp    # IBenchScenario interface
│   │   ├── benchmark_runner.cpp    # measurement runner
│   │   ├── bench_common.hpp        # shared benchmark utilities
│   │   └── hft/
│   │       ├── bench_hft_macro.cpp
│   │       ├── bench_hft_macro_scenarios.cpp
│   │       └── hft_macro_workload.hpp
│   ├── scripts/
│   │   ├── run_benchmarks.sh
│   │   ├── run_hft_macro_scenarios.sh
│   │   ├── run_remote_compare.sh
│   │   ├── run_remote_hft_macro_scenarios_tuned.sh
│   │   ├── setup_remote_nohz_full.sh
│   │   ├── merge_benchmark_metrics.py
│   │   ├── plot_benchmark.py
│   │   ├── plot_hft_macro_scenarios.py
│   │   └── plot_version_comparison.py
│   └── results/                    # generated CSV + plots
├── report/                         # design docs + analysis reports
└── server_results/                 # remote benchmark run artifacts
```

---

## Design Notes

- **Correctness first**: Phase 1 used `std::map` + `std::list` for a straightforward, verifiable implementation.
- **Phase 2a**: Pool allocator eliminates `malloc`/`free` from the hot path — instruction count drops 22–38%.
- **Phase 2b**: O(1) cancel index transforms the engine for cancel-dominated workloads. The trade-off (hash-map overhead on every match) is acceptable given 97% cancellation in real markets.
- **Phase 2e**: `absl::flat_hash_map`'s Swiss-table design handles HFT spatial locality better than both `std::unordered_map` and hand-rolled open-addressing.
- **Phase 3**: HFT benchmarks replace the original ad-hoc workload mix with empirically grounded order flow: exponential depth decay, spatial concentration at the best price, cancel clusters, and a Zero-Intelligence macro model.
- **Phase 6a**: Gateway-owned identity moves external id validation and id-to-handle mapping out of the matching core; cancel/modify use pool-index handles.
- **Phase 7**: A near-best ring plus cold map proved the value of fixed-index lookup, direct level pooling, and forced inlining on tiny hot helpers.
- **Phase 8b**: A unified array side book removes hot/cold migration; a fixed occupancy tree finds the next active price.
- **Phase 9**: Per-scenario macro diagnosis isolates new-level add as the main basic-operation tail; aggressive Linux isolation does not materially move p99.

---

## Time Complexity

| Function | Average | Notes |
|---|---|---|
| `cancel_order()` | O(1) | Resolve `OrderHandle` by pool index + intrusive-list erase |
| `add_limit_order()` | O(K + H) | Match K makers; resting lookup is O(1), next-best traversal is O(H) bitmap levels |
| `add_market_order()` | O(K + L*H) | Match K makers and advance across L price levels through the occupancy tree |

K = makers matched, L = price levels drained, H = occupancy-tree height (fixed at 3).

---

## Roadmap

1. **Phase 1** ✓ — Functional core + tests + benchmark harness
2. **Phase 2** ✓ — Intrusive queue + O(1) cancel index + hash table engineering
3. **Phase 3** ✓ — HFT micro + macro benchmarks, realistic workload modeling
4. **Phase 4** ✓ — Price-level storage strategy and ChunkPool experiment
5. **Phase 5** ✓ — Production profiling with window-isolated `perf record`
6. **Phase 6** ✓ — Gateway-owned identity and handle-based matching core
7. **Phase 7** ✓ — Hot ring buffer + cold map price-level storage
8. **Phase 8** ✓ — Unified array side book + fixed occupancy tree
9. **Phase 9** ✓ — Per-scenario macro diagnosis + Linux system-level isolation
10. **Next** — Profile-guided instruction reduction in `add_rest_new_level`
11. Market data, execution, risk, tslib, lfutils
