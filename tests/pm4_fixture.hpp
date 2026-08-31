#pragma once

#include "nyxora/gpu/pm4.hpp"

#include <cstdint>

namespace nyxora::test {

constexpr std::uint32_t pm4_packet3(gpu::pm4::Type3Opcode opcode, std::uint32_t payload_words,
                                    std::uint32_t flags = 0) {
    return (3U << 30U) | ((payload_words - 1U) << 16U) |
           (static_cast<std::uint32_t>(opcode) << 8U) | flags;
}

} // namespace nyxora::test
