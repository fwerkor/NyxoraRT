#include "nyxora/runtime/fault.hpp"

#include <cstdlib>
#include <sstream>
#include <string>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mutex>
#elif defined(__x86_64__)
#include <array>
#include <csignal>
#include <exception>
#include <mutex>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#endif

namespace nyxora::runtime {
namespace {

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
struct ActiveCapture {
    GuestFault* fault{};
    EntryRecoveryState* recovery{};
    GuestAddress recovery_address{};
};

std::once_flag install_once;
thread_local ActiveCapture* active_capture = nullptr;

GuestFaultKind windows_fault_kind(DWORD code) noexcept {
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
        return GuestFaultKind::access_violation;
    }
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        return GuestFaultKind::illegal_instruction;
    }
    return GuestFaultKind::unknown;
}

LONG CALLBACK guest_vectored_handler(EXCEPTION_POINTERS* pointers) noexcept {
    auto* capture = active_capture;
    if (capture == nullptr || capture->fault == nullptr || capture->recovery == nullptr ||
        capture->recovery->host_stack == 0 || capture->recovery_address == 0 ||
        pointers == nullptr || pointers->ExceptionRecord == nullptr || pointers->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto code = pointers->ExceptionRecord->ExceptionCode;
    const auto kind = windows_fault_kind(code);
    if (kind == GuestFaultKind::unknown) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    GuestAddress address = 0;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        pointers->ExceptionRecord->NumberParameters >= 2) {
        address = static_cast<GuestAddress>(pointers->ExceptionRecord->ExceptionInformation[1]);
    }
    *capture->fault = GuestFault{
        .kind = kind,
        .native_code = static_cast<int>(code),
        .address = address,
        .instruction_pointer = static_cast<GuestAddress>(pointers->ContextRecord->Rip),
    };
    pointers->ContextRecord->Rsp = capture->recovery->host_stack;
    pointers->ContextRecord->Rip = capture->recovery_address;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void install_handlers() {
    if (::AddVectoredExceptionHandler(1, guest_vectored_handler) == nullptr) {
        throw std::runtime_error("unable to install guest vectored exception handler");
    }
}
#elif defined(__x86_64__)

struct SignalState {
    int signal_number{};
    struct sigaction previous {};
};

struct ActiveCapture {
    GuestFault* fault{};
    EntryRecoveryState* recovery{};
    GuestAddress recovery_address{};
};

std::array<SignalState, 3> signal_states{{
    {SIGSEGV, {}},
    {SIGBUS, {}},
    {SIGILL, {}},
}};
std::once_flag install_once;
thread_local ActiveCapture* active_capture = nullptr;

SignalState* signal_state(int signal_number) noexcept {
    for (auto& state : signal_states) {
        if (state.signal_number == signal_number) {
            return &state;
        }
    }
    return nullptr;
}

GuestAddress instruction_pointer(void* native_context) noexcept {
    if (native_context == nullptr) {
        return 0;
    }
    auto* context = static_cast<ucontext_t*>(native_context);
#if defined(__linux__) && defined(__x86_64__)
    return static_cast<GuestAddress>(context->uc_mcontext.gregs[REG_RIP]);
#elif defined(__APPLE__) && defined(__x86_64__)
    return static_cast<GuestAddress>(context->uc_mcontext->__ss.__rip);
#elif defined(__APPLE__) && defined(__aarch64__)
    return static_cast<GuestAddress>(context->uc_mcontext->__ss.__pc);
#else
    (void)context;
    return 0;
#endif
}

bool redirect_to_recovery(void* native_context, GuestAddress stack,
                          GuestAddress instruction) noexcept {
    if (native_context == nullptr || stack == 0 || instruction == 0) {
        return false;
    }
    auto* context = static_cast<ucontext_t*>(native_context);
#if defined(__linux__) && defined(__x86_64__)
    context->uc_mcontext.gregs[REG_RSP] = static_cast<greg_t>(stack);
    context->uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(instruction);
    return true;
#elif defined(__APPLE__) && defined(__x86_64__)
    context->uc_mcontext->__ss.__rsp = stack;
    context->uc_mcontext->__ss.__rip = instruction;
    return true;
#else
    (void)context;
    (void)stack;
    (void)instruction;
    return false;
#endif
}

GuestFaultKind fault_kind(int signal_number) noexcept {
    switch (signal_number) {
    case SIGSEGV:
        return GuestFaultKind::access_violation;
    case SIGBUS:
        return GuestFaultKind::bus_error;
    case SIGILL:
        return GuestFaultKind::illegal_instruction;
    default:
        return GuestFaultKind::unknown;
    }
}

[[noreturn]] void restore_default_and_reraise(int signal_number) noexcept {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)::sigaction(signal_number, &action, nullptr);
    (void)::raise(signal_number);
    _Exit(128 + signal_number);
}

