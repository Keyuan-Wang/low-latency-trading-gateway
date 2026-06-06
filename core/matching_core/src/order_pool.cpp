# include "matching/order_pool.hpp"
#include "matching/types.hpp"
#include <cassert>


namespace matching {

OrderPool::OrderPool(std::size_t capacity) : pool_(capacity) {
    for (auto& o : pool_) {
        o.next = free_head_;
        free_head_ = &o;
    }
}

std::size_t OrderPool::capacity() const noexcept {
    return pool_.size();
}

}