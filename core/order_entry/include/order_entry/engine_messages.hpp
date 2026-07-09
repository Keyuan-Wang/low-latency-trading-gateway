#pragma once

#include "order_entry/protocol.hpp"

#include <cstdint>
#include <type_traits>


namespace llmes::order_entry {

struct SessionToken {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;

    friend constexpr bool operator==(const SessionToken&, const SessionToken&) = default;
};

// msg types to the matching engine
enum class EngineCommandType : std::uint8_t {
    NewLimit,
    Cancel,
    Modify,
    SessionClosed,
    Shutdown,
};

struct EngineCommand {
    std::uint64_t request_sequence      = 0;
    std::uint64_t client_order_id       = 0;
    std::uint64_t price                 = 0;
    std::uint64_t quantity              = 0;

    Side side                           = Side::Buy;
    SessionToken session{};
    EngineCommandType type              = EngineCommandType::NewLimit;
};

enum class EngineResponseType : std::uint8_t {
    Accepted,
    Rejected,
    Cancelled,
    Modified,
    Trade,
};


struct EngineResponse {
    std::uint64_t client_order_id       = 0;
    std::uint64_t price                 = 0;
    std::uint64_t quantity              = 0;

    RejectReason reject_reason          = RejectReason::None;
    SessionToken session{};
    EngineResponseType type             = EngineResponseType::Rejected;
};

static_assert(std::is_trivially_copyable_v<SessionToken>);
static_assert(std::is_trivially_copyable_v<EngineCommand>);
static_assert(std::is_trivially_copyable_v<EngineResponse>);


}   // namespace llmes::order_entry
