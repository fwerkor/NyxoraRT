#include "test.hpp"
#include "pm4_fixture.hpp"
#include "nyxora/gpu/null_backend.hpp"

#include <array>
#include <optional>

NYXORA_TEST(null_gpu_backend_models_submission_timeline_and_pm4_semantics) {
    using namespace nyxora::gpu::pm4;
    nyxora::gpu::NullBackend gpu;
    const std::array<std::uint32_t, 8> graphics{
        nyxora::test::pm4_packet3(Type3Opcode::set_context_reg, 2), 0x20, 0x1234,
        nyxora::test::pm4_packet3(Type3Opcode::num_instances, 1), 2,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_auto, 2), 6, 0,
    };
    const std::array<std::uint32_t, 5> compute{
        nyxora::test::pm4_packet3(Type3Opcode::dispatch_direct, 4, 0x2), 4, 2, 1, 1,
    };

    gpu.submit_graphics(graphics);
    gpu.submit_compute(2, compute);
    const auto tick = gpu.flush();
    gpu.wait(tick);

    const auto stats = gpu.stats();
    NYXORA_CHECK(stats.graphics_submissions == 1);
    NYXORA_CHECK(stats.compute_submissions == 1);
    NYXORA_CHECK(stats.dwords_consumed == graphics.size() + compute.size());
    NYXORA_CHECK(stats.packets_decoded == 4);
    NYXORA_CHECK(stats.register_writes == 1);
    NYXORA_CHECK(stats.draw_calls == 1);
    NYXORA_CHECK(stats.dispatch_calls == 1);
    NYXORA_CHECK(gpu.completed_timeline() == tick);
}

NYXORA_TEST(gpu_backend_rejects_invalid_pm4_before_backend_execution) {
    nyxora::gpu::NullBackend gpu;
    constexpr std::uint32_t unsupported_opcode = 0x7b;
    constexpr std::uint32_t header = (3U << 30U) | (0U << 16U) | (unsupported_opcode << 8U);
    const std::array<std::uint32_t, 2> stream{header, 0};

    bool threw = false;
    try {
        gpu.submit_graphics(stream);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    NYXORA_CHECK(threw);
    NYXORA_CHECK(gpu.stats().graphics_submissions == 0);
    NYXORA_CHECK(gpu.stats().dwords_consumed == 0);
}

namespace {

class RecordingBackend final : public nyxora::gpu::Backend {
public:
    std::uint64_t flush() override { return 0; }
    void wait(std::uint64_t) override {}

    std::optional<nyxora::gpu::pm4::Submission> graphics_submission;

protected:
    void execute_graphics(const nyxora::gpu::pm4::Submission& submission) override {
        graphics_submission = submission;
    }

    void execute_compute(std::uint32_t, const nyxora::gpu::pm4::Submission&) override {}
};

} // namespace

NYXORA_TEST(gpu_backend_receives_draw_with_discovered_shader_programs) {
    using namespace nyxora::gpu::pm4;
    RecordingBackend gpu;
    const std::array<std::uint32_t, 11> stream{
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 3), 0x08, 0x1000, 0x1,
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 3), 0x48, 0x2000, 0x2,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_auto, 2), 3, 0,
    };

    gpu.submit_graphics(stream);
    NYXORA_CHECK(gpu.graphics_submission.has_value());
    const auto* draw = std::get_if<DrawIndexAuto>(&gpu.graphics_submission->commands.back());
    NYXORA_CHECK(draw != nullptr && draw->pixel_shader.has_value() &&
                 draw->vertex_shader.has_value());
    NYXORA_CHECK(draw->pixel_shader->address == 0x10000100000ULL);
    NYXORA_CHECK(draw->vertex_shader->address == 0x20000200000ULL);
}
