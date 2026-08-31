#pragma once

#include "nyxora/gpu/pm4.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>

namespace nyxora::gpu {

struct SubmissionStats {
    std::uint64_t graphics_submissions{};
    std::uint64_t compute_submissions{};
    std::uint64_t dwords_consumed{};
    std::uint64_t packets_decoded{};
    std::uint64_t register_writes{};
    std::uint64_t draw_calls{};
    std::uint64_t dispatch_calls{};
};

class Backend {
public:
    virtual ~Backend() = default;

    void submit_graphics(std::span<const std::uint32_t> command_stream);
    void submit_compute(std::uint32_t queue, std::span<const std::uint32_t> command_stream);
    virtual std::uint64_t flush() = 0;
    virtual void wait(std::uint64_t timeline_value) = 0;

protected:
    virtual void execute_graphics(const pm4::Submission& submission) = 0;
    virtual void execute_compute(std::uint32_t queue, const pm4::Submission& submission) = 0;

private:
    pm4::Processor graphics_processor_{pm4::QueueType::graphics};
    std::unordered_map<std::uint32_t, pm4::Processor> compute_processors_;
};

} // namespace nyxora::gpu
