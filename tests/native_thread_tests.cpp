#include "test.hpp"
#include "nyxora/memory/native_arena.hpp"
#include "nyxora/runtime/native_thread.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
struct EntryObservation {
    std::uint64_t rsp{};
    std::uint64_t rbp{};
};
}

NYXORA_TEST(guest_stack_reserves_guarded_writable_memory) {
    const auto page = nyxora::memory::NativeArena::page_size();
    auto stack = nyxora::runtime::GuestStack::create(page * 2U);
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(stack->size() >= page * 2U);
    NYXORA_CHECK(stack->top() > stack->base());
    NYXORA_CHECK(stack->contains(stack->base()));
    NYXORA_CHECK(stack->contains(stack->top() - 1));
    NYXORA_CHECK(!stack->contains(stack->top()));
}

NYXORA_TEST(entry_trampoline_switches_to_guest_stack_and_sysv_arguments) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));

    // mov [rdi],rsp; mov [rdi+8],rbp; mov rax,rsi; add rax,rdx; ret
    const std::array<std::byte, 13> guest_code{
        std::byte{0x48}, std::byte{0x89}, std::byte{0x27},
        std::byte{0x48}, std::byte{0x89}, std::byte{0x6f}, std::byte{0x08},
        std::byte{0x48}, std::byte{0x89}, std::byte{0xf0},
        std::byte{0x48}, std::byte{0x01}, std::byte{0xd0},
    };
    const std::byte ret = std::byte{0xc3};
    NYXORA_CHECK(code->copy(0, guest_code));
    NYXORA_CHECK(code->copy(guest_code.size(), std::span<const std::byte>(&ret, 1)));
    NYXORA_CHECK(code->flush_instruction_cache(0, guest_code.size() + 1));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    auto stack = nyxora::runtime::GuestStack::create(page * 4U);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());

    EntryObservation observation{};
    const auto result = trampoline->invoke(
        reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), stack->top(),
        reinterpret_cast<std::uint64_t>(&observation), 20, 22);

    NYXORA_CHECK(result == 42);
    NYXORA_CHECK(stack->contains(observation.rsp));
    NYXORA_CHECK(stack->contains(observation.rbp));
    NYXORA_CHECK((observation.rbp & 0xfU) == 0);
#endif
}
