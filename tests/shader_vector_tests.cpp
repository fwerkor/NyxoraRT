#include "test.hpp"
#include "nyxora/gpu/shader/vector.hpp"

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr std::uint32_t vop1(std::uint32_t op, std::uint32_t dst, std::uint32_t src) {
    return 0x7e000000U | (dst << 17U) | (op << 9U) | src;
}
constexpr std::uint32_t vop2(std::uint32_t op, std::uint32_t dst,
                             std::uint32_t first, std::uint32_t second) {
    return (op << 25U) | (dst << 17U) | (second << 9U) | first;
}
constexpr std::uint32_t endpgm = 0xbf810000U;
}

NYXORA_TEST(vector_decode_vop1_vop2_full_literal_words_and_source_registers) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 6> code{
        vop1(1, 0, 255), 0xdeadbeefU, vop2(0x25, 1, 256, 0),
        vop2(0x1d, 2, 129, 1), vop1(0x37, 3, 258), endpgm,
    };
    const auto decoded = decode_vector_program(code);
    NYXORA_CHECK(decoded && decoded->instructions.size() == 5);
    NYXORA_CHECK(decoded->dword_count == code.size());
    NYXORA_CHECK(decoded->instructions[0].word_count == 2);
    NYXORA_CHECK(decoded->instructions[0].sources[0].kind == VectorSourceKind::constant);
    NYXORA_CHECK(decoded->instructions[0].sources[0].value == 0xdeadbeefU);
    NYXORA_CHECK(decoded->instructions[1].offset_dwords == 2);
    NYXORA_CHECK(decoded->instructions[1].sources[0].kind == VectorSourceKind::vgpr);
    NYXORA_CHECK(decoded->instructions[1].sources[0].value == 0);
    NYXORA_CHECK(decoded->instructions[1].sources[1].value == 0);
    NYXORA_CHECK(decoded->instructions[2].sources[0].value == 1);
    NYXORA_CHECK(decoded->instructions[3].sources[0].value == 2);
}

NYXORA_TEST(vector_decode_supports_32bit_integer_alu_opcodes_only) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 10> code{
        vop1(0,0,0), vop1(1,0,128), vop1(0x37,1,256),
        vop2(0x13,2,256,1), vop2(0x14,3,256,1),
        vop2(0x1b,4,256,1), vop2(0x1c,5,256,1),
        vop2(0x26,6,256,1), vop2(0x16,7,129,1), endpgm,
    };
    const auto decoded = decode_vector_program(code);
    NYXORA_CHECK(decoded && decoded->instructions.size() == 10);
    NYXORA_CHECK(decoded->instructions[0].op == VectorOp::nop);
    NYXORA_CHECK(decoded->instructions[3].op == VectorOp::min_u32);
    NYXORA_CHECK(decoded->instructions[4].op == VectorOp::max_u32);
    NYXORA_CHECK(decoded->instructions[5].op == VectorOp::and_b32);
    NYXORA_CHECK(decoded->instructions[6].op == VectorOp::or_b32);
    NYXORA_CHECK(decoded->instructions[7].op == VectorOp::sub_nc_u32);
    NYXORA_CHECK(decoded->instructions[8].op == VectorOp::lshrrev_b32);
}

NYXORA_TEST(vector_decode_rejects_unknown_opcodes_and_unmodelled_sources) {
    using nyxora::gpu::shader::decode_vector_program;
    const std::array<std::uint32_t, 2> unknown_vop1{vop1(4,0,128), endpgm};
    const std::array<std::uint32_t, 2> float_vop2{vop2(3,0,128,0), endpgm};
    const std::array<std::uint32_t, 2> scalar_source{vop1(1,0,0), endpgm};
    const std::array<std::uint32_t, 2> modifier{vop1(1,0,249), endpgm};
    const std::array<std::uint32_t, 2> bad_vop1_nop{vop1(0,1,128), endpgm};
    const std::array<std::uint32_t, 2> unsupported_sopp{0xbf800000U, endpgm};
    const std::array<std::uint32_t, 2> unsupported_vopc{0x7c000000U, endpgm};
    const std::array<std::uint32_t, 1> unterminated{vop1(1,0,128)};
    const std::array<std::uint32_t, 2> early_end{endpgm, endpgm};
    const std::array<std::uint32_t, 0> empty{};
    NYXORA_CHECK(!decode_vector_program(unknown_vop1));
    NYXORA_CHECK(!decode_vector_program(float_vop2));
    NYXORA_CHECK(!decode_vector_program(scalar_source));
    NYXORA_CHECK(!decode_vector_program(modifier));
    NYXORA_CHECK(!decode_vector_program(bad_vop1_nop));
    NYXORA_CHECK(!decode_vector_program(unsupported_sopp));
    NYXORA_CHECK(!decode_vector_program(unsupported_vopc));
    NYXORA_CHECK(!decode_vector_program(unterminated));
    NYXORA_CHECK(!decode_vector_program(early_end));
    NYXORA_CHECK(!decode_vector_program(empty));
}

