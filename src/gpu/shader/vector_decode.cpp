#include "nyxora/gpu/shader/vector.hpp"

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>

namespace nyxora::gpu::shader {
namespace {

std::optional<VectorSource> parse_source(std::uint32_t code,
                                         std::span<const std::uint32_t> words,
                                         std::size_t pc, bool& literal) {
    if (code >= 256 && code <= 511) {
        return VectorSource{VectorSourceKind::vgpr, code - 256};
    }
    if (code >= 128 && code <= 192) {
        return VectorSource{VectorSourceKind::constant, code - 128};
    }
    if (code >= 193 && code <= 208) {
        return VectorSource{VectorSourceKind::constant,
                            static_cast<std::uint32_t>(192 - static_cast<std::int32_t>(code))};
    }
    if (code == 255 && pc + 1 < words.size()) {
        literal = true;
        return VectorSource{VectorSourceKind::constant, words[pc + 1]};
    }
    // SGPR, EXEC, VCC, DPP, SDWA and floating inline constants require more
    // shader state and are deliberately not treated as register zero.
    return std::nullopt;
}

std::optional<VectorOp> vop2_name(std::uint32_t opcode) {
    switch (opcode) {
    case 0x13: return VectorOp::min_u32;
    case 0x14: return VectorOp::max_u32;
    case 0x16: return VectorOp::lshrrev_b32;
    case 0x1a: return VectorOp::lshlrev_b32;
    case 0x1b: return VectorOp::and_b32;
    case 0x1c: return VectorOp::or_b32;
    case 0x1d: return VectorOp::xor_b32;
    case 0x25: return VectorOp::add_nc_u32;
    case 0x26: return VectorOp::sub_nc_u32;
    default: return std::nullopt;
    }
}

} // namespace

const char* vector_op_name(VectorOp op) noexcept {
    switch (op) {
    case VectorOp::nop: return "v_nop";
    case VectorOp::end_program: return "s_endpgm";
    case VectorOp::mov_b32: return "v_mov_b32";
    case VectorOp::not_b32: return "v_not_b32";
    case VectorOp::add_nc_u32: return "v_add_nc_u32";
    case VectorOp::sub_nc_u32: return "v_sub_nc_u32";
    case VectorOp::and_b32: return "v_and_b32";
    case VectorOp::or_b32: return "v_or_b32";
    case VectorOp::xor_b32: return "v_xor_b32";
    case VectorOp::min_u32: return "v_min_u32";
    case VectorOp::max_u32: return "v_max_u32";
    case VectorOp::lshlrev_b32: return "v_lshlrev_b32";
    case VectorOp::lshrrev_b32: return "v_lshrrev_b32";
    }
    return "unknown";
}

std::optional<VectorProgram>
decode_vector_program(std::span<const std::uint32_t> words) {
    if (words.empty() || words.size() > 65536) {
        return std::nullopt;
    }
    VectorProgram program{};
    program.dword_count = static_cast<std::uint32_t>(words.size());
    program.instructions.reserve(words.size());
    for (std::size_t pc = 0; pc < words.size();) {
        const auto word = words[pc];
        VectorInstruction instruction{};
        instruction.offset_dwords = static_cast<std::uint32_t>(pc);
        instruction.word_count = 1;
        bool literal = false;
        if (word == 0xbf810000U) {
            instruction.op = VectorOp::end_program;
            if (pc + 1 != words.size()) {
                return std::nullopt;
            }
        } else if ((word & 0xfe000000U) == 0x7e000000U) {
            const auto opcode = (word >> 9U) & 0xffU;
            if (opcode != 0 && opcode != 1 && opcode != 0x37) {
                return std::nullopt;
            }
            instruction.op = opcode == 0 ? VectorOp::nop
                             : opcode == 1 ? VectorOp::mov_b32
                                           : VectorOp::not_b32;
            instruction.destination = static_cast<std::uint8_t>((word >> 17U) & 0xffU);
            if (opcode != 0) {
                const auto source = parse_source(word & 0x1ffU, words, pc, literal);
                if (!source) {
                    return std::nullopt;
                }
                instruction.sources[0] = *source;
                instruction.source_count = 1;
            } else if ((word & 0x1ffU) != 0 || instruction.destination != 0) {
                // Do not silently discard unused source/destination fields.
                return std::nullopt;
            }
        } else if ((word & 0x80000000U) == 0U &&
                   ((word >> 25U) & 0x3fU) < 0x3eU) {
            const auto opcode = vop2_name((word >> 25U) & 0x3fU);
            if (!opcode) {
                return std::nullopt;
            }
            instruction.op = *opcode;
            instruction.destination = static_cast<std::uint8_t>((word >> 17U) & 0xffU);
            const auto first = parse_source(word & 0x1ffU, words, pc, literal);
            if (!first) {
                return std::nullopt;
            }
            instruction.sources[0] = *first;
            instruction.sources[1] = VectorSource{
                VectorSourceKind::vgpr, (word >> 9U) & 0xffU,
            };
            instruction.source_count = 2;
        } else {
            return std::nullopt;
        }
        if (literal) {
            instruction.word_count = 2;
        }
        program.instructions.push_back(instruction);
        pc += instruction.word_count;
    }
    if (program.instructions.back().op != VectorOp::end_program) {
        return std::nullopt;
    }
    return program;
}

void write_vector_program_json(std::ostream& out, const VectorProgram& program) {
    out << "{\"isa\":\"vop1-vop2-subset\",\"lanes\":" << diagnostic_wave_lanes
        << ",\"dwords\":" << program.dword_count << ",\"instructions\":[";
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        if (i) {
            out << ",";
        }
        const auto& ins = program.instructions[i];
        out << "{\"offset\":" << ins.offset_dwords
            << ",\"size\":" << static_cast<unsigned>(ins.word_count)
            << ",\"op\":\"" << vector_op_name(ins.op)
            << "\",\"dst\":" << static_cast<unsigned>(ins.destination)
            << ",\"src\":[";
        for (std::uint8_t j = 0; j < ins.source_count; ++j) {
            if (j) {
                out << ",";
            }
            out << "{\"kind\":\""
                << (ins.sources[j].kind == VectorSourceKind::vgpr ? "vgpr" : "constant")
                << "\",\"value\":" << ins.sources[j].value << "}";
        }
        out << "]}";
    }
    out << "]}\n";
}

} // namespace nyxora::gpu::shader
