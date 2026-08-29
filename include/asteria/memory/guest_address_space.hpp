#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "asteria/base/types.hpp"

namespace asteria::memory {

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

struct RegionInfo {
    GuestAddress base{};
    GuestSize size{};
    Protection protection{Protection::none};
    std::string name;
};

class GuestAddressSpace {
public:
    bool map(GuestAddress base, GuestSize size, Protection protection, std::string name);
    bool unmap(GuestAddress base, GuestSize size);
    bool protect(GuestAddress base, GuestSize size, Protection protection);
    bool write(GuestAddress address, std::span<const std::byte> bytes);
    bool zero(GuestAddress address, GuestSize size);

    [[nodiscard]] std::span<const std::byte> view(GuestAddress address, GuestSize size) const;
    [[nodiscard]] const RegionInfo* find(GuestAddress address) const noexcept;
    [[nodiscard]] std::vector<RegionInfo> regions() const;

private:
    struct Region {
        RegionInfo info;
        std::vector<std::byte> storage;
    };

    [[nodiscard]] std::map<GuestAddress, Region>::iterator find_region(GuestAddress address);
    [[nodiscard]] std::map<GuestAddress, Region>::const_iterator find_region(GuestAddress address) const;
    std::map<GuestAddress, Region> regions_;
};

} // namespace asteria::memory
