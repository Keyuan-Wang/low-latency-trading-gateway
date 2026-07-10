# SPSC Ring Buffer Cloud Benchmark Results

**Date:** 2026-06-17  
**Source:** `core/SPSC/spsc_ring_buffer.hpp`  
**Artifacts:** `server_results/spsc_full_20260617/`  
**Host:** `167.233.107.151` — Hetzner, AMD EPYC-Milan, 4 vCPU, KVM, `g++ -O3`, `taskset -c 2,3`

All numbers below come from a **single full run** of all five modes (50M messages each).

---

## 1. Evolution Ladder

Implementations are ordered in source as a deliberate progression — each step adds one
idea on top of the previous:

| Step | CLI mode | Class | What it adds |
|:---:|---|---|---|
| 0 | `mutex` | `SpscRingBufferMutex` | `std::mutex` baseline |
| 1 | `atomicv1` | `SpscRingBufferAtomicV1` | lock-free atomics, `seq_cst`, **no padding** |
| 2 | `atomicv2` | `SpscRingBufferAtomicV2` | **cache-line padding** + relaxed/acquire/release, reloads remote index every op |
| 3 | `atomicv3` | `SpscRingBufferAtomicV3` | **cached opponent** head/tail (modulo indices) |
| 4 | `atomicv4` | `SpscRingBufferAtomicV4` | **cached local** monotonic head/tail counters |

All variants: capacity **1024**, power-of-two, bitmask / `idx_of` indexing.

```
mutex ──► atomicv1 ──► atomicv2 ──► atomicv3 ──► atomicv4
 lock      seq_cst      padding       opp cache     local cache
           no pad       no cache      modulo idx    monotonic idx
```

---

## 2. Environment & Workload

| Item | Value |
|---|---|
| Messages per run | 50,000,000 |
| CPU affinity | `taskset -c 2,3` |
| Latency | `taskset -c 2,3 ./test <mode> 50000000` |
| PMC | `taskset -c 2,3 perf stat -r 5 -d ./test <mode> 50000000` |

**Metrics:** `ns/msg` = end-to-end average (push + pop + cross-core sync + spin-wait).
`checksum=ok` verifies `sum(1..N) = N*(N+1)/2`.

---

## 3. Results

### 3.1 Latency (no perf)

| Step | Mode | seconds | ns/msg | Mmsg/s | checksum |
|:---:|---|---:|---:|---:|---|
| 0 | mutex | 4.915 | 98.3 | 10.2 | ok |
| 1 | atomicv1 | 1.798 | 36.0 | 27.8 | ok |
| 2 | atomicv2 | 0.376 | 7.51 | 133 | ok |
| 3 | atomicv3 | 0.217 | **4.35** | **230** | ok |
| 4 | atomicv4 | 0.235 | 4.70 | 213 | ok |

### 3.2 Speedup chain

| Transition | Latency gain | Throughput gain |
|---|---:|---:|
| mutex → atomicv1 | **2.7×** | 2.7× |
| atomicv1 → atomicv2 | **4.8×** | 4.8× |
| atomicv2 → atomicv3 | **1.7×** | 1.7× |
| atomicv3 → atomicv4 | v3 **8.1% faster** | v3 **8.1% higher** |
| mutex → atomicv3 | **22.6×** | 22.6× |

### 3.3 PMC (5-run mean)

| Step | Mode | time elapsed | cycles/msg | insn/msg | CPI | branch miss | L1d miss | ctx sw |
|:---:|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | mutex | 4.76 s ±5.6% | 604 | 312 | 1.94 | 8.1% | 1.7% | 1,194 |
| 1 | atomicv1 | 1.82 s ±1.4% | 234 | 22.6 | 10.4 | 9.3% | 9.7% | 13 |
| 2 | atomicv2 | 0.444 s ±2.5% | 56.4 | 20.6 | 2.73 | 6.5% | 8.4% | 5 |
| 3 | atomicv3 | 0.240 s ±0.2% | **30.3** | **19.0** | 1.59 | 0.5% | 3.7% | 3 |
| 4 | atomicv4 | 0.267 s ±0.2% | 33.7 | 23.6 | 1.43 | 0.6% | 3.6% | 3 |

Raw logs: `server_results/spsc_full_20260617/latency.log`, `pmc.log`

---

## 4. What Each Step Buys

### Step 0 → 1: mutex to lock-free seq_cst

