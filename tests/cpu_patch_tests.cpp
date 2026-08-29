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


NYXORA_TEST(windows_tcb_patcher_uses_side_thunks_for_nonzero_offsets) {
    const std::array<std::byte, 27> code{
        // mov rax, qword ptr fs:[8]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // cmp rax, qword ptr fs:[0x10]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x3b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // xor rax, qword ptr fs:[0x28]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x33}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x28}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    auto memory = make_space(code);
    constexpr nyxora::GuestAddress arena_base = 0x2000;
    constexpr nyxora::GuestSize arena_size = 0x1000;
    NYXORA_CHECK(memory.map(arena_base, arena_size,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "cpu-patches"));
    nyxora::runtime::TcbPatchArena arena{
        .base = arena_base,
        .size = arena_size,
        .used = 0,
    };
    constexpr std::uint32_t teb_offset = 0x1480;
    const auto report = nyxora::runtime::patch_tcb_accesses(
        memory, 0x1000, code.size(),
        nyxora::runtime::TcbPatchPolicy{.mode = nyxora::runtime::TcbPatchMode::fs_to_windows_teb,
                                        .windows_teb_offset = teb_offset},
        &arena);
    NYXORA_CHECK(report.has_value());
    NYXORA_CHECK(report->rewritten == 3);
    NYXORA_CHECK(report->unsupported == 0);
    NYXORA_CHECK(report->trampoline_bytes == arena.used);
    NYXORA_CHECK(arena.used > 0);

    const auto patched = memory.view(0x1000, code.size());
    NYXORA_CHECK(patched[0] == std::byte{0xe9});
    NYXORA_CHECK(patched[9] == std::byte{0xe9});
    NYXORA_CHECK(patched[18] == std::byte{0xe9});
    for (std::size_t site : {std::size_t{0}, std::size_t{9}, std::size_t{18}}) {
        NYXORA_CHECK(patched[site + 5] == std::byte{0x90});
        NYXORA_CHECK(patched[site + 8] == std::byte{0x90});
    }

    const auto thunks = memory.view(arena_base, arena.used);
    NYXORA_CHECK(thunks.size() == arena.used);
    NYXORA_CHECK(thunks[0] == std::byte{0x65});
}


NYXORA_TEST(windows_tcb_patcher_rejects_rsp_flag_operations_instead_of_mispatching) {
    const std::array<std::byte, 9> code{
        // cmp rsp, qword ptr fs:[8]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x3b}, std::byte{0x24}, std::byte{0x25},
        std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    auto memory = make_space(code);
    NYXORA_CHECK(memory.map(0x2000, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "cpu-patches"));
    nyxora::runtime::TcbPatchArena arena{.base = 0x2000, .size = 0x1000, .used = 0};
    const auto report = nyxora::runtime::patch_tcb_accesses(
        memory, 0x1000, code.size(),
        nyxora::runtime::TcbPatchPolicy{.mode = nyxora::runtime::TcbPatchMode::fs_to_windows_teb,
                                        .windows_teb_offset = 0x1480},
        &arena);
    NYXORA_CHECK(report.has_value());
    NYXORA_CHECK(report->rewritten == 0);
    NYXORA_CHECK(report->unsupported == 1);
    NYXORA_CHECK(arena.used == 0);
    NYXORA_CHECK(memory.view(0x1000, code.size())[0] == std::byte{0x64});
}
