#include "test.hpp"
#include "nyxora/gpu/shader/scalar_decode.hpp"

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t sop1(unsigned op, unsigned dst, unsigned src) {
    return 0xbe800000U | (dst << 16U) | (op << 8U) | src;
}
constexpr std::uint32_t sop2(unsigned op, unsigned dst, unsigned lhs, unsigned rhs) {
    return 0x80000000U | (op << 23U) | (dst << 16U) | (rhs << 8U) | lhs;
}
constexpr std::uint32_t sopc(unsigned op, unsigned lhs, unsigned rhs) {
    return 0xbf000000U | (op << 16U) | (rhs << 8U) | lhs;
}
constexpr std::uint32_t sopk(unsigned op, unsigned dst, unsigned immediate) {
    return 0xb0000000U | (op << 23U) | (dst << 16U) | immediate;
}
constexpr std::uint32_t sopp(unsigned op, unsigned immediate = 0) {
    return 0xbf800000U | (op << 16U) | immediate;
}

} // namespace

NYXORA_TEST(shader_scalar_decode_all_five_families_and_register_sources) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 7> words{
        sopk(0, 0, 0xfffd), sop1(3, 1, 0), sop2(0, 2, 1, 131),
        sop1(7, 3, 2), sopc(6, 2, 3), sop2(0x12, 4, 2, 3), sopp(1),
    };
    const auto decoded = decode_scalar_program(words);
    NYXORA_CHECK(decoded && decoded->instructions.size() == 7);
    NYXORA_CHECK(decoded->dword_count == 7);
    const auto& ins = decoded->instructions;
    NYXORA_CHECK(ins[0].family == ScalarFamily::sopk && ins[0].opcode == ScalarOp::movk_i32);
    NYXORA_CHECK(ins[0].immediate == -3 && ins[0].dst_sgpr == 0);
    NYXORA_CHECK(ins[0].sources[0].kind == ScalarSourceKind::constant);
    NYXORA_CHECK(ins[0].sources[0].value == static_cast<std::uint32_t>(-3));
    NYXORA_CHECK(ins[1].family == ScalarFamily::sop1 && ins[1].opcode == ScalarOp::mov_b32);
    NYXORA_CHECK(ins[1].sources[0].kind == ScalarSourceKind::sgpr && ins[1].sources[0].value == 0);
    NYXORA_CHECK(ins[2].family == ScalarFamily::sop2 && ins[2].opcode == ScalarOp::add_u32);
    NYXORA_CHECK(ins[2].source_count == 2 && ins[2].sources[1].value == 3);
    NYXORA_CHECK(ins[3].opcode == ScalarOp::not_b32);
    NYXORA_CHECK(ins[4].family == ScalarFamily::sopc && ins[4].opcode == ScalarOp::cmp_eq_u32);
    NYXORA_CHECK(!ins[4].dst_sgpr && ins[4].source_count == 2);
    NYXORA_CHECK(ins[5].opcode == ScalarOp::xor_b32);
    NYXORA_CHECK(ins[6].family == ScalarFamily::sopp &&
                 ins[6].opcode == ScalarOp::end_program);
}

NYXORA_TEST(shader_scalar_decode_literal_uses_two_dwords_shared_by_sources) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 4> words{
        sop2(0, 3, 255, 255), 0xdeadbeefU,
        sop1(3, 4, 193), sopp(1),
    };
    const auto decoded = decode_scalar_program(words);
    NYXORA_CHECK(decoded && decoded->instructions.size() == 3);
    NYXORA_CHECK(decoded->instructions[0].word_count == 2);
    NYXORA_CHECK(decoded->instructions[0].sources[0].value == 0xdeadbeefU);
    NYXORA_CHECK(decoded->instructions[0].sources[1].value == 0xdeadbeefU);
    NYXORA_CHECK(decoded->instructions[1].offset_dwords == 2);
    NYXORA_CHECK(decoded->instructions[1].sources[0].value == 0xffffffffU);
    NYXORA_CHECK(decoded->instructions[2].offset_dwords == 3);
}

NYXORA_TEST(shader_scalar_decode_branch_target_skips_literal_correctly) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 5> words{
        sopp(2, 2), sop1(3, 0, 255), 0x34567891U, sopp(0), sopp(1),
    };
    const auto decoded = decode_scalar_program(words);
    NYXORA_CHECK(decoded && decoded->instructions.size() == 4);
    NYXORA_CHECK(decoded->instructions[0].branch_target_dwords == 3U);
    NYXORA_CHECK(decoded->instructions[2].offset_dwords == 3);
}

NYXORA_TEST(shader_scalar_decode_rejects_branch_into_literal_word) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 4> words{
        sopp(2, 1), sop1(3, 0, 255), 0xfeedfaceU, sopp(1),
    };
    NYXORA_CHECK(!decode_scalar_program(words));
}

NYXORA_TEST(shader_scalar_decode_rejects_unsupported_sources_and_destinations) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 2> invalid_source{sop1(3, 0, 111), sopp(1)};
    NYXORA_CHECK(!decode_scalar_program(invalid_source));
    const std::array<std::uint32_t, 2> invalid_dest{sop1(3, 126, 128), sopp(1)};
    NYXORA_CHECK(!decode_scalar_program(invalid_dest));
    const std::array<std::uint32_t, 2> invalid_sop2{sop2(0, 2, 220, 130), sopp(1)};
    NYXORA_CHECK(!decode_scalar_program(invalid_sop2));
    const std::array<std::uint32_t, 2> bad_literal{sop1(3, 1, 255), sopp(1)};
    // END_PGM can serve as a literal, but then there is no terminator.
    NYXORA_CHECK(!decode_scalar_program(bad_literal));
}

