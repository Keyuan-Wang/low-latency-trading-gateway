#include "matching/pricelevel_pool.hpp"

#include <cassert>

namespace matching {

PriceLevelPool::PriceLevelPool(std::size_t capacity) {
    pool_.resize(capacity);

    Slot* p = nullptr;
    for (auto& slot : pool_) {
        slot.next = p;
        free_head_ = &slot;

        p = &slot;
    }
}

}   // namespace matching