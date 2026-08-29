#include "test.hpp"
#include "nyxora/hle/libkernel.hpp"
#include "nyxora/runtime/hle_registry.hpp"
#include "nyxora/runtime/native_thread.hpp"
#include "nyxora/runtime/symbol_registry.hpp"
#include "nyxora/runtime/thread_manager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <cstring>

namespace {
std::uint64_t returns_123() {
    return 123;
}

nyxora::runtime::SymbolKey no_arg_test_key() {
    return nyxora::runtime::SymbolKey{
        .nid = "bridge-test",
        .library = "test",
        .module = "test",
        .library_version = 1,
        .module_major = 1,
        .module_minor = 0,
        .kind = nyxora::runtime::SymbolKind::function,
    };
}

void emit_mov_imm64(std::span<std::byte> output, std::size_t& at, std::byte opcode,
                    std::uint64_t value) {
    NYXORA_CHECK(at <= output.size() && output.size() - at >= 10);
    output[at++] = std::byte{0x48};
    output[at++] = opcode;
    std::memcpy(output.data() + at, &value, sizeof(value));
    at += sizeof(value);
}

nyxora::runtime::SymbolKey libkernel_key(const char* nid) {
    return nyxora::runtime::SymbolKey{
        .nid = nid,
        .library = "libkernel",
        .module = "libkernel",
        .library_version = 1,
        .module_major = 1,
        .module_minor = 1,
        .kind = nyxora::runtime::SymbolKind::function,
    };
}
}

NYXORA_TEST(hle_no_arg_bridge_is_guest_callable) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    NYXORA_CHECK(hle.register_no_arg(no_arg_test_key(), returns_123, "returns_123"));
    const auto binding = symbols.resolve(no_arg_test_key());
    NYXORA_CHECK(binding.has_value());
    NYXORA_CHECK(binding->hle);

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    NYXORA_CHECK(trampoline->invoke(binding->address, stack->top()) == 123);
#endif
}

NYXORA_TEST(libkernel_core_registers_process_time_and_cpu_hle) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    NYXORA_CHECK(hle.size() == 8);

    const auto frequency = symbols.resolve(libkernel_key("BNowx2l588E"));
    const auto cpu = symbols.resolve(libkernel_key("g0VTBxfJyu0"));
    NYXORA_CHECK(frequency.has_value());
    NYXORA_CHECK(cpu.has_value());

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    NYXORA_CHECK(trampoline->invoke(frequency->address, stack->top()) == 1'000'000'000ULL);
    NYXORA_CHECK(trampoline->invoke(cpu->address, stack->top()) == 0);
#endif
}

namespace {
std::uint64_t hle_sum_four(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
    return a + b * 10U + c * 100U + d * 1000U;
}
}

NYXORA_TEST(hle_four_register_bridge_is_guest_callable) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry registry(symbols);
    nyxora::runtime::SymbolKey key{
        .nid = "sum4",
        .library = "test",
        .module = "test",
        .library_version = 1,
        .module_major = 1,
        .module_minor = 0,
        .kind = nyxora::runtime::SymbolKind::function,
    };
    NYXORA_CHECK(registry.register_function(
        key, reinterpret_cast<nyxora::GuestAddress>(&hle_sum_four), "sum4"));
    const auto binding = symbols.resolve(key);
    NYXORA_CHECK(binding.has_value());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    std::array<std::byte, 64> guest_code{};
    guest_code.fill(std::byte{0x90});
    std::size_t at = 0;
    emit_mov_imm64(guest_code, at, std::byte{0xbf}, 1); // rdi
    emit_mov_imm64(guest_code, at, std::byte{0xbe}, 2); // rsi
    emit_mov_imm64(guest_code, at, std::byte{0xba}, 3); // rdx
    emit_mov_imm64(guest_code, at, std::byte{0xb9}, 4); // rcx
    emit_mov_imm64(guest_code, at, std::byte{0xb8}, binding->address); // rax
    NYXORA_CHECK(guest_code.size() - at >= 11);
    guest_code[at++] = std::byte{0x48};
    guest_code[at++] = std::byte{0x83};
    guest_code[at++] = std::byte{0xec};
    guest_code[at++] = std::byte{0x08}; // sub rsp,8 for a nested SysV call
    guest_code[at++] = std::byte{0xff};
    guest_code[at++] = std::byte{0xd0}; // call rax
    guest_code[at++] = std::byte{0x48};
    guest_code[at++] = std::byte{0x83};
    guest_code[at++] = std::byte{0xc4};
    guest_code[at++] = std::byte{0x08}; // add rsp,8
    guest_code[at++] = std::byte{0xc3}; // ret
    NYXORA_CHECK(code->copy(0, std::span<const std::byte>(guest_code.data(), at)));
    NYXORA_CHECK(code->flush_instruction_cache(0, at));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    const auto result = trampoline->invoke(
        reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), stack->top());
    NYXORA_CHECK(result == 4321);
#endif
}

NYXORA_TEST(libkernel_pthread_create_and_join_work_through_guest_hle_calls) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls);

    const auto create_binding = symbols.resolve(libkernel_key("OxhIB8LB-PQ"));
    const auto join_binding = symbols.resolve(libkernel_key("h9CcP3J0oVM"));
    NYXORA_CHECK(create_binding.has_value());
    NYXORA_CHECK(join_binding.has_value());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));

    // Child start routine: return its single pointer/integer argument.
    const std::array<std::byte, 4> child_code{
        std::byte{0x48}, std::byte{0x89}, std::byte{0xf8}, std::byte{0xc3},
    };
    NYXORA_CHECK(code->copy(0, child_code));

    nyxora::GuestAddress handle = 0;
    nyxora::GuestAddress child_result = 0;
    constexpr nyxora::GuestAddress argument = 0x1122334455667788ULL;

    std::array<std::byte, 64> create_code{};
    create_code.fill(std::byte{0x90});
    std::size_t create_at = 0;
    emit_mov_imm64(create_code, create_at, std::byte{0xbf},
                   reinterpret_cast<std::uint64_t>(&handle));       // rdi = pthread_t*
    emit_mov_imm64(create_code, create_at, std::byte{0xbe}, 0);    // rsi = attr
    emit_mov_imm64(create_code, create_at, std::byte{0xba},
                   reinterpret_cast<std::uint64_t>(code->host_pointer())); // rdx = start
    emit_mov_imm64(create_code, create_at, std::byte{0xb9}, argument);      // rcx = arg
    emit_mov_imm64(create_code, create_at, std::byte{0xb8}, create_binding->address);
    NYXORA_CHECK(create_code.size() - create_at >= 11);
    create_code[create_at++] = std::byte{0x48};
    create_code[create_at++] = std::byte{0x83};
    create_code[create_at++] = std::byte{0xec};
    create_code[create_at++] = std::byte{0x08}; // sub rsp,8
    create_code[create_at++] = std::byte{0xff};
    create_code[create_at++] = std::byte{0xd0}; // call rax
    create_code[create_at++] = std::byte{0x48};
    create_code[create_at++] = std::byte{0x83};
    create_code[create_at++] = std::byte{0xc4};
    create_code[create_at++] = std::byte{0x08}; // add rsp,8
    create_code[create_at++] = std::byte{0xc3}; // ret
    constexpr std::size_t create_offset = 0x100;
    NYXORA_CHECK(code->copy(create_offset,
                            std::span<const std::byte>(create_code.data(), create_at)));

    std::array<std::byte, 48> join_code{};
    join_code.fill(std::byte{0x90});
    std::size_t join_at = 0;
    // The handle value is filled after pthread_create returns.
    const auto handle_immediate_offset = join_at + 2;
    emit_mov_imm64(join_code, join_at, std::byte{0xbf}, 0); // rdi = handle
    emit_mov_imm64(join_code, join_at, std::byte{0xbe},
                   reinterpret_cast<std::uint64_t>(&child_result));
    emit_mov_imm64(join_code, join_at, std::byte{0xb8}, join_binding->address);
    NYXORA_CHECK(join_code.size() - join_at >= 11);
    join_code[join_at++] = std::byte{0x48};
    join_code[join_at++] = std::byte{0x83};
    join_code[join_at++] = std::byte{0xec};
    join_code[join_at++] = std::byte{0x08}; // sub rsp,8
    join_code[join_at++] = std::byte{0xff};
    join_code[join_at++] = std::byte{0xd0};
    join_code[join_at++] = std::byte{0x48};
    join_code[join_at++] = std::byte{0x83};
    join_code[join_at++] = std::byte{0xc4};
    join_code[join_at++] = std::byte{0x08}; // add rsp,8
    join_code[join_at++] = std::byte{0xc3};
    constexpr std::size_t join_offset = 0x200;
    NYXORA_CHECK(code->copy(join_offset,
                            std::span<const std::byte>(join_code.data(), join_at)));
    NYXORA_CHECK(code->flush_instruction_cache(0, page));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());

    nyxora::runtime::ScopedGuestThreadManager manager_scope(manager);
    const auto create_result = trampoline->invoke(
        reinterpret_cast<nyxora::GuestAddress>(code->host_pointer(create_offset)), stack->top());
    NYXORA_CHECK(create_result == 0);
    NYXORA_CHECK(handle != 0);
    NYXORA_CHECK(manager.size() == 1);

    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    NYXORA_CHECK(code->copy(join_offset + handle_immediate_offset,
                            std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(&handle), sizeof(handle))));
    NYXORA_CHECK(code->flush_instruction_cache(join_offset, join_at));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    const auto join_result = trampoline->invoke(
        reinterpret_cast<nyxora::GuestAddress>(code->host_pointer(join_offset)), stack->top());
    NYXORA_CHECK(join_result == 0);
    NYXORA_CHECK(child_result == argument);
    NYXORA_CHECK(manager.size() == 0);
#endif
}
