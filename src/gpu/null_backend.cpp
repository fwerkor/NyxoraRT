#include "nyxora/gpu/null_backend.hpp"

#include <algorithm>
#include <variant>

namespace nyxora::gpu {

void NullBackend::account(const pm4::Submission& submission) {
    stats_.dwords_consumed += submission.dwords_consumed;
    stats_.packets_decoded += submission.packets_decoded;
    for (const auto& command : submission.commands) {
        if (std::holds_alternative<pm4::RegisterWrite>(command)) {
            ++stats_.register_writes;
        } else if (std::holds_alternative<pm4::DrawIndexAuto>(command)) {
            ++stats_.draw_calls;
        } else if (std::holds_alternative<pm4::DispatchDirect>(command)) {
            ++stats_.dispatch_calls;
        }
    }
}

void NullBackend::execute_graphics(const pm4::Submission& submission) {
    ++stats_.graphics_submissions;
    account(submission);
}

void NullBackend::execute_compute(std::uint32_t, const pm4::Submission& submission) {
    ++stats_.compute_submissions;
    account(submission);
}

std::uint64_t NullBackend::flush() {
    return ++timeline_;
}

void NullBackend::wait(std::uint64_t timeline_value) {
    completed_timeline_ = std::max(completed_timeline_, std::min(timeline_value, timeline_));
}

} // namespace nyxora::gpu
