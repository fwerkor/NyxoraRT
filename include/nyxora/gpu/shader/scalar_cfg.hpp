#pragma once

#include "nyxora/gpu/shader/scalar_decode.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <vector>

namespace nyxora::gpu::shader {

struct ScalarBasicBlock {
    std::uint32_t id{};
    std::uint32_t start_dword{};
    std::uint32_t end_dword{}; // exclusive
    std::size_t first_instruction{};
    std::size_t instruction_count{};
    std::vector<std::uint32_t> successors;
    std::vector<std::uint32_t> predecessors;
    bool reachable{};
};

struct ScalarControlFlowGraph {
    ScalarDecodedProgram program;
    std::vector<ScalarBasicBlock> blocks;
};

// Computes branches at instruction boundaries including literal dwords,
// unreachable blocks, backedges and conditional fallthrough edges.
[[nodiscard]] std::optional<ScalarControlFlowGraph>
build_scalar_cfg(ScalarDecodedProgram program);

void write_scalar_cfg_json(std::ostream& out, const ScalarControlFlowGraph& graph);

} // namespace nyxora::gpu::shader
