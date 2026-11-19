#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <vector>

namespace nyxora::gpu::shader {

inline constexpr std::uint32_t diagnostic_wave_lanes = 32;
inline constexpr std::uint32_t diagnostic_vgpr_count = 256;

enum class VectorSourceKind : std::uint8_t { vgpr, constant };
enum class VectorOp : std::uint8_t {
    nop, end_program, mov_b32, not_b32, add_nc_u32, sub_nc_u32,
    and_b32, or_b32, xor_b32, min_u32, max_u32,
    lshlrev_b32, lshrrev_b32,
};

struct VectorSource {
    VectorSourceKind kind{};
    std::uint32_t value{}; // VGPR number or full uint32_t constant bits
};

struct VectorInstruction {
    std::uint32_t offset_dwords{};
    std::uint8_t word_count{};
    VectorOp op{};
    std::uint8_t destination{};
    std::array<VectorSource, 2> sources{};
    std::uint8_t source_count{};
};

struct VectorProgram {
    std::vector<VectorInstruction> instructions;
    std::uint32_t dword_count{};
};

[[nodiscard]] std::optional<VectorProgram>
decode_vector_program(std::span<const std::uint32_t> words);
[[nodiscard]] const char* vector_op_name(VectorOp op) noexcept;
void write_vector_program_json(std::ostream& out, const VectorProgram& program);

// Lanes are stored register-major: VGPR[register * 32 + lane]. A strict
// single-wave diagnostic model with no EXEC masks, VCC or cross-lane effects.
struct VectorMachineState {
    std::array<std::uint32_t, diagnostic_vgpr_count * diagnostic_wave_lanes> vgpr{};
};

[[nodiscard]] VectorMachineState make_seeded_vector_state();

[[nodiscard]] std::optional<VectorMachineState>
execute_vector_program(const VectorProgram& program, VectorMachineState initial = {});

void write_vector_state_json(std::ostream& out, const VectorMachineState& state);

// Vulkan diagnostic kernel uses descriptor set 0 / binding 0: 8192 uint32_t,
// one workgroup of 32 local invocations. Only whitelisted vector ALU operations.
[[nodiscard]] std::optional<std::vector<std::uint32_t>>
compile_vector_diagnostic_spirv(const VectorProgram& program);

} // namespace nyxora::gpu::shader
