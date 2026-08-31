#include "nyxora/gpu/backend.hpp"

namespace nyxora::gpu {

void Backend::submit_graphics(std::span<const std::uint32_t> command_stream) {
    execute_graphics(graphics_processor_.process(command_stream));
}

void Backend::submit_compute(std::uint32_t queue, std::span<const std::uint32_t> command_stream) {
    auto [it, inserted] = compute_processors_.try_emplace(queue, pm4::QueueType::compute);
    (void)inserted;
    execute_compute(queue, it->second.process(command_stream));
}

} // namespace nyxora::gpu
