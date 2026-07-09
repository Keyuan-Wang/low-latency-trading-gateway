/**
 * @file order_book_impl.hpp
 * @brief Template implementation of @ref llmes::matching_core::OrderBook.
 *
 * @note Included at the end of @ref order_book.hpp; do not include directly.
 */


#include <cassert>


#ifndef LLMES_ORDER_BOOK_IMPL_INCLUDED
#include "order_book.hpp"
#endif

namespace llmes::matching_core {


namespace {

/**
 * @brief Whether a limit order crosses the opposite side's best quote.
 *
 * @param taker_side            Side of the incoming limit order.
 * @param limit_price           Limit price of the taker.
 * @param best_opposite_price   Best price on the opposite book (lowest ask or highest bid).
 * @return True if at least one share can match at @p best_opposite_price.
 */
template <Side S>
inline bool can_cross_limit(std::int64_t limit_price, std::int64_t best_opposite_price) {
    if constexpr (S == Side::Sell)  return limit_price <= best_opposite_price;  // ask book
    else                            return limit_price >= best_opposite_price;  // bid book
}

}  // namespace

/**
 * @copydoc OrderBook::cancel_order
 */
template <TradeSink Sink>
ErrorCode OrderBook<Sink>::cancel_order(std::uint64_t order_id) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) {
        return ErrorCode::UnknownOrderId;
    }

    Order* o = it->second;
    order_index_.erase(it);
    o->parent_level->erase(*o);
    pool_.release(o);

    return ErrorCode::Success;
}

/**
 * @copydoc OrderBook::modify_order
 */
template <TradeSink Sink>
AddResult OrderBook<Sink>::modify_order(std::uint64_t order_id, Side side, std::int64_t price,
                                        std::uint64_t quantity, std::uint64_t timestamp) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) {
        AddResult out{};
        out.initial_quantity = quantity;
        out.code = ErrorCode::UnknownOrderId;
        return out;
    }

    Order* o = it->second;
    order_index_.erase(it);
    o->parent_level->erase(*o);
    pool_.release(o);

    return add_limit_order(order_id, side, price, quantity, timestamp);
}

/**
 * @copydoc OrderBook::add_limit_order
 */
template <TradeSink Sink>
AddResult OrderBook<Sink>::add_limit_order(std::uint64_t order_id, Side side, std::int64_t price,
                                           std::uint64_t quantity, std::uint64_t timestamp) {
    AddResult out{};
    out.initial_quantity = quantity;

    if (quantity == 0) {
        out.code = ErrorCode::InvalidQuantity;
        return out;
    }

    if (order_index_.find(order_id) != order_index_.end()) {
        out.code = ErrorCode::DuplicateOrderId;
        return out;
    }

    if (side == Side::Buy) {
        out.remaining_quantity = matching_engine_limit<Side::Buy>(out, order_id, price, quantity);
    } else {
        out.remaining_quantity = matching_engine_limit<Side::Sell>(out, order_id, price, quantity);
    }

    if (out.remaining_quantity == 0) {
        out.code = ErrorCode::Success;
        return out;
    }

    // add remaining limit order to book
    Order* node = pool_.acquire();
    // TODO: WHAT IF POOL IS ALREADY EMPTY?
    assert(node != nullptr);

    // set node
    node->id = order_id;
    node->price = price;
    node->quantity = out.remaining_quantity;
    node->timestamp = timestamp;
    node->prev = nullptr;
    node->next = nullptr;

    PriceLevel* level = nullptr;
    if (side == Side::Buy) {
        level = bids_.get_or_create(price);
    } else {
        level = asks_.get_or_create(price);
    }

    level->push_back(*node);
    node->parent_level = level;

    // output
    out.code = ErrorCode::Success;
    order_index_[order_id] = node;
    return out;
}

/**
 * @copydoc OrderBook::add_market_order
 */
template <TradeSink Sink>
AddResult OrderBook<Sink>::add_market_order(std::uint64_t order_id, Side side, std::uint64_t quantity,
                                            std::uint64_t timestamp) {

    AddResult out{};
    out.initial_quantity = quantity;

    if (quantity == 0) {
        out.code = ErrorCode::InvalidQuantity;
        return out;
    }

    if (order_index_.find(order_id) != order_index_.end()) {
        out.code = ErrorCode::DuplicateOrderId;
        return out;
    }

    if (side == Side::Buy) {
        out.remaining_quantity = matching_engine_market<Side::Buy>(out, order_id, quantity);
    } else {
        out.remaining_quantity = matching_engine_market<Side::Sell>(out, order_id, quantity);
    }

    if (out.remaining_quantity == 0) {
        out.code = ErrorCode::Success;
        return out;
    }

    out.code = ErrorCode::MarketRemainderCancelled;
    return out;
}

template<TradeSink Sink>
template <Side S>
std::uint64_t OrderBook<Sink>::matching_engine_limit(AddResult& out,
                                                     std::uint64_t order_id,
                                                     std::int64_t price,
                                                     std::uint64_t quantity) {
    // Consume opposite-side liquidity while the limit price permits crossing.
    auto& oppo_book = opposite_book<S>();
    while (quantity > 0 && !oppo_book.empty()) {
        const std::int64_t best_price = oppo_book.best_price();
        if (!can_cross_limit<S>(price, best_price)) {
            break;
        }

        auto& price_level = oppo_book.best_level();

        while (quantity > 0 && !price_level.empty()) {
            Order& maker = price_level.front();

            const std::uint64_t fill = std::min(quantity, maker.quantity);

            sink_.push_trade(order_id, maker.id, maker.price, fill);

            maker.quantity -= fill;
            quantity -= fill;
            out.filled_quantity += fill;

            if (maker.quantity == 0) {
                Order* maker_ptr = &maker;
                order_index_.erase(maker.id);
                price_level.erase(*maker_ptr);
                pool_.release(maker_ptr);
            }
        }

        if (price_level.empty())
            oppo_book.erase_best();
    }

    return quantity;
}

template <TradeSink Sink>
template <Side S>
std::uint64_t OrderBook<Sink>::matching_engine_market(AddResult& out, std::uint64_t order_id, std::uint64_t quantity) {
    auto& oppo_book = opposite_book<S>();
    while (quantity > 0 && !oppo_book.empty()) {
        auto& price_level = oppo_book.best_level();

        while (quantity > 0 && !price_level.empty()) {
            Order& maker = price_level.front();

            const std::uint64_t fill = std::min(quantity, maker.quantity);

            sink_.push_trade(order_id, maker.id, maker.price, fill);

            maker.quantity -= fill;
            quantity -= fill;
            out.filled_quantity += fill;

            if (maker.quantity == 0) {
                Order* maker_ptr = &maker;
                order_index_.erase(maker.id);
                price_level.erase(*maker_ptr);
                pool_.release(maker_ptr);
            }
        }

        if (price_level.empty())
            oppo_book.erase_best();
    }

    return quantity;
}

}  // namespace llmes::matching_core
