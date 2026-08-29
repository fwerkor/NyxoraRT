#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "nyxora/base/types.hpp"
#include "nyxora/memory/native_arena.hpp"

namespace nyxora::runtime {

class GuestStack {
public:
    GuestStack() = default;

    [[nodiscard]] static std::optional<GuestStack> create(GuestSize usable_size,
                                                           std::size_t guard_pages = 1);

    [[nodiscard]] GuestAddress base() const noexcept { return base_; }
    [[nodiscard]] GuestAddress top() const noexcept { return base_ + size_; }
    [[nodiscard]] GuestSize size() const noexcept { return size_; }
    [[nodiscard]] bool contains(GuestAddress address) const noexcept {
        return address >= base_ && address < top();
    }

private:
    GuestStack(memory::NativeArena arena, GuestAddress base, GuestSize size)
        : arena_(std::move(arena)), base_(base), size_(size) {}

    memory::NativeArena arena_;
    GuestAddress base_{};
    GuestSize size_{};
};

class EntryTrampoline {
public:
    EntryTrampoline() = default;

    [[nodiscard]] static std::optional<EntryTrampoline> create();
    [[nodiscard]] static bool supported() noexcept;

    [[nodiscard]] std::uint64_t invoke(GuestAddress entry, GuestAddress stack_top,
                                       std::uint64_t arg0 = 0, std::uint64_t arg1 = 0,
                                       std::uint64_t arg2 = 0) const;

private:
    explicit EntryTrampoline(memory::NativeArena code) : code_(std::move(code)) {}

    memory::NativeArena code_;
};

} // namespace nyxora::runtime
