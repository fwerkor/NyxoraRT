#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "nyxora/base/types.hpp"
#include "nyxora/memory/native_arena.hpp"
#include "nyxora/runtime/symbol_registry.hpp"

namespace nyxora::runtime {

struct LateImportRecord {
    std::size_t index{};
    GuestAddress patch_address{};
    GuestAddress thunk_address{};
    SymbolKey key;
    std::string symbol_name;
};

class LateImportTable {
public:
    LateImportTable() = default;

    [[nodiscard]] static std::optional<LateImportTable> create(std::size_t capacity = 128);
    [[nodiscard]] static bool supported() noexcept;

    [[nodiscard]] std::optional<GuestAddress> get_or_create(GuestAddress patch_address,
                                                            const SymbolKey& key,
                                                            std::string symbol_name);
    bool bind_patch(GuestAddress patch_address, GuestAddress target) noexcept;

    [[nodiscard]] const LateImportRecord* find_by_patch(GuestAddress patch_address) const noexcept;
    [[nodiscard]] std::uint64_t call_count(std::size_t index) const noexcept;
    [[nodiscard]] std::span<const LateImportRecord> records() const noexcept { return records_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

private:
    static constexpr std::size_t kThunkSize = 32;
    static constexpr std::size_t kDataSlotSize = 16;

    LateImportTable(memory::NativeArena arena, std::size_t capacity, GuestSize data_offset)
        : arena_(std::move(arena)), capacity_(capacity), data_offset_(data_offset) {}

    [[nodiscard]] GuestAddress thunk_address(std::size_t index) const noexcept;
    [[nodiscard]] std::uint64_t* target_slot(std::size_t index) noexcept;
    [[nodiscard]] const std::uint64_t* target_slot(std::size_t index) const noexcept;
    [[nodiscard]] const std::uint64_t* counter_slot(std::size_t index) const noexcept;

    memory::NativeArena arena_;
    std::size_t capacity_{};
    GuestSize data_offset_{};
    std::vector<LateImportRecord> records_;
};

} // namespace nyxora::runtime
