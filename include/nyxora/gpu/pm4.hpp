#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nyxora::gpu::pm4 {

enum class PacketType : std::uint8_t { type0 = 0, type1 = 1, type2 = 2, type3 = 3 };

struct PacketView {
    PacketType type{};
    std::uint32_t header{};
    std::uint16_t type0_base{};
    std::uint8_t type3_opcode{};
    bool predicate{};
    bool compute{};
    std::span<const std::uint32_t> payload;
};

class Decoder {
public:
    explicit Decoder(std::span<const std::uint32_t> stream) : stream_(stream) {}

    [[nodiscard]] bool done() const noexcept { return cursor_ == stream_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    PacketView next();

private:
    std::span<const std::uint32_t> stream_;
    std::size_t cursor_{};
};

std::vector<PacketView> decode_all(std::span<const std::uint32_t> stream);

} // namespace nyxora::gpu::pm4
