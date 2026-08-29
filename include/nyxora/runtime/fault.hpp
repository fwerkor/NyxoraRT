#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>

#include "nyxora/base/types.hpp"
#include "nyxora/runtime/native_thread.hpp"

namespace nyxora::runtime {

enum class GuestFaultKind {
    access_violation,
    bus_error,
    illegal_instruction,
    unknown,
};

struct GuestFault {
    GuestFaultKind kind{GuestFaultKind::unknown};
    int native_code{};
    GuestAddress address{};
    GuestAddress instruction_pointer{};
};

struct GuestInvocationResult {
    std::uint64_t value{};
    std::optional<GuestFault> fault;

    [[nodiscard]] bool completed() const noexcept { return !fault.has_value(); }
};

class GuestFaultException final : public std::runtime_error {
public:
    explicit GuestFaultException(GuestFault fault);

    [[nodiscard]] const GuestFault& fault() const noexcept { return fault_; }

private:
    GuestFault fault_;
};

[[nodiscard]] GuestInvocationResult invoke_guest_captured(
    const EntryTrampoline& trampoline, GuestAddress entry, GuestAddress stack_top,
    std::uint64_t arg0 = 0, std::uint64_t arg1 = 0, std::uint64_t arg2 = 0);

[[noreturn]] void terminate_guest_execution(std::uint64_t value) noexcept;

} // namespace nyxora::runtime
