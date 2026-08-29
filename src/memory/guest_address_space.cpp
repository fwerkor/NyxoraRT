#include "nyxora/memory/guest_address_space.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace nyxora::memory {
namespace {
bool end_address(GuestAddress base, GuestSize size, GuestAddress& end) {
    if (size == 0 || base > std::numeric_limits<GuestAddress>::max() - size) {
        return false;
    }
    end = base + size;
    return true;
}
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
    region.storage.resize(static_cast<std::size_t>(size));
    regions_.emplace(base, std::move(region));
    return true;
}

bool GuestAddressSpace::unmap(GuestAddress base, GuestSize size) {
    const auto it = regions_.find(base);
    if (it == regions_.end() || it->second.info.size != size) {
        return false;
    }
    regions_.erase(it);
    return true;
}

bool GuestAddressSpace::protect(GuestAddress base, GuestSize size, Protection protection) {
    const auto it = regions_.find(base);
    if (it == regions_.end() || it->second.info.size != size) {
        return false;
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
    std::memcpy(it->second.storage.data() + offset, bytes.data(), bytes.size());
    return true;
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
