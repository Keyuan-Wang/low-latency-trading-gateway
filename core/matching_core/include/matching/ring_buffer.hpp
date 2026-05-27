#pragma once

#include "matching/order_book.hpp"
#include "types.hpp"
#include "absl/container/flat_hash_map.h"

#include <limits>
#include <memory>


namespace matching {

template <bool IsAsk>
class RingBuffer {
private:
    static constexpr int RingSize = 32;     // size of ring buffer

    // Note the price must be signed 64 int, for modulo taking
    static constexpr std::int64_t Invalid = std::numeric_limits<std::int64_t>::min();    // invalid price

    struct Slot {
        std::int64_t price = Invalid;
        PriceLevel level;  // pointer to intrusive list with price
    };

    std::array<Slot, RingSize> ring_buffer_;    // light-weight order book on hot path

    absl::flat_hash_map<std::int64_t, PriceLevel> cold_map_;  // heavy-weight order book on cold path

    std::int64_t best_price_ = Invalid;
    std::size_t best_price_idx_ = 15;

    inline std::size_t calc_idx(std::int64_t price) const {
        return (best_price_idx_ + static_cast<std::size_t>(price - best_price_)) & (RingSize - 1);
    };

    // is a better than b?
    inline bool better(std::int64_t a, std::int64_t b) const {
        if constexpr (IsAsk)    return a < b;       // for ask book, the lower price the better
        else                    return a > b;       // for bid book, the higher price the better
    }

public:
    PriceLevel* find(std::int64_t price);

    void insert(std::int64_t price, Order* order);

    void erase(std::int64_t price);

    std::int64_t best_price() { return best_price_; }
    bool empty() const { return best_price_ == Invalid; }

    PriceLevel* best_price_level() {
        return (best_price_ == Invalid) ? nullptr : &ring_buffer_[best_price_idx_].level;
    }
};

template <bool IsAsk>
PriceLevel* RingBuffer<IsAsk>::find(std::int64_t price) {
    // the book has not been initialized yet
    if (best_price_ == Invalid)     return nullptr;

    std::size_t idx = calc_idx(price);
    auto& slot = ring_buffer_[idx];

    // hot ring hit
    if (slot.price == price)
        return &(slot.level);
    
    // hot ring hit but outdated data, move it to cold map
    if (!slot.price.empty()) {
        cold_map_[slot.price] = slot.level;
        slot.price = Invalid;
        // How to set slot.level to invalid?
    }

    // reaching here means current slot is empty (slot.price == Invalid)
    auto it = cold_map_.find(price);
    if (it == cold_map_.end())  return nullptr;     // current price does not exist
    
    // move pricelevel from cold map to hot array
    slot.level = it->second;
    cold_map_.erase(it);
    slot.price = price;

    return &(slot.level);
}

}