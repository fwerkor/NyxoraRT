#pragma once

#include <cstdint>
#include <span>

namespace nyxora::gpu {

struct SubmissionStats {
    std::uint64_t graphics_submissions{};
    std::uint64_t compute_submissions{};
    std::uint64_t dwords_consumed{};
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual void submit_graphics(std::span<const std::uint32_t> command_stream) = 0;
    virtual void submit_compute(std::uint32_t queue, std::span<const std::uint32_t> command_stream) = 0;
    virtual std::uint64_t flush() = 0;
    virtual void wait(std::uint64_t timeline_value) = 0;
};

} // namespace nyxora::gpu
