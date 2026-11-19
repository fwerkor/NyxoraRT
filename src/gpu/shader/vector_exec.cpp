#include "nyxora/gpu/shader/vector.hpp"

#include <cstdint>
#include <ostream>

namespace nyxora::gpu::shader {
namespace {

std::uint32_t source_at(const VectorMachineState& state, VectorSource source,
                        std::uint32_t lane) {
    return source.kind == VectorSourceKind::constant ? source.value
            : state.vgpr[source.value * diagnostic_wave_lanes + lane];
}

} // namespace

VectorMachineState make_seeded_vector_state() {
    VectorMachineState state{};
    for (std::size_t i = 0; i < state.vgpr.size(); ++i) {
        state.vgpr[i] =
            static_cast<std::uint32_t>(i * 1664525U + 1013904223U) ^ 0x9e3779b9U;
    }
    return state;
}

std::optional<VectorMachineState>
execute_vector_program(const VectorProgram& program, VectorMachineState initial) {
    if (program.instructions.empty() || program.instructions.size() > 8192 ||
        program.dword_count == 0 || program.dword_count > 65536 ||
        program.instructions.back().op != VectorOp::end_program) {
        return std::nullopt;
    }
    std::uint32_t cursor = 0;
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        const auto& ins = program.instructions[i];
        if (ins.offset_dwords != cursor || ins.word_count < 1 ||
            ins.word_count > 2 || ins.word_count > program.dword_count - cursor ||
            (ins.op == VectorOp::end_program && i + 1 != program.instructions.size())) {
            return std::nullopt;
        }
        const auto expected = ins.op == VectorOp::end_program || ins.op == VectorOp::nop ? 0U
                           : ins.op == VectorOp::mov_b32 || ins.op == VectorOp::not_b32 ? 1U
                           : 2U;
        if (ins.source_count != expected) {
            return std::nullopt;
        }
        for (std::uint8_t j = 0; j < ins.source_count; ++j) {
            const auto kind = ins.sources[j].kind;
            if ((kind != VectorSourceKind::constant && kind != VectorSourceKind::vgpr) ||
                (kind == VectorSourceKind::vgpr &&
                 ins.sources[j].value >= diagnostic_vgpr_count)) {
                return std::nullopt;
            }
        }
        cursor += ins.word_count;
    }
    if (cursor != program.dword_count) {
        return std::nullopt;
    }
    for (const auto& ins : program.instructions) {
        if (ins.op == VectorOp::end_program || ins.op == VectorOp::nop) {
            continue;
        }
        for (std::uint32_t lane = 0; lane < diagnostic_wave_lanes; ++lane) {
            const auto lhs = source_at(initial, ins.sources[0], lane);
            const auto rhs = ins.source_count == 2 ? source_at(initial, ins.sources[1], lane) : 0U;
            std::uint32_t result = 0;
            switch (ins.op) {
            case VectorOp::mov_b32: result = lhs; break;
            case VectorOp::not_b32: result = ~lhs; break;
            case VectorOp::add_nc_u32: result = lhs + rhs; break;
            case VectorOp::sub_nc_u32: result = lhs - rhs; break;
            case VectorOp::and_b32: result = lhs & rhs; break;
            case VectorOp::or_b32: result = lhs | rhs; break;
            case VectorOp::xor_b32: result = lhs ^ rhs; break;
            case VectorOp::min_u32: result = lhs < rhs ? lhs : rhs; break;
            case VectorOp::max_u32: result = lhs > rhs ? lhs : rhs; break;
            case VectorOp::lshlrev_b32: result = rhs << (lhs & 31U); break;
            case VectorOp::lshrrev_b32: result = rhs >> (lhs & 31U); break;
            default: return std::nullopt;
            }
            initial.vgpr[static_cast<std::size_t>(ins.destination) * diagnostic_wave_lanes +
                         lane] = result;
        }
    }
    return initial;
}

void write_vector_state_json(std::ostream& out, const VectorMachineState& state) {
    out << "{\"lanes\":" << diagnostic_wave_lanes << ",\"vgpr\":[";
    for (std::size_t i = 0; i < state.vgpr.size(); ++i) {
        if (i) {
            out << ",";
        }
        out << state.vgpr[i];
    }
    out << "]}\n";
}

} // namespace nyxora::gpu::shader