Removing the mutex eliminates heavy lock contention (**1,194 context-switches**, 49%
frontend stalls) and cuts latency from **98 ns to 36 ns** (2.7×). But `head_`/`tail_`
share a cache line and every access is `seq_cst` — CPI stays at **10.4**, dominated by
cross-core coherency (9.7% L1d miss rate).

### Step 1 → 2: padding + proper memory ordering

`SpscRingBufferAtomicV2` isolates `head_`/`tail_` on separate cache lines and uses
relaxed/acquire/release. Latency drops **36 ns → 7.5 ns** (4.8×). CPI falls from 10.4
to 2.73. The remote index is still acquire-loaded on **every** operation.

### Step 2 → 3: cached opponent indices

`SpscRingBufferAtomicV3` caches the opponent's index locally (`producer_tail_cache_`,
`consumer_head_cache_`) and only acquire-loads across cores when the ring appears
full/empty. Latency drops **7.5 ns → 4.4 ns** (1.7×). Cycles/msg nearly halve (56 → 30).
Branch miss rate collapses to **0.5%**. This is the step into the production tier.

### Step 3 → 4: cached local monotonic counters

`SpscRingBufferAtomicV4` replaces modulo indices with monotonic local counters mapped
via `idx_of(counter)`. Same ordering and opponent-cache pattern, but **24% more
instructions** per message (23.6 vs 19.0). Latency regresses **4.4 ns → 4.7 ns**
(~8% slower). CPI improves (1.59 → 1.43) but total work is higher.

**Similar finding as orderbook:** Must be careful of the tradeoff between cache efficiency
and instruction/op. **Latency is driven by total cycles, not CPI alone.**

---

## 5. Assembly Note (atomicv3, cloud `-O3`)

Disassembly confirms all `std::atomic` loads/stores in v3 `push`/`pop` lower to plain
`mov` on x86-64 — no `lock` prefix, no real `xchg` (only `xchg ax,ax` = 2-byte NOP).

| Operation | Instruction |
|---|---|
| `head_.store(release)` | `mov [base+0x2000], reg` |
| `tail_.store(release)` | `mov [base+0x2040], reg` |
| `tail_.load(acquire)` (push slow path) | `mov reg, [base+0x2040]` |
| `head_.load(acquire)` (pop slow path) | `mov reg, [base+0x2000]` |

---

## 6. Visual Summary

```
Latency (ns/msg)
mutex    ████████████████████████████████████████  98.3
atomicv1 ███████████████                             36.0
atomicv2 ███                                          7.5
atomicv4 ██▌                                          4.7
atomicv3 ██                                           4.4

Throughput (Mmsg/s)
mutex    ████                                        10.2
atomicv1 ███████████                                 27.8
atomicv2 ██████████████████████████████████████     133
atomicv4 ████████████████████████████████████████   213
atomicv3 ████████████████████████████████████████████ 230

Cycles per message
mutex    ████████████████████████████████████████  604
atomicv1 ███████████████                             234
atomicv2 ███▌                                         56
atomicv4 ██                                           34
atomicv3 ██                                           30
```

---

## 7. Conclusions

| Priority | Recommendation |
|---|---|
| **Adopt** | `SpscRingBufferAtomicV3` — best throughput (4.35 ns/msg, 230 Mmsg/s) |
| **Consider** | `SpscRingBufferAtomicV4` when monotonic counter semantics matter (~8% cost) |
| **Avoid** | `SpscRingBufferMutex` on hot paths (~23× slower) |
| **Prototype** | `SpscRingBufferAtomicV1` for correctness checks only |
| **Reference** | `SpscRingBufferAtomicV2` shows cost of no opponent cache (7.5 ns) |

**Design principles validated:**

1. Lock-free beats mutex.
2. Cache-line padding + relaxed/acquire/release beats seq_cst.
3. Opponent-index caching is the key step into sub-5 ns territory.
4. Local monotonic counters are optional; modulo indices with opponent cache win here.

---

## 8. Limitations

- End-to-end `ns/msg`, not single-op microbenchmark.
- Busy-spin workload; no spin-then-yield evaluation.
- KVM VM; absolute numbers may differ on bare metal.
- Single producer / consumer, capacity 1024 only.

---

## Appendix: Reproduction

```bash
cd /root/low-latency-trading-gateway/core/SPSC
g++ -O3 -std=c++20 -pthread test.cpp -o test

for mode in mutex atomicv1 atomicv2 atomicv3 atomicv4; do
  taskset -c 2,3 ./test "$mode" 50000000
  taskset -c 2,3 perf stat -r 5 -d ./test "$mode" 50000000
done
```