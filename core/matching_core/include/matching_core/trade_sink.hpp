#pragma once

#include "types.hpp"
#include <concepts>

namespace llmes::matching_core {


// define a concept EventSink, for unified API
template <typename Sink>
concept TradeSink = requires(Sink sink,
                             std::vector<Trade>& out,
                             std::uint64_t taker_id,
                             std::uint64_t maker_id,
                             std::int64_t price,
                             std::uint64_t quantity) {
    { sink.push_trade(taker_id, maker_id, price, quantity) } -> std::same_as<void>;
    { sink.pop_trade(out) } -> std::same_as<void>;
};





struct NullTradeSink {
    [[gnu::always_inline]] inline void push_trade(std::uint64_t taker_id,
                                                  std::uint64_t maker_id,
                                                  std::int64_t price,
                                                  std::uint64_t quantity) noexcept {}
    void pop_trade(std::vector<Trade>& out) noexcept {}
};


class VectorTradeSink {
public:
    explicit VectorTradeSink(std::vector<Trade>& trades) noexcept
        : trades_(trades) {}

    [[gnu::always_inline]] inline void push_trade(std::uint64_t taker_id,
                                                  std::uint64_t maker_id,
                                                  std::int64_t price,
                                                  std::uint64_t quantity) {
        trades_.emplace_back(taker_id, maker_id, price, quantity);
    }

    [[gnu::always_inline]] inline void pop_trade(std::vector<Trade>& out) {
        out.swap(trades_);
    }

private:
    std::vector<Trade>& trades_;
};


template <typename Queue>
class SpscTradeSink {
public:
    explicit SpscTradeSink(Queue& queue) noexcept : queue_(queue) {}

    [[gnu::always_inline]] inline void push_trade(std::uint64_t taker_id,
                                                  std::uint64_t maker_id,
                                                  std::int64_t price,
                                                  std::uint64_t quantity) {
        while(!queue_.push(Trade{taker_id, maker_id, price, quantity})) {
        };
    }

    [[gnu::always_inline]] inline void emplace_trade(std::uint64_t taker_id,
                                                     std::uint64_t maker_id,
                                                     std::int64_t price,
                                                     std::uint64_t quantity) {
        while(!queue_.emplace(taker_id, maker_id, price, quantity)) {
        };
    }

    [[gnu::always_inline]] inline void pop_trade(std::vector<Trade>& out) {
        Trade trade;
        while (queue_.pop(trade)) {
            out.push_back(trade);
        }
    }

private:
    Queue& queue_;
};


}