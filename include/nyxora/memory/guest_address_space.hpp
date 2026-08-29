#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <utility>

#include "nyxora/base/types.hpp"
#include "nyxora/memory/native_arena.hpp"
#include "nyxora/memory/protection.hpp"

namespace nyxora::memory {

struct RegionInfo {
    GuestAddress base{};
    GuestSize size{};
    Protection protection{Protection::none};
    std::string name;
};

class GuestAddressSpace {
public:
    GuestAddressSpace() = default;
    GuestAddressSpace(const GuestAddressSpace&) = delete;
    GuestAddressSpace& operator=(const GuestAddressSpace&) = delete;
    GuestAddressSpace(GuestAddressSpace&&) noexcept = default;
    GuestAddressSpace& operator=(GuestAddressSpace&&) noexcept = default;

    [[nodiscard]] static std::optional<GuestAddressSpace>
    reserve_native(GuestSize size, GuestAddress preferred_base = 0);

    [[nodiscard]] bool native_backed() const noexcept { return native_.has_value(); }
    [[nodiscard]] GuestAddress native_base() const noexcept {
        return native_ ? native_->base() : GuestAddress{0};
    }
    [[nodiscard]] GuestSize native_size() const noexcept {
        return native_ ? native_->size() : GuestSize{0};
    }

    bool map(GuestAddress base, GuestSize size, Protection protection, std::string name);
    bool unmap(GuestAddress base, GuestSize size);
    bool protect(GuestAddress base, GuestSize size, Protection protection);
    bool protect_range(GuestAddress base, GuestSize size, Protection protection);
    bool write(GuestAddress address, std::span<const std::byte> bytes);
    bool patch(GuestAddress address, std::span<const std::byte> bytes);
    bool flush_instruction_cache(GuestAddress address, GuestSize size) noexcept;
    bool zero(GuestAddress address, GuestSize size);

    [[nodiscard]] std::span<const std::byte> view(GuestAddress address, GuestSize size) const;
    [[nodiscard]] const RegionInfo* find(GuestAddress address) const noexcept;
    [[nodiscard]] std::vector<RegionInfo> regions() const;

private:
    struct Region {
        RegionInfo info;
        std::vector<std::byte> storage;
    };

    explicit GuestAddressSpace(NativeArena native) : native_(std::move(native)) {}
    [[nodiscard]] std::map<GuestAddress, Region>::iterator find_region(GuestAddress address);
    [[nodiscard]] std::map<GuestAddress, Region>::const_iterator find_region(GuestAddress address) const;
    [[nodiscard]] std::optional<GuestSize> native_offset(GuestAddress address) const noexcept;

    std::map<GuestAddress, Region> regions_;
    std::optional<NativeArena> native_;
};

} // namespace nyxora::memory
