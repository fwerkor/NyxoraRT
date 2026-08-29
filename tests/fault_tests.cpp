#include "test.hpp"
#include "nyxora/memory/native_arena.hpp"
#include "nyxora/runtime/fault.hpp"
#include "nyxora/runtime/native_thread.hpp"

#include <array>
#include <cstddef>

NYXORA_TEST(guest_fault_capture_reports_invalid_memory_access) {
#if !defined(_WIN32) && defined(__x86_64__)
    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    // mov rax, qword ptr [0]; ret
    const std::array<std::byte, 9> guest_code{
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xc3},
    };
    NYXORA_CHECK(code->copy(0, guest_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, guest_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());

    const auto result = nyxora::runtime::invoke_guest_captured(
        *trampoline, reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), stack->top());
    NYXORA_CHECK(!result.completed());
    NYXORA_CHECK(result.fault.has_value());
    NYXORA_CHECK(result.fault->kind == nyxora::runtime::GuestFaultKind::access_violation);
    NYXORA_CHECK(result.fault->address == 0);
    const auto code_base = reinterpret_cast<nyxora::GuestAddress>(code->host_pointer());
    NYXORA_CHECK(result.fault->instruction_pointer >= code_base);
    NYXORA_CHECK(result.fault->instruction_pointer < code_base + guest_code.size());
#endif
}

NYXORA_TEST(guest_fault_capture_preserves_normal_return) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    const std::array<std::byte, 6> guest_code{
        std::byte{0xb8}, std::byte{0x2a}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xc3},
    };
    NYXORA_CHECK(code->copy(0, guest_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, guest_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    const auto result = nyxora::runtime::invoke_guest_captured(
        *trampoline, reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), stack->top());
    NYXORA_CHECK(result.completed());
    NYXORA_CHECK(result.value == 42);
#endif
}
