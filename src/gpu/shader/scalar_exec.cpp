#include "nyxora/gpu/shader/scalar_exec.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <optional>
#include <vector>

namespace nyxora::gpu::shader {
namespace {

bool supported(ScalarOp op) {
    switch (op) {
    case ScalarOp::nop:
    case ScalarOp::end_program:
    case ScalarOp::branch:
    case ScalarOp::branch_scc0:
    case ScalarOp::branch_scc1:
    case ScalarOp::mov_b32:
    case ScalarOp::not_b32:
    case ScalarOp::add_u32:
    case ScalarOp::sub_u32:
    case ScalarOp::and_b32:
    case ScalarOp::or_b32:
    case ScalarOp::xor_b32:
    case ScalarOp::cmp_eq_u32:
    case ScalarOp::movk_i32:
        return true;
    default:
        return false;
    }
}

std::optional<std::uint32_t> source_value(const ScalarMachineState& state,
                                          ScalarSource source) {
    if (source.kind == ScalarSourceKind::constant) {
        return source.value;
    }
    if (source.kind == ScalarSourceKind::sgpr && source.value < state.sgpr.size()) {
        return state.sgpr[source.value];
    }
    return std::nullopt;
}

} // namespace

void write_scalar_state_json(std::ostream& out, const ScalarMachineState& state) {
    out << "{\"sgpr\":[";
    for (std::size_t i = 0; i < state.sgpr.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << state.sgpr[i];
    }
    out << "],\"scc\":" << (state.scc ? 1 : 0)
        << ",\"steps\":" << state.steps << "}\n";
}

std::optional<ScalarMachineState>
execute_scalar_subset(const ScalarControlFlowGraph& cfg, ScalarMachineState initial,
                      std::size_t max_steps) {
    if (max_steps == 0 || max_steps > 1'000'000 || !build_scalar_cfg(cfg.program)) {
        return std::nullopt;
    }
    const auto& program = cfg.program;
    const auto invalid = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> offset_map(program.dword_count, invalid);
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        const auto& ins = program.instructions[i];
        if (!supported(ins.opcode) ||
            ins.source_count > 2 ||
            (ins.dst_sgpr && *ins.dst_sgpr >= initial.sgpr.size())) {
            return std::nullopt;
        }
        for (std::uint8_t j = 0; j < ins.source_count; ++j) {
            if (ins.sources[j].kind == ScalarSourceKind::sgpr &&
                ins.sources[j].value >= initial.sgpr.size()) {
                return std::nullopt;
            }
        }
        offset_map[ins.offset_dwords] = i;
    }

    auto state = initial;
    state.steps = 0;
    std::uint32_t pc = 0;
    while (state.steps < max_steps) {
        if (pc >= offset_map.size() || offset_map[pc] == invalid) {
            return std::nullopt;
        }
        const auto& ins = program.instructions[offset_map[pc]];
        ++state.steps;
        const auto next = pc + ins.word_count;

        if (ins.opcode == ScalarOp::end_program) {
            return state;
        }
        if (ins.opcode == ScalarOp::nop) {
            pc = next;
            continue;
        }
        if (is_scalar_branch(ins.opcode)) {
            if (!ins.branch_target_dwords) {
                return std::nullopt;
            }
            const bool take = ins.opcode == ScalarOp::branch ||
                              (ins.opcode == ScalarOp::branch_scc0 && !state.scc) ||
                              (ins.opcode == ScalarOp::branch_scc1 && state.scc);
            pc = take ? *ins.branch_target_dwords : next;
            continue;
        }

        if (ins.opcode == ScalarOp::mov_b32 || ins.opcode == ScalarOp::movk_i32 ||
            ins.opcode == ScalarOp::not_b32) {
            if (!ins.dst_sgpr || ins.source_count != 1) {
                return std::nullopt;
            }
            const auto src = source_value(state, ins.sources[0]);
            if (!src) {
                return std::nullopt;
            }
            const auto result = ins.opcode == ScalarOp::not_b32 ? ~*src : *src;
            state.sgpr[*ins.dst_sgpr] = result;
            if (ins.opcode == ScalarOp::not_b32) {
                state.scc = result != 0;
            }
            pc = next;
            continue;
        }
        if (ins.source_count != 2) {
            return std::nullopt;
        }
        const auto lhs = source_value(state, ins.sources[0]);
        const auto rhs = source_value(state, ins.sources[1]);
        if (!lhs || !rhs) {
            return std::nullopt;
        }
        if (ins.opcode == ScalarOp::cmp_eq_u32) {
            state.scc = *lhs == *rhs;
            pc = next;
            continue;
        }
        if (!ins.dst_sgpr) {
            return std::nullopt;
        }
        std::uint32_t result = 0;
        switch (ins.opcode) {
        case ScalarOp::add_u32:
            result = *lhs + *rhs;
            state.scc = result < *lhs;
            break;
        case ScalarOp::sub_u32:
            result = *lhs - *rhs;
            state.scc = *lhs >= *rhs;
            break;
        case ScalarOp::and_b32:
            result = *lhs & *rhs;
            state.scc = result != 0;
            break;
        case ScalarOp::or_b32:
            result = *lhs | *rhs;
            state.scc = result != 0;
            break;
        case ScalarOp::xor_b32:
            result = *lhs ^ *rhs;
            state.scc = result != 0;
            break;
        default:
            return std::nullopt;
        }
        state.sgpr[*ins.dst_sgpr] = result;
        pc = next;
    }
    return std::nullopt;
}

} // namespace nyxora::gpu::shader
