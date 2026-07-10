# Project History

This file records the development history of the low-latency-trading-gateway project. It covers the SPSC lock-free ring buffer, the order-entry binary protocol, and the epoll-based networking layer.

For the matching-engine and order-book optimization history (Phases 1-11), see the sibling repository [`low-latency-matching-engine`](https://github.com/Keyuan-Wang/low-latency-matching-engine).

## Source Records

- `report/spsc_cloud_benchmark_20260617.md`
- `report/order_entry_protocol_codec_design.md`
- `server_results/spsc_full_20260617/` (in the original `llmes` archive)
- `server_results/spsc_opt_compare_20260617/` (in the original `llmes` archive)

## SPSC Lock-Free Ring Buffer

### Motivation

The intended runtime model for the trading system is:

- one gateway thread handles network I/O and protocol parsing;
- one matching thread owns the order book;
- commands flow from gateway to matching through an SPSC queue;
- execution reports and trades flow back through another SPSC queue.

That requires a queue whose overhead is small relative to a nanosecond-scale matching core. A mutex queue is not acceptable on this path.

### Implementation Ladder

The SPSC implementation lives in `core/SPSC/spsc_ring_buffer.hpp`. The source intentionally keeps several versions as an optimization ladder:

| Step | CLI mode | Class | Design |
|:---:|---|---|---|
| 0 | `mutex` | `SpscRingBufferMutex` | `std::mutex` baseline |
| 1 | `atomicv1` | `SpscRingBufferAtomicV1` | lock-free atomics, default `seq_cst`, no padding |
| 2 | `atomicv2` | `SpscRingBufferAtomicV2` | cache-line padding + relaxed/acquire/release |
| 3 | `atomicv3` | `SpscRingBufferAtomicV3` | cached opponent head/tail with modulo indices |
| 4 | `atomicv4` | `SpscRingBufferAtomicV4` | cached local monotonic counters |

All versions use a power-of-two ring capacity and mask-based indexing.

### Cloud Benchmark

Configuration: Hetzner CCX23, AMD EPYC-Milan, `g++ -O3 -std=c++20 -pthread`, `taskset -c 2,3`, 50,000,000 messages, ring capacity 1024.

Latency result:

| Step | Mode | ns/msg | Mmsg/s |
|:---:|---|---:|---:|
| 0 | mutex | 98.3 | 10.2 |
| 1 | atomicv1 | 36.0 | 27.8 |
| 2 | atomicv2 | 7.51 | 133 |
| 3 | atomicv3 | **4.35** | **230** |
| 4 | atomicv4 | 4.70 | 213 |

PMC result:

| Mode | cycles/msg | instructions/msg | CPI | branch miss | Context switches |
|---|---:|---:|---:|---:|---:|
| mutex | 604 | 312 | 1.94 | 8.1% | 1,194 |
| atomicv1 | 234 | 22.6 | 10.4 | 9.3% | 13 |
| atomicv2 | 56.4 | 20.6 | 2.73 | 6.5% | 5 |
| **atomicv3** | **30.3** | **19.0** | 1.59 | 0.5% | 3 |
| atomicv4 | 33.7 | 23.6 | 1.43 | 0.6% | 3 |

### Decision

`SpscRingBufferAtomicV3` was adopted as the recommended queue variant. The key mechanism is reduced cross-core traffic: it only acquire-loads the remote index when the ring appears full or empty. On x86-64, the acquire/release atomics lower to ordinary `mov` instructions; there are no `lock`-prefixed operations on the steady path.

`SpscRingBufferAtomicV4` (monotonic local counters) has slightly better CPI but executes more instructions per message and is about 8% slower.

Full report: `report/spsc_cloud_benchmark_20260617.md`.

## Order-Entry Protocol

### Design

The order-entry protocol uses a fixed 64-byte frame: 32B header + 32B payload.

The header includes magic/version, message type, payload length, flags, sequence number, session id, and reserved space.

| Requests | Responses |
|---|---|
| `NewOrder`, `CancelOrder`, `ModifyOrder`, `Heartbeat`, `Logout` | `Accepted`, `Rejected`, `Cancelled`, `Modified`, `Trade` |

All payloads are 32 bytes. Small control messages use reserved payload bytes. This wastes a few bytes but removes variable-length framing from the hot parser path.

### Implementation

| Component | Role |
|---|---|
| `protocol.hpp` | wire constants, message types, request/response structs |
| `codec.hpp` | explicit little-endian encode/decode helpers |
| `frame_parser.hpp` | per-session ring-buffer parser for TCP byte streams |
| `session.hpp` | sequence/order-id validation and response generation |
| `engine_messages.hpp` | `EngineCommand`/`EngineResponse` for cross-thread communication |

The codec writes fields by explicit wire offsets rather than using `memcpy(struct)`, avoiding C++ padding and ABI assumptions.

The frame parser uses a fixed-size ring buffer. `append()` handles arbitrary TCP byte chunks including write wrap. `try_parse()` only consumes complete 64-byte frames. Because frame size and parser capacity are aligned, parse-time frames remain physically contiguous in the ring.

### Tests

`order_entry_tests` covers codec round trips, parser edge cases (partial reads, multi-frame input, bad frames, buffer full, append wrap), and session validation paths.

### Blocking TCP Baseline

A minimal blocking TCP echo benchmark measured a full local round trip:

| Messages | Total ns | Avg RTT ns/msg | One-way estimate ns/msg |
|---:|---:|---:|---:|
| 100,000 | 4,907,399,662 | 49,074 | 24,537 |

This measures kernel TCP, not the matching engine hot path.

Full design document: `report/order_entry_protocol_codec_design.md`.

## Epoll Gateway

### Single-Threaded Prototype

The first epoll server (`order_entry_epoll_server`) replaced the blocking single-client server with a non-blocking, multi-client event loop:

- `epoll_create1` + level-triggered `EPOLLIN`
- Non-blocking listen socket with accept loop (drain to `EAGAIN`)
- Per-connection `FrameParser` + `OrderEntrySession` in an `unordered_map<int, Connection>`
- `recv → parser.append → try_parse loop → session.handle_message → send`
- Connection cleanup: `epoll_ctl DEL` + `erase` + `close`

### Two-Thread Architecture (SPSC + eventfd)

The gateway was then split into two threads connected by SPSC queues:

```text
Gateway thread              SPSC cmd queue           Matching thread (stub)
  epoll + parse         -->  EngineCommand       -->  process command
  encode + send         <--  EngineResponse      <--  push response
                        SPSC rsp queue + eventfd
```

Key design decisions:

- **Connection no longer holds `OrderEntrySession`**. Each connection holds a `SessionToken` (slot + generation) for routing responses back to the correct fd.
- **`SessionSlot` array** maps `SessionToken.slot` to fd. Generation counter prevents ABA when slots are reused.
- **Sequence validation and Heartbeat/Logout** stay in the gateway thread (fast reject, no queue overhead).
- **Order messages** (NewOrder, Cancel, Modify) are converted to `EngineCommand` and pushed to the command queue.
- **`eventfd`** notifies the gateway thread when the matching thread pushes responses. The gateway registers `eventfd` with epoll alongside listen and client fds.
- **Matching thread** currently runs a stub engine (unconditional accept). Replacing it with a real order book is an integration concern, not a gateway concern.

## Current Status

The gateway project contains three completed layers:

- **SPSC ring buffer**: lock-free, 4.35 ns/msg at 230 Mmsg/s, with a documented optimization ladder from mutex to cached-opponent atomics.
- **Order-entry protocol**: fixed 64-byte binary frames, explicit little-endian codec, ring-buffer frame parser, session validation.
- **Epoll gateway**: non-blocking multi-client TCP server, two-thread architecture with SPSC command/response queues and eventfd notification.

The matching thread intentionally runs a stub. Wiring a real order book belongs in a future integration layer and is out of scope for this repository.
