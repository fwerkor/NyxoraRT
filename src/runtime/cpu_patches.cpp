#include "nyxora/runtime/cpu_patches.hpp"
#include "nyxora/runtime/tls.hpp"

#include <array>
#include <cstring>
#include <limits>

#include <Zydis/Zydis.h>

namespace nyxora::runtime {
namespace {

bool is_tcb_mnemonic(ZydisMnemonic mnemonic) noexcept {
    return mnemonic == ZYDIS_MNEMONIC_MOV || mnemonic == ZYDIS_MNEMONIC_CMP ||
           mnemonic == ZYDIS_MNEMONIC_XOR;
}

bool is_tcb_access(const ZydisDecodedInstruction& instruction,
                   const ZydisDecodedOperand* operands) noexcept {
    if (!is_tcb_mnemonic(instruction.mnemonic) || instruction.operand_count_visible < 2) {
        return false;
    }
    const auto& destination = operands[0];
    const auto& source = operands[1];
    return destination.type == ZYDIS_OPERAND_TYPE_REGISTER && destination.size == 64 &&
           source.type == ZYDIS_OPERAND_TYPE_MEMORY && source.size == 64 &&
           source.mem.segment == ZYDIS_REGISTER_FS && source.mem.base == ZYDIS_REGISTER_NONE &&
           source.mem.index == ZYDIS_REGISTER_NONE && source.mem.disp.has_displacement &&
           source.mem.disp.value >= 0 &&
           static_cast<std::uint64_t>(source.mem.disp.value) < sizeof(GuestTcb);
}

std::optional<std::size_t> fs_prefix_offset(const ZydisDecodedInstruction& instruction) noexcept {
    std::optional<std::size_t> result;
    for (std::size_t i = 0; i < instruction.raw.prefix_count; ++i) {
        if (instruction.raw.prefixes[i].value != 0x64U) {
            continue;
        }
        if (result) {
            return std::nullopt;
        }
        result = i;
    }
    return result;
}

bool patch_linux(memory::GuestAddressSpace& memory, GuestAddress address,
                 const ZydisDecodedInstruction& instruction) {
    const auto prefix = fs_prefix_offset(instruction);
    if (!prefix) {
        return false;
    }
    const std::array<std::byte, 1> gs_prefix{std::byte{0x65}};
    return memory.patch(address + *prefix, gs_prefix);
}

bool patch_windows(memory::GuestAddressSpace& memory, GuestAddress address,
                   const ZydisDecodedInstruction& instruction,
                   const ZydisDecodedOperand* operands, std::uint32_t teb_offset) {
    if (operands[1].mem.disp.value != 0 || instruction.raw.disp.size != 32) {
        return false;
    }
    const auto prefix = fs_prefix_offset(instruction);
    if (!prefix || instruction.raw.disp.offset > instruction.length - sizeof(teb_offset)) {
        return false;
    }

    const std::array<std::byte, 1> gs_prefix{std::byte{0x65}};
    std::array<std::byte, sizeof(teb_offset)> displacement{};
    std::memcpy(displacement.data(), &teb_offset, sizeof(teb_offset));
    return memory.patch(address + *prefix, gs_prefix) &&
           memory.patch(address + instruction.raw.disp.offset, displacement);
}

} // namespace

TcbPatchPolicy host_tcb_patch_policy() noexcept {
#if defined(__linux__) && defined(__x86_64__)
    return TcbPatchPolicy{.mode = TcbPatchMode::fs_to_gs, .windows_teb_offset = 0};
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const auto offset = windows_guest_tcb_teb_offset();
    return offset ? TcbPatchPolicy{.mode = TcbPatchMode::fs_to_windows_teb,
                                   .windows_teb_offset = *offset}
                  : TcbPatchPolicy{};
#else
    return TcbPatchPolicy{};
#endif
}

std::optional<TcbPatchReport> patch_tcb_accesses(memory::GuestAddressSpace& memory,
                                                  GuestAddress base, GuestSize size,
                                                  TcbPatchPolicy policy) {
    if (size == 0 || size > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    const auto bytes = memory.view(base, size);
    if (bytes.size() != static_cast<std::size_t>(size)) {
        return std::nullopt;
    }

    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                       ZYDIS_STACK_WIDTH_64))) {
        return std::nullopt;
    }

    TcbPatchReport report{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        const auto status = ZydisDecoderDecodeFull(&decoder, bytes.data() + offset,
                                                   bytes.size() - offset, &instruction, operands);
        if (!ZYAN_SUCCESS(status) || instruction.length == 0) {
            ++report.decode_failures;
            ++offset;
            continue;
        }
        ++report.decoded_instructions;

        if (is_tcb_access(instruction, operands)) {
            bool patched = false;
            switch (policy.mode) {
            case TcbPatchMode::fs_to_gs:
                patched = patch_linux(memory, base + offset, instruction);
                break;
            case TcbPatchMode::fs_to_windows_teb:
                patched = patch_windows(memory, base + offset, instruction, operands,
                                        policy.windows_teb_offset);
                break;
            case TcbPatchMode::none:
                break;
            }
            if (patched) {
                ++report.rewritten;
            } else {
                ++report.unsupported;
            }
        }

        offset += instruction.length;
    }

    if (report.rewritten != 0 && !memory.flush_instruction_cache(base, size)) {
        return std::nullopt;
    }
    return report;
}

} // namespace nyxora::runtime
