#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <thread>

#include "nyxora/base/types.hpp"
#include "nyxora/runtime/fault.hpp"
#include "nyxora/runtime/native_thread.hpp"
#include "nyxora/runtime/tls.hpp"

namespace nyxora::runtime {

class GuestThread {
public:
    GuestThread() = default;
    ~GuestThread();

    GuestThread(const GuestThread&) = delete;
    GuestThread& operator=(const GuestThread&) = delete;
    GuestThread(GuestThread&& other) noexcept;
    GuestThread& operator=(GuestThread&& other) noexcept;

    [[nodiscard]] static std::optional<GuestThread> start(
        const TlsRegistry& tls_registry, GuestAddress entry, GuestSize stack_size,
        std::uint64_t arg0 = 0, std::uint64_t arg1 = 0, std::uint64_t arg2 = 0);

    [[nodiscard]] bool joinable() const noexcept { return worker_.joinable(); }
    GuestInvocationResult join();

private:
    struct State {
        State(GuestStack stack_in, EntryTrampoline trampoline_in, GuestThreadContext context_in)
            : stack(std::move(stack_in)), trampoline(std::move(trampoline_in)),
              context(std::move(context_in)) {}

        GuestStack stack;
        EntryTrampoline trampoline;
        GuestThreadContext context;
        GuestInvocationResult result;
        std::exception_ptr host_exception;
    };

    GuestThread(std::unique_ptr<State> state, std::thread worker)
        : state_(std::move(state)), worker_(std::move(worker)) {}

    void join_noexcept() noexcept;

    std::unique_ptr<State> state_;
    std::thread worker_;
};

} // namespace nyxora::runtime
