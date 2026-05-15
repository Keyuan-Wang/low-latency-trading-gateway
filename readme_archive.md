```
                        ┌─────────────────────────────┐
                        │     Market Data Feed         │
                        │ (Simulated external source)  │
                        └─────────────┬───────────────┘
                                      │ raw ticks
                                      ▼
┌─────────────────────────────────────────────────────────────────────┐
│                 MARKET DATA PIPELINE  (Module 2)                    │
│                                                                     │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────────┐   │
│  │ SPSC Ring    │───▶│ Bloom Filter │───▶│ LRU Cache           │   │
│  │ Buffer       │    │(dedup/replay │    │(unordered_map+list)  │   │
│  │ (lfutils)    │    │ protection)  │    └──────────┬───────────┘   │
│  └──────────────┘    └──────────────┘               │               │
│                                              stale detection        │
│                                         (alert if no tick for N ms) │
└──────────────────────────────────────────────────────┬──────────────┘
                                                       │ cleaned ticks
                                                       ▼
┌──────────────────────────────────────────────────────────────────────┐
│                    MATCHING CORE  (Module 1)                         │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │                  Order Book (Skip List)                      │    │
│  │                                                              │    │
│  │   Bid Side (Buy)           Ask Side (Sell)                   │    │
│  │   Price▲ (High→Low)        Price▼ (Low→High)                 │    │
│  │   ┌───┬──────┐              ┌───┬──────┐                     │    │
│  │   │100│OrderA│              │101│OrderC│ ← each price level  │    │
│  │   │   │OrderB│              │102│OrderD│   is an intrusive   │    │
│  │   ├───┼──────┤              ├───┼──────┤   linked list       │    │
│  │   │ 99│OrderE│              │103│OrderF│                     │    │
│  │   └───┴──────┘              └───┴──────┘                     │    │
│  └──────────────────────────────────────────────────────────────┘    │
│                                                                      │
│  Input: Limit Order / Market Order / Cancel Request                  │
│  Output: Trade { buyer_id, seller_id, price, qty }                   │
│  Memory: SoA layout + pmr pool (0 alloc/op on hot path)              │
└──────────────────────────────────────┬───────────────────────────────┘
                                       │ Trade event stream
                                       ▼
┌─────────────────────────────────────────────────────────────────────┐
│                   EXECUTION LAYER  (Module 3)                       │
│                                                                     │
│  ┌──────────────────┐                                               │
│  │ Event Loop       │  consumes Trade events                        │
│  │ priority_queue   │                                               │
│  │ <Event>          │                                               │
│  └────────┬─────────┘                                               │
│           ▼                                                         │
│  ┌──────────────────────────────────────────┐                       │
│  │ Strategy Dispatch (std::variant + visit) │                       │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐  │                       │
│  │  │ VWAP     │ │ TWAP     │ │ Grid     │  │                       │
│  │  │ Strategy │ │ Strategy │ │ Strategy │  │                       │
│  │  └──────────┘ └──────────┘ └──────────┘  │                       │
│  └──────────────────────────────────────────┘                       │
│           │                                                         │
│           ▼                                                         │
│  ┌──────────────┐                                                   │
│  │ Risk Throttle│  order rate > threshold → circuit breaker + kill  │
│  └──────────────┘                                                   │
└──────────────────────────────────────┬──────────────────────────────┘
                                       │ execution results
                                       ▼
┌─────────────────────────────────────────────────────────────────────┐
│                  RISK & SIMULATION  (Module 4)                      │
│                                                                     │
│  ┌──────────────────┐    ┌──────────────────┐    ┌──────────────┐   │
│  │ tslib            │───▶│ Bootstrap       │───▶│ Reservoir    │   │
│  │ rolling returns  │    │ resampling       │    │ Sampling     │   │
│  │ (Welford's algo) │    │ (Monte Carlo)    │    │(online Top-K)│   │
│  └──────────────────┘    │ → max drawdown   │    └──────────────┘   │
│                          │   distribution   │                       │
│                          └──────────────────┘                       │
└─────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════
                      Support Libraries (header-only)
═══════════════════════════════════════════════════════════════════════

  ┌─────────────────────────┐      ┌─────────────────────────┐
  │  tslib                  │      │  lfutils                │
  │  (Time-Series Library)  │      │  (Lock-Free Utilities)  │
  │                         │      │                         │
  │  • Segment Tree         │      │  • SPSC Queue           │
  │    (lazy propagation)   │      │  • Bounded Ring Buffer  │
  │  • Fenwick Tree         │      │  • Object Pool (pmr)    │
  │  • Welford online stats │      │  • Cache line isolation │
  │  • Monotonic Deque      │      │    (alignas(64))        │
  │    (sliding window)     │      │                         │
  │  • Rolling Sharpe/MDD   │      │                         │
  └─────────────────────────┘      └─────────────────────────┘
```