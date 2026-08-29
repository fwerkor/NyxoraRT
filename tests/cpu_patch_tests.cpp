#include "test.hpp"
#include "nyxora/memory/guest_address_space.hpp"
#include "nyxora/runtime/cpu_patches.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

nyxora::memory::GuestAddressSpace make_space(std::span<const std::byte> code) {
    nyxora::memory::GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(0x1000, code.size(),
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "code"));
    NYXORA_CHECK(memory.write(0x1000, code));
    return memory;
}

} // namespace

NYXORA_TEST(tcb_patcher_rewrites_only_decoded_fs_memory_operands) {
    const std::array<std::byte, 37> code{
        // mov rax, qword ptr fs:[0]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // mov rax, 0x64 -- the immediate byte must not be treated as an FS prefix.
        std::byte{0x48}, std::byte{0xb8}, std::byte{0x64}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // cmp rax, qword ptr fs:[0x20]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x3b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x20}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // xor rax, qword ptr fs:[8]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x33}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    auto memory = make_space(code);
    const auto report = nyxora::runtime::patch_tcb_accesses(
        memory, 0x1000, code.size(),
        nyxora::runtime::TcbPatchPolicy{.mode = nyxora::runtime::TcbPatchMode::fs_to_gs});
    NYXORA_CHECK(report.has_value());
    NYXORA_CHECK(report->rewritten == 3);
    NYXORA_CHECK(report->unsupported == 0);

    const auto patched = memory.view(0x1000, code.size());
    NYXORA_CHECK(patched[0] == std::byte{0x65});
    NYXORA_CHECK(patched[11] == std::byte{0x64});
    NYXORA_CHECK(patched[19] == std::byte{0x65});
    NYXORA_CHECK(patched[28] == std::byte{0x65});
}

NYXORA_TEST(windows_tcb_patcher_inlines_teb_slot_for_fs_zero_only) {
    const std::array<std::byte, 18> code{
        std::byte{0x64}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x64}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    auto memory = make_space(code);
    constexpr std::uint32_t teb_offset = 0x1480;
    const auto report = nyxora::runtime::patch_tcb_accesses(
        memory, 0x1000, code.size(),
        nyxora::runtime::TcbPatchPolicy{.mode = nyxora::runtime::TcbPatchMode::fs_to_windows_teb,
                                        .windows_teb_offset = teb_offset});
    NYXORA_CHECK(report.has_value());
    NYXORA_CHECK(report->rewritten == 1);
    NYXORA_CHECK(report->unsupported == 1);

    const auto patched = memory.view(0x1000, code.size());
    NYXORA_CHECK(patched[0] == std::byte{0x65});
    std::uint32_t displacement = 0;
    std::memcpy(&displacement, patched.data() + 5, sizeof(displacement));
    NYXORA_CHECK(displacement == teb_offset);
    NYXORA_CHECK(patched[9] == std::byte{0x64});
}
