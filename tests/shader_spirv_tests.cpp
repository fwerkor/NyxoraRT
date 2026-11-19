#include "test.hpp"
#include "nyxora/gpu/shader/spirv.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace {
constexpr std::uint32_t sop1(unsigned opcode, unsigned dst, unsigned source) {
    return 0xbe800000U | (dst << 16U) | (opcode << 8U) | source;
}
constexpr std::uint32_t sop2(unsigned opcode, unsigned dst, unsigned lhs, unsigned rhs) {
    return 0x80000000U | (opcode << 23U) | (dst << 16U) | (rhs << 8U) | lhs;
}
constexpr std::uint32_t sopk(unsigned dst, unsigned imm) {
    return 0xb0000000U | (dst << 16U) | imm;
}
constexpr std::uint32_t sopp(unsigned opcode, unsigned imm = 0) {
    return 0xbf800000U | (opcode << 16U) | imm;
}
constexpr std::uint32_t sopc(unsigned opcode, unsigned lhs, unsigned rhs) {
    return 0xbf000000U | (opcode << 16U) | (rhs << 8U) | lhs;
}
std::optional<std::vector<std::uint32_t>>
compile(std::span<const std::uint32_t> code) {
    const auto decoded = nyxora::gpu::shader::decode_scalar_program(code);
    if (!decoded) {
        return std::nullopt;
    }
    const auto cfg = nyxora::gpu::shader::build_scalar_cfg(*decoded);
    return cfg ? nyxora::gpu::shader::compile_scalar_diagnostic_spirv(*cfg)
               : std::nullopt;
}

bool valid_word_counts(const std::vector<std::uint32_t>& words) {
    std::size_t cursor = 5;
    while (cursor < words.size()) {
        const auto count = words[cursor] >> 16U;
        if (count == 0 || count > words.size() - cursor) {
            return false;
        }
        cursor += count;
    }
    return cursor == words.size();
}

std::size_t count_opcode(const std::vector<std::uint32_t>& words, std::uint32_t opcode) {
    std::size_t count = 0;
    for (std::size_t cursor = 5; cursor < words.size(); cursor += words[cursor] >> 16U) {
        if ((words[cursor] & 0xffffU) == opcode) {
            ++count;
        }
    }
    return count;
}

} // namespace

NYXORA_TEST(shader_spirv_emits_well_formed_module_for_empty_compute_program) {
    const std::array<std::uint32_t, 2> code{sopp(0), sopp(1)};
    const auto words = compile(code);
    NYXORA_CHECK(words && words->size() > 45);
    NYXORA_CHECK((*words)[0] == 0x07230203U);
    NYXORA_CHECK((*words)[1] == 0x00010300U);
    NYXORA_CHECK((*words)[3] > 12);
    NYXORA_CHECK((*words)[4] == 0);
    NYXORA_CHECK(valid_word_counts(*words));
    NYXORA_CHECK(count_opcode(*words, 0) == 1);   // OpNop
    NYXORA_CHECK(count_opcode(*words, 15) == 1);  // OpEntryPoint
    NYXORA_CHECK(count_opcode(*words, 54) == 1);  // OpFunction
    NYXORA_CHECK(count_opcode(*words, 253) == 1); // OpReturn
}

NYXORA_TEST(shader_spirv_emits_observable_register_and_scc_updates) {
    const std::array<std::uint32_t, 11> code{
        sopk(0, 0xfffe), sop1(3, 1, 0), sop1(7, 2, 1),
        sop2(0, 3, 1, 129), sop2(1, 4, 3, 130),
        sop2(0x0e, 5, 3, 4), sop2(0x10, 6, 5, 129),
        sop2(0x12, 7, 6, 193), sopc(6, 7, 0), sopp(0), sopp(1),
    };
    const auto words = compile(code);
    NYXORA_CHECK(words && valid_word_counts(*words));
    NYXORA_CHECK(count_opcode(*words, 59) == 1);  // buffer variable
    NYXORA_CHECK(count_opcode(*words, 65) >= 10); // access chains
    NYXORA_CHECK(count_opcode(*words, 62) >= 15); // SGPR + SCC stores
    NYXORA_CHECK(count_opcode(*words, 128) == 1); // OpIAdd
    NYXORA_CHECK(count_opcode(*words, 130) == 1); // OpISub
    NYXORA_CHECK(count_opcode(*words, 199) == 1); // OpBitwiseAnd
    NYXORA_CHECK(count_opcode(*words, 197) == 1); // OpBitwiseOr
    NYXORA_CHECK(count_opcode(*words, 198) == 1); // OpBitwiseXor
    NYXORA_CHECK(count_opcode(*words, 200) == 1); // OpNot
    NYXORA_CHECK(count_opcode(*words, 170) == 1); // OpIEqual
    NYXORA_CHECK(count_opcode(*words, 169) >= 6); // OpSelect
}

NYXORA_TEST(shader_spirv_rejects_branch_barrier_and_wait_without_semantics) {
    const std::array<std::uint32_t, 2> unconditional{sopp(2, 0), sopp(1)};
    const std::array<std::uint32_t, 2> conditional{sopp(4, 0), sopp(1)};
    const std::array<std::uint32_t, 2> barrier{sopp(10), sopp(1)};
    const std::array<std::uint32_t, 2> waitcnt{sopp(12), sopp(1)};
    NYXORA_CHECK(!compile(unconditional));
    NYXORA_CHECK(!compile(conditional));
    NYXORA_CHECK(!compile(barrier));
    NYXORA_CHECK(!compile(waitcnt));
}

NYXORA_TEST(shader_spirv_rejects_malformed_program_and_excessive_size) {
    const std::array<std::uint32_t, 1> invalid{sopp(0)};
    NYXORA_CHECK(!compile(invalid));
    std::vector<std::uint32_t> giant(4097, sopp(0));
    giant.back() = sopp(1);
    NYXORA_CHECK(!compile(giant));

    const std::array<std::uint32_t, 2> simple{sopk(1, 2), sopp(1)};
    auto decoded = nyxora::gpu::shader::decode_scalar_program(simple);
    NYXORA_CHECK(decoded);
    auto cfg = nyxora::gpu::shader::build_scalar_cfg(*decoded);
    NYXORA_CHECK(cfg);
    cfg->blocks[0].reachable = false;
    NYXORA_CHECK(!nyxora::gpu::shader::compile_scalar_diagnostic_spirv(*cfg));
}

NYXORA_TEST(shader_spirv_literals_and_zero_register_dont_confuse_id_allocator) {
    const std::array<std::uint32_t, 6> code{
        sop1(3, 0, 255), 0xffffffffU,
        sop2(0, 0, 0, 255), 0x00ffffffU,
        sopk(101, 0), sopp(1),
    };
    const auto words = compile(code);
    NYXORA_CHECK(words && valid_word_counts(*words));
    NYXORA_CHECK(count_opcode(*words, 43) >= 6); // Constants
    NYXORA_CHECK(count_opcode(*words, 62) >= 4); // Register + SCC stores
    NYXORA_CHECK(count_opcode(*words, 253) == 1);
}
