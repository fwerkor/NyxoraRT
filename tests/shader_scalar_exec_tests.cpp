#include "test.hpp"
#include "nyxora/gpu/shader/scalar_exec.hpp"

#include <array>
#include <sstream>
#include <string>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t sopk(unsigned dst, unsigned imm) {
    return 0xb0000000U | (dst << 16U) | imm;
}
constexpr std::uint32_t sop1(unsigned op, unsigned dst, unsigned src) {
    return 0xbe800000U | (dst << 16U) | (op << 8U) | src;
}
constexpr std::uint32_t sop2(unsigned op, unsigned dst, unsigned lhs, unsigned rhs) {
    return 0x80000000U | (op << 23U) | (dst << 16U) | (rhs << 8U) | lhs;
}
constexpr std::uint32_t sopc(unsigned op, unsigned lhs, unsigned rhs) {
    return 0xbf000000U | (op << 16U) | (rhs << 8U) | lhs;
}
constexpr std::uint32_t sopp(unsigned op, unsigned imm = 0) {
    return 0xbf800000U | (op << 16U) | imm;
}

std::optional<nyxora::gpu::shader::ScalarMachineState>
run(std::span<const std::uint32_t> words, std::size_t max_steps = 65536) {
    const auto decoded = nyxora::gpu::shader::decode_scalar_program(words);
    if (!decoded) {
        return std::nullopt;
    }
    const auto cfg = nyxora::gpu::shader::build_scalar_cfg(*decoded);
    return cfg ? nyxora::gpu::shader::execute_scalar_subset(*cfg, {}, max_steps)
               : std::nullopt;
}

} // namespace

NYXORA_TEST(shader_scalar_exec_wraps_add_and_sets_carry_in_scc) {
    const std::array<std::uint32_t, 4> code{
        sopk(0, 0xffff), sop2(0, 1, 0, 129), sop1(3, 2, 1), sopp(1),
    };
    const auto state = run(code);
    NYXORA_CHECK(state);
    NYXORA_CHECK(state->sgpr[0] == 0xffffffffU);
    NYXORA_CHECK(state->sgpr[1] == 0);
    NYXORA_CHECK(state->sgpr[2] == 0);
    NYXORA_CHECK(state->scc && state->steps == 4);
}

NYXORA_TEST(shader_scalar_exec_subtract_no_borrow_semantics) {
    const std::array<std::uint32_t, 5> code{
        sopk(0, 7), sop2(1, 1, 0, 129),
        sop2(1, 2, 129, 0), sop2(1, 3, 0, 0), sopp(1),
    };
    const auto state = run(code);
    NYXORA_CHECK(state);
    NYXORA_CHECK(state->sgpr[1] == 6);
    NYXORA_CHECK(state->sgpr[2] == 0xfffffffaU);
    NYXORA_CHECK(state->sgpr[3] == 0);
    NYXORA_CHECK(state->scc);
}

NYXORA_TEST(shader_scalar_exec_bitwise_ops_and_scc_updates) {
    const std::array<std::uint32_t, 7> code{
        sopk(0, 0x00ff), sop1(7, 1, 0), sop2(0x0e, 2, 1, 128),
        sop2(0x10, 3, 2, 129), sop2(0x12, 4, 3, 193),
        sopc(6, 4, 0), sopp(1),
    };
    const auto state = run(code);
    NYXORA_CHECK(state);
    NYXORA_CHECK(state->sgpr[0] == 255U);
    NYXORA_CHECK(state->sgpr[1] == 0xffffff00U);
    NYXORA_CHECK(state->sgpr[2] == 0);
    NYXORA_CHECK(state->sgpr[3] == 1);
    NYXORA_CHECK(state->sgpr[4] == 0xfffffffeU);
    NYXORA_CHECK(!state->scc);
}

NYXORA_TEST(shader_scalar_exec_backward_scc_loop_counts_to_zero) {
    const std::array<std::uint32_t, 6> code{
        sopk(0, 3), sopk(1, 1), sop2(1, 0, 0, 1),
        sopc(6, 0, 128), sopp(4, 0xfffd), sopp(1),
    };
    const auto state = run(code);
    NYXORA_CHECK(state && state->sgpr[0] == 0);
    NYXORA_CHECK(state->scc);
    NYXORA_CHECK(state->steps == 12);
    NYXORA_CHECK(!run(code, 11));
}

NYXORA_TEST(shader_scalar_exec_conditional_scc_branch_skips_instructions) {
    const std::array<std::uint32_t, 6> code{
        sopk(0, 1), sopc(6, 0, 129), sopp(5, 1),
        sopk(1, 7), sopk(1, 9), sopp(1),
    };
    const auto state = run(code);
    NYXORA_CHECK(state && state->sgpr[1] == 9 && state->scc);
    NYXORA_CHECK(state->steps == 5);
}

NYXORA_TEST(shader_scalar_exec_serializes_machine_state) {
    const std::array<std::uint32_t, 2> code{sopk(0, 9), sopp(1)};
    const auto state = run(code);
    NYXORA_CHECK(state);
    std::ostringstream out;
    nyxora::gpu::shader::write_scalar_state_json(out, *state);
    const auto json = out.str();
    NYXORA_CHECK(json.find("\"sgpr\":[9,0,0") != std::string::npos);
    NYXORA_CHECK(json.find("\"scc\":0,\"steps\":2") != std::string::npos);
    NYXORA_CHECK(json.back() == '\n');
}

NYXORA_TEST(shader_scalar_exec_respects_initial_registers) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 2> code{sop2(0, 12, 10, 11), sopp(1)};
    const auto decoded = decode_scalar_program(code);
    NYXORA_CHECK(decoded);
    const auto cfg = build_scalar_cfg(*decoded);
    NYXORA_CHECK(cfg);
    ScalarMachineState initial{};
    initial.sgpr[10] = 0xfffffff0U;
    initial.sgpr[11] = 0x30U;
    initial.steps = 9000;
    const auto state = execute_scalar_subset(*cfg, initial);
    NYXORA_CHECK(state && state->sgpr[12] == 0x20U);
    NYXORA_CHECK(state->scc && state->steps == 2);
}

NYXORA_TEST(shader_scalar_exec_rejects_wave_dependent_and_sync_instructions) {
    const std::array<std::uint32_t, 2> vcc{sopp(6), sopp(1)};
    const std::array<std::uint32_t, 2> barrier{sopp(10), sopp(1)};
    const std::array<std::uint32_t, 2> waitcnt{sopp(12), sopp(1)};
    NYXORA_CHECK(!run(vcc));
    NYXORA_CHECK(!run(barrier));
    NYXORA_CHECK(!run(waitcnt));
}

NYXORA_TEST(shader_scalar_exec_bounds_infinite_loops_and_bad_budgets) {
    const std::array<std::uint32_t, 2> loop{sopp(2, 0xffff), sopp(1)};
    NYXORA_CHECK(!run(loop, 4));
    NYXORA_CHECK(!run(loop, 0));
    NYXORA_CHECK(!run(loop, 1'000'001));
}
