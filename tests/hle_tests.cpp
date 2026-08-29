#include "test.hpp"
#include "nyxora/hle/libkernel.hpp"
#include "nyxora/runtime/hle_registry.hpp"
#include "nyxora/runtime/native_thread.hpp"
#include "nyxora/runtime/symbol_registry.hpp"

#include <cstdint>

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
    NYXORA_CHECK(hle.size() == 4);

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