NYXORA_TEST(vector_execute_lanewise_register_values_and_integer_semantics) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 8> code{
        vop1(1,7,129), vop2(0x25,1,256,7),
        vop2(0x26,2,263,1), vop2(0x1d,3,263,2),
        vop2(0x13,4,258,3), vop2(0x14,5,258,3),
        vop2(0x1a,6,129,5), endpgm,
    };
    const auto program = decode_vector_program(code);
    NYXORA_CHECK(program);
    VectorMachineState initial{};
    for (std::uint32_t lane = 0; lane < diagnostic_wave_lanes; ++lane) {
        initial.vgpr[lane] = lane * 3U;
    }
    const auto output = execute_vector_program(*program, initial);
    NYXORA_CHECK(output);
    for (std::uint32_t lane = 0; lane < diagnostic_wave_lanes; ++lane) {
        const auto reg = [&](std::uint32_t index) {
            return output->vgpr[index * diagnostic_wave_lanes + lane];
        };
        NYXORA_CHECK(reg(0) == lane * 3U && reg(7) == 1);
        NYXORA_CHECK(reg(1) == lane * 3U + 1);
        NYXORA_CHECK(reg(2) == 1U - (lane * 3U + 1U));
        NYXORA_CHECK(reg(3) == (1U ^ reg(2)));
        NYXORA_CHECK(reg(4) == (reg(2) < reg(3) ? reg(2) : reg(3)));
        NYXORA_CHECK(reg(5) == (reg(2) > reg(3) ? reg(2) : reg(3)));
        NYXORA_CHECK(reg(6) == (reg(5) << 1));
    }
}

NYXORA_TEST(vector_execute_32bit_shifts_and_overflow) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 6> code{
        vop1(1,0,255), 0xffffffffU,
        vop2(0x25,1,256,0),
        vop2(0x16,2,160,0),
        vop2(0x1a,3,160,0), endpgm,
    };
    const auto program = decode_vector_program(code);
    NYXORA_CHECK(program);
    const auto result = execute_vector_program(*program);
    NYXORA_CHECK(result);
    NYXORA_CHECK(result->vgpr[0] == 0xffffffffU);
    NYXORA_CHECK(result->vgpr[diagnostic_wave_lanes] == 0xfffffffeU);
    // src0=160 is inline 32; masked to 0, so shifts are no-ops.
    NYXORA_CHECK(result->vgpr[2 * diagnostic_wave_lanes] == 0xffffffffU);
    NYXORA_CHECK(result->vgpr[3 * diagnostic_wave_lanes] == 0xffffffffU);
}

NYXORA_TEST(vector_execute_fails_closed_for_incorrect_structure) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 2> code{vop1(1,0,128), endpgm};
    const auto decoded = decode_vector_program(code);
    NYXORA_CHECK(decoded);
    auto bad = *decoded;
    bad.instructions[0].source_count = 3;
    NYXORA_CHECK(!execute_vector_program(bad));
    bad = *decoded;
    bad.instructions[0].sources[0] = {VectorSourceKind::vgpr, 256};
    NYXORA_CHECK(!execute_vector_program(bad));
    bad = *decoded;
    bad.instructions[0].sources[0].kind = static_cast<VectorSourceKind>(33);
    NYXORA_CHECK(!execute_vector_program(bad));
    bad = *decoded;
    bad.instructions[1].word_count = 2;
    NYXORA_CHECK(!execute_vector_program(bad));
    bad = *decoded;
    bad.instructions.back().op = VectorOp::nop;
    NYXORA_CHECK(!execute_vector_program(bad));
    bad = *decoded;
    bad.dword_count = 1;
    NYXORA_CHECK(!execute_vector_program(bad));
}


NYXORA_TEST(vector_json_contains_dword_lanes_and_register_major_state) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 2> code{vop1(1, 0, 193), endpgm};
    const auto program = decode_vector_program(code);
    NYXORA_CHECK(program);
    std::ostringstream decoded;
    write_vector_program_json(decoded, *program);
    const auto json = decoded.str();
    NYXORA_CHECK(json.find("\"isa\":\"vop1-vop2-subset\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"lanes\":32") != std::string::npos);
    NYXORA_CHECK(json.find("\"kind\":\"constant\",\"value\":4294967295") != std::string::npos);
    const auto state = execute_vector_program(*program);
    NYXORA_CHECK(state);
    std::ostringstream output;
    write_vector_state_json(output, *state);
    const auto result = output.str();
    NYXORA_CHECK(result.find("\"vgpr\":[4294967295,4294967295") != std::string::npos);
    NYXORA_CHECK(result.back() == '\n');
}
