#pragma once

#include <cstdint>

namespace nyxora::memory {

enum class Protection : std::uint8_t {
    none = 0,
    read = 1,
    write = 2,
    execute = 4,
};

constexpr Protection operator|(Protection lhs, Protection rhs) noexcept {
    return static_cast<Protection>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
}

constexpr bool has(Protection value, Protection flag) noexcept {
    return (static_cast<unsigned>(value) & static_cast<unsigned>(flag)) != 0;
}

} // namespace nyxora::memory
