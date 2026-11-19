#include "test.hpp"
#include "nyxora/gpu/shader/vector.hpp"

#include <array>
#include <cstdint>
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
} // namespace

namespace {
std::size_t vector_spirv_opcode_count(const std::vector<std::uint32_t>& module,
                                      std::uint32_t target) {
    std::size_t count = 0;
    for (std::size_t pc = 5; pc < module.size();) {
        const auto header = module[pc];
        const auto n = header >> 16U;
        if (n == 0 || n > module.size() - pc) {
            return 0;
        }
        if ((header & 0xffffU) == target) {
            ++count;
        }
        pc += n;
    }
    return count;
}
}

NYXORA_TEST(vector_spirv_compiles_32_lane_alu_kernel) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 11> code{
        vop1(1,0,255), 0xffffffffU,
        vop2(0x25,1,256,0), vop2(0x26,2,256,1),
        vop2(0x1b,3,256,1), vop2(0x1c,4,256,1),
        vop2(0x1d,5,256,1), vop2(0x13,6,256,1),
        vop2(0x14,7,256,1), vop2(0x16,8,129,1), endpgm,
    };
    const auto decoded = decode_vector_program(code);
    NYXORA_CHECK(decoded);
    const auto module = compile_vector_diagnostic_spirv(*decoded);
    NYXORA_CHECK(module && module->size() > 60);
    NYXORA_CHECK((*module)[0] == 0x07230203U);
    NYXORA_CHECK((*module)[1] == 0x00010300U);
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 15) == 1); // EntryPoint
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 16) == 1); // LocalSize 32
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 59) == 2); // StorageBuffer and Input
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 128) >= 1);
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 130) == 1);
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 199) >= 1);
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 197) == 1);
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 198) == 1);
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 169) == 2); // min/max
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 194) == 1);
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 253) == 1);
}

NYXORA_TEST(vector_spirv_compiler_rejects_forged_invalid_programs) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 2> code{vop1(1,0,128), endpgm};
    const auto decoded = decode_vector_program(code);
    NYXORA_CHECK(decoded);
    auto bad = *decoded;
    bad.instructions[0].sources[0].kind = static_cast<VectorSourceKind>(22);
    NYXORA_CHECK(!compile_vector_diagnostic_spirv(bad));
    bad = *decoded;
    bad.instructions[0].sources[0] = {VectorSourceKind::vgpr, 256};
    NYXORA_CHECK(!compile_vector_diagnostic_spirv(bad));
    bad = *decoded;
    bad.instructions[0].source_count = 2;
    NYXORA_CHECK(!compile_vector_diagnostic_spirv(bad));
    bad = *decoded;
    bad.dword_count = 1;
    NYXORA_CHECK(!compile_vector_diagnostic_spirv(bad));
    bad = *decoded;
    bad.instructions.back().op = VectorOp::nop;
    NYXORA_CHECK(!compile_vector_diagnostic_spirv(bad));
    std::array<std::uint32_t, 1> empty{endpgm};
    const auto minimal = decode_vector_program(empty);
    NYXORA_CHECK(minimal && compile_vector_diagnostic_spirv(*minimal));
}

NYXORA_TEST(vector_spirv_not_and_left_shift_are_not_silent_noops) {
    using namespace nyxora::gpu::shader;
    const std::array<std::uint32_t, 4> code{
        vop1(1,0,129), vop1(0x37,1,256), vop2(0x1a,2,129,1), endpgm,
    };
    const auto decoded = decode_vector_program(code);
    NYXORA_CHECK(decoded);
    const auto module = compile_vector_diagnostic_spirv(*decoded);
    NYXORA_CHECK(module && vector_spirv_opcode_count(*module, 200) == 1);
    NYXORA_CHECK(vector_spirv_opcode_count(*module, 196) == 1);
}
