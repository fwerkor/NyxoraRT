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

bool GuestAddressSpace::unmap_range(GuestAddress base, GuestSize size) {
    GuestAddress end{};
    if (!end_address(base, size, end)) {
        return false;
    }

    struct Removal {
        GuestAddress region_base{};
        GuestAddress overlap_base{};
        GuestSize overlap_size{};
        Protection protection{Protection::none};
    };
    std::vector<Removal> removals;
    std::map<GuestAddress, Region> prepared;

    auto prepare_overlap = [&](const Region& original, GuestAddress overlap_base,
                               GuestAddress overlap_end) {
        removals.push_back(Removal{original.info.base, overlap_base,
                                   overlap_end - overlap_base, original.info.protection});
        const auto original_end = original.info.base + original.info.size;
        if (overlap_base > original.info.base) {
            Region before;
            before.info = RegionInfo{original.info.base, overlap_base - original.info.base,
                                     original.info.protection, original.info.name};
            if (!native_) {
                before.storage.assign(original.storage.begin(),
                                      original.storage.begin() +
                                          static_cast<std::ptrdiff_t>(before.info.size));
            }
            prepared.emplace(before.info.base, std::move(before));
        }
        if (overlap_end < original_end) {
            Region after;
            after.info = RegionInfo{overlap_end, original_end - overlap_end,
                                    original.info.protection, original.info.name};
            if (!native_) {
                const auto storage_offset = overlap_end - original.info.base;
                after.storage.assign(
                    original.storage.begin() + static_cast<std::ptrdiff_t>(storage_offset),
                    original.storage.end());
            }
            prepared.emplace(after.info.base, std::move(after));
        }
    };

    try {
        auto containing = find_region(base);
        if (containing != regions_.end() && containing->first < base) {
            const auto& original = containing->second;
            const auto overlap_end = std::min(end, original.info.base + original.info.size);
            if (base < overlap_end) {
                prepare_overlap(original, base, overlap_end);
            }
        }
        for (auto it = regions_.lower_bound(base); it != regions_.end() && it->first < end; ++it) {
            const auto& original = it->second;
            const auto overlap_base = std::max(base, original.info.base);
            const auto overlap_end = std::min(end, original.info.base + original.info.size);
            if (overlap_base < overlap_end) {
                prepare_overlap(original, overlap_base, overlap_end);
            }
        }
    } catch (const std::bad_alloc&) {
        return false;
    }

    if (native_) {
        const auto page = static_cast<GuestSize>(NativeArena::page_size());
        for (const auto& removal : removals) {
            const auto offset = native_offset(removal.overlap_base);
            if (!offset || removal.overlap_base % page != 0 || removal.overlap_size % page != 0) {
                return false;
            }
        }
        std::size_t applied = 0;
        for (; applied < removals.size(); ++applied) {
            const auto& removal = removals[applied];
            const auto offset = *native_offset(removal.overlap_base);
            if (!native_->protect(offset, removal.overlap_size, Protection::none)) {
                while (applied != 0) {
                    --applied;
                    const auto& restore = removals[applied];
                    const auto restore_offset = *native_offset(restore.overlap_base);
                    (void)native_->protect(restore_offset, restore.overlap_size, restore.protection);
                }
                return false;
            }
        }
    }

    for (const auto& removal : removals) {
        regions_.erase(removal.region_base);
    }
    regions_.merge(prepared);
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


bool GuestAddressSpace::protect_range(GuestAddress base, GuestSize size, Protection protection) {
    GuestAddress end{};
    if (!end_address(base, size, end)) {
        return false;
    }
    auto it = find_region(base);
    if (it == regions_.end()) {
        return false;
    }
    const auto original_base = it->second.info.base;
    const auto original_size = it->second.info.size;
    if (original_base > std::numeric_limits<GuestAddress>::max() - original_size ||
        end > original_base + original_size) {
        return false;
    }
    if (base == original_base && size == original_size) {
        return protect(base, size, protection);
    }

    const auto before_size = base - original_base;
    const auto after_size = original_base + original_size - end;
    std::map<GuestAddress, Region> replacements;
    const auto make_region = [&](GuestAddress piece_base, GuestSize piece_size,
                                 Protection piece_protection, GuestSize storage_offset) {
        Region piece;
        piece.info = RegionInfo{piece_base, piece_size, piece_protection, it->second.info.name};
        if (!native_) {
            const auto begin =
                it->second.storage.begin() + static_cast<std::ptrdiff_t>(storage_offset);
            piece.storage.assign(begin, begin + static_cast<std::ptrdiff_t>(piece_size));
        }
        replacements.emplace(piece_base, std::move(piece));
    };

    if (before_size != 0) {
        make_region(original_base, before_size, it->second.info.protection, 0);
    }
    make_region(base, size, protection, before_size);
    if (after_size != 0) {
        make_region(end, after_size, it->second.info.protection, before_size + size);
    }

    if (native_) {
        const auto offset = native_offset(base);
        const auto page = static_cast<GuestSize>(NativeArena::page_size());
        if (!offset || size % page != 0 || !native_->protect(*offset, size, protection)) {
            return false;
        }
    }

    regions_.erase(it);
    regions_.merge(replacements);
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

bool GuestAddressSpace::flush_instruction_cache(GuestAddress address, GuestSize size) noexcept {
    if (size == 0) {
        return true;
    }
    const auto it = find_region(address);
    if (it == regions_.end()) {
        return false;
    }
    const auto offset = address - it->second.info.base;
    if (size > it->second.info.size - offset) {
        return false;
    }
    if (!native_) {
        return true;
    }
    const auto arena_offset = native_offset(address);
    return arena_offset && native_->flush_instruction_cache(*arena_offset, size);
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
