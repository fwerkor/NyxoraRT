#include "test.hpp"
#include "nyxora/hle/libkernel.hpp"
#include "nyxora/runtime/hle_registry.hpp"
#include "nyxora/runtime/kernel_services.hpp"
#include "nyxora/runtime/native_thread.hpp"
#include "nyxora/runtime/symbol_registry.hpp"
#include "nyxora/runtime/thread_manager.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <span>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>

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


#if defined(_WIN32) && defined(_M_X64)
std::uint64_t host_stack_local_address() {
    volatile std::uint64_t marker = 0;
    return reinterpret_cast<std::uint64_t>(&marker);
}
#endif
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
    NYXORA_CHECK(hle.size() == 127);

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
    const auto self_binding = symbols.resolve(libkernel_key("EotR8a3ASf4"));
    NYXORA_CHECK(create_binding.has_value());
    NYXORA_CHECK(join_binding.has_value());
    NYXORA_CHECK(self_binding.has_value());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));

    // Child start routine: call pthread_self and return its opaque handle.
    std::array<std::byte, 32> child_code{};
    child_code.fill(std::byte{0x90});
    std::size_t child_at = 0;
    emit_mov_imm64(child_code, child_at, std::byte{0xb8}, self_binding->address);
    NYXORA_CHECK(child_code.size() - child_at >= 11);
    child_code[child_at++] = std::byte{0x48};
    child_code[child_at++] = std::byte{0x83};
    child_code[child_at++] = std::byte{0xec};
    child_code[child_at++] = std::byte{0x08};
    child_code[child_at++] = std::byte{0xff};
    child_code[child_at++] = std::byte{0xd0};
    child_code[child_at++] = std::byte{0x48};
    child_code[child_at++] = std::byte{0x83};
    child_code[child_at++] = std::byte{0xc4};
    child_code[child_at++] = std::byte{0x08};
    child_code[child_at++] = std::byte{0xc3};
    NYXORA_CHECK(code->copy(0, std::span<const std::byte>(child_code.data(), child_at)));

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
    NYXORA_CHECK(child_result == handle);
    NYXORA_CHECK(manager.size() == 0);
#endif
}


NYXORA_TEST(libkernel_pthread_self_and_detach_use_runtime_thread_identity) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls);

    const auto self_binding = symbols.resolve(libkernel_key("EotR8a3ASf4"));
    const auto detach_binding = symbols.resolve(libkernel_key("+U1R4WtXvoc"));
    NYXORA_CHECK(self_binding.has_value());
    NYXORA_CHECK(detach_binding.has_value());

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());

    nyxora::runtime::ScopedGuestThreadManager manager_scope(manager);
    NYXORA_CHECK(trampoline->invoke(self_binding->address, stack->top()) == manager.root_handle());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    const std::array<std::byte, 1> child_code{std::byte{0xc3}};
    NYXORA_CHECK(code->copy(0, child_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, child_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    nyxora::GuestAddress handle = 0;
    NYXORA_CHECK(manager.create(
                     &handle, 0,
                     reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), 0,
                     64 * 1024) == 0);
    NYXORA_CHECK(trampoline->invoke(detach_binding->address, stack->top(), handle) == 0);
    NYXORA_CHECK(manager.join(handle, nullptr) == nyxora::runtime::GuestThreadManager::kPosixEinval);
#endif
}


NYXORA_TEST(libkernel_memory_hle_uses_runtime_guest_address_space) {
#if defined(__x86_64__) || defined(_M_X64)
    constexpr nyxora::GuestSize guest_page = 0x4000;
    const auto host_page = static_cast<nyxora::GuestSize>(nyxora::memory::NativeArena::page_size());
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(guest_page * 5 + host_page);
    NYXORA_CHECK(memory.has_value());
    const auto arena_base = memory->native_base();
    const auto data_base = (arena_base + guest_page - 1) / guest_page * guest_page;
    NYXORA_CHECK(memory->map(data_base, guest_page * 2,
                             nyxora::memory::Protection::read |
                                 nyxora::memory::Protection::write,
                             "hle-data"));

    nyxora::runtime::KernelServices services(*memory);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto direct_size = symbols.resolve(libkernel_key("pO96TwzOm5E"));
    const auto mprotect = symbols.resolve(libkernel_key("vSMAm3cxYTY"));
    NYXORA_CHECK(direct_size.has_value());
    NYXORA_CHECK(mprotect.has_value());

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    nyxora::runtime::ScopedGuestThreadManager manager_scope(manager);
    NYXORA_CHECK(trampoline->invoke(direct_size->address, stack->top()) == memory->native_size());
    NYXORA_CHECK(trampoline->invoke(mprotect->address, stack->top(), data_base, guest_page, 1) == 0);

    const auto* first = memory->find(data_base);
    const auto* second = memory->find(data_base + guest_page);
    NYXORA_CHECK(first != nullptr);
    NYXORA_CHECK(second != nullptr);
    NYXORA_CHECK(first->protection == nyxora::memory::Protection::read);
    NYXORA_CHECK(second->protection ==
                 (nyxora::memory::Protection::read | nyxora::memory::Protection::write));
    const std::array<std::byte, 1> value{std::byte{0x55}};
    NYXORA_CHECK(!memory->write(data_base, value));
    NYXORA_CHECK(memory->write(data_base + guest_page, value));

    NYXORA_CHECK(trampoline->invoke(mprotect->address, stack->top(), data_base, guest_page, 2) == 0);
    const auto* read_write = memory->find(data_base);
    NYXORA_CHECK(read_write != nullptr);
    NYXORA_CHECK(read_write->protection ==
                 (nyxora::memory::Protection::read | nyxora::memory::Protection::write));
    NYXORA_CHECK(memory->write(data_base, value));
#endif
}

