#include "nyxora/runtime/cpu_patches.hpp"
#include "nyxora/runtime/tls.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <vector>

#include <Zydis/Zydis.h>

namespace nyxora::runtime {
namespace {

constexpr std::size_t kNearJumpSize = 5;

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

std::optional<unsigned> gpr_index(ZydisRegister reg) noexcept {
    switch (reg) {
    case ZYDIS_REGISTER_RAX: return 0;
    case ZYDIS_REGISTER_RCX: return 1;
    case ZYDIS_REGISTER_RDX: return 2;
    case ZYDIS_REGISTER_RBX: return 3;
    case ZYDIS_REGISTER_RSP: return 4;
    case ZYDIS_REGISTER_RBP: return 5;
    case ZYDIS_REGISTER_RSI: return 6;
    case ZYDIS_REGISTER_RDI: return 7;
    case ZYDIS_REGISTER_R8: return 8;
    case ZYDIS_REGISTER_R9: return 9;
    case ZYDIS_REGISTER_R10: return 10;
    case ZYDIS_REGISTER_R11: return 11;
    case ZYDIS_REGISTER_R12: return 12;
    case ZYDIS_REGISTER_R13: return 13;
    case ZYDIS_REGISTER_R14: return 14;
    case ZYDIS_REGISTER_R15: return 15;
    default: return std::nullopt;
    }
}

void append_u32(std::vector<std::byte>& code, std::uint32_t value) {
    const auto offset = code.size();
    code.resize(offset + sizeof(value));
    std::memcpy(code.data() + offset, &value, sizeof(value));
}

void emit_rex(std::vector<std::byte>& code, bool r, bool b) {
    code.push_back(static_cast<std::byte>(0x48U | (r ? 0x04U : 0U) | (b ? 0x01U : 0U)));
}

void emit_mov_gs_absolute(std::vector<std::byte>& code, unsigned destination,
                          std::uint32_t displacement) {
    code.push_back(std::byte{0x65});
    emit_rex(code, destination >= 8U, false);
    code.push_back(std::byte{0x8b});
    code.push_back(static_cast<std::byte>(0x04U | ((destination & 7U) << 3U)));
    code.push_back(std::byte{0x25});
    append_u32(code, displacement);
}

void emit_mov_from_base(std::vector<std::byte>& code, unsigned destination, unsigned base,
                        std::uint32_t displacement) {
    emit_rex(code, destination >= 8U, base >= 8U);
    code.push_back(std::byte{0x8b});
    const auto low_base = base & 7U;
    code.push_back(static_cast<std::byte>(0x80U | ((destination & 7U) << 3U) |
                                          (low_base == 4U ? 4U : low_base)));
    if (low_base == 4U) {
        code.push_back(std::byte{0x24});
    }
    append_u32(code, displacement);
}

void emit_register_op(std::vector<std::byte>& code, std::byte opcode, unsigned destination,
                      unsigned source) {
    emit_rex(code, destination >= 8U, source >= 8U);
    code.push_back(opcode);
    code.push_back(static_cast<std::byte>(0xc0U | ((destination & 7U) << 3U) | (source & 7U)));
}

void emit_push(std::vector<std::byte>& code, unsigned reg) {
    if (reg >= 8U) {
        code.push_back(std::byte{0x41});
    }
    code.push_back(static_cast<std::byte>(0x50U + (reg & 7U)));
}

void emit_pop(std::vector<std::byte>& code, unsigned reg) {
    if (reg >= 8U) {
        code.push_back(std::byte{0x41});
    }
    code.push_back(static_cast<std::byte>(0x58U + (reg & 7U)));
}

void emit_enter_scratch(std::vector<std::byte>& code, unsigned scratch) {
    const std::byte move_below_red_zone[] = {
        std::byte{0x48}, std::byte{0x8d}, std::byte{0x64}, std::byte{0x24}, std::byte{0x80},
    }; // lea rsp,[rsp-128]
    code.insert(code.end(), std::begin(move_below_red_zone), std::end(move_below_red_zone));
    emit_push(code, scratch);
}

void emit_leave_scratch(std::vector<std::byte>& code, unsigned scratch) {
    emit_pop(code, scratch);
    const std::byte restore_stack[] = {
        std::byte{0x48}, std::byte{0x8d}, std::byte{0xa4}, std::byte{0x24},
        std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    }; // lea rsp,[rsp+128]
    code.insert(code.end(), std::begin(restore_stack), std::end(restore_stack));
}

std::optional<std::int32_t> relative_displacement(GuestAddress next_instruction,
                                                   GuestAddress target) noexcept {
    if (target >= next_instruction) {
        const auto delta = target - next_instruction;
        if (delta > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(delta);
    }
    const auto delta = next_instruction - target;
    constexpr std::uint64_t min_magnitude =
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1U;
    if (delta > min_magnitude) {
        return std::nullopt;
    }
    if (delta == min_magnitude) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return -static_cast<std::int32_t>(delta);
}

bool append_near_jump(std::vector<std::byte>& code, GuestAddress instruction_address,
                      GuestAddress target) {
    const auto displacement = relative_displacement(instruction_address + kNearJumpSize, target);
    if (!displacement) {
        return false;
    }
    code.push_back(std::byte{0xe9});
    const auto value = static_cast<std::uint32_t>(*displacement);
    append_u32(code, value);
    return true;
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

bool patch_windows_inline(memory::GuestAddressSpace& memory, GuestAddress address,
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

std::optional<std::vector<std::byte>> build_windows_tcb_thunk(
    const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands,
    std::uint32_t teb_offset, GuestAddress thunk_address, GuestAddress return_address) {
    const auto destination = gpr_index(operands[0].reg.value);
    if (!destination) {
        return std::nullopt;
    }
    const auto tcb_offset = static_cast<std::uint32_t>(operands[1].mem.disp.value);
    std::vector<std::byte> code;
    code.reserve(48);

    if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV) {
        emit_mov_gs_absolute(code, *destination, teb_offset);
        if (tcb_offset != 0) {
            emit_mov_from_base(code, *destination, *destination, tcb_offset);
        }
    } else if (instruction.mnemonic == ZYDIS_MNEMONIC_CMP ||
               instruction.mnemonic == ZYDIS_MNEMONIC_XOR) {
        if (*destination == 4U) {
            return std::nullopt;
        }
        const unsigned scratch = *destination == 0U ? 3U : 0U;
        emit_enter_scratch(code, scratch);
        emit_mov_gs_absolute(code, scratch, teb_offset);
        if (tcb_offset != 0) {
            emit_mov_from_base(code, scratch, scratch, tcb_offset);
        }
        emit_register_op(code,
                         instruction.mnemonic == ZYDIS_MNEMONIC_CMP ? std::byte{0x3b}
                                                                    : std::byte{0x33},
                         *destination, scratch);
        emit_leave_scratch(code, scratch);
    } else {
        return std::nullopt;
    }

    const auto jump_address = thunk_address + static_cast<GuestSize>(code.size());
    if (!append_near_jump(code, jump_address, return_address)) {
        return std::nullopt;
    }
    return code;
}

bool patch_windows_trampoline(memory::GuestAddressSpace& memory, GuestAddress address,
                              const ZydisDecodedInstruction& instruction,
                              const ZydisDecodedOperand* operands, std::uint32_t teb_offset,
                              TcbPatchArena* arena, GuestSize& emitted_bytes) {
    if (arena == nullptr || instruction.length < kNearJumpSize || arena->used > arena->size) {
        return false;
    }
    const auto thunk_address = arena->base + arena->used;
    const auto thunk = build_windows_tcb_thunk(instruction, operands, teb_offset, thunk_address,
                                               address + instruction.length);
    if (!thunk || thunk->size() > arena->size - arena->used) {
        return false;
    }

    std::array<std::byte, ZYDIS_MAX_INSTRUCTION_LENGTH> site{};
    site.fill(std::byte{0x90});
    std::vector<std::byte> jump;
    jump.reserve(kNearJumpSize);
    if (!append_near_jump(jump, address, thunk_address) || jump.size() != kNearJumpSize) {
        return false;
    }
    std::copy(jump.begin(), jump.end(), site.begin());

    if (!memory.write(thunk_address, *thunk) ||
        !memory.patch(address, std::span<const std::byte>(site.data(), instruction.length)) ||
        !memory.flush_instruction_cache(thunk_address, static_cast<GuestSize>(thunk->size()))) {
        return false;
    }
    arena->used += static_cast<GuestSize>(thunk->size());
    emitted_bytes += static_cast<GuestSize>(thunk->size());
    return true;
}

bool patch_windows(memory::GuestAddressSpace& memory, GuestAddress address,
                   const ZydisDecodedInstruction& instruction,
                   const ZydisDecodedOperand* operands, std::uint32_t teb_offset,
                   TcbPatchArena* arena, GuestSize& emitted_bytes) {
    if (operands[1].mem.disp.value == 0) {
        return patch_windows_inline(memory, address, instruction, operands, teb_offset);
    }
    return patch_windows_trampoline(memory, address, instruction, operands, teb_offset, arena,
                                    emitted_bytes);
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
                                                  TcbPatchPolicy policy, TcbPatchArena* arena) {
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
                                        policy.windows_teb_offset, arena,
                                        report.trampoline_bytes);
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
