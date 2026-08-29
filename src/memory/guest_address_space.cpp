#include "nyxora/memory/guest_address_space.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace nyxora::memory {
namespace {

bool end_address(GuestAddress base, GuestSize size, GuestAddress& end) {
    if (size == 0 || base > std::numeric_limits<GuestAddress>::max() - size) {
        return false;
    }
    end = base + size;
    return true;
}

} // namespace

std::optional<GuestAddressSpace> GuestAddressSpace::reserve_native(GuestSize size,
                                                                   GuestAddress preferred_base) {
    auto arena = NativeArena::reserve(size, preferred_base);
    if (!arena) {
        return std::nullopt;
    }
    return GuestAddressSpace(std::move(*arena));
}

std::optional<GuestSize> GuestAddressSpace::native_offset(GuestAddress address) const noexcept {
    if (!native_ || address < native_->base()) {
        return std::nullopt;
    }
    const auto offset = address - native_->base();
    return offset <= native_->size() ? std::optional<GuestSize>{offset} : std::nullopt;
}

bool GuestAddressSpace::map(GuestAddress base, GuestSize size, Protection protection,
                            std::string name) {
    GuestAddress end{};
    if (!end_address(base, size, end) || size > std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    auto next = regions_.lower_bound(base);
    if (next != regions_.end() && next->first < end) {
        return false;
    }
    if (next != regions_.begin()) {
        const auto& previous = std::prev(next)->second.info;
        if (previous.base + previous.size > base) {
            return false;
        }
    }

    Region region;
    region.info = RegionInfo{base, size, protection, std::move(name)};
    if (native_) {
        const auto offset = native_offset(base);
        if (!offset || size > native_->size() - *offset || !native_->protect(*offset, size, protection)) {
            return false;
        }
    } else {
        region.storage.resize(static_cast<std::size_t>(size));
    }
    regions_.emplace(base, std::move(region));
    return true;
}

bool GuestAddressSpace::unmap(GuestAddress base, GuestSize size) {
    const auto it = regions_.find(base);
    if (it == regions_.end() || it->second.info.size != size) {
        return false;
    }
    if (native_) {
        const auto offset = native_offset(base);
        if (!offset || !native_->protect(*offset, size, Protection::none)) {
            return false;
        }
    }
    regions_.erase(it);
    return true;
}

bool GuestAddressSpace::protect(GuestAddress base, GuestSize size, Protection protection) {
    const auto it = regions_.find(base);
    if (it == regions_.end() || it->second.info.size != size) {
        return false;
    }
    if (native_) {
        const auto offset = native_offset(base);
        if (!offset || !native_->protect(*offset, size, protection)) {
            return false;
        }
    }
    it->second.info.protection = protection;
    return true;
}

std::map<GuestAddress, GuestAddressSpace::Region>::iterator
GuestAddressSpace::find_region(GuestAddress address) {
    auto it = regions_.upper_bound(address);
    if (it == regions_.begin()) {
        return regions_.end();
    }
    --it;
    return address < it->second.info.base + it->second.info.size ? it : regions_.end();
}

std::map<GuestAddress, GuestAddressSpace::Region>::const_iterator
GuestAddressSpace::find_region(GuestAddress address) const {
    auto it = regions_.upper_bound(address);
    if (it == regions_.begin()) {
        return regions_.end();
    }
    --it;
    return address < it->second.info.base + it->second.info.size ? it : regions_.end();
}

bool GuestAddressSpace::write(GuestAddress address, std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return true;
    }
    auto it = find_region(address);
    if (it == regions_.end() || !has(it->second.info.protection, Protection::write)) {
        return false;
    }
    const auto offset = address - it->second.info.base;
    if (bytes.size() > it->second.info.size - offset) {
        return false;
    }
    if (native_) {
        const auto arena_offset = native_offset(address);
        return arena_offset && native_->copy(*arena_offset, bytes);
    }
    std::memcpy(it->second.storage.data() + offset, bytes.data(), bytes.size());
    return true;
}

bool GuestAddressSpace::patch(GuestAddress address, std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return true;
    }
    auto it = find_region(address);
    if (it == regions_.end()) {
        return false;
    }
    const auto offset = address - it->second.info.base;
    if (bytes.size() > it->second.info.size - offset) {
        return false;
    }
    if (!native_) {
        std::memcpy(it->second.storage.data() + offset, bytes.data(), bytes.size());
        return true;
    }

    const auto region_offset = native_offset(it->second.info.base);
    const auto patch_offset = native_offset(address);
    if (!region_offset || !patch_offset) {
        return false;
    }
    const auto original = it->second.info.protection;
    if (!has(original, Protection::write)) {
        if (!native_->protect(*region_offset, it->second.info.size,
                              Protection::read | Protection::write)) {
            return false;
        }
    }
    const bool copied = native_->copy(*patch_offset, bytes);
    if (!has(original, Protection::write) &&
        !native_->protect(*region_offset, it->second.info.size, original)) {
        return false;
    }
    return copied;
}

bool GuestAddressSpace::zero(GuestAddress address, GuestSize size) {
    auto it = find_region(address);
    if (it == regions_.end() || !has(it->second.info.protection, Protection::write)) {
        return false;
    }
    const auto offset = address - it->second.info.base;
    if (size > it->second.info.size - offset) {
        return false;
    }
    if (native_) {
        const auto arena_offset = native_offset(address);
        if (!arena_offset) {
            return false;
        }
        std::memset(native_->host_pointer(*arena_offset), 0, static_cast<std::size_t>(size));
        return true;
    }
    std::fill_n(it->second.storage.begin() + static_cast<std::ptrdiff_t>(offset),
                static_cast<std::size_t>(size), std::byte{0});
    return true;
}

std::span<const std::byte> GuestAddressSpace::view(GuestAddress address, GuestSize size) const {
    const auto it = find_region(address);
    if (it == regions_.end()) {
        return {};
    }
    const auto offset = address - it->second.info.base;
    if (size > it->second.info.size - offset) {
        return {};
    }
    if (native_) {
        const auto arena_offset = native_offset(address);
        if (!arena_offset) {
            return {};
        }
        return {static_cast<const std::byte*>(native_->host_pointer(*arena_offset)),
                static_cast<std::size_t>(size)};
    }
    return {it->second.storage.data() + offset, static_cast<std::size_t>(size)};
}

const RegionInfo* GuestAddressSpace::find(GuestAddress address) const noexcept {
    const auto it = find_region(address);
    return it == regions_.end() ? nullptr : &it->second.info;
}

std::vector<RegionInfo> GuestAddressSpace::regions() const {
    std::vector<RegionInfo> result;
    result.reserve(regions_.size());
    for (const auto& [_, region] : regions_) {
        result.push_back(region.info);
    }
    return result;
}

} // namespace nyxora::memory