NYXORA_TEST(shader_scalar_decode_rejects_unsupported_opcodes_in_each_family) {
    using nyxora::gpu::shader::decode_scalar_program;
    const std::array<std::uint32_t, 2> sop1_bad{sop1(32, 0, 128), sopp(1)};
    const std::array<std::uint32_t, 2> sop2_bad{sop2(35, 0, 128, 128), sopp(1)};
    const std::array<std::uint32_t, 2> sopc_bad{sopc(12, 128, 128), sopp(1)};
    const std::array<std::uint32_t, 2> sopk_bad{sopk(15, 0, 0), sopp(1)};
    const std::array<std::uint32_t, 2> sopp_bad{sopp(100), sopp(1)};
    const std::array<std::uint32_t, 2> other_bad{0xd0010203U, sopp(1)};
    NYXORA_CHECK(!decode_scalar_program(sop1_bad));
    NYXORA_CHECK(!decode_scalar_program(sop2_bad));
    NYXORA_CHECK(!decode_scalar_program(sopc_bad));
    NYXORA_CHECK(!decode_scalar_program(sopk_bad));
    NYXORA_CHECK(!decode_scalar_program(sopp_bad));
    NYXORA_CHECK(!decode_scalar_program(other_bad));
}

NYXORA_TEST(shader_scalar_decode_must_end_and_validate_branches) {
    using nyxora::gpu::shader::decode_scalar_program;
    const std::array<std::uint32_t, 0> empty{};
    const std::array<std::uint32_t, 1> no_end{sopp(0)};
    const std::array<std::uint32_t, 2> early_end{sopp(1), sopp(0)};
    const std::array<std::uint32_t, 2> negative_target{sopp(2, 0xfffe), sopp(1)};
    const std::array<std::uint32_t, 2> past_end{sopp(2, 100), sopp(1)};
    NYXORA_CHECK(!decode_scalar_program(empty));
    NYXORA_CHECK(!decode_scalar_program(no_end));
    NYXORA_CHECK(!decode_scalar_program(early_end));
    NYXORA_CHECK(!decode_scalar_program(negative_target));
    NYXORA_CHECK(!decode_scalar_program(past_end));
    std::vector<std::uint32_t> too_long(65537, sopp(0));
    NYXORA_CHECK(!decode_scalar_program(too_long));
}

NYXORA_TEST(shader_scalar_decode_conditional_branches_preserve_targets) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 4> words{sopp(4, 1), sopp(6, 0), sopp(0), sopp(1)};
    const auto decoded = decode_scalar_program(words);
    NYXORA_CHECK(decoded && decoded->instructions[0].branch_target_dwords == 2U);
    NYXORA_CHECK(decoded->instructions[1].branch_target_dwords == 2U);
    NYXORA_CHECK(is_scalar_branch(decoded->instructions[0].opcode));
    NYXORA_CHECK(is_scalar_conditional_branch(decoded->instructions[1].opcode));
    NYXORA_CHECK(!is_scalar_branch(ScalarOp::waitcnt));
    NYXORA_CHECK(is_scalar_branch(ScalarOp::branch));
    NYXORA_CHECK(!is_scalar_conditional_branch(ScalarOp::branch));
}

NYXORA_TEST(shader_scalar_decode_json_represents_typed_operands_and_dword_widths) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 4> words{
        sop1(3, 0, 255), 0x12345678U, sopc(6, 0, 193), sopp(1),
    };
    const auto decoded = decode_scalar_program(words);
    NYXORA_CHECK(decoded);
    std::ostringstream output;
    write_scalar_program_json(output, *decoded);
    const auto json = output.str();
    NYXORA_CHECK(json.find("\"isa\":\"scalar-subset\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"dwords\":4") != std::string::npos);
    NYXORA_CHECK(json.find("\"size\":2,\"op\":\"s_mov_b32\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"kind\":\"constant\",\"value\":305419896") != std::string::npos);
    NYXORA_CHECK(json.find("\"kind\":\"sgpr\",\"value\":0") != std::string::npos);
    NYXORA_CHECK(json.find("\"op\":\"s_cmp_eq_u32\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"value\":4294967295") != std::string::npos);
    NYXORA_CHECK(!json.empty() && json.back() == '\n');
}

NYXORA_TEST(shader_scalar_decode_one_word_add_sub_boolean_variants) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 6> words{
        sop2(0, 0, 128, 129), sop2(1, 1, 0, 129),
        sop2(0x0e, 2, 1, 128), sop2(0x10, 3, 2, 129),
        sop2(0x12, 4, 3, 193), sopp(1),
    };
    const auto decoded = decode_scalar_program(words);
    NYXORA_CHECK(decoded && decoded->instructions.size() == 6);
    NYXORA_CHECK(decoded->instructions[0].opcode == ScalarOp::add_u32);
    NYXORA_CHECK(decoded->instructions[1].opcode == ScalarOp::sub_u32);
    NYXORA_CHECK(decoded->instructions[2].opcode == ScalarOp::and_b32);
    NYXORA_CHECK(decoded->instructions[3].opcode == ScalarOp::or_b32);
    NYXORA_CHECK(decoded->instructions[4].opcode == ScalarOp::xor_b32);
}