void dispatch_previous(int signal_number, siginfo_t* info, void* native_context) noexcept {
    auto* state = signal_state(signal_number);
    if (state == nullptr) {
        restore_default_and_reraise(signal_number);
    }

    const auto& previous = state->previous;
    if (previous.sa_handler == SIG_IGN) {
        return;
    }
    if (previous.sa_handler == SIG_DFL) {
        restore_default_and_reraise(signal_number);
    }
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(signal_number, info, native_context);
        return;
    }
    if (previous.sa_handler != nullptr) {
        previous.sa_handler(signal_number);
        return;
    }
    restore_default_and_reraise(signal_number);
}

void guest_signal_handler(int signal_number, siginfo_t* info, void* native_context) noexcept {
    auto* capture = active_capture;
    if (capture == nullptr || capture->fault == nullptr || capture->recovery == nullptr ||
        capture->recovery->host_stack == 0 || capture->recovery_address == 0) {
        dispatch_previous(signal_number, info, native_context);
        return;
    }

    const GuestFault fault{
        .kind = fault_kind(signal_number),
        .native_code = signal_number,
        .address = info == nullptr ? 0 : reinterpret_cast<GuestAddress>(info->si_addr),
        .instruction_pointer = instruction_pointer(native_context),
    };
    if (!redirect_to_recovery(native_context, capture->recovery->host_stack,
                              capture->recovery_address)) {
        dispatch_previous(signal_number, info, native_context);
        return;
    }
    *capture->fault = fault;
}

void install_handlers() {
    std::size_t installed = 0;
    for (; installed < signal_states.size(); ++installed) {
        auto& state = signal_states[installed];
        struct sigaction action {};
        action.sa_sigaction = guest_signal_handler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (::sigaction(state.signal_number, &action, &state.previous) == 0) {
            continue;
        }

        while (installed != 0) {
            --installed;
            auto& rollback = signal_states[installed];
            if (::sigaction(rollback.signal_number, &rollback.previous, nullptr) != 0) {
                std::terminate();
            }
        }
        throw std::runtime_error("unable to install guest fault signal handler");
    }
}
#endif

std::string fault_message(const GuestFault& fault) {
    std::ostringstream message;
    message << "guest execution fault at 0x" << std::hex << fault.instruction_pointer;
    return message.str();
}

} // namespace

GuestFaultException::GuestFaultException(GuestFault fault)
    : std::runtime_error(fault_message(fault)), fault_(fault) {}

GuestInvocationResult invoke_guest_captured(const EntryTrampoline& trampoline,
                                             GuestAddress entry, GuestAddress stack_top,
                                             std::uint64_t arg0, std::uint64_t arg1,
                                             std::uint64_t arg2) {
#if (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))) || defined(__x86_64__)
    std::call_once(install_once, install_handlers);

    EntryRecoveryState recovery{};
    GuestFault fault{};
    ActiveCapture capture{
        .fault = &fault,
        .recovery = &recovery,
        .recovery_address = trampoline.recovery_address(),
    };
    auto* previous_capture = active_capture;
    active_capture = &capture;

    try {
        const auto value = trampoline.invoke(entry, stack_top, arg0, arg1, arg2, &recovery);
        active_capture = previous_capture;
        if (fault.native_code != 0) {
            return GuestInvocationResult{.value = 0, .fault = fault};
        }
        return GuestInvocationResult{.value = value, .fault = std::nullopt};
    } catch (...) {
        active_capture = previous_capture;
        throw;
    }
#else
    return GuestInvocationResult{.value = trampoline.invoke(entry, stack_top, arg0, arg1, arg2),
                                 .fault = std::nullopt};
#endif
}

} // namespace nyxora::runtime
