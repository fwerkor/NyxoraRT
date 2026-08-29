#include "nyxora/gpu/null_backend.hpp"

#include <algorithm>

namespace nyxora::gpu {

void NullBackend::submit_graphics(std::span<const std::uint32_t> command_stream) {
    ++stats_.graphics_submissions;
    stats_.dwords_consumed += command_stream.size();
}

void NullBackend::submit_compute(std::uint32_t, std::span<const std::uint32_t> command_stream) {
    ++stats_.compute_submissions;
    stats_.dwords_consumed += command_stream.size();
}

std::uint64_t NullBackend::flush() {
    return ++timeline_;
}

void NullBackend::wait(std::uint64_t timeline_value) {
    completed_timeline_ = std::max(completed_timeline_, std::min(timeline_value, timeline_));
}

} // namespace nyxora::gpu
