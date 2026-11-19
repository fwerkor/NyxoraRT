#include "nyxora/gpu/shader/scalar_cfg.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <ostream>
#include <set>
#include <utility>
#include <vector>

namespace nyxora::gpu::shader {

std::optional<ScalarControlFlowGraph>
build_scalar_cfg(ScalarDecodedProgram program) {
    if (program.instructions.empty() || program.dword_count == 0 ||
        program.dword_count > 65536U ||
        program.instructions.back().opcode != ScalarOp::end_program) {
        return std::nullopt;
    }

    const auto invalid = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> offset_to_index(program.dword_count, invalid);
    std::uint32_t cursor = 0;
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        const auto& ins = program.instructions[i];
        if (ins.offset_dwords != cursor || ins.word_count == 0 ||
            ins.word_count > 2 || ins.word_count > program.dword_count - cursor ||
            (ins.opcode == ScalarOp::end_program && i + 1 != program.instructions.size())) {
            return std::nullopt;
        }
        offset_to_index[cursor] = i;
        cursor += ins.word_count;
    }
    if (cursor != program.dword_count) {
        return std::nullopt;
    }

    std::set<std::uint32_t> leaders{0};
    for (const auto& ins : program.instructions) {
        if (is_scalar_branch(ins.opcode)) {
            if (!ins.branch_target_dwords ||
                *ins.branch_target_dwords >= program.dword_count ||
                offset_to_index[*ins.branch_target_dwords] == invalid) {
                return std::nullopt;
            }
            leaders.insert(*ins.branch_target_dwords);
            const auto next = ins.offset_dwords + ins.word_count;
            if (next < program.dword_count) {
                leaders.insert(next);
            }
        } else if (ins.branch_target_dwords) {
            return std::nullopt;
        }
    }

    ScalarControlFlowGraph graph{.program = std::move(program), .blocks = {}};
    std::map<std::uint32_t, std::uint32_t> label_to_block;
    std::vector<std::uint32_t> sorted(leaders.begin(), leaders.end());
    graph.blocks.reserve(sorted.size());
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const auto start = sorted[i];
        const auto end = i + 1 < sorted.size() ? sorted[i + 1] : graph.program.dword_count;
        const auto first = offset_to_index[start];
        const auto last = end == graph.program.dword_count
                              ? graph.program.instructions.size()
                              : offset_to_index[end];
        if (first == invalid || last == invalid || first >= last) {
            return std::nullopt;
        }
        const auto id = static_cast<std::uint32_t>(i);
        graph.blocks.push_back(ScalarBasicBlock{
            .id = id,
            .start_dword = start,
            .end_dword = end,
            .first_instruction = first,
            .instruction_count = last - first,
            .successors = {},
            .predecessors = {},
            .reachable = false,
        });
        label_to_block.emplace(start, id);
    }

    for (auto& block : graph.blocks) {
        const auto& last = graph.program.instructions[
            block.first_instruction + block.instruction_count - 1];
        auto add_edge = [&](std::uint32_t label) {
            const auto it = label_to_block.find(label);
            if (it == label_to_block.end()) {
                return false;
            }
            if (std::find(block.successors.begin(), block.successors.end(), it->second) ==
                block.successors.end()) {
                block.successors.push_back(it->second);
            }
            return true;
        };
        if (is_scalar_branch(last.opcode)) {
            if (!add_edge(*last.branch_target_dwords)) {
                return std::nullopt;
            }
            if (is_scalar_conditional_branch(last.opcode) &&
                block.end_dword < graph.program.dword_count &&
                !add_edge(block.end_dword)) {
                return std::nullopt;
            }
        } else if (last.opcode != ScalarOp::end_program &&
                   block.end_dword < graph.program.dword_count &&
                   !add_edge(block.end_dword)) {
            return std::nullopt;
        }
    }

    for (const auto& block : graph.blocks) {
        for (const auto successor : block.successors) {
            graph.blocks[successor].predecessors.push_back(block.id);
        }
    }

    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        auto& block = graph.blocks[id];
        if (block.reachable) {
            continue;
        }
        block.reachable = true;
        for (const auto successor : block.successors) {
            if (!graph.blocks[successor].reachable) {
                stack.push_back(successor);
            }
        }
    }
    return graph;
}

void write_scalar_cfg_json(std::ostream& out, const ScalarControlFlowGraph& graph) {
    out << "{\"isa\":\"scalar-cfg\",\"dwords\":" << graph.program.dword_count
        << ",\"blocks\":[";
    for (std::size_t i = 0; i < graph.blocks.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        const auto& block = graph.blocks[i];
        out << "{\"id\":" << block.id << ",\"start\":" << block.start_dword
            << ",\"end\":" << block.end_dword
            << ",\"first_instruction\":" << block.first_instruction
            << ",\"instruction_count\":" << block.instruction_count
            << ",\"reachable\":" << (block.reachable ? "true" : "false")
            << ",\"successors\":[";
        for (std::size_t j = 0; j < block.successors.size(); ++j) {
            if (j != 0) {
                out << ",";
            }
            out << block.successors[j];
        }
        out << "],\"predecessors\":[";
        for (std::size_t j = 0; j < block.predecessors.size(); ++j) {
            if (j != 0) {
                out << ",";
            }
            out << block.predecessors[j];
        }
        out << "]}";
    }
    out << "]}\n";
}

} // namespace nyxora::gpu::shader
