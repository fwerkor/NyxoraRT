#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "nyxora/base/types.hpp"
#include "nyxora/memory/guest_address_space.hpp"

namespace nyxora::runtime {

enum class TcbPatchMode {
    none,
    fs_to_gs,
    fs_to_windows_teb,
};

struct TcbPatchPolicy {
    TcbPatchMode mode{TcbPatchMode::none};
    std::uint32_t windows_teb_offset{};
};

struct TcbPatchReport {
    std::size_t decoded_instructions{};
    std::size_t decode_failures{};
    std::size_t rewritten{};
    std::size_t unsupported{};
};

[[nodiscard]] TcbPatchPolicy host_tcb_patch_policy() noexcept;
[[nodiscard]] std::optional<TcbPatchReport>
patch_tcb_accesses(memory::GuestAddressSpace& memory, GuestAddress base, GuestSize size,
                   TcbPatchPolicy policy);

} // namespace nyxora::runtime
