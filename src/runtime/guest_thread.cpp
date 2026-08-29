#include "nyxora/runtime/guest_thread.hpp"

#include <stdexcept>
#include <utility>

namespace nyxora::runtime {

GuestThread::~GuestThread() {
    join_noexcept();
}

GuestThread::GuestThread(GuestThread&& other) noexcept
    : state_(std::move(other.state_)), worker_(std::move(other.worker_)) {}

GuestThread& GuestThread::operator=(GuestThread&& other) noexcept {
    if (this != &other) {
        join_noexcept();
        state_ = std::move(other.state_);
        worker_ = std::move(other.worker_);
    }
    return *this;
}

std::optional<GuestThread> GuestThread::start(const TlsRegistry& tls_registry,
                                              GuestAddress entry, GuestSize stack_size,
                                              std::uint64_t arg0, std::uint64_t arg1,
                                              std::uint64_t arg2) {
    if (entry == 0) {
        return std::nullopt;
    }

    auto stack = GuestStack::create(stack_size);
    auto trampoline = EntryTrampoline::create();
    auto context = GuestThreadContext::create(tls_registry);
    if (!stack || !trampoline || !context) {
        return std::nullopt;
    }

    auto state = std::make_unique<State>(std::move(*stack), std::move(*trampoline),
                                         std::move(*context));
    auto* raw_state = state.get();
    std::thread worker([raw_state, entry, arg0, arg1, arg2] {
        try {
            ScopedGuestThreadContext context_scope(raw_state->context);
            ScopedGuestSegment segment_scope(raw_state->context);
            raw_state->result = invoke_guest_captured(raw_state->trampoline, entry,
                                                       raw_state->stack.top(), arg0, arg1, arg2);
        } catch (...) {
            raw_state->host_exception = std::current_exception();
        }
    });

    return GuestThread(std::move(state), std::move(worker));
}

GuestInvocationResult GuestThread::join() {
    if (state_ == nullptr) {
        throw std::runtime_error("guest thread has no execution state");
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (state_->host_exception) {
        std::rethrow_exception(state_->host_exception);
    }
    return state_->result;
}

void GuestThread::join_noexcept() noexcept {
    if (worker_.joinable()) {
        try {
            worker_.join();
        } catch (...) {
            std::terminate();
        }
    }
}

} // namespace nyxora::runtime
