#pragma once

#include "types.hpp"

#include <vector>


namespace matching {

template <bool IsAsk>
class ArraySideBook {
public:
    ArraySideBook();

    bool empty() noexcept;
    std::int64_t best_price() noexcept;
    PriceLevel& best_level() noexcept;
    PriceLevel* get_or_create(std::uint64_t price) noexcept;
    void erase_best() noexcept;

private:
    std::uint64_t base_price;
    std::vector<PriceLevel> levels_;
    

};

}   // namespace matching