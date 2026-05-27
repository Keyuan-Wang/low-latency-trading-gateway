#pragma once

#include "matching/order_book.hpp"
#include "types.hpp"
#include "intrusive_list.hpp"
#include "absl/container/flat_hash_map.h"
#include <limits>
#include <memory>


namespace matching {

template <bool IsAsk>
class RingBook {
private:
    static constexpr int RingSize = 32;     // size of ring buffer

    static constexpr std::uint64_t Invalid = std::numeric_limits<std::uint64_t>::min();    // invalid price

    struct Slot {
        std::uint64_t price = Invalid;
        std::unique_ptr<PriceLevel> level;  // pointer to intrusive list with price
    };

    std::array<Slot, RingSize> ring_buffer_;    // light-weight order book on hot path

    absl::flat_hash_map<std::uint64_t, std::unique_ptr<PriceLevel>> cold_map_;  // heavy-weight order book on cold path

    std::uint64_t best_price_ = Invalid;
    std::uint64_t best_price_idx_ = 15;

    // is a better than b?
    bool better(std::uint64_t a, std::uint64_t b) const {
        if constexpr (IsAsk)    return a < b;       // for ask book, the lower price the better
        else                    return a > b;       // for bid book, the higher price the better
    }
};
}