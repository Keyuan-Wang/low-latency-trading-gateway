#pragma once

/**
 * @file order_book.hpp
 * @brief Central limit order book (Phase 1): declarations for @ref llmes::matching_core::OrderBook and related types.
 */

#include <concepts>
#include <cstdint>
#include <map>
#include <utility>
#include <cstddef>  // std::byte, std::size_t
#include <unordered_map>


#include "types.hpp"
#include "price_level.hpp"
#include "order_pool.hpp"
#include "array_side_book.hpp"
#include "trade_sink.hpp"

namespace llmes::matching_core {
/**
 * @brief Price-level container for one side of the book.
 *
 * @details Phase 4 V1 deliberately keeps the storage as an ordered map.  The
 * wrapper isolates the operations OrderBook needs so later phases can replace
 * the backing container without rewriting matching logic.
 */
template <bool IsAsk>
class SideBook {
private:
    std::map<std::int64_t, PriceLevel, PriceCompare<IsAsk>> levels_{};
public:
    [[nodiscard]] [[gnu::always_inline]] bool empty() const noexcept {
        return levels_.empty();
    }

    [[nodiscard]] [[gnu::always_inline]] std::int64_t best_price() const {
        return levels_.begin()->first;
    }

    [[nodiscard]] [[gnu::always_inline]] PriceLevel& best_level() {
        return levels_.begin()->second;
    }

    [[nodiscard]] [[gnu::always_inline]] std::pair<PriceLevel*, bool> get_or_create(std::int64_t price) {
        auto [it, inserted] = levels_.try_emplace(price);
        return {&it->second, inserted};
    }

    [[gnu::always_inline]] void erase_best() {
        levels_.erase(levels_.begin());
    }
};

using AskBook = SideBook<true>;
using BidBook = SideBook<false>;

/**
 * @brief Phase-1 central limit order book (two-sided, price–time priority at each level).
 *
 * @details
 * - Bids and asks are stored in separate @ref BidBook / @ref AskBook maps.
 * - Each price maps to a @ref PriceLevel (`std::list`) for FIFO per level.
 * - Resting orders are found by business order id through an internal hash map.
 */
template <TradeSink Sink = NullTradeSink>
class OrderBook {
public:
    /** @brief Constructs an empty book with a default-initialized trade sink. */
    explicit OrderBook(std::size_t pool_capacity = 100000)
        requires std::default_initializable<Sink>
        : sink_{}, pool_(pool_capacity) {}

    /** @brief Constructs an empty book with the given trade sink. */
    explicit OrderBook(Sink sink, std::size_t pool_capacity = 100000)
        : sink_(std::move(sink)), pool_(pool_capacity) {}

    /**
     * @brief Submit a limit order: match against the opposite side, rest remainder on book.
     *
     * @param order_id   Business/reporting order id.
     * @param side       @ref Side::Buy consumes asks; @ref Side::Sell consumes bids.
     * @param price      Limit price; used for crossing check and for resting level.
     * @param quantity   Desired quantity (> 0).
     * @param timestamp  Opaque event time (stored on resting portion).
     * @return @ref AddResult with fill/rest fields set; trades are emitted via the book's sink.
     *
     * @retval ErrorCode::Success Resting portion (if any) posted; or fully filled.
     * @retval ErrorCode::InvalidQuantity @p quantity == 0.
     * @retval ErrorCode::DuplicateOrderId @p order_id is already live.
     */
    AddResult add_limit_order(std::uint64_t order_id, Side side, std::int64_t price,
                              std::uint64_t quantity, std::uint64_t timestamp);

    /**
     * @brief Submit a market order: match aggressively; do not post remainder to the book.
     *
     * @param order_id   Unique order id for this aggressive order.
     * @param side       Buy sweeps asks; sell sweeps bids.
     * @param quantity   Desired quantity (> 0).
     * @param timestamp  Unused in current implementation (reserved).
     * @return @ref AddResult; leftover sets @ref ErrorCode::MarketRemainderCancelled.
     *
     * @retval ErrorCode::Success Fully filled.
     * @retval ErrorCode::MarketRemainderCancelled Partially filled; remainder discarded.
     * @retval ErrorCode::DuplicateOrderId @p order_id is already live.
     */
    AddResult add_market_order(std::uint64_t order_id, Side side, std::uint64_t quantity,
                               std::uint64_t timestamp);

    /**
     * @brief Atomically replace a resting order addressed by order id.
     *
     * @param order_id  Business order id of the live resting order.
     * @param side      Side for the replacement order.
     * @param price     Limit price for the replacement order.
     * @param quantity  Quantity for the replacement order (> 0).
     * @param timestamp Opaque event time for the replacement order.
     * @return @ref AddResult from the replacement add, or an error result.
     */
    AddResult modify_order(std::uint64_t order_id, Side side, std::int64_t price,
                           std::uint64_t quantity, std::uint64_t timestamp);

    /**
     * @brief Remove a resting order by order id.
     *
     * @param order_id Business order id of the live resting order.
     * @return @ref ErrorCode::Success if removed from book.
     */
    ErrorCode cancel_order(std::uint64_t order_id);

private:
    ArraySideBook<false> bids_;   ///< Bid price levels (best bid at @c begin()).
    ArraySideBook<true> asks_;   ///< Ask price levels (best ask at @c begin()).

    Sink sink_;
    OrderPool pool_;
    std::unordered_map<std::uint64_t, Order*> order_index_;

    template <Side S>
    [[gnu::always_inline]] auto& opposite_book() {
        if constexpr (S == Side::Buy)   return asks_;
        else                            return bids_;
    };

    template <Side S>
    std::uint64_t matching_engine_limit(AddResult& out,
                                        std::uint64_t order_id,
                                        std::int64_t price,
                                        std::uint64_t quantity);

    template <Side S>
    std::uint64_t matching_engine_market(AddResult& out, std::uint64_t order_id, std::uint64_t quantity);
};

}  // namespace llmes::matching_core



#define LLMES_ORDER_BOOK_IMPL_INCLUDED
#include "order_book_impl.hpp"
#undef LLMES_ORDER_BOOK_IMPL_INCLUDED
