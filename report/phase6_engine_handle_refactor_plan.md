# Phase 6 Engine-Assigned Handle Refactor Plan

## Context

The Phase 5 production `perf record` run showed that the cancel index is the largest remaining cost center in the current matching engine. The current implementation uses:

```cpp
absl::flat_hash_map<std::uint64_t, Order*> id_to_order_;
```

The map serves two roles:

1. Reject duplicate `order_id` values on add.
2. Resolve cancel/modify requests in O(1) average time.

The profile showed that this structure is expensive even with `absl::flat_hash_map`: add-path `contains`, add-path `emplace`, cancel `find`, and cancel `erase` together account for roughly half of macro cycles. Earlier ideas to optimize this table in place are no longer the primary direction. The key design update is to move duplicate-id and invalid-cancel validation out of the matching core and into the exchange gateway layer.

## Revised Business Boundary

The new assumption is:

- The exchange gateway generates or validates all external order ids.
- `add_limit_order()` never receives a duplicate order id.
- `cancel_order()` and `modify_order()` never receive a nonexistent target in the normal hot path.
- `cancel-before-insert` is not a matching-core responsibility.
- The matching core may use an internal handle for cancel/modify lookup.

Under this boundary, `order_id` remains useful as business data for trade reports and audit trails, but it is no longer the lookup key inside the book.

This is closer to a production exchange architecture: the gateway owns client-facing order identity and validation; the matching core owns the minimal state needed to maintain price-time priority and execute book mutations.

## Why Hash-Table Tricks Are No Longer The Right Direction

With the old API contract, the hash table did real semantic work:

- It rejected duplicate ids before matching.
- It resolved arbitrary external ids during cancel/modify.
- It represented failed cancel/modify lookup.

If those responsibilities remain in the matching core, the hash table is difficult to improve further. `absl::flat_hash_map` already uses Swiss-table SIMD probing and prefetching. Replacing `contains + emplace` with `find + emplace_hint` does not help because Abseil's hint-taking overloads are non-binding and do not preserve the miss probe for insertion. Removing the add-path duplicate check would change semantics under the old contract because a duplicate order could match before being rejected.

With the revised business boundary, the correct move is not to optimize the hash table. The correct move is to remove it from the hot path entirely.

## Replacement: Order-Pool Handles

The replacement is an internal handle that directly identifies an order's slot in the order pool.

The current `OrderPool` is already backed by a contiguous `std::vector<Order>`. This means every live order has a dense slot index:

```text
slot_index = &order - pool.data()
```

The simplest handle is therefore:

```text
OrderHandle = slot_index
```

A production-safe version can use a generational slotmap:

```text
OrderHandle = [ generation : high bits ] [ slot_index : low bits ]
```

The performance win comes from the slot index. Cancel/modify lookup becomes one array access instead of a hash calculation plus Swiss-table probe. The generation is not the performance mechanism; it is a safety mechanism for stale handles and slot reuse.

## What Generation Solves

Generation solves the ABA problem caused by slot reuse.

Without generation:

```text
slot 42 contains order A
A is canceled and slot 42 is released
slot 42 is reused for order B
a stale cancel for A with handle=42 arrives
the engine accidentally cancels B
```

With generation:

```text
A handle = { index=42, generation=7 }
B handle = { index=42, generation=8 }
```

The stale cancel for A fails the generation check and cannot remove B.

Under the strict benchmark assumption that cancel/modify targets are always valid, generation is not required for correctness of the measured workload. It is still valuable as a low-cost safety belt for real systems, debug builds, and validation runs. The implementation can support both:

- minimal benchmark mode: `OrderHandle = slot_index`
- safety mode: `OrderHandle = {generation, slot_index}`

The first isolates the maximum performance potential of direct indexing. The second validates the engineering form likely needed in a production system.

## API Direction

The matching core should separate business ids from lookup handles.

`add_limit_order()` still receives the external/business order id:

```cpp
AddResult add_limit_order(std::uint64_t order_id,
                          Side side,
                          std::int64_t price,
                          std::uint64_t quantity,
                          std::uint64_t timestamp);
```

If the order rests, `AddResult` returns an engine handle:

```cpp
struct AddResult {
    ErrorCode code;
    std::uint64_t initial_quantity;
    std::uint64_t filled_quantity;
    std::uint64_t remaining_quantity;
    OrderHandle handle;
    std::vector<Trade> trades;
};
```

If the order fully matches and does not rest, `handle` is invalid.

Cancel and modify use the handle:

```cpp
ErrorCode cancel_order(OrderHandle handle);

AddResult modify_order(OrderHandle handle,
                       std::uint64_t new_order_id,
                       Side side,
                       std::int64_t price,
                       std::uint64_t quantity,
                       std::uint64_t timestamp);
```

The `order_id` remains in `Order` for trade reporting:

```cpp
struct Order {
    std::uint64_t id;       // business/reporting id
    std::int64_t price;
    std::uint64_t quantity;
    std::uint64_t timestamp;
    Order* prev;
    Order* next;
    IntrusiveList* parent_level;
};
```

The gateway maintains any required `order_id -> OrderHandle` mapping outside the matching hot path.

## Expected Hot-Path Changes

`add_limit_order()`:

- remove `pending_cancel_ids_.contains(order_id)`
- remove `id_to_order_.contains(order_id)`
- remove `id_to_order_.emplace(order_id, node)`
- return `out.handle` when a remainder rests
- keep `order_id` only as the business id stored in `Order.id` and used in `Trade`

`cancel_order()`:

- resolve the handle directly through `OrderPool`
- erase from the parent price level
- release the slot

`modify_order()`:

- resolve the old handle directly through `OrderPool`
- erase and release the old order
- call `add_limit_order()` for the replacement
- return the replacement handle if it rests

`add_market_order()`:

- remove duplicate-id checks
- when makers are fully filled, erase from the level and release the slot
- no hash erase

## Benchmark Implications

The benchmark must change to measure the new business contract fairly.

`bench_hft_macro` currently self-generates ids and tracks live orders by id. After the handle refactor it must track live resting orders by engine handle. The business `order_id` remains available for reporting, but cancel/modify target selection should use handles.

The planning-book design requires special care. A handle from `planning_book_` is not necessarily valid in `book_`, because each book has its own pool. There are two viable approaches:

1. Ensure deterministic slot allocation so the planning and measured books produce identical handles for the same replayed operation stream.
2. Track two handles per logical order: one for `planning_book_`, one for `book_`.

The second approach is more explicit and robust, but it requires more benchmark bookkeeping.

## Comparability With Earlier Phases

This refactor changes the business boundary of the matching core. Therefore, raw comparisons against earlier phases would not be apples-to-apples unless those phases are adapted to the same boundary.

Future work should refactor the earlier phase baselines under the same assumptions:

- no duplicate-id validation in the matching core
- no cancel-before-insert path in the matching core
- cancel/modify receive an engine-resolved target, not an arbitrary external id
- business order ids are retained only for reporting and audit output

Only after these changes can Phase 6 results be compared fairly against Phase 1/2/3/4/5 variants. The old benchmarks measured both matching and some gateway-like validation duties. The new benchmark should measure the matching engine proper.

The comparison plan should therefore include:

1. Preserve the historical results as records of the old API contract.
2. Create a new comparable benchmark lineage where all relevant baselines use the same engine-handle contract.
3. Re-run the legacy micro benchmarks and HFT macro benchmark across the refactored baselines.
4. Report old-contract and new-contract results separately.

## Proposed Implementation Sequence

1. Add `OrderHandle` and invalid-handle constants to `types.hpp`.
2. Extend `OrderPool` with slot-index access and handle generation.
3. Change `AddResult` to return a handle for resting remainders.
4. Remove `id_to_order_` and `pending_cancel_ids_` from `OrderBook`.
5. Change cancel/modify APIs to use `OrderHandle`.
6. Update unit tests for handle-based cancel/modify and stale-handle behavior.
7. Update `bench_hft_macro` to track engine handles.
8. Re-run correctness tests and smoke benchmarks.
9. Run 10-trial production PMC and window-isolated `perf record`.
10. Refactor prior phase baselines to the same handle contract for comparable performance history.

## Next Optimization After Handles

Once the hash-map cancel index is removed, the next major cost center remains the price-level container:

- `std::map::get_or_create`
- RB-tree lower_bound traversal
- level insertion / erase rebalancing
- allocator calls for level create/destroy

The first follow-up should be a pooled allocator for `std::map` nodes, because it preserves pointer stability (`Order::parent_level`) while attacking allocator churn. Only after measuring that should the project revisit a structural price-level replacement such as `absl::btree_map`, hot contiguous storage, or a hot/cold hybrid.

## Working Rule

Phase 6 is not a micro-optimization of `absl::flat_hash_map`. It is a business-boundary refactor: move id validation to the gateway layer and let the matching core operate on engine handles. The target is to remove the cancel-index hash map from the hot path entirely. After that, re-profile and optimize the price-level container.