NYXORA_TEST(libkernel_file_hle_reads_only_inside_guest_root) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto unique = std::to_string(reinterpret_cast<std::uintptr_t>(&libkernel_file_hle_reads_only_inside_guest_root));
    const auto temp = std::filesystem::temp_directory_path();
    const auto root = temp / ("nyxora-hle-" + unique);
    const auto outside = temp / ("nyxora-hle-outside-" + unique + ".bin");
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path root;
        std::filesystem::path outside;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
            error.clear();
            std::filesystem::remove(outside, error);
        }
    } cleanup{root, outside};
    {
        std::ofstream output(root / "data.bin", std::ios::binary);
        output << "nyxora-file";
    }
    {
        std::ofstream output(outside, std::ios::binary);
        output << "outside";
    }

    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress data_base = 0x20000;
    NYXORA_CHECK(memory.map(data_base, 0x4000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "guest-io"));
    constexpr char guest_path[] = "/app0/data.bin";
    NYXORA_CHECK(memory.write(
        data_base,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(guest_path), sizeof(guest_path))));

    nyxora::runtime::KernelServices services(memory);
    nyxora::runtime::TlsRegistry denied_tls;
    nyxora::runtime::GuestThreadManager denied_manager(denied_tls, &services);
    nyxora::runtime::SymbolRegistry denied_symbols;
    nyxora::runtime::HleRegistry denied_hle(denied_symbols);
    nyxora::hle::libkernel::register_core(denied_hle);
    const auto denied_open = denied_symbols.resolve(libkernel_key("1G3lF1Gg1k8"));
    NYXORA_CHECK(denied_open.has_value());
    {
        auto denied_stack = nyxora::runtime::GuestStack::create(64 * 1024);
        auto denied_trampoline = nyxora::runtime::EntryTrampoline::create();
        NYXORA_CHECK(denied_stack.has_value());
        NYXORA_CHECK(denied_trampoline.has_value());
        nyxora::runtime::ScopedGuestThreadManager denied_scope(denied_manager);
        const auto result = denied_trampoline->invoke(denied_open->address, denied_stack->top(),
                                                       data_base, 0, 0);
        NYXORA_CHECK(static_cast<std::uint32_t>(result) ==
                     nyxora::runtime::KernelServices::kErrorEacces);
    }

    NYXORA_CHECK(services.set_guest_root(root));
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto open = symbols.resolve(libkernel_key("1G3lF1Gg1k8"));
    const auto read = symbols.resolve(libkernel_key("Cg4srZ6TKbU"));
    const auto close = symbols.resolve(libkernel_key("UK2Tl2DWUns"));
    NYXORA_CHECK(open.has_value());
    NYXORA_CHECK(read.has_value());
    NYXORA_CHECK(close.has_value());

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    nyxora::runtime::ScopedGuestThreadManager manager_scope(manager);

    const auto fd = trampoline->invoke(open->address, stack->top(), data_base, 0, 0);
    NYXORA_CHECK(fd >= 3 && fd < 1024);
    constexpr nyxora::GuestAddress buffer = data_base + 0x100;
    const auto count = trampoline->invoke(read->address, stack->top(), fd, buffer, 11);
    NYXORA_CHECK(count == 11);
    const auto bytes = memory.view(buffer, 11);
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    NYXORA_CHECK(text == "nyxora-file");
    NYXORA_CHECK(trampoline->invoke(close->address, stack->top(), fd) == 0);

    const auto denied = trampoline->invoke(open->address, stack->top(), data_base, 0x0200, 0);
    NYXORA_CHECK(static_cast<std::uint32_t>(denied) == nyxora::runtime::KernelServices::kErrorEacces);

    const auto traversal = std::string("/app0/../") + outside.filename().string();
    NYXORA_CHECK(memory.write(
        data_base,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(traversal.c_str()),
                                  traversal.size() + 1)));
    const auto escaped = trampoline->invoke(open->address, stack->top(), data_base, 0, 0);
    NYXORA_CHECK(static_cast<std::uint32_t>(escaped) ==
                 nyxora::runtime::KernelServices::kErrorEnoent);
#endif
}

