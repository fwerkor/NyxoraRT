#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace nyxora {
using GuestAddress = std::uint64_t;
using GuestSize = std::uint64_t;

[[nodiscard]] constexpr std::optional<GuestSize> checked_align_up(GuestSize value,
                                                                  GuestSize alignment) noexcept {
    if (value == 0 || alignment == 0 ||
        value > std::numeric_limits<GuestSize>::max() - (alignment - 1U)) {
        return std::nullopt;
    }
    return (value + alignment - 1U) / alignment * alignment;
}

} // namespace nyxora
