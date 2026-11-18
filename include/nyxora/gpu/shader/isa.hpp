#pragma once

#include "nyxora/gpu/guest_resources.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nyxora::gpu::shader {

// Initial RDNA/GCN SOPP-only control-flow decoder for synthetic homebrew
// programs. Other ISA formats (SOP1/SOP2/VALU/VMEM/DS/...) are not decoded.
enum class ScalarOpcode : std::uint8_t {
    nop,
    end_program,
    branch,
    branch_scc0,
    branch_scc1,
    branch_vccz,
    branch_vccnz,
    branch_execz,
    branch_execnz,
    barrier,
    waitcnt,
};

struct ScalarInstruction {
    std::uint32_t offset_dwords{};
    ScalarOpcode opcode{};
    std::int16_t immediate{};
    std::optional<std::uint32_t> branch_target_dwords;
};

struct ScalarProgram {
    std::vector<ScalarInstruction> instructions;
};

// All words must belong to the currently supported SOPP subset, the program
// must terminate with S_ENDPGM, and all branch destinations must be in bounds.
[[nodiscard]] std::optional<ScalarProgram>
decode_sopp_program(std::span<const std::uint32_t> words);

[[nodiscard]] std::optional<ScalarProgram>
decode_guest_sopp_program(const GuestMemoryReader& reader, const pm4::ShaderProgram& binding,
                          std::size_t word_count);

} // namespace nyxora::gpu::shader