NYXORA_TEST(libkernel_mutex_hle_preserves_posix_and_orbis_error_conventions) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress slot = 0x30000;
    NYXORA_CHECK(memory.map(slot, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "mutex"));
    NYXORA_CHECK(memory.zero(slot, 8));
    nyxora::runtime::KernelServices services(memory);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);

    const auto sce_init = symbols.resolve(libkernel_key("cmo1RIYva9o"));
    const auto posix_lock = symbols.resolve(libkernel_key("7H0iTOciTLo"));
    const auto posix_unlock = symbols.resolve(libkernel_key("2Z+PpY6CaJg"));
    const auto orbis_destroy = symbols.resolve(libkernel_key("2Of0f+3mhhE"));
    NYXORA_CHECK(sce_init.has_value());
    NYXORA_CHECK(posix_lock.has_value());
    NYXORA_CHECK(posix_unlock.has_value());
    NYXORA_CHECK(orbis_destroy.has_value());

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    nyxora::runtime::ScopedGuestThreadManager manager_scope(manager);

    NYXORA_CHECK(trampoline->invoke(sce_init->address, stack->top(), slot, 0, 0) == 0);
    NYXORA_CHECK(trampoline->invoke(posix_lock->address, stack->top(), slot) == 0);
    NYXORA_CHECK(trampoline->invoke(posix_lock->address, stack->top(), slot) ==
                 nyxora::runtime::KernelServices::kPosixEdeadlk);
    NYXORA_CHECK(trampoline->invoke(posix_unlock->address, stack->top(), slot) == 0);
    NYXORA_CHECK(trampoline->invoke(orbis_destroy->address, stack->top(), slot) == 0);
    const auto destroyed_again = trampoline->invoke(orbis_destroy->address, stack->top(), slot);
    NYXORA_CHECK(static_cast<std::uint32_t>(destroyed_again) ==
                 nyxora::runtime::KernelServices::kErrorEinval);

    NYXORA_CHECK(memory.zero(slot, 8));
    NYXORA_CHECK(trampoline->invoke(posix_lock->address, stack->top(), slot) == 0);
    NYXORA_CHECK(trampoline->invoke(posix_unlock->address, stack->top(), slot) == 0);
    NYXORA_CHECK(trampoline->invoke(orbis_destroy->address, stack->top(), slot) == 0);
#endif
}


NYXORA_TEST(libkernel_services_propagate_into_pthread_guest_workers) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 4U);
    NYXORA_CHECK(memory.has_value());
    nyxora::runtime::KernelServices services(*memory);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto direct_size = symbols.resolve(libkernel_key("pO96TwzOm5E"));
    NYXORA_CHECK(direct_size.has_value());

    nyxora::GuestAddress handle = 0;
    NYXORA_CHECK(manager.create(&handle, 0, direct_size->address, 0, 64 * 1024) == 0);
    nyxora::GuestAddress result = 0;
    NYXORA_CHECK(manager.join(handle, &result) == 0);
    NYXORA_CHECK(result == memory->native_size());
#endif
}


NYXORA_TEST(windows_hle_bridge_calls_host_cpp_on_the_os_stack) {
#if defined(_WIN32) && defined(_M_X64)
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    const auto custom_key = libkernel_key("host-stack-test");
    NYXORA_CHECK(hle.register_no_arg(custom_key, host_stack_local_address, "host stack test"));
    const auto binding = symbols.resolve(custom_key);
    NYXORA_CHECK(binding.has_value());
    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    const auto local_address = trampoline->invoke(binding->address, stack->top());
    NYXORA_CHECK(local_address != 0);
    NYXORA_CHECK(!stack->contains(local_address));
#endif
}


NYXORA_TEST(libkernel_pthread_exit_uses_guest_recovery_and_preserves_status) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto exit_binding = symbols.resolve(libkernel_key("FJrT5LuUBAU"));
    NYXORA_CHECK(exit_binding.has_value());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    std::array<std::byte, 64> guest_code{};
    guest_code.fill(std::byte{0x90});
    std::size_t at = 0;
    constexpr std::uint64_t exit_status = 0x1122334455667788ULL;
    emit_mov_imm64(guest_code, at, std::byte{0xbf}, exit_status); // rdi
    emit_mov_imm64(guest_code, at, std::byte{0xb8}, exit_binding->address); // rax
    NYXORA_CHECK(guest_code.size() - at >= 18);
    guest_code[at++] = std::byte{0x48};
    guest_code[at++] = std::byte{0x83};
    guest_code[at++] = std::byte{0xec};
    guest_code[at++] = std::byte{0x08}; // sub rsp,8 for nested SysV call
    guest_code[at++] = std::byte{0xff};
    guest_code[at++] = std::byte{0xd0}; // call rax; must not return
    guest_code[at++] = std::byte{0x48};
    guest_code[at++] = std::byte{0x83};
    guest_code[at++] = std::byte{0xc4};
    guest_code[at++] = std::byte{0x08};
    guest_code[at++] = std::byte{0xb8};
    guest_code[at++] = std::byte{0xef};
    guest_code[at++] = std::byte{0xbe};
    guest_code[at++] = std::byte{0xad};
    guest_code[at++] = std::byte{0xde}; // mov eax,0xdeadbeef
    guest_code[at++] = std::byte{0xc3};
    NYXORA_CHECK(code->copy(0, std::span<const std::byte>(guest_code.data(), at)));
    NYXORA_CHECK(code->flush_instruction_cache(0, at));
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
    NYXORA_CHECK(result.value == exit_status);
#endif
}


