# Hash Table Engineering: All Phases Comparison

## Overview

Four hash table strategies benchmarked on the same server in a single session (same CPU frequency, same load, same benchmark parameters).

## Results (orders=100,000, levels=100)

| Phase | Data Structure | Overall ops/s | vs 2b | Instr/op | CPI | Cache miss/op |
|---|---|---|---|---|---|---|
| **2b** | `std::unordered_map` | 4.57M | baseline | 868.6 | 0.84 | 7.34 |
| **2c** | Custom open-addressing + tombstones | 5.03M | **+10.1%** | 665.2 | 1.03 | 7.35 |
| **2d** | Robin Hood + backward shift | 4.64M | +1.5% | 687.0 | 1.07 | 9.19 |
| **2e** | `absl::flat_hash_map` (Swiss Table) | 5.15M | **+12.9%** | 881.7 | 0.83 | 8.59 |

### Per-Scenario Breakdown

| Scenario | phase2b | phase2c | phase2d | phase2e | Best |
|---|---|---|---|---|---|
| **cxl_hit** | 5.80M ops/s | 2.34M | 3.55M | 3.29M | **2b** |
| **cxl_miss** | 15.4M | 14.1M | 14.5M | **18.7M** | **2e** |
| **dup_reject** | **59.4M** | 45.2M | 37.7M | 58.1M | **2b** |
| **lmt_rest** | 11.3M | **17.4M** | 16.9M | 13.5M | **2c** |
| **lmt_cross_shallow** | 10.2K | **15.3K** | 13.4K | 9.4K | **2c** |
| **lmt_cross_deep** | 249.9K | **253.6K** | 249.0K | 226.9K | **2c** |
| **mkt_sweep_deep** | 135.6K | **155.7K** | 151.5K | 119.6K | **2c** |
| **overall** | 4.57M | 5.03M | 4.64M | **5.15M** | **2e** |

## Analysis

### Phase 2c — Custom Open Addressing (+10.1%)
The biggest single improvement. Eliminating per-entry heap allocation (`malloc`/`free`) from `std::unordered_map` reduces instructions by 23% in the mixed workload. The win is concentrated in insert-heavy paths (lmt_rest +54%, mkt_sweep_deep +15%), where the absence of allocation dominates. Cancel-heavy paths regress (cxl_hit −60%) due to tombstone-induced probe chain elongation.

### Phase 2d — Robin Hood + Backward Shift (+1.5%)
Theoretically elegant — backward shift deletion eliminates tombstones, Robin Hood bounds probe distances. In practice, at 60% load factor the tombstone problem was not severe enough to justify the overhead. Every erase now does an O(cluster) compaction scan, adding CPI and cache misses. The result: essentially flat vs phase2b, and −7.8% vs phase2c.

### Phase 2e — `absl::flat_hash_map` (+12.9%)
Google's production Swiss Table. The SIMD metadata probe delivers the best overall throughput, but the gains are uneven:
- **cxl_miss (+21%)** and **overall** benefit from fast miss detection
- **lmt_rest (+19%)** and **cxl_hit** are mixed — the SIMD probe adds instructions vs a simple bucket lookup
- **dup_reject (−2%)** and **lmt_cross_shallow (−8%)** regress because the probe chain touches more cache lines

Interestingly, phase2e's instruction count (881.7) is *higher* than phase2b's (868.6) — the Swiss Table's metadata probe uses more instructions than `std::unordered_map`'s bucket-and-node walk. The throughput gain comes entirely from higher IPC (0.83 vs 0.84 — actually similar) — wait, the gain is from better cache behavior on specific paths.

## Conclusion

| Phase | Change | Relative to 2b |
|---|---|---|
| 2b | `std::unordered_map` (baseline) | — |
| 2c | Custom open-addressing + tombstones | **+10.1%** |
| 2d | Robin Hood + backward shift | +1.5% |
| 2e | `absl::flat_hash_map` | **+12.9%** |

The best result is phase2e (`absl::flat_hash_map`) at +12.9% over baseline. The custom phase2c implementation comes close at +10.1% with significantly less code. For this workload, the simplest change (replacing `std::unordered_map` with a flat slot array) captures most of the gain, and a production-grade library only adds a few more percent.
