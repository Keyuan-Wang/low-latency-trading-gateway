# low-latency-trading-gateway

**Low-latency trading gateway: epoll I/O, SPSC queues, and a fixed-frame order-entry protocol.**

The pure matching engine lives in the sibling repo **`low-latency-matching-engine`**. This repo does **not** depend on the order book.

## What is in this repo

```text
core/order_entry/   64B binary protocol, codec, frame parser, session validation
core/SPSC/          SPSC ring-buffer variants (mutex → atomic + opponent-index cache)
examples/           blocking + epoll server/client prototypes
report/             protocol design + SPSC cloud benchmark
```

## Protocol

Fixed **64-byte** frames: 32B header + 32B payload (`lltg::order_entry`).

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

## Thread model (epoll + SPSC + eventfd)

```text
Client ←TCP→ Gateway thread ←SPSC→ Matching thread (stub)
               │                        │
         epoll + parse            process EngineCommand
         encode + send            push EngineResponse
               │                        │
               └──── eventfd notify ────┘
```

- **Gateway thread**: epoll event loop, non-blocking accept, per-connection `FrameParser`, sequence validation, Heartbeat/Logout handling. Order messages are converted to `EngineCommand` and pushed to the SPSC command queue.
- **Matching thread**: spin-polls the command queue, produces `EngineResponse`, writes `eventfd` to wake the gateway's `epoll_wait`.
- **Session routing**: each connection holds a `SessionToken` (slot + generation). A `SessionSlot` array maps tokens back to fds for response delivery.

The matching thread currently runs a **stub** engine. Wiring a real order book belongs in a future integration layer and is intentionally out of this repo.

## Build

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLTG_BUILD_TESTS=ON

cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Useful targets:

```bash
ctest --test-dir build --output-on-failure
cmake --build build --target order_entry_epoll_server order_entry_epoll_bench_client
cmake --build build --target spsc_tests
```

Standalone SPSC microbench (optional):

```bash
cd core/SPSC
g++ -O3 -std=c++20 -pthread test.cpp -o test
./test all 50000000
```

## Related repos

| Repo | URL | Role |
|---|---|---|
| `low-latency-trading-gateway` (this) | https://github.com/Keyuan-Wang/low-latency-trading-gateway | epoll + SPSC + order-entry protocol |
| `low-latency-matching-engine` | https://github.com/Keyuan-Wang/low-latency-matching-engine | Pure matching engine (no networking) |
