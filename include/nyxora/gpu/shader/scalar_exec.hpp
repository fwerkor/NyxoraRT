#pragma once

#include "nyxora/gpu/shader/scalar_cfg.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>

namespace nyxora::gpu::shader {

struct ScalarMachineState {
    std::array<std::uint32_t, 102> sgpr{};
    bool scc{};
    std::uint64_t steps{};
};

// Reference interpreter for the supported SGPR/SCC scalar subset. It is
// deliberately CPU-only and rejects VCC/EXEC branches, WAITCNT/BARRIER and
// other guest GPU semantics that need wave or device state. max_steps bounds
// even a syntactically valid infinite-loop program.
void write_scalar_state_json(std::ostream& out, const ScalarMachineState& state);

[[nodiscard]] std::optional<ScalarMachineState>
execute_scalar_subset(const ScalarControlFlowGraph& cfg, ScalarMachineState initial = {},
                      std::size_t max_steps = 65536);

} // namespace nyxora::gpu::shader
