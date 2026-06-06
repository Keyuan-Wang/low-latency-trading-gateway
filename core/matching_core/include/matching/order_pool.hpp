#pragma once

#include <cstddef>
#include <vector>

#include "types.hpp"

namespace matching {

class OrderPool {
private:
    std::vector<Order> pool_;   // Note: the pool_ should not be resize during running
    Order* free_head_ = nullptr;

public:
    explicit OrderPool(std::size_t capacity);

    [[nodiscard]] [[gnu::always_inline]] OrderHandle acquire();  // Returns kInvalidHandle if the pool is full.
    void    release(Order* o);    // return a freed slot back to top of stack
    [[nodiscard]] Order* resolve(OrderHandle h) noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept;

    // return the index of order in the pool_
    [[nodiscard]] inline std::size_t index_of(const Order* o) const noexcept { return o - pool_.data(); };
    // return the order pointer at pool_[idx]
    inline Order* at(std::size_t idx) noexcept { return &pool_[idx]; };
};

}