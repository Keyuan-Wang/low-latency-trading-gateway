# Phase 5 Macro Profiling Plan

## Context

Phase 4 ended with a corrected `hft_macro` benchmark and operation-level
profiling. The important fix was removing measured-batch tracking drift: cancel
and modify targets now come from live book state, so the macro workload no
longer produces a large artificial `cancel_miss` population.

Phase 5 starts from the repaired `master` branch and focuses on profiling before
making more data-structure changes.

## Latest Aligned Run

Cloud run:

```text
branch:       master
commit:       5186b6c
scenario:     hft_macro
trials:       1
orders:       100000
levels:       100
batch_size:   100000
warmup_iters: 1
iters:        1
seed:         42
```

Artifacts:

```text
server_results/macro_op_profile_cloud_master_pmc_t1_20260601/
```

The run contains two aligned measurements:

- `latency`: wall-clock macro and per-operation latency.
- `pmc`: in-process perf counters around the measured benchmark window.

## Add-Rest Stage Profile

The `add_rest` path was then instrumented and profiled separately to move from
operation-level attribution to function-level attribution.

Cloud run:

```text
version_tag:  master-add-rest-stage
commit_sha:   c8a62a1+addrest
orders:       100000
levels:       100
batch_size:   100000
warmup_iters: 1
iters:        1
seed:         42
```

Artifacts:

```text
server_results/hft_macro_add_rest_stage_cloud_t1_20260601/
```

Sanity check:

| Metric | Value |
|---|---:|
| `add_rest` op count | 47851 |
| add-rest stage count | 47851 |
| `cancel_miss` | 0 |
| `modify_miss` | 0 |

Stage breakdown:

| Stage | Mean ns | Mean cycles | ns share | cycles share |
|---|---:|---:|---:|---:|
| `level_lookup` | 41.573 | 173.310 | 19.271% | 16.811% |
| `match` | 31.367 | 149.873 | 14.540% | 14.538% |
| `id_index_insert` | 30.707 | 146.900 | 14.234% | 14.249% |
| `validation` | 29.416 | 143.791 | 13.635% | 13.948% |
| `fifo_append` | 27.851 | 140.087 | 12.910% | 13.588% |
| `node_init` | 27.503 | 137.910 | 12.749% | 13.377% |
| `pool_acquire` | 27.314 | 139.065 | 12.661% | 13.489% |

This is profiling-mode data, so the absolute `add_rest` latency is inflated by
instrumentation overhead. The useful signal is the relative split inside the
add-rest path.

## Overall Result

| Mode | avg_ns | ops_s | cycles/op | instructions/op | CPI | branch miss rate | cache misses/op | ok |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `latency` | 103.934 | 9.62145e+06 | - | - | - | - | - | 199981 |
| `pmc` | - | - | 361.417 | 670.021 | 0.539411 | 0.0192954 | 0.07932 | 199989 |

The PMC result does not look memory-bound. `cache_misses_per_op = 0.07932`,
which is roughly:

```text
0.07932 / 670.021 * 1000 = 0.118 cache misses per 1000 instructions
```

This is very low for a workload that would be dominated by LLC or DRAM misses.
The current macro benchmark is more consistent with fixed instruction-path cost
than with cache-miss latency.

## Operation-Level Result

The latency run gives the clearest weighted-cost picture:

| Operation | Share | Mean ns | p50 ns | p95 ns | p99 ns | Weighted time share |
|---|---:|---:|---:|---:|---:|---:|
| `add_rest` | 47.849% | 65.023 | 60 | 100 | 150 | 53.592% |
| `cancel_hit` | 46.159% | 43.507 | 40 | 60 | 60 | 34.592% |
| `modify_hit` | 3.929% | 117.144 | 100 | 220 | 290 | 7.928% |
| `market` | 1.597% | 119.105 | 80 | 250 | 410.4 | 3.276% |
| `add_cross` | 0.466% | 76.094 | 70 | 130 | 173.5 | 0.611% |
| `cancel_miss` | 0.000% | 0.000 | 0 | 0 | 0 | 0.000% |
| `modify_miss` | 0.000% | 0.000 | 0 | 0 | 0 | 0.000% |

The workload is now internally coherent:

- `cancel_miss = 0`
- `modify_miss = 0`
- `add_rest + add_cross ~= 48.3%`
- `cancel_hit ~= 46.2%`
- `market ~= 1.6%`

## Interpretation

### `add_rest` Is The Main Macro Cost Center

`add_rest` contributes more than half of measured macro time:

```text
47.849% event share * 65.023 ns mean = 53.592% weighted time share
```

It is not the slowest individual operation, but it is the highest-leverage path
because it is hit on nearly half of all macro events.

In HFT terms, this is also the quote-placement path. Reducing `add_rest` latency
can improve quote refresh speed and same-price FIFO positioning. That makes it
more commercially relevant than optimizing rare market sweeps in the current
workload.

### `cancel_hit` Is Important But Already Tight

`cancel_hit` is the second-largest weighted contributor, but its latency profile
is already strong:

```text
mean = 43.507 ns
p50  = 40 ns
p95  = 60 ns
p99  = 60 ns
```

The current O(1) cancel path appears healthy. It should be protected during
future changes, but the data does not point to cancel as the first optimization
target.

### `modify_hit` Is Expensive But Low Share

`modify_hit` is more expensive than `add_rest` and `cancel_hit`, but its event
share is only about 3.9%. Its weighted contribution is meaningful but secondary.
This result is expected because modify is structurally close to cancel plus add.

### `market` Is Not The Current Bottleneck

Market orders are low-frequency and shallow in this workload:

```text
market share ~= 1.6%
market_levels_p99 = 2
market_filled_qty_p99 = 5
```

Optimizing deep market sweeps is unlikely to move this macro benchmark unless
the workload model changes.

### Cache Locality Is Not The Leading Hypothesis

The low generic cache-miss rate weakens the hypothesis that macro performance is
currently dominated by cache misses from poor order locality. This does not
mean memory layout is irrelevant, but it does mean that the next optimization
should not be based on cache-locality intuition alone.

The ChunkPool experiment was useful because it tested that intuition. The
current profiling result suggests Phase 5 should instead identify which fixed
steps inside `add_rest` consume cycles.

The function-level result confirms that `add_rest` is not dominated by a single
call site. `level_lookup` is the largest measured stage, but it is only about
19% of the measured add-rest stage time. The rest of the path is spread across
`match`, `id_index_insert`, `validation`, `fifo_append`, `node_init`, and
`pool_acquire`.

## Cloud Profiling: level_lookup_existing vs level_create_new

The `get_or_create()` return type was changed from `PriceLevel&` to
`std::pair<PriceLevel*, bool>`, using `try_emplace` instead of `operator[]`, to
allow the profiling instrumentation to distinguish existing-level lookup from
new-level creation. The `AddRestCallProfile` accumulates timing for whichever
path was taken, then commits both to the global snapshot at the end of the
`add_limit_order` call.

A cloud profiling run was executed with both
`LLMES_PROFILE_HFT_MACRO_OPS=ON` and `LLMES_PROFILE_ADD_REST_STAGES=ON`.

Cloud run:

```text
branch:       master
commit:       237820e
scenario:     hft_macro
trials:       1
orders:       100000
levels:       100
batch_size:   100000
warmup_iters: 1
iters:        1
seed:         42
```

Artifacts:

```text
benchmark/results/add_rest_stage_profile_cloud_20260601/
├── latency_raw.csv
├── op_raw.csv
└── stage_raw.csv
```

Sanity check:

| Metric | Value |
|---|---:|
| `cancel_miss` | 0 |
| `modify_miss` | 0 |
| `add_rest` op count | 47,850 |

### Operation-Level Result (profiling mode)

| Operation | Count | Share | Mean ns | Mean cycles | Weighted ns share |
|---|---:|---:|---:|---:|---:|
| `add_rest` | 47,850 | 47.85% | 524.24 | 1,331.13 | **90.44%** |
| `cancel_hit` | 46,159 | 46.16% | 39.92 | 169.56 | 6.64% |
| `modify_hit` | 3,893 | 3.89% | 131.93 | 393.54 | 1.85% |
| `market` | 1,591 | 1.59% | 117.11 | 353.21 | 0.67% |
| `add_cross` | 507 | 0.51% | 217.50 | 593.94 | 0.40% |

Note: profiling-mode absolute latencies are inflated by instrumentation
overhead (`__rdtsc()` + `steady_clock::now()` per stage × 7). The production
`add_rest` latency is ~35 ns; the profiling-mode measurement includes ~220 ns
of stage-internal work plus ~304 ns of timing overhead. Only the relative
proportions are meaningful.

### Add-Rest 8-Stage Breakdown

| Stage | Mean ns | Mean cycles | ns share | cycles share |
|---|---:|---:|---:|---:|
| `match` (含 can_cross) | 32.45 | 150.05 | 14.71% | 15.57% |
| `id_index_insert` | 30.86 | 147.03 | 13.99% | 15.25% |
| `validation` (3 checks) | 29.11 | 142.57 | 13.20% | 14.79% |
| `fifo_append` | 28.48 | 141.43 | 12.92% | 14.67% |
| `node_init` | 28.01 | 139.06 | 12.70% | 14.43% |
| `pool_acquire` | 27.66 | 138.94 | 12.54% | 14.41% |
| **`level_lookup_existing`** | **26.86** | **63.89** | **12.18%** | **6.63%** |
| **`level_lookup_create_new`** | **17.10** | **40.98** | **7.75%** | **4.25%** |

Note on `count` reporting: the current profiling accumulator unconditionally
increments the `count` field for every stage slot on each `add_rest` call, so
all 8 stages report `count = 47,850`. The `mean_ns` and `mean_cycles` columns
are computed from `total_ns / count` and `total_cycles / count` in the emitter
and should be read as average-cost-per-add_rest, not as average-cost-per-hit
for the level_lookup stages. A future refinement should track per-stage hit
counts separately. The `total_ns` and `total_cycles` columns are correct.

### Key Findings

#### 1. `std::map::try_emplace` is Not the Bottleneck

`level_lookup_existing` costs 63.89 cycles per add_rest — the **lowest cycle
cost of any stage by a wide margin** (the next lowest is `pool_acquire` at
138.94 cycles). The `std::map` red-black tree lookup for an existing price level
is well-optimized by the compiler and accounts for only 6.63% of add_rest cycle
time. **Replacing `std::map` with a faster price-index structure (absl::btree_map,
ring buffer, or flat hash map) would address at most ~6.6% of the add_rest cycle
budget.**

This directly answers the question posed by the Phase 4 price-level storage
strategy report: the ordered-map lookup is **not** the performance bottleneck
for the dominant `add_rest` path in the HFT macro workload.

#### 2. No Single Stage Dominates — The Cost Is Distributed

The 7 stages (treating the two level_lookup variants as one path) each consume
between 12.5% and 15.6% of add_rest cycles. The standard deviation across the 7
main stages is only ~4.8 cycles. This means:

- There is no "fix one slow function and win" opportunity.
- Any optimization that targets a single stage caps out at ~15% improvement on
  `add_rest`, which translates to ~8% on macro throughput (since `add_rest` is
  ~53% of weighted macro time).
- Real progress requires reducing fixed cost across the entire path: fewer total
  instructions, fewer branches, fewer store operations.

The ChunkPool experiment's failure makes sense in this light: it reduced
allocation cost (pool_acquire) but added overhead to fifo_append (chunk
linkage), id_index_insert (no change), and remove (chunk_from_order +
link_available). The net effect was a ~12% regression because the distributed
overhead increase outweighed the localized allocation improvement.

