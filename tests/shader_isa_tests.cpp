#include "test.hpp"

#include "nyxora/gpu/shader/isa.hpp"
#include "nyxora/gpu/pm4.hpp"
#include "pm4_fixture.hpp"
#include <variant>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <sstream>
#include <string>

namespace {

constexpr std::uint32_t sopp(std::uint32_t opcode, std::uint16_t immediate = 0) {
    return 0xbf800000U | (opcode << 16U) | immediate;
}

constexpr auto rw = nyxora::memory::Protection::read | nyxora::memory::Protection::write;

} // namespace

NYXORA_TEST(shader_sopp_decodes_nop_branch_and_termination) {
    using nyxora::gpu::shader::ScalarOpcode;
    const std::array<std::uint32_t, 4> words{
        sopp(0, 0), sopp(2, 1), sopp(0, 3), sopp(1, 0),
    };
    const auto decoded = nyxora::gpu::shader::decode_sopp_program(words);
    NYXORA_CHECK(decoded && decoded->instructions.size() == words.size());
    NYXORA_CHECK(decoded->instructions[0].opcode == ScalarOpcode::nop);
    NYXORA_CHECK(!decoded->instructions[0].branch_target_dwords);
    NYXORA_CHECK(decoded->instructions[1].opcode == ScalarOpcode::branch);
    NYXORA_CHECK(decoded->instructions[1].branch_target_dwords == 3U);
    NYXORA_CHECK(decoded->instructions[1].immediate == 1);
    NYXORA_CHECK(decoded->instructions[3].opcode == ScalarOpcode::end_program);
}

NYXORA_TEST(shader_sopp_decodes_conditional_negative_branches) {
    using nyxora::gpu::shader::ScalarOpcode;
    const std::array<std::uint32_t, 9> words{
        sopp(0), sopp(4, 0xfffe), sopp(5, 2), sopp(6, 0),
        sopp(7, 0), sopp(8, 0), sopp(9, 0), sopp(10), sopp(1),
    };
    const auto decoded = nyxora::gpu::shader::decode_sopp_program(words);
    NYXORA_CHECK(decoded && decoded->instructions.size() == 9);
    NYXORA_CHECK(decoded->instructions[1].immediate == -2);
    NYXORA_CHECK(decoded->instructions[1].branch_target_dwords == 0U);
    NYXORA_CHECK(decoded->instructions[1].opcode == ScalarOpcode::branch_scc0);
    NYXORA_CHECK(decoded->instructions[2].branch_target_dwords == 5U);
    NYXORA_CHECK(decoded->instructions[2].opcode == ScalarOpcode::branch_scc1);
    NYXORA_CHECK(decoded->instructions[3].opcode == ScalarOpcode::branch_vccz);
    NYXORA_CHECK(decoded->instructions[4].opcode == ScalarOpcode::branch_vccnz);
    NYXORA_CHECK(decoded->instructions[5].opcode == ScalarOpcode::branch_execz);
    NYXORA_CHECK(decoded->instructions[6].opcode == ScalarOpcode::branch_execnz);
    NYXORA_CHECK(decoded->instructions[7].opcode == ScalarOpcode::barrier);
}

NYXORA_TEST(shader_sopp_rejects_bad_encoding_and_incomplete_programs) {
    using nyxora::gpu::shader::decode_sopp_program;
    const std::array<std::uint32_t, 0> empty{};
    NYXORA_CHECK(!decode_sopp_program(empty));
    const std::array<std::uint32_t, 1> no_end{sopp(0)};
    NYXORA_CHECK(!decode_sopp_program(no_end));
    const std::array<std::uint32_t, 2> trailing{sopp(1), sopp(0)};
    NYXORA_CHECK(!decode_sopp_program(trailing));
    const std::array<std::uint32_t, 2> unknown{sopp(3), sopp(1)};
    NYXORA_CHECK(!decode_sopp_program(unknown));
    const std::array<std::uint32_t, 2> invalid{0xfeedfaceU, sopp(1)};
    NYXORA_CHECK(!decode_sopp_program(invalid));
    const std::array<std::uint32_t, 2> branch_past{sopp(2, 4), sopp(1)};
    NYXORA_CHECK(!decode_sopp_program(branch_past));
    const std::array<std::uint32_t, 2> branch_before{sopp(2, 0xfffd), sopp(1)};
    NYXORA_CHECK(!decode_sopp_program(branch_before));
    std::vector<std::uint32_t> oversized(65537U, sopp(0));
    NYXORA_CHECK(!decode_sopp_program(oversized));
}