NYXORA_TEST(libkernel_pthread_attributes_control_stack_size_and_detach_state) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress data_base = 0x50000;
    constexpr nyxora::GuestAddress attr_slot = data_base;
    constexpr nyxora::GuestAddress stack_size_out = data_base + 0x20;
    constexpr nyxora::GuestAddress detach_out = data_base + 0x30;
    NYXORA_CHECK(memory.map(data_base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "pthread-attr"));
    NYXORA_CHECK(memory.zero(data_base, 0x1000));

    nyxora::runtime::KernelServices services(memory);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);

    const auto init = symbols.resolve(libkernel_key("wtkt-teR1so"));
    const auto destroy = symbols.resolve(libkernel_key("zHchY8ft5pk"));
    const auto get_stack = symbols.resolve(libkernel_key("0qOtCR-ZHck"));
    const auto set_stack = symbols.resolve(libkernel_key("2Q0z6rnBrTE"));
    const auto get_detach = symbols.resolve(libkernel_key("VUT1ZSrHT0I"));
    const auto set_detach = symbols.resolve(libkernel_key("E+tyo3lp5Lw"));
    NYXORA_CHECK(init.has_value());
    NYXORA_CHECK(destroy.has_value());
    NYXORA_CHECK(get_stack.has_value());
    NYXORA_CHECK(set_stack.has_value());
    NYXORA_CHECK(get_detach.has_value());
    NYXORA_CHECK(set_detach.has_value());

    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    nyxora::runtime::ScopedGuestThreadManager manager_scope(manager);

    NYXORA_CHECK(trampoline->invoke(init->address, stack->top(), attr_slot) == 0);
    NYXORA_CHECK(trampoline->invoke(get_stack->address, stack->top(), attr_slot, stack_size_out) == 0);
    const auto initial_stack = memory.view(stack_size_out, sizeof(std::uint64_t));
    std::uint64_t initial_stack_size = 0;
    std::memcpy(&initial_stack_size, initial_stack.data(), sizeof(initial_stack_size));
    NYXORA_CHECK(initial_stack_size == nyxora::runtime::KernelServices::kDefaultThreadStackSize);

    constexpr std::uint64_t requested_stack = 32 * 1024;
    NYXORA_CHECK(trampoline->invoke(set_stack->address, stack->top(), attr_slot, requested_stack) == 0);
    NYXORA_CHECK(trampoline->invoke(set_stack->address, stack->top(), attr_slot, 4096) ==
                 nyxora::runtime::KernelServices::kPosixEinval);
    NYXORA_CHECK(trampoline->invoke(get_stack->address, stack->top(), attr_slot, stack_size_out) == 0);
    const auto updated_stack = memory.view(stack_size_out, sizeof(std::uint64_t));
    std::uint64_t updated_stack_size = 0;
    std::memcpy(&updated_stack_size, updated_stack.data(), sizeof(updated_stack_size));
    NYXORA_CHECK(updated_stack_size == requested_stack);

    NYXORA_CHECK(trampoline->invoke(get_detach->address, stack->top(), attr_slot, detach_out) == 0);
    const auto initial_detach = memory.view(detach_out, sizeof(std::uint32_t));
    std::uint32_t detach_state = 0;
    std::memcpy(&detach_state, initial_detach.data(), sizeof(detach_state));
    NYXORA_CHECK(detach_state == 0);
    NYXORA_CHECK(trampoline->invoke(set_detach->address, stack->top(), attr_slot, 1) == 0);
    NYXORA_CHECK(trampoline->invoke(set_detach->address, stack->top(), attr_slot, 2) ==
                 nyxora::runtime::KernelServices::kPosixEinval);
    NYXORA_CHECK(trampoline->invoke(get_detach->address, stack->top(), attr_slot, detach_out) == 0);
    const auto detached_bytes = memory.view(detach_out, sizeof(std::uint32_t));
    std::memcpy(&detach_state, detached_bytes.data(), sizeof(detach_state));
    NYXORA_CHECK(detach_state == 1);

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    const std::array<std::byte, 1> child_code{std::byte{0xc3}};
    NYXORA_CHECK(code->copy(0, child_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, child_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));
    nyxora::GuestAddress handle = 0;
    NYXORA_CHECK(manager.create(
                     &handle, attr_slot,
                     reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), 0) == 0);
    NYXORA_CHECK(handle != 0);
    NYXORA_CHECK(manager.join(handle, nullptr) == nyxora::runtime::GuestThreadManager::kPosixEinval);

    NYXORA_CHECK(trampoline->invoke(destroy->address, stack->top(), attr_slot) == 0);
    const auto slot_bytes = memory.view(attr_slot, sizeof(std::uint64_t));
    std::uint64_t destroyed_handle = 1;
    std::memcpy(&destroyed_handle, slot_bytes.data(), sizeof(destroyed_handle));
    NYXORA_CHECK(destroyed_handle == 0);
#endif
}

NYXORA_TEST(libkernel_orbis_pthread_attribute_errors_use_kernel_encoding) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0x60000;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "orbis-attr"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto init = symbols.resolve(libkernel_key("nsYoNRywwNg"));
    const auto set_stack = symbols.resolve(libkernel_key("UTXzJbWhhTE"));
    const auto destroy = symbols.resolve(libkernel_key("62KCwEMmzcM"));
    NYXORA_CHECK(init.has_value());
    NYXORA_CHECK(set_stack.has_value());
    NYXORA_CHECK(destroy.has_value());
    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    nyxora::runtime::ScopedGuestThreadManager manager_scope(manager);
    NYXORA_CHECK(trampoline->invoke(init->address, stack->top(), base) == 0);
    const auto invalid = trampoline->invoke(set_stack->address, stack->top(), base, 4096);
    NYXORA_CHECK(static_cast<std::uint32_t>(invalid) == 0x80020016U);
    NYXORA_CHECK(trampoline->invoke(destroy->address, stack->top(), base) == 0);
#endif
}


