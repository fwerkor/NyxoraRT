#pragma once

#include "nyxora/gpu/backend.hpp"

namespace nyxora::gpu {

class NullBackend final : public Backend {
public:
    std::uint64_t flush() override;
    void wait(std::uint64_t timeline_value) override;

    [[nodiscard]] SubmissionStats stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t completed_timeline() const noexcept { return completed_timeline_; }

protected:
    void execute_graphics(const pm4::Submission& submission) override;
    void execute_compute(std::uint32_t queue, const pm4::Submission& submission) override;

private:
    void account(const pm4::Submission& submission);

    SubmissionStats stats_{};
    std::uint64_t timeline_{};
    std::uint64_t completed_timeline_{};
};

} // namespace nyxora::gpu
