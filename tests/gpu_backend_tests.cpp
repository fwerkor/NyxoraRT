#include "test.hpp"
#include "asteria/gpu/null_backend.hpp"

#include <array>

ASTERIA_TEST(null_gpu_backend_models_submission_timeline) {
    asteria::gpu::NullBackend gpu;
    const std::array<std::uint32_t, 3> commands{1, 2, 3};
    gpu.submit_graphics(commands);
    gpu.submit_compute(2, commands);
    const auto tick = gpu.flush();
    gpu.wait(tick);
    ASTERIA_CHECK(gpu.stats().graphics_submissions == 1);
    ASTERIA_CHECK(gpu.stats().compute_submissions == 1);
    ASTERIA_CHECK(gpu.stats().dwords_consumed == 6);
    ASTERIA_CHECK(gpu.completed_timeline() == tick);
}