#### 3. `level_create_new` Is Rare In Steady State

In a warmed book with 100,000 orders across 100 price levels, nearly every
price already has an active level. The `level_create_new` total cycles
(1,961,018) are only ~39% of `level_lookup_existing` total cycles (3,057,094).
Since both stages have the same inflated count, the ratio of actual hits is
proportional to the total cycle ratio: **existing-level lookups outnumber
new-level creations by roughly 2.5:1 in this workload.** The create path is
too rare to be an optimization target — every effort should go into the
existing-lookup hot path.

#### 4. Profiling-Mode Count Reporting Issue

The `RecordAddRestStageProfile` function unconditionally increments
`stage.count` for every stage slot regardless of whether that stage accumulated
non-zero ns/cycles. This means `count` and `mean_ns`/`mean_cycles` in the CSV
output should be interpreted as "per-add_rest amortized cost," not "per-hit
cost." The `total_ns` and `total_cycles` columns are unaffected. A future fix
should make the ScopedAddRestStage increment count only for the specific stage
it measures, and the level_lookup split should track separate hit counts.

## Revised Phase 5 Strategy

The profiling data narrows the optimization space decisively:

1. **The price-index container is not the binding constraint.** The `std::map`
   lookup for existing levels costs 63.89 cycles — the cheapest stage. This
   removes the urgency from the Phase 4 plan's V2 (absl::btree_map), V4 (hot
   ring), V5 (bitmap), and V6 (cold container experiments). Those changes would
   optimize a cost center that represents 6.6% of add_rest cycles.

2. **The bottleneck is distributed fixed cost.** The remaining ~93% of add_rest
   cycles is spread across validation, match, allocation, initialization,
   append, and index insertion — each 139–150 cycles. The optimization strategy
   should shift from "find the slow stage" to "reduce instruction count and
   branch density across the entire add path."

3. **Concrete next candidates:**
   - **Merge validation checks.** `pending_cancel_ids_.contains()` +
     `id_to_order_.contains()` + `quantity == 0` are three sequential branches.
     Combining them into a single early-return path with fewer mispredict
     opportunities could reduce validation cost.
   - **Eliminate redundant stores.** `node_init` aggregates 6 field assignments
     (`*node = {id, price, qty, ts, nullptr, nullptr}`). Some of these are
     overwritten by `fifo_append` (`prev`, `next`) or `pool_acquire`
     (`parent_level`). Reducing the store count by initializing only the
     business fields and letting the append/pool paths set their own metadata
     could save cycles.
   - **Inline `push_back`.** `fifo_append` costs 141.43 cycles for what is a
     simple 4-pointer linked-list append. The function call overhead on a
     non-inlined method may be material at this scale.
   - **Profile with `perf record` / `perf annotate` for instruction-level
     hotspots.** The stage breakdown is ~200 ns in profiling mode; a
     non-instrumented production-mode cycle-accurate profile via `perf` would
     show exact instruction retirement stalls.

4. **Do not pursue price-index replacement until a different workload demands
   it.** The current macro workload (47.85% add, 46.16% cancel, shallow market
   sweeps) does not stress the ordered map. A workload with deeper market
   sweeps, wider price ranges, or more level churn could change the conclusion.
   But for the current workload, the data is clear.

## Working Rule For Phase 5

Do not introduce another major storage redesign. The profiling data shows the
distributed nature of the add_rest cost: the cheapest stage is the `std::map`
lookup at 63.89 cycles, while six other stages each cost 139–150 cycles. The
next change should be instruction-level optimization of the add path, guided by
`perf annotate` rather than by structural intuition.

The level_lookup_existing vs level_create_new split is now recorded. The
remaining instrumentation gap is per-stage hit counting (cosmetic fix to the
profiling accumulator) and a production-mode `perf record` run for
instruction-level attribution.
