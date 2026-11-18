#include "test.hpp"
#include "pm4_fixture.hpp"
#include "nyxora/gpu/null_backend.hpp"
#include "nyxora/gpu/pm4.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <variant>

namespace {

template <typename F>
bool rejects(F&& action) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

NYXORA_TEST(pm4_indexed_draw_snapshots_shader_and_index_state) {
    using namespace nyxora::gpu::pm4;
    const std::array<std::uint32_t, 16> stream{
        nyxora::test::pm4_packet3(Type3Opcode::index_type, 1), 1,
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 3), 0x48, 0x1234, 0x2,
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 3), 0x08, 0x5678, 0x1,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_2, 5),
        1024, 0x89abcdef, 0x12345678, 512, 0x11,
    };
    Processor gpu(QueueType::graphics);
    const auto result = gpu.process(stream);
    NYXORA_CHECK(result.packets_decoded == 4);
    NYXORA_CHECK(gpu.state().index_type() == 1U);
    NYXORA_CHECK(result.commands.size() == 6);
    const auto* draw = std::get_if<DrawIndex2>(&result.commands.back());
    NYXORA_CHECK(draw != nullptr);
    NYXORA_CHECK(draw->max_size == 1024U);
    NYXORA_CHECK(draw->index_address == 0x1234567889abcdefULL);
    NYXORA_CHECK(draw->index_count == 512U);
    NYXORA_CHECK(draw->initiator == 0x11U);
    NYXORA_CHECK(draw->index_type == 1U);
    NYXORA_CHECK(draw->vertex_shader && draw->vertex_shader->address == 0x20000123400ULL);
    NYXORA_CHECK(draw->pixel_shader && draw->pixel_shader->address == 0x10000567800ULL);
}

NYXORA_TEST(pm4_index_format_tracks_direct_and_indexed_uconfig_writes) {
    using namespace nyxora::gpu::pm4;
    Processor gpu(QueueType::graphics);
    const std::array<std::uint32_t, 18> stream{
        nyxora::test::pm4_packet3(Type3Opcode::set_uconfig_reg_index, 2),
        0x20000243, 0x401,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_2, 5),
        8, 0x1000, 0, 8, 0,
        nyxora::test::pm4_packet3(Type3Opcode::set_uconfig_reg, 2),
        0x243, 0x402,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_2, 5),
        4, 0x2000, 0, 4, 0,
    };
    const auto result = gpu.process(stream);
    const auto* first = std::get_if<DrawIndex2>(&result.commands[1]);
    const auto* second = std::get_if<DrawIndex2>(&result.commands[3]);
    NYXORA_CHECK(first && first->index_type == 1U && first->index_address == 0x1000ULL);
    NYXORA_CHECK(second && second->index_type == 2U && second->index_address == 0x2000ULL);
    NYXORA_CHECK(gpu.state().index_type() == 2U);
    NYXORA_CHECK(gpu.state().register_value(RegisterSpace::uconfig, 0xc243) == 0x402U);
}

NYXORA_TEST(pm4_indexed_draw_rejects_invalid_packets_transactionally) {
    using namespace nyxora::gpu::pm4;
    Processor gpu(QueueType::graphics);
    const std::array<std::uint32_t, 3> original{
        nyxora::test::pm4_packet3(Type3Opcode::set_uconfig_reg, 2), 0x243, 1,
    };
    (void)gpu.process(original);
    const std::array<std::uint32_t, 12> invalid{
        nyxora::test::pm4_packet3(Type3Opcode::set_uconfig_reg, 2), 0x243, 2,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_2, 5),
        5, 0, 0, 5, 0,
        nyxora::test::pm4_packet3(Type3Opcode::set_uconfig_reg_index, 2),
        0x30000243, 3,
    };
    NYXORA_CHECK(rejects([&] { (void)gpu.process(invalid); }));
    NYXORA_CHECK(gpu.state().index_type() == 1U);
    NYXORA_CHECK(gpu.state().register_value(RegisterSpace::uconfig, 0xc243) == 1U);

    const std::array<std::uint32_t, 5> short_draw{
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_2, 4), 5, 0, 0, 5,
    };
    NYXORA_CHECK(rejects([&] { (void)gpu.process(short_draw); }));
    NYXORA_CHECK(gpu.state().index_type() == 1U);

    const std::array<std::uint32_t, 6> predicated_draw{
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_2, 5, 1),
        5, 0, 0, 5, 0,
    };
    NYXORA_CHECK(rejects([&] { (void)gpu.process(predicated_draw); }));

    Processor compute(QueueType::compute);
    NYXORA_CHECK(rejects([&] { (void)compute.process(predicated_draw); }));
    const std::array<std::uint32_t, 6> compute_draw{
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_2, 5, 2),
        5, 0, 0, 5, 0,
    };
    NYXORA_CHECK(rejects([&] { (void)compute.process(compute_draw); }));
    const std::array<std::uint32_t, 2> compute_index{
        nyxora::test::pm4_packet3(Type3Opcode::index_type, 1), 1,
    };
    NYXORA_CHECK(rejects([&] { (void)compute.process(compute_index); }));
}

NYXORA_TEST(null_gpu_backend_counts_indexed_draws) {
    using namespace nyxora::gpu::pm4;
    nyxora::gpu::NullBackend gpu;
    const std::array<std::uint32_t, 8> stream{
        nyxora::test::pm4_packet3(Type3Opcode::index_type, 1), 1,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_2, 5),
        12, 0x2000, 0, 12, 0,
    };
    gpu.submit_graphics(stream);
    const auto stats = gpu.stats();
    NYXORA_CHECK(stats.graphics_submissions == 1);
    NYXORA_CHECK(stats.draw_calls == 1);
    NYXORA_CHECK(stats.packets_decoded == 2);
}
