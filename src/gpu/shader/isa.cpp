#include "nyxora/gpu/shader/isa.hpp"

#include <cstdint>
#include <ostream>
#include <stdexcept>

namespace nyxora::gpu::shader {
namespace {

std::optional<ScalarOpcode> classify(std::uint32_t opcode) {
    switch (opcode) {
    case 0:
        return ScalarOpcode::nop;
    case 1:
        return ScalarOpcode::end_program;
    case 2:
        return ScalarOpcode::branch;
    case 4:
        return ScalarOpcode::branch_scc0;
    case 5:
        return ScalarOpcode::branch_scc1;
    case 6:
        return ScalarOpcode::branch_vccz;
    case 7:
        return ScalarOpcode::branch_vccnz;
    case 8:
        return ScalarOpcode::branch_execz;
    case 9:
        return ScalarOpcode::branch_execnz;
    case 10:
        return ScalarOpcode::barrier;
    case 12:
        return ScalarOpcode::waitcnt;
    default:
        return std::nullopt;
    }
}

const char* opcode_name(ScalarOpcode opcode) {
    switch (opcode) {
    case ScalarOpcode::nop: return "s_nop";
    case ScalarOpcode::end_program: return "s_endpgm";
    case ScalarOpcode::branch: return "s_branch";
    case ScalarOpcode::branch_scc0: return "s_cbranch_scc0";
    case ScalarOpcode::branch_scc1: return "s_cbranch_scc1";
    case ScalarOpcode::branch_vccz: return "s_cbranch_vccz";
    case ScalarOpcode::branch_vccnz: return "s_cbranch_vccnz";
    case ScalarOpcode::branch_execz: return "s_cbranch_execz";
    case ScalarOpcode::branch_execnz: return "s_cbranch_execnz";
    case ScalarOpcode::barrier: return "s_barrier";
    case ScalarOpcode::waitcnt: return "s_waitcnt";
    }
    throw std::runtime_error("invalid decoded scalar opcode");
}

bool is_branch(ScalarOpcode opcode) {
    switch (opcode) {
    case ScalarOpcode::branch:
    case ScalarOpcode::branch_scc0:
    case ScalarOpcode::branch_scc1:
    case ScalarOpcode::branch_vccz:
    case ScalarOpcode::branch_vccnz:
    case ScalarOpcode::branch_execz:
    case ScalarOpcode::branch_execnz:
        return true;
    default:
        return false;
    }
}

} // namespace

std::optional<ScalarProgram> decode_sopp_program(std::span<const std::uint32_t> words) {
    if (words.empty() || words.size() > 65536U) {
        return std::nullopt;
    }
    ScalarProgram result;
    result.instructions.reserve(words.size());
    bool terminated = false;
    for (std::size_t pc = 0; pc < words.size(); ++pc) {
        const auto word = words[pc];
        if (terminated || (word & 0xff800000U) != 0xbf800000U) {
            return std::nullopt;
        }
        const auto opcode = classify((word >> 16U) & 0x7fU);
        if (!opcode) {
            return std::nullopt;
        }
        // Preserve the signed immediate independently of host integer
        // representation; branch displacement is relative to the next word.
        const auto low = static_cast<std::int32_t>(word & 0xffffU);
        const auto immediate = static_cast<std::int16_t>(
            low >= 0x8000 ? low - 0x10000 : low);
        ScalarInstruction instruction{
            .offset_dwords = static_cast<std::uint32_t>(pc),
            .opcode = *opcode,
            .immediate = immediate,
            .branch_target_dwords = std::nullopt,
        };
        if (is_branch(*opcode)) {
            const auto target = static_cast<std::int64_t>(pc) + 1 + immediate;
            if (target < 0 || target >= static_cast<std::int64_t>(words.size())) {
                return std::nullopt;
            }
            instruction.branch_target_dwords = static_cast<std::uint32_t>(target);
        }
        result.instructions.push_back(instruction);
        terminated = *opcode == ScalarOpcode::end_program;
    }
    if (!terminated) {
        return std::nullopt;
    }
    return result;
}

void write_sopp_json(std::ostream& output, const ScalarProgram& program) {
    output << "{\"isa\":\"sopp-subset\",\"instructions\":[";
    bool first = true;
    for (const auto& instruction : program.instructions) {
        if (!first) {
            output << ",";
        }
        first = false;
        output << "{\"offset_dwords\":" << instruction.offset_dwords
               << ",\"opcode\":\"" << opcode_name(instruction.opcode)
               << "\",\"immediate\":" << instruction.immediate
               << ",\"branch_target_dwords\":";
        if (instruction.branch_target_dwords) {
            output << *instruction.branch_target_dwords;
        } else {
            output << "null";
        }
        output << "}";
    }
    output << "]}\n";
}

std::optional<ScalarProgram>
decode_guest_sopp_program(const GuestMemoryReader& reader, const pm4::ShaderProgram& binding,
                          std::size_t word_count) {
    const auto words = snapshot_shader_words(reader, binding, word_count);
    return words ? decode_sopp_program(*words) : std::nullopt;
}

} // namespace nyxora::gpu::shader