NYXORA_TEST(libkernel_condition_wait_atomically_releases_and_reacquires_mutex) {
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0x70000;
    constexpr nyxora::GuestAddress mutex_slot = base;
    constexpr nyxora::GuestAddress cond_slot = base + 0x20;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "condvar"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    NYXORA_CHECK(services.mutex_init(mutex_slot, 0, 0) == 0);
    NYXORA_CHECK(services.cond_init(cond_slot, 0) == 0);

    std::atomic<bool> waiter_has_mutex{false};
    std::atomic<int> waiter_result{-1};
    std::thread waiter([&] {
        const auto lock_result = services.mutex_lock(mutex_slot);
        if (lock_result != 0) {
            waiter_result.store(lock_result, std::memory_order_release);
            return;
        }
        waiter_has_mutex.store(true, std::memory_order_release);
        const auto wait_result = services.cond_wait(cond_slot, mutex_slot);
        if (wait_result != 0) {
            waiter_result.store(wait_result, std::memory_order_release);
            return;
        }
        const auto unlock_result = services.mutex_unlock(mutex_slot);
        waiter_result.store(unlock_result, std::memory_order_release);
    });

    while (!waiter_has_mutex.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // This lock cannot complete until cond_wait has enqueued the waiter and released the mutex.
    NYXORA_CHECK(services.mutex_lock(mutex_slot) == 0);
    NYXORA_CHECK(services.cond_signal(cond_slot) == 0);
    NYXORA_CHECK(services.mutex_unlock(mutex_slot) == 0);
    waiter.join();
    NYXORA_CHECK(waiter_result.load(std::memory_order_acquire) == 0);
    NYXORA_CHECK(services.cond_destroy(cond_slot) == 0);
    NYXORA_CHECK(services.mutex_destroy(mutex_slot) == 0);
}

NYXORA_TEST(libkernel_condition_hle_registers_posix_and_orbis_error_conventions) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0x80000;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "cond-hle"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto posix_init = symbols.resolve(libkernel_key("0TyVk4MSLt0"));
    const auto posix_destroy = symbols.resolve(libkernel_key("RXXqi4CtF8w"));
    const auto orbis_wait = symbols.resolve(libkernel_key("WKAXJ4XBPQ4"));
    NYXORA_CHECK(posix_init.has_value());
    NYXORA_CHECK(posix_destroy.has_value());
    NYXORA_CHECK(orbis_wait.has_value());
    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    nyxora::runtime::ScopedGuestThreadManager manager_scope(manager);
    NYXORA_CHECK(trampoline->invoke(posix_init->address, stack->top(), base, 0) == 0);
    const auto not_owned = trampoline->invoke(orbis_wait->address, stack->top(), base, base + 0x20);
    NYXORA_CHECK(static_cast<std::uint32_t>(not_owned) == 0x80020001U);
    NYXORA_CHECK(trampoline->invoke(posix_destroy->address, stack->top(), base) == 0);
#endif
}


NYXORA_TEST(libkernel_pthread_exit_value_flows_through_guest_thread_join) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto exit_binding = symbols.resolve(libkernel_key("FJrT5LuUBAU"));
    NYXORA_CHECK(exit_binding.has_value());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    std::array<std::byte, 48> child_code{};
    child_code.fill(std::byte{0x90});
    std::size_t at = 0;
    constexpr std::uint64_t exit_status = 0xa1b2c3d4e5f60718ULL;
    emit_mov_imm64(child_code, at, std::byte{0xbf}, exit_status);
    emit_mov_imm64(child_code, at, std::byte{0xb8}, exit_binding->address);
    child_code[at++] = std::byte{0x48};
    child_code[at++] = std::byte{0x83};
    child_code[at++] = std::byte{0xec};
    child_code[at++] = std::byte{0x08};
    child_code[at++] = std::byte{0xff};
    child_code[at++] = std::byte{0xd0};
    child_code[at++] = std::byte{0x0f};
    child_code[at++] = std::byte{0x0b}; // unreachable if pthread_exit works
    NYXORA_CHECK(code->copy(0, std::span<const std::byte>(child_code.data(), at)));
    NYXORA_CHECK(code->flush_instruction_cache(0, at));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls);
    nyxora::GuestAddress handle = 0;
    NYXORA_CHECK(manager.create(
                     &handle, 0,
                     reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), 0,
                     64 * 1024) == 0);
    nyxora::GuestAddress result = 0;
    NYXORA_CHECK(manager.join(handle, &result) == 0);
    NYXORA_CHECK(result == exit_status);
#endif
}


