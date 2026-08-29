#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "nyxora/base/types.hpp"
#include "nyxora/memory/protection.hpp"

namespace nyxora::memory {

class NativeArena {
public:
    NativeArena() = default;
    ~NativeArena();

    NativeArena(const NativeArena&) = delete;
    NativeArena& operator=(const NativeArena&) = delete;
    NativeArena(NativeArena&& other) noexcept;
    NativeArena& operator=(NativeArena&& other) noexcept;

    [[nodiscard]] static std::optional<NativeArena> reserve(GuestSize size,
                                                            GuestAddress preferred_base = 0);
    [[nodiscard]] static std::size_t page_size() noexcept;
    [[nodiscard]] static bool exact_reservation_supported() noexcept;

    [[nodiscard]] GuestAddress base() const noexcept { return base_; }
    [[nodiscard]] GuestSize size() const noexcept { return size_; }
    [[nodiscard]] explicit operator bool() const noexcept { return base_ != 0; }

    bool protect(GuestSize offset, GuestSize size, Protection protection);
    bool copy(GuestSize offset, std::span<const std::byte> bytes);

    [[nodiscard]] void* host_pointer(GuestSize offset = 0) noexcept;
    [[nodiscard]] const void* host_pointer(GuestSize offset = 0) const noexcept;

private:
    NativeArena(GuestAddress base, GuestSize size) : base_(base), size_(size) {}
    void release() noexcept;

    GuestAddress base_{};
    GuestSize size_{};
};

} // namespace nyxora::memory
