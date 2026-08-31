#include "nyxora/runtime/late_imports.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nyxora::runtime {
namespace {


void write_i32(std::array<std::byte, 32>& code, std::size_t offset, std::int64_t displacement) {
    if (displacement < std::numeric_limits<std::int32_t>::min() ||
        displacement > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error("late-import thunk displacement exceeds rel32 range");
    }
    const auto value = static_cast<std::int32_t>(displacement);
    std::memcpy(code.data() + offset, &value, sizeof(value));
}

std::array<std::byte, 32> make_thunk(GuestAddress code_address, GuestAddress data_address) {
    std::array<std::byte, 32> code{
        std::byte{0x4c}, std::byte{0x8b}, std::byte{0x1d}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0},                                                                      // mov r11,[rip+target]
        std::byte{0x4d}, std::byte{0x85}, std::byte{0xdb},                                 // test r11,r11
        std::byte{0x75}, std::byte{0x11},                                                   // jnz resolved
        std::byte{0x4c}, std::byte{0x8d}, std::byte{0x15}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0},                                                         // lea r10,[rip+counter]
        std::byte{0xf0}, std::byte{0x49}, std::byte{0xff}, std::byte{0x02},                // lock inc qword [r10]
        std::byte{0x31}, std::byte{0xc0},                                                   // xor eax,eax
        std::byte{0x0f}, std::byte{0x57}, std::byte{0xc0},                                 // xorps xmm0,xmm0
        std::byte{0xc3},                                                                    // ret
        std::byte{0x41}, std::byte{0xff}, std::byte{0xe3},                                 // resolved: jmp r11
    };
    write_i32(code, 3, static_cast<std::int64_t>(data_address) -
                           static_cast<std::int64_t>(code_address + 7U));
    write_i32(code, 15, static_cast<std::int64_t>(data_address + 8U) -
                            static_cast<std::int64_t>(code_address + 19U));
    return code;
}

} // namespace

bool LateImportTable::supported() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return true;
#else
    return false;
#endif
}

std::optional<LateImportTable> LateImportTable::create(std::size_t capacity) {
    if (!supported() || capacity == 0 ||
        capacity > std::numeric_limits<GuestSize>::max() / kThunkSize ||
        capacity > std::numeric_limits<GuestSize>::max() / kDataSlotSize) {
        return std::nullopt;
    }

    const auto page = static_cast<GuestSize>(memory::NativeArena::page_size());
    const auto code_size = checked_align_up(static_cast<GuestSize>(capacity * kThunkSize), page);
    const auto data_size = checked_align_up(static_cast<GuestSize>(capacity * kDataSlotSize), page);
    if (!code_size || !data_size || *code_size > std::numeric_limits<GuestSize>::max() - *data_size) {
        return std::nullopt;
    }

    auto arena = memory::NativeArena::reserve(*code_size + *data_size);
    if (!arena ||
        !arena->protect(0, *code_size, memory::Protection::read | memory::Protection::write) ||
        !arena->protect(*code_size, *data_size,
                        memory::Protection::read | memory::Protection::write)) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < capacity; ++index) {
        const auto code_address = arena->base() + index * kThunkSize;
        const auto data_address = arena->base() + *code_size + index * kDataSlotSize;
        const auto thunk = make_thunk(code_address, data_address);
        if (!arena->copy(index * kThunkSize, thunk)) {
            return std::nullopt;
        }
    }
    if (!arena->flush_instruction_cache(0, capacity * kThunkSize) ||
        !arena->protect(0, *code_size,
                        memory::Protection::read | memory::Protection::execute)) {
        return std::nullopt;
    }

    return LateImportTable(std::move(*arena), capacity, *code_size);
}

std::optional<GuestAddress> LateImportTable::get_or_create(GuestAddress patch_address,
                                                            const SymbolKey& key,
                                                            std::string symbol_name) {
    if (const auto* existing = find_by_patch(patch_address); existing != nullptr) {
        if (!(existing->key == key)) {
            return std::nullopt;
        }
        return existing->thunk_address;
    }
    if (records_.size() >= capacity_) {
        return std::nullopt;
    }

    const auto index = records_.size();
    records_.push_back(LateImportRecord{
        .index = index,
        .patch_address = patch_address,
        .thunk_address = thunk_address(index),
        .key = key,
        .symbol_name = std::move(symbol_name),
    });
    return records_.back().thunk_address;
}

bool LateImportTable::bind_patch(GuestAddress patch_address, GuestAddress target) noexcept {
    const auto* record = find_by_patch(patch_address);
    if (record == nullptr || target == 0) {
        return false;
    }
    auto* slot = target_slot(record->index);
    std::atomic_ref<std::uint64_t>(*slot).store(target, std::memory_order_release);
    return true;
}

const LateImportRecord* LateImportTable::find_by_patch(GuestAddress patch_address) const noexcept {
    for (const auto& record : records_) {
        if (record.patch_address == patch_address) {
            return &record;
        }
    }
    return nullptr;
}

std::uint64_t LateImportTable::call_count(std::size_t index) const noexcept {
    const auto* counter = counter_slot(index);
    if (counter == nullptr) {
        return 0;
    }
    return std::atomic_ref<std::uint64_t>(*const_cast<std::uint64_t*>(counter))
        .load(std::memory_order_acquire);
}

GuestAddress LateImportTable::thunk_address(std::size_t index) const noexcept {
    return index < capacity_ ? arena_.base() + index * kThunkSize : 0;
}

std::uint64_t* LateImportTable::target_slot(std::size_t index) noexcept {
    if (index >= capacity_) {
        return nullptr;
    }
    return reinterpret_cast<std::uint64_t*>(arena_.base() + data_offset_ + index * kDataSlotSize);
}

const std::uint64_t* LateImportTable::target_slot(std::size_t index) const noexcept {
    if (index >= capacity_) {
        return nullptr;
    }
    return reinterpret_cast<const std::uint64_t*>(arena_.base() + data_offset_ + index * kDataSlotSize);
}

const std::uint64_t* LateImportTable::counter_slot(std::size_t index) const noexcept {
    const auto* target = target_slot(index);
    return target == nullptr ? nullptr : target + 1;
}

} // namespace nyxora::runtime