NYXORA_TEST(shader_sopp_json_preserves_branch_targets_and_signed_immediates) {
    const std::array<std::uint32_t, 3> words{
        sopp(0), sopp(2, 0xfffe), sopp(1),
    };
    const auto decoded = nyxora::gpu::shader::decode_sopp_program(words);
    NYXORA_CHECK(decoded);
    std::ostringstream out;
    nyxora::gpu::shader::write_sopp_json(out, *decoded);
    const auto json = out.str();
    NYXORA_CHECK(json.find("\"isa\":\"sopp-subset\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"opcode\":\"s_branch\",\"immediate\":-2") !=
                 std::string::npos);
    NYXORA_CHECK(json.find("\"branch_target_dwords\":0") != std::string::npos);
    NYXORA_CHECK(json.find("\"opcode\":\"s_endpgm\"") != std::string::npos);
    NYXORA_CHECK(!json.empty() && json.back() == '\n');
}

NYXORA_TEST(shader_sopp_waitcnt_is_preserved_without_executing) {
    using nyxora::gpu::shader::ScalarOpcode;
    const std::array<std::uint32_t, 2> program{sopp(12, 0x7f), sopp(1)};
    const auto decoded = nyxora::gpu::shader::decode_sopp_program(program);
    NYXORA_CHECK(decoded && decoded->instructions.size() == 2);
    NYXORA_CHECK(decoded->instructions[0].opcode == ScalarOpcode::waitcnt);
    NYXORA_CHECK(decoded->instructions[0].immediate == 0x7f);
}


NYXORA_TEST(gpu_pm4_to_guest_shader_and_index_data_end_to_end) {
    using namespace nyxora::gpu;
    using namespace nyxora::gpu::pm4;
    using nyxora::memory::Protection;
    nyxora::memory::GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(0x11000, 0x1000, Protection::read | Protection::write, "cs"));
    NYXORA_CHECK(memory.map(0x21000, 0x1000, Protection::read | Protection::write, "index"));

    const std::array<std::byte, 8> code{
        std::byte{0}, std::byte{0}, std::byte{0x80}, std::byte{0xbf},
        std::byte{0}, std::byte{0}, std::byte{0x81}, std::byte{0xbf},
    };
    const std::array<std::byte, 4> index_data{
        std::byte{5}, std::byte{0}, std::byte{8}, std::byte{0},
    };
    NYXORA_CHECK(memory.write(0x11000, code));
    NYXORA_CHECK(memory.write(0x21000, index_data));
    NYXORA_CHECK(memory.protect(0x11000, 0x1000, Protection::read | Protection::execute));
    GuestMemoryReader reader(memory);

    const std::array<std::uint32_t, 9> compute{
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 3, 2),
        0x20c, 0x110, 0,
        nyxora::test::pm4_packet3(Type3Opcode::dispatch_direct, 4, 2),
        1, 1, 1, 1,
    };
    Processor cp(QueueType::compute);
    const auto dispatch = cp.process(compute);
    const auto* command = std::get_if<DispatchDirect>(&dispatch.commands.back());
    NYXORA_CHECK(command && command->compute_shader);
    NYXORA_CHECK(command->compute_shader->address == 0x11000);
    const auto decoded = nyxora::gpu::shader::decode_guest_sopp_program(
        reader, *command->compute_shader, 2);
    NYXORA_CHECK(decoded && decoded->instructions.size() == 2);

    const std::array<std::uint32_t, 10> graphics{
        nyxora::test::pm4_packet3(Type3Opcode::index_type, 1), 0,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_2, 5),
        2, 0x21000, 0, 2, 0,
        nyxora::test::pm4_packet3(Type3Opcode::num_instances, 1), 1,
    };
    Processor gp(QueueType::graphics);
    const auto submission = gp.process(graphics);
    const auto* draw = std::get_if<DrawIndex2>(&submission.commands[1]);
    NYXORA_CHECK(draw != nullptr);
    const auto indices = resolve_index_buffer(reader, *draw);
    NYXORA_CHECK(indices && indices->indices == std::vector<std::uint32_t>({5, 8}));
}

NYXORA_TEST(shader_sopp_can_decode_guest_resident_program) {
    nyxora::memory::GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(0x4000, 256, rw, "shader"));
    const std::array<std::byte, 8> bytes{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0xbf},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x81}, std::byte{0xbf},
    };
    NYXORA_CHECK(memory.write(0x4000, bytes));
    nyxora::gpu::GuestMemoryReader reader(memory);
    const nyxora::gpu::pm4::ShaderProgram shader{
        .stage = nyxora::gpu::pm4::ShaderStage::compute, .address = 0x4000,
    };
    const auto decoded = nyxora::gpu::shader::decode_guest_sopp_program(reader, shader, 2);
    NYXORA_CHECK(decoded && decoded->instructions.size() == 2);
    NYXORA_CHECK(decoded->instructions.back().opcode ==
                 nyxora::gpu::shader::ScalarOpcode::end_program);
    NYXORA_CHECK(memory.protect(0x4000, 256, nyxora::memory::Protection::none));
    NYXORA_CHECK(!nyxora::gpu::shader::decode_guest_sopp_program(reader, shader, 2));
}
