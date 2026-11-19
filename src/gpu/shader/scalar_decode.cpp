#include "nyxora/gpu/shader/scalar_decode.hpp"

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <unordered_set>

namespace nyxora::gpu::shader {
namespace {

std::int32_t sign_extend16(std::uint32_t low) {
    return low >= 0x8000U ? static_cast<std::int32_t>(low) - 0x10000
                          : static_cast<std::int32_t>(low);
}

std::optional<ScalarOp> decode_sopp_opcode(std::uint32_t code) {
    switch (code) {
    case 0: return ScalarOp::nop;
    case 1: return ScalarOp::end_program;
    case 2: return ScalarOp::branch;
    case 4: return ScalarOp::branch_scc0;
    case 5: return ScalarOp::branch_scc1;
    case 6: return ScalarOp::branch_vccz;
    case 7: return ScalarOp::branch_vccnz;
    case 8: return ScalarOp::branch_execz;
    case 9: return ScalarOp::branch_execnz;
    case 10: return ScalarOp::barrier;
    case 12: return ScalarOp::waitcnt;
    default: return std::nullopt;
    }
}

std::optional<ScalarSource> decode_source(std::uint8_t raw,
                                          std::span<const std::uint32_t> words,
                                          std::size_t pc, bool& literal_used) {
    if (raw <= 101U) {
        return ScalarSource{ScalarSourceKind::sgpr, raw};
    }
    if (raw >= 128U && raw <= 192U) {
        return ScalarSource{ScalarSourceKind::constant, raw - 128U};
    }
    if (raw >= 193U && raw <= 208U) {
        return ScalarSource{ScalarSourceKind::constant,
                            static_cast<std::uint32_t>(-static_cast<std::int32_t>(raw - 192U))};
    }
    if (raw == 255U && pc + 1U < words.size()) {
        literal_used = true;
        return ScalarSource{ScalarSourceKind::constant, words[pc + 1U]};
    }
    return std::nullopt;
}

bool valid_destination(std::uint32_t dst) { return dst <= 101U; }

} // namespace

bool is_scalar_conditional_branch(ScalarOp opcode) noexcept {
    switch (opcode) {
    case ScalarOp::branch_scc0:
    case ScalarOp::branch_scc1:
    case ScalarOp::branch_vccz:
    case ScalarOp::branch_vccnz:
    case ScalarOp::branch_execz:
    case ScalarOp::branch_execnz:
        return true;
    default:
        return false;
    }
}

bool is_scalar_branch(ScalarOp opcode) noexcept {
    return opcode == ScalarOp::branch || is_scalar_conditional_branch(opcode);
}

const char* scalar_op_name(ScalarOp opcode) {
    switch (opcode) {
    case ScalarOp::nop: return "s_nop";
    case ScalarOp::end_program: return "s_endpgm";
    case ScalarOp::branch: return "s_branch";
    case ScalarOp::branch_scc0: return "s_cbranch_scc0";
    case ScalarOp::branch_scc1: return "s_cbranch_scc1";
    case ScalarOp::branch_vccz: return "s_cbranch_vccz";
    case ScalarOp::branch_vccnz: return "s_cbranch_vccnz";
    case ScalarOp::branch_execz: return "s_cbranch_execz";
    case ScalarOp::branch_execnz: return "s_cbranch_execnz";
    case ScalarOp::barrier: return "s_barrier";
    case ScalarOp::waitcnt: return "s_waitcnt";
    case ScalarOp::mov_b32: return "s_mov_b32";
    case ScalarOp::not_b32: return "s_not_b32";
    case ScalarOp::add_u32: return "s_add_u32";
    case ScalarOp::sub_u32: return "s_sub_u32";
    case ScalarOp::and_b32: return "s_and_b32";
    case ScalarOp::or_b32: return "s_or_b32";
    case ScalarOp::xor_b32: return "s_xor_b32";
    case ScalarOp::cmp_eq_u32: return "s_cmp_eq_u32";
    case ScalarOp::movk_i32: return "s_movk_i32";
    }
    return "unknown";
}

std::optional<ScalarDecodedProgram>
decode_scalar_program(std::span<const std::uint32_t> words) {
    if (words.empty() || words.size() > 65536U) {
        return std::nullopt;
    }
    ScalarDecodedProgram program{};
    program.dword_count = static_cast<std::uint32_t>(words.size());
    program.instructions.reserve(words.size());

    for (std::size_t pc = 0; pc < words.size();) {
        const auto word = words[pc];
        ScalarDecodedInstruction ins{};
        ins.offset_dwords = static_cast<std::uint32_t>(pc);
        ins.word_count = 1;
        bool literal_used = false;
        auto read_source = [&](std::uint8_t raw, std::uint8_t slot) {
            const auto source = decode_source(raw, words, pc, literal_used);
            if (!source) {
                return false;
            }
            ins.sources[slot] = *source;
            ++ins.source_count;
            return true;
        };

        if ((word & 0xff800000U) == 0xbf800000U) {
            ins.family = ScalarFamily::sopp;
            const auto opcode = decode_sopp_opcode((word >> 16U) & 0x7fU);
            if (!opcode) {
                return std::nullopt;
            }
            ins.opcode = *opcode;
            ins.immediate = sign_extend16(word & 0xffffU);
            if (is_scalar_branch(*opcode)) {
                const auto target = static_cast<std::int64_t>(pc) + 1 + ins.immediate;
                if (target < 0 || target >= static_cast<std::int64_t>(words.size())) {
                    return std::nullopt;
                }
                ins.branch_target_dwords = static_cast<std::uint32_t>(target);
            }
        } else if ((word & 0xff800000U) == 0xbe800000U) {
            ins.family = ScalarFamily::sop1;
            const auto opcode = (word >> 8U) & 0xffU;
            if (opcode == 3U) {
                ins.opcode = ScalarOp::mov_b32;
            } else if (opcode == 7U) {
                ins.opcode = ScalarOp::not_b32;
            } else {
                return std::nullopt;
            }
            const auto dst = (word >> 16U) & 0x7fU;
            if (!valid_destination(dst) || !read_source(static_cast<std::uint8_t>(word & 0xffU), 0)) {
                return std::nullopt;
            }
            ins.dst_sgpr = static_cast<std::uint8_t>(dst);
        } else if ((word & 0xff800000U) == 0xbf000000U) {
            ins.family = ScalarFamily::sopc;
            const auto opcode = (word >> 16U) & 0x7fU;
            if (opcode != 6U) {
                return std::nullopt;
            }
            ins.opcode = ScalarOp::cmp_eq_u32;
            if (!read_source(static_cast<std::uint8_t>(word & 0xffU), 0) ||
                !read_source(static_cast<std::uint8_t>((word >> 8U) & 0xffU), 1)) {
                return std::nullopt;
            }
        } else if ((word & 0xf0000000U) == 0xb0000000U) {
            ins.family = ScalarFamily::sopk;
            if (((word >> 23U) & 0x1fU) != 0) {
                return std::nullopt;
            }
            const auto dst = (word >> 16U) & 0x7fU;
            if (!valid_destination(dst)) {
                return std::nullopt;
            }
            ins.opcode = ScalarOp::movk_i32;
            ins.dst_sgpr = static_cast<std::uint8_t>(dst);
            ins.immediate = sign_extend16(word & 0xffffU);
            ins.sources[0] = ScalarSource{ScalarSourceKind::constant,
                                         static_cast<std::uint32_t>(ins.immediate)};
            ins.source_count = 1;
        } else if ((word & 0xc0000000U) == 0x80000000U) {
            ins.family = ScalarFamily::sop2;
            const auto opcode = (word >> 23U) & 0x7fU;
            switch (opcode) {
            case 0x00: ins.opcode = ScalarOp::add_u32; break;
            case 0x01: ins.opcode = ScalarOp::sub_u32; break;
            case 0x0e: ins.opcode = ScalarOp::and_b32; break;
            case 0x10: ins.opcode = ScalarOp::or_b32; break;
            case 0x12: ins.opcode = ScalarOp::xor_b32; break;
            default: return std::nullopt;
            }
            const auto dst = (word >> 16U) & 0x7fU;
            if (!valid_destination(dst) ||
                !read_source(static_cast<std::uint8_t>(word & 0xffU), 0) ||
                !read_source(static_cast<std::uint8_t>((word >> 8U) & 0xffU), 1)) {
                return std::nullopt;
            }
            ins.dst_sgpr = static_cast<std::uint8_t>(dst);
        } else {
            return std::nullopt;
        }

        if (literal_used) {
            ins.word_count = 2;
        }
        program.instructions.push_back(ins);
        pc += ins.word_count;
        if (ins.opcode == ScalarOp::end_program && pc != words.size()) {
            return std::nullopt;
        }
    }

    if (program.instructions.empty() ||
        program.instructions.back().opcode != ScalarOp::end_program) {
        return std::nullopt;
    }
    std::unordered_set<std::uint32_t> boundaries;
    boundaries.reserve(program.instructions.size());
    for (const auto& ins : program.instructions) {
        boundaries.insert(ins.offset_dwords);
    }
    for (const auto& ins : program.instructions) {
        if (ins.branch_target_dwords && !boundaries.contains(*ins.branch_target_dwords)) {
            return std::nullopt;
        }
    }
    return program;
}

void write_scalar_program_json(std::ostream& out, const ScalarDecodedProgram& program) {
    out << "{\"isa\":\"scalar-subset\",\"dwords\":" << program.dword_count
        << ",\"instructions\":[";
    bool first = true;
    for (const auto& ins : program.instructions) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << "{\"offset\":" << ins.offset_dwords
            << ",\"size\":" << static_cast<unsigned>(ins.word_count)
            << ",\"op\":\"" << scalar_op_name(ins.opcode) << "\"";
        if (ins.dst_sgpr) {
            out << ",\"dst\":" << static_cast<unsigned>(*ins.dst_sgpr);
        }
        out << ",\"src\":[";
        for (std::uint8_t i = 0; i < ins.source_count; ++i) {
            if (i != 0) {
                out << ",";
            }
            const auto& source = ins.sources[i];
            out << "{\"kind\":\""
                << (source.kind == ScalarSourceKind::sgpr ? "sgpr" : "constant")
                << "\",\"value\":" << source.value << "}";
        }
        out << "]";
        if (ins.family == ScalarFamily::sopp || ins.family == ScalarFamily::sopk) {
            out << ",\"immediate\":" << ins.immediate;
        }
        if (ins.branch_target_dwords) {
            out << ",\"target\":" << *ins.branch_target_dwords;
        }
        out << "}";
    }
    out << "]}\n";
}

} // namespace nyxora::gpu::shader
