#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <vector>

namespace nyxora::gpu::shader {

// Initial, deliberately restricted AMD GCN scalar frontend. The raw dword
// offsets are kept to handle 64-bit instructions with literal operands.
enum class ScalarFamily : std::uint8_t { sop1, sop2, sopc, sopk, sopp };
enum class ScalarOp : std::uint8_t {
    nop, end_program, branch, branch_scc0, branch_scc1,
    branch_vccz, branch_vccnz, branch_execz, branch_execnz,
    barrier, waitcnt, mov_b32, not_b32, add_u32, sub_u32,
    and_b32, or_b32, xor_b32, cmp_eq_u32, movk_i32,
};
enum class ScalarSourceKind : std::uint8_t { sgpr, constant };

struct ScalarSource {
    ScalarSourceKind kind{};
    // The scalar register number or raw uint32_t bits of the constant.
    std::uint32_t value{};
};

struct ScalarDecodedInstruction {
    std::uint32_t offset_dwords{};
    std::uint8_t word_count{};
    ScalarFamily family{};
    ScalarOp opcode{};
    std::optional<std::uint8_t> dst_sgpr;
    std::array<ScalarSource, 2> sources{};
    std::uint8_t source_count{};
    // Used by SOPP and SOPK, sign-extended to 32 bits.
    std::int32_t immediate{};
    std::optional<std::uint32_t> branch_target_dwords;
};

struct ScalarDecodedProgram {
    std::vector<ScalarDecodedInstruction> instructions;
    std::uint32_t dword_count{};
};

// Only the listed opcodes and sources are supported; a literal source
// consumes exactly one additional word. Every branch destination must be an
// instruction boundary, and the last instruction must be S_ENDPGM.
[[nodiscard]] std::optional<ScalarDecodedProgram>
decode_scalar_program(std::span<const std::uint32_t> words);

[[nodiscard]] const char* scalar_op_name(ScalarOp opcode);
void write_scalar_program_json(std::ostream& out, const ScalarDecodedProgram& program);

[[nodiscard]] bool is_scalar_conditional_branch(ScalarOp opcode) noexcept;
[[nodiscard]] bool is_scalar_branch(ScalarOp opcode) noexcept;

} // namespace nyxora::gpu::shader
