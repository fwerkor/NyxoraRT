#pragma once

#include "nyxora/gpu/backend.hpp"

namespace nyxora::gpu {

class NullBackend final : public Backend {
public:
    void submit_graphics(std::span<const std::uint32_t> command_stream) override;
    void submit_compute(std::uint32_t queue, std::span<const std::uint32_t> command_stream) override;
    std::uint64_t flush() override;
    void wait(std::uint64_t timeline_value) override;

    [[nodiscard]] SubmissionStats stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t completed_timeline() const noexcept { return completed_timeline_; }

private:
    SubmissionStats stats_{};
    std::uint64_t timeline_{};
    std::uint64_t completed_timeline_{};
};

} // namespace nyxora::gpu
