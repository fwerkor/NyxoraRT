#include "test.hpp"
#include "nyxora/gpu/shader/scalar_cfg.hpp"

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr std::uint32_t sopp(unsigned opcode, unsigned immediate = 0) {
    return 0xbf800000U | (opcode << 16U) | immediate;
}
constexpr std::uint32_t sop1(unsigned opcode, unsigned dst, unsigned src) {
    return 0xbe800000U | (dst << 16U) | (opcode << 8U) | src;
}
constexpr std::uint32_t sopk(unsigned dst, unsigned immediate) {
    return 0xb0000000U | (dst << 16U) | immediate;
}
} // namespace

NYXORA_TEST(shader_cfg_single_block_straight_line) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 3> code{sopk(0, 5), sopp(0), sopp(1)};
    const auto parsed = decode_scalar_program(code);
    NYXORA_CHECK(parsed);
    const auto graph = build_scalar_cfg(*parsed);
    NYXORA_CHECK(graph && graph->blocks.size() == 1);
    const auto& block = graph->blocks.front();
    NYXORA_CHECK(block.start_dword == 0 && block.end_dword == 3);
    NYXORA_CHECK(block.instruction_count == 3 && block.first_instruction == 0);
    NYXORA_CHECK(block.reachable && block.successors.empty() && block.predecessors.empty());
}

NYXORA_TEST(shader_cfg_conditional_target_skips_a_literal_word) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 5> code{
        sopk(0, 1), sopp(4, 2), sop1(3, 1, 255), 0xfeedbeefU, sopp(1),
    };
    const auto parsed = decode_scalar_program(code);
    NYXORA_CHECK(parsed);
    const auto graph = build_scalar_cfg(*parsed);
    NYXORA_CHECK(graph && graph->blocks.size() == 3);
    NYXORA_CHECK(graph->blocks[0].start_dword == 0 && graph->blocks[0].end_dword == 2);
    NYXORA_CHECK(graph->blocks[0].successors == std::vector<std::uint32_t>({2, 1}));
    NYXORA_CHECK(graph->blocks[1].start_dword == 2 && graph->blocks[1].end_dword == 4);
    NYXORA_CHECK(graph->blocks[1].instruction_count == 1);
    NYXORA_CHECK(graph->blocks[1].successors == std::vector<std::uint32_t>{2});
    NYXORA_CHECK(graph->blocks[2].predecessors == std::vector<std::uint32_t>({0, 1}));
    NYXORA_CHECK(graph->blocks[0].reachable && graph->blocks[1].reachable &&
                 graph->blocks[2].reachable);
}

NYXORA_TEST(shader_cfg_marks_unreachable_fallthrough_after_branch) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 4> code{sopp(2, 2), sopp(0), sopp(0), sopp(1)};
    const auto decoded = decode_scalar_program(code);
    NYXORA_CHECK(decoded);
    const auto graph = build_scalar_cfg(*decoded);
    NYXORA_CHECK(graph && graph->blocks.size() == 3);
    NYXORA_CHECK(graph->blocks[0].successors == std::vector<std::uint32_t>{2});
    NYXORA_CHECK(!graph->blocks[1].reachable);
    NYXORA_CHECK(graph->blocks[1].successors == std::vector<std::uint32_t>{2});
    NYXORA_CHECK(graph->blocks[2].reachable);
}

NYXORA_TEST(shader_cfg_detects_backward_loop_without_fake_exit) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 3> code{sopp(0), sopp(2, 0xfffe), sopp(1)};
    const auto decoded = decode_scalar_program(code);
    NYXORA_CHECK(decoded);
    const auto graph = build_scalar_cfg(*decoded);
    NYXORA_CHECK(graph && graph->blocks.size() == 2);
    NYXORA_CHECK(graph->blocks[0].successors == std::vector<std::uint32_t>{0});
    NYXORA_CHECK(graph->blocks[0].predecessors == std::vector<std::uint32_t>{0});
    NYXORA_CHECK(!graph->blocks[1].reachable);
    NYXORA_CHECK(graph->blocks[1].successors.empty());
}

NYXORA_TEST(shader_cfg_deduplicates_branch_and_fallthrough_edges) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 2> code{sopp(5, 0), sopp(1)};
    const auto decoded = decode_scalar_program(code);
    NYXORA_CHECK(decoded);
    const auto graph = build_scalar_cfg(*decoded);
    NYXORA_CHECK(graph && graph->blocks.size() == 2);
    NYXORA_CHECK(graph->blocks[0].successors == std::vector<std::uint32_t>{1});
    NYXORA_CHECK(graph->blocks[1].predecessors == std::vector<std::uint32_t>{0});
}

NYXORA_TEST(shader_cfg_rejects_structurally_invalid_programs) {
    using namespace nyxora::gpu::shader;
    const auto invalid = build_scalar_cfg(ScalarDecodedProgram{});
    NYXORA_CHECK(!invalid);

    const std::array<std::uint32_t, 2> code{sopp(0), sopp(1)};
    const auto parsed = decode_scalar_program(code);
    NYXORA_CHECK(parsed);

    auto wrong_size = *parsed;
    wrong_size.dword_count = 1;
    NYXORA_CHECK(!build_scalar_cfg(std::move(wrong_size)));

    auto bad_offset = *parsed;
    bad_offset.instructions[1].offset_dwords = 0;
    NYXORA_CHECK(!build_scalar_cfg(std::move(bad_offset)));

    auto bad_terminator = *parsed;
    bad_terminator.instructions.back().opcode = ScalarOp::nop;
    NYXORA_CHECK(!build_scalar_cfg(std::move(bad_terminator)));

    auto bad_branch = *parsed;
    bad_branch.instructions[0].opcode = ScalarOp::branch;
    bad_branch.instructions[0].branch_target_dwords = 2;
    NYXORA_CHECK(!build_scalar_cfg(std::move(bad_branch)));

    auto unnecessary_target = *parsed;
    unnecessary_target.instructions[0].branch_target_dwords = 1;
    NYXORA_CHECK(!build_scalar_cfg(std::move(unnecessary_target)));
}

NYXORA_TEST(shader_cfg_json_records_reachability_and_adjacency) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 3> code{sopp(2, 1), sopp(0), sopp(1)};
    const auto parsed = decode_scalar_program(code);
    NYXORA_CHECK(parsed);
    const auto graph = build_scalar_cfg(*parsed);
    NYXORA_CHECK(graph);
    std::ostringstream out;
    write_scalar_cfg_json(out, *graph);
    const auto json = out.str();
    NYXORA_CHECK(json.find("\"isa\":\"scalar-cfg\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"reachable\":false") != std::string::npos);
    NYXORA_CHECK(json.find("\"successors\":[2]") != std::string::npos);
    NYXORA_CHECK(json.find("\"predecessors\":[0,1]") != std::string::npos);
    NYXORA_CHECK(!json.empty() && json.back() == '\n');
}
