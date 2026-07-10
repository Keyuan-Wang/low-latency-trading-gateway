# llmes-gateway

**Low-latency trading gateway: epoll I/O, SPSC queues, and a fixed-frame order-entry protocol.**

Split from the original [`llmes`](../llmes) monorepo (full git history preserved). The pure matching engine lives in the sibling repo **`llmes-orderbook`**. This repo does **not** depend on the order book.

## What is in this repo

```text
core/order_entry/   64B binary protocol, codec, frame parser, session validation
core/SPSC/          SPSC ring-buffer variants (mutex → atomic + opponent-index cache)
examples/           blocking + epoll server/client prototypes
report/             protocol design + SPSC cloud benchmark
```

## Protocol

Fixed **64-byte** frames: 32B header + 32B payload (`llmes::order_entry`).

| Requests | Responses |
|---|---|
| `NewOrder`, `CancelOrder`, `ModifyOrder`, `Heartbeat`, `Logout` | `Accepted`, `Rejected`, `Cancelled`, `Modified`, `Trade` |

See [`report/order_entry_protocol_codec_design.md`](report/order_entry_protocol_codec_design.md).

## SPSC

| Variant | Latency | Throughput |
|---|---:|---:|
| Mutex baseline | 98.3 ns/msg | 10.2 Mmsg/s |
| Atomic + padding + acquire/release | 7.51 ns/msg | 133 Mmsg/s |
| **Atomic + opponent-index cache** | **4.35 ns/msg** | **230 Mmsg/s** |

See [`report/spsc_cloud_benchmark_20260617.md`](report/spsc_cloud_benchmark_20260617.md).

## Thread model (epoll prototype)

```text
Gateway thread          SPSC cmd queue         Matching thread (stub)
  parse / session   -->  EngineCommand     -->  map to EngineResponse
  encode / send     <--  EngineResponse    <--  (no OrderBook yet)
                    SPSC rsp queue + eventfd
```

`order_entry_epoll_server` currently uses a **stub** matching thread. Wiring a real book belongs in a future integration layer and is intentionally out of this repo.

## Build

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLMES_BUILD_TESTS=ON

cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Useful targets:

```bash
cmake --build build --target order_entry_tests spsc_tests
cmake --build build --target order_entry_blocking_server order_entry_blocking_client
cmake --build build --target order_entry_epoll_server
cmake --build build --target order_entry_echo_bench_server order_entry_echo_bench_client
```

Standalone SPSC microbench (optional):

```bash
cd core/SPSC
g++ -O3 -std=c++20 -pthread test.cpp -o test
./test all 50000000
```

## Related repos

| Repo | Role |
|---|---|
| `llmes-gateway` (this) | epoll + SPSC + order-entry protocol |
| `llmes-orderbook` | Pure matching engine (no networking) |
| `llmes` | Archive of the original monorepo + `server_results` |