NYXORA_TEST(libkernel_condition_broadcast_wakes_all_waiters) {
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0x90000;
    constexpr nyxora::GuestAddress mutex_slot = base;
    constexpr nyxora::GuestAddress cond_slot = base + 0x20;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "cond-broadcast"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    NYXORA_CHECK(services.mutex_init(mutex_slot, 0, 0) == 0);
    NYXORA_CHECK(services.cond_init(cond_slot, 0) == 0);

    std::atomic<int> ready{0};
    std::atomic<int> completed{0};
    std::array<std::thread, 2> waiters;
    for (auto& waiter : waiters) {
        waiter = std::thread([&] {
            if (services.mutex_lock(mutex_slot) != 0) {
                return;
            }
            ready.fetch_add(1, std::memory_order_release);
            if (services.cond_wait(cond_slot, mutex_slot) == 0) {
                completed.fetch_add(1, std::memory_order_release);
                (void)services.mutex_unlock(mutex_slot);
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    // Acquiring the mutex proves the second waiter has also enqueued and released it.
    NYXORA_CHECK(services.mutex_lock(mutex_slot) == 0);
    NYXORA_CHECK(services.cond_broadcast(cond_slot) == 0);
    NYXORA_CHECK(services.mutex_unlock(mutex_slot) == 0);
    for (auto& waiter : waiters) {
        waiter.join();
    }
    NYXORA_CHECK(completed.load(std::memory_order_acquire) == 2);
    NYXORA_CHECK(services.cond_destroy(cond_slot) == 0);
    NYXORA_CHECK(services.mutex_destroy(mutex_slot) == 0);
}


NYXORA_TEST(libkernel_condition_attributes_and_relative_timeout_preserve_mutex_ownership) {
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0xa0000;
    constexpr nyxora::GuestAddress attr_slot = base;
    constexpr nyxora::GuestAddress clock_out = base + 0x20;
    constexpr nyxora::GuestAddress pshared_out = base + 0x30;
    constexpr nyxora::GuestAddress mutex_slot = base + 0x40;
    constexpr nyxora::GuestAddress cond_slot = base + 0x60;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "cond-attrs"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);

    NYXORA_CHECK(services.cond_attr_init(attr_slot) == 0);
    NYXORA_CHECK(services.cond_attr_get_clock(attr_slot, clock_out) == 0);
    std::uint32_t clock_id = 99;
    auto bytes = memory.view(clock_out, sizeof(clock_id));
    std::memcpy(&clock_id, bytes.data(), sizeof(clock_id));
    NYXORA_CHECK(clock_id == 0);
    NYXORA_CHECK(services.cond_attr_set_clock(attr_slot, 4) == 0);
    NYXORA_CHECK(services.cond_attr_set_clock(attr_slot, 3) ==
                 nyxora::runtime::KernelServices::kPosixEinval);
    NYXORA_CHECK(services.cond_attr_get_pshared(attr_slot, pshared_out) == 0);
    std::uint32_t pshared = 99;
    bytes = memory.view(pshared_out, sizeof(pshared));
    std::memcpy(&pshared, bytes.data(), sizeof(pshared));
    NYXORA_CHECK(pshared == 0);
    NYXORA_CHECK(services.cond_attr_set_pshared(attr_slot, 0) == 0);
    NYXORA_CHECK(services.cond_attr_set_pshared(attr_slot, 1) ==
                 nyxora::runtime::KernelServices::kPosixEinval);

    NYXORA_CHECK(services.mutex_init(mutex_slot, 0, 0) == 0);
    NYXORA_CHECK(services.cond_init(cond_slot, attr_slot) == 0);
    NYXORA_CHECK(services.mutex_lock(mutex_slot) == 0);
    const auto before = std::chrono::steady_clock::now();
    NYXORA_CHECK(services.cond_reltimed_wait(cond_slot, mutex_slot, 2'000) ==
                 nyxora::runtime::KernelServices::kPosixEtimedout);
    NYXORA_CHECK(std::chrono::steady_clock::now() >= before);
    // Timed wait must return with the mutex reacquired.
    NYXORA_CHECK(services.mutex_unlock(mutex_slot) == 0);
    NYXORA_CHECK(services.cond_destroy(cond_slot) == 0);
    NYXORA_CHECK(services.mutex_destroy(mutex_slot) == 0);
    NYXORA_CHECK(services.cond_attr_destroy(attr_slot) == 0);
}

NYXORA_TEST(libkernel_monotonic_condition_timed_wait_uses_attribute_clock) {
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0xb0000;
    constexpr nyxora::GuestAddress attr_slot = base;
    constexpr nyxora::GuestAddress mutex_slot = base + 0x20;
    constexpr nyxora::GuestAddress cond_slot = base + 0x40;
    constexpr nyxora::GuestAddress deadline_address = base + 0x60;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "cond-monotonic"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    NYXORA_CHECK(services.cond_attr_init(attr_slot) == 0);
    NYXORA_CHECK(services.cond_attr_set_clock(attr_slot, 4) == 0);
    NYXORA_CHECK(services.mutex_init(mutex_slot, 0, 0) == 0);
    NYXORA_CHECK(services.cond_init(cond_slot, attr_slot) == 0);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    const auto since_epoch = deadline.time_since_epoch();
    const auto whole_seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    const std::array<std::int64_t, 2> guest_deadline{
        whole_seconds.count(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch - whole_seconds).count(),
    };
    NYXORA_CHECK(memory.write(
        deadline_address,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(guest_deadline.data()),
                                   sizeof(guest_deadline))));

    std::atomic<bool> waiting{false};
    std::atomic<int> result{-1};
    std::thread waiter([&] {
        if (services.mutex_lock(mutex_slot) != 0) {
            return;
        }
        waiting.store(true, std::memory_order_release);
        result.store(services.cond_timed_wait(cond_slot, mutex_slot, deadline_address),
                     std::memory_order_release);
        (void)services.mutex_unlock(mutex_slot);
    });
    while (!waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    NYXORA_CHECK(services.mutex_lock(mutex_slot) == 0);
    NYXORA_CHECK(services.cond_signal(cond_slot) == 0);
    NYXORA_CHECK(services.mutex_unlock(mutex_slot) == 0);
    waiter.join();
    NYXORA_CHECK(result.load(std::memory_order_acquire) == 0);
    NYXORA_CHECK(services.cond_destroy(cond_slot) == 0);
    NYXORA_CHECK(services.mutex_destroy(mutex_slot) == 0);
    NYXORA_CHECK(services.cond_attr_destroy(attr_slot) == 0);
}

NYXORA_TEST(libkernel_condition_timed_hle_uses_posix_and_orbis_timeout_codes) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0xc0000;
    constexpr nyxora::GuestAddress mutex_slot = base;
    constexpr nyxora::GuestAddress cond_slot = base + 0x20;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "cond-time-hle"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto posix_rel = symbols.resolve(libkernel_key("K953PF5u6Pc"));
    const auto orbis_rel = symbols.resolve(libkernel_key("BmMjYxmew1w"));
    NYXORA_CHECK(posix_rel.has_value());
    NYXORA_CHECK(orbis_rel.has_value());
    NYXORA_CHECK(services.mutex_init(mutex_slot, 0, 0) == 0);
    NYXORA_CHECK(services.cond_init(cond_slot, 0) == 0);
    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    nyxora::runtime::ScopedGuestThreadManager scope(manager);
    NYXORA_CHECK(services.mutex_lock(mutex_slot) == 0);
    NYXORA_CHECK(trampoline->invoke(posix_rel->address, stack->top(), cond_slot, mutex_slot, 0) ==
                 nyxora::runtime::KernelServices::kPosixEtimedout);
    NYXORA_CHECK(services.mutex_unlock(mutex_slot) == 0);
    NYXORA_CHECK(services.mutex_lock(mutex_slot) == 0);
    const auto encoded = trampoline->invoke(orbis_rel->address, stack->top(), cond_slot, mutex_slot, 0);
    NYXORA_CHECK(static_cast<std::uint32_t>(encoded) == 0x8002003cU);
    NYXORA_CHECK(services.mutex_unlock(mutex_slot) == 0);
#endif
}


NYXORA_TEST(libkernel_semaphore_wait_post_trywait_and_overflow_are_synchronized) {
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0xd0000;
    constexpr nyxora::GuestAddress sem_slot = base;
    constexpr nyxora::GuestAddress value_out = base + 0x20;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "semaphore"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    NYXORA_CHECK(services.sem_init(sem_slot, 123, 0) == 0);
    NYXORA_CHECK(services.sem_try_wait(sem_slot) ==
                 nyxora::runtime::KernelServices::kPosixEagain);

    std::atomic<bool> waiting{false};
    std::atomic<int> result{-1};
    std::thread waiter([&] {
        waiting.store(true, std::memory_order_release);
        result.store(services.sem_wait(sem_slot), std::memory_order_release);
    });
    while (!waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    NYXORA_CHECK(services.sem_post(sem_slot) == 0);
    waiter.join();
    NYXORA_CHECK(result.load(std::memory_order_acquire) == 0);
    NYXORA_CHECK(services.sem_get_value(sem_slot, value_out) == 0);
    std::uint32_t value = 99;
    auto bytes = memory.view(value_out, sizeof(value));
    std::memcpy(&value, bytes.data(), sizeof(value));
    NYXORA_CHECK(value == 0);
    NYXORA_CHECK(services.sem_destroy(sem_slot) == 0);

    NYXORA_CHECK(services.sem_init(sem_slot, 0,
                                   nyxora::runtime::KernelServices::kSemaphoreValueMax) == 0);
    NYXORA_CHECK(services.sem_post(sem_slot) ==
                 nyxora::runtime::KernelServices::kPosixEoverflow);
    NYXORA_CHECK(services.sem_destroy(sem_slot) == 0);
}

NYXORA_TEST(libkernel_posix_semaphore_hle_sets_thread_local_errno) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0xe0000;
    constexpr nyxora::GuestAddress sem_slot = base;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "sem-hle"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto init = symbols.resolve(libkernel_key("pDuPEf3m4fI"));
    const auto trywait = symbols.resolve(libkernel_key("WBWzsRifCEA"));
    const auto error = symbols.resolve(libkernel_key("9BcDykPmo1I"));
    NYXORA_CHECK(init.has_value());
    NYXORA_CHECK(trywait.has_value());
    NYXORA_CHECK(error.has_value());
    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    nyxora::runtime::ScopedGuestThreadManager scope(manager);
    NYXORA_CHECK(trampoline->invoke(init->address, stack->top(), sem_slot, 0, 0) == 0);
    NYXORA_CHECK(trampoline->invoke(trywait->address, stack->top(), sem_slot) ==
                 std::numeric_limits<std::uint64_t>::max());
    const auto errno_address = trampoline->invoke(error->address, stack->top());
    NYXORA_CHECK(errno_address != 0);
    NYXORA_CHECK(*reinterpret_cast<const std::int32_t*>(errno_address) ==
                 nyxora::runtime::KernelServices::kPosixEagain);
#endif
}

NYXORA_TEST(libkernel_semaphore_timed_wait_prefers_available_token_and_orbis_encodes_timeout) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0xf0000;
    constexpr nyxora::GuestAddress sem_slot = base;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "sem-time"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    // An available token wins before timeout-pointer validation, matching libkernel.
    NYXORA_CHECK(services.sem_init(sem_slot, 0, 1) == 0);
    NYXORA_CHECK(services.sem_timed_wait(sem_slot, 0) == 0);
    NYXORA_CHECK(services.sem_destroy(sem_slot) == 0);
    NYXORA_CHECK(services.sem_init(sem_slot, 0, 0) == 0);

    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto timed = symbols.resolve(libkernel_key("fjN6NQHhK8k"));
    NYXORA_CHECK(timed.has_value());
    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    nyxora::runtime::ScopedGuestThreadManager scope(manager);
    const auto encoded = trampoline->invoke(timed->address, stack->top(), sem_slot, 0);
    NYXORA_CHECK(static_cast<std::uint32_t>(encoded) == 0x8002003cU);
#endif
}


NYXORA_TEST(libkernel_sleep_hle_validates_timespec_and_clears_remaining_time) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0x100000;
    constexpr nyxora::GuestAddress request = base;
    constexpr nyxora::GuestAddress remaining = base + 0x20;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "sleep-hle"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    nyxora::runtime::TlsRegistry tls;
    nyxora::runtime::GuestThreadManager manager(tls, &services);
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto nanosleep = symbols.resolve(libkernel_key("NhpspxdjEKU"));
    const auto kernel_nanosleep = symbols.resolve(libkernel_key("QvsZxomvUHs"));
    const auto error = symbols.resolve(libkernel_key("9BcDykPmo1I"));
    NYXORA_CHECK(nanosleep.has_value());
    NYXORA_CHECK(kernel_nanosleep.has_value());
    NYXORA_CHECK(error.has_value());
    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    nyxora::runtime::ScopedGuestThreadManager scope(manager);

    const std::array<std::int64_t, 2> invalid{0, 1'000'000'000};
    NYXORA_CHECK(memory.write(
        request, std::span<const std::byte>(reinterpret_cast<const std::byte*>(invalid.data()),
                                            sizeof(invalid))));
    NYXORA_CHECK(trampoline->invoke(nanosleep->address, stack->top(), request, remaining) ==
                 std::numeric_limits<std::uint64_t>::max());
    const auto errno_address = trampoline->invoke(error->address, stack->top());
    NYXORA_CHECK(*reinterpret_cast<const std::int32_t*>(errno_address) ==
                 nyxora::runtime::KernelServices::kPosixEinval);
    const auto encoded = trampoline->invoke(kernel_nanosleep->address, stack->top(), request, remaining);
    NYXORA_CHECK(static_cast<std::uint32_t>(encoded) == 0x80020016U);

    const std::array<std::int64_t, 2> short_sleep{0, 1'000'000};
    const std::array<std::int64_t, 2> dirty_remaining{123, 456};
    NYXORA_CHECK(memory.write(
        request, std::span<const std::byte>(reinterpret_cast<const std::byte*>(short_sleep.data()),
                                            sizeof(short_sleep))));
    NYXORA_CHECK(memory.write(
        remaining,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(dirty_remaining.data()),
                                   sizeof(dirty_remaining))));
    NYXORA_CHECK(trampoline->invoke(nanosleep->address, stack->top(), request, remaining) == 0);
    const auto remaining_bytes = memory.view(remaining, sizeof(dirty_remaining));
    std::array<std::int64_t, 2> cleared{};
    std::memcpy(cleared.data(), remaining_bytes.data(), sizeof(cleared));
    NYXORA_CHECK(cleared[0] == 0);
    NYXORA_CHECK(cleared[1] == 0);
#endif
}

NYXORA_TEST(libkernel_usleep_sleep_and_yield_bindings_are_callable) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::HleRegistry hle(symbols);
    nyxora::hle::libkernel::register_core(hle);
    const auto usleep = symbols.resolve(libkernel_key("QcteRwbsnV0"));
    const auto sleep = symbols.resolve(libkernel_key("0wu33hunNdE"));
    const auto sched_yield = symbols.resolve(libkernel_key("6XG4B33N09g"));
    const auto pthread_yield = symbols.resolve(libkernel_key("T72hz6ffq08"));
    NYXORA_CHECK(usleep.has_value());
    NYXORA_CHECK(sleep.has_value());
    NYXORA_CHECK(sched_yield.has_value());
    NYXORA_CHECK(pthread_yield.has_value());
    auto stack = nyxora::runtime::GuestStack::create(64 * 1024);
    auto trampoline = nyxora::runtime::EntryTrampoline::create();
    NYXORA_CHECK(stack.has_value());
    NYXORA_CHECK(trampoline.has_value());
    NYXORA_CHECK(trampoline->invoke(usleep->address, stack->top(), 0) == 0);
    NYXORA_CHECK(trampoline->invoke(sleep->address, stack->top(), 0) == 0);
    NYXORA_CHECK(trampoline->invoke(sched_yield->address, stack->top()) == 0);
    NYXORA_CHECK(trampoline->invoke(pthread_yield->address, stack->top()) == 0);
#endif
}


