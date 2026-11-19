#pragma once

#include "nyxora/gpu/shader/scalar_cfg.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace nyxora::gpu::shader {

// Translates only straight-line, side-effect-free-on-GPU scalar ALU programs
// into a diagnostic Vulkan 1.1 compute module. Binding 0 of descriptor set 0
// is one 103-element uint32 storage buffer: SGPR[0..101] and SCC at index 102.
// Dispatch exactly one workgroup with a single invocation. This is NOT a
// translator for graphics, vector ops, barriers, wave semantics or PS5 shaders.
[[nodiscard]] std::optional<std::vector<std::uint32_t>>
compile_scalar_diagnostic_spirv(const ScalarControlFlowGraph& cfg);

} // namespace nyxora::gpu::shader