NYXORA_TEST(libkernel_semaphore_destroy_cannot_remove_a_published_waiter) {
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0x110000;
    constexpr nyxora::GuestAddress sem_slot = base;
    NYXORA_CHECK(memory.map(base, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "sem-destroy-race"));
    NYXORA_CHECK(memory.zero(base, 0x1000));
    nyxora::runtime::KernelServices services(memory);

    bool observed_busy = false;
    for (int round = 0; round < 100 && !observed_busy; ++round) {
        NYXORA_CHECK(services.sem_init(sem_slot, 0, 0) == 0);
        std::atomic<bool> entered{false};
        std::atomic<int> result{-1};
        std::thread waiter([&] {
            entered.store(true, std::memory_order_release);
            result.store(services.sem_wait(sem_slot), std::memory_order_release);
        });
        while (!entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Give the worker a scheduling window to publish itself. If destroy wins the race, this
        // round is cleaned up completely and retried instead of asserting with a joinable thread.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        const auto destroy_result = services.sem_destroy(sem_slot);
        if (destroy_result == nyxora::runtime::KernelServices::kPosixEbusy) {
            observed_busy = true;
            NYXORA_CHECK(services.sem_post(sem_slot) == 0);
        }
        waiter.join();

        if (observed_busy) {
            NYXORA_CHECK(result.load(std::memory_order_acquire) == 0);
            NYXORA_CHECK(services.sem_destroy(sem_slot) == 0);
        } else {
            NYXORA_CHECK(destroy_result == 0);
            NYXORA_CHECK(result.load(std::memory_order_acquire) ==
                         nyxora::runtime::KernelServices::kPosixEinval);
        }
    }
    NYXORA_CHECK(observed_busy);
}
