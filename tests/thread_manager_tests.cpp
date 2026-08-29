#include "test.hpp"
#include "nyxora/memory/native_arena.hpp"
#include "nyxora/runtime/thread_manager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

NYXORA_TEST(thread_manager_creates_joins_and_reclaims_opaque_guest_thread) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::TlsRegistry registry;
    nyxora::runtime::GuestThreadManager manager(registry);
    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    // mov rax,rdi; ret
    const std::array<std::byte, 4> start_code{
        std::byte{0x48}, std::byte{0x89}, std::byte{0xf8}, std::byte{0xc3},
    };
    NYXORA_CHECK(code->copy(0, start_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, start_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    nyxora::GuestAddress handle = 0;
    constexpr nyxora::GuestAddress expected = 0x12345678ULL;
    NYXORA_CHECK(manager.create(
                     &handle, 0,
                     reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), expected,
                     64 * 1024) == 0);
    NYXORA_CHECK(handle != 0);
    NYXORA_CHECK(manager.size() == 1);
    nyxora::GuestAddress result = 0;
    NYXORA_CHECK(manager.join(handle, &result) == 0);
    NYXORA_CHECK(result == expected);
    NYXORA_CHECK(manager.size() == 0);
    NYXORA_CHECK(manager.join(handle, nullptr) == nyxora::runtime::GuestThreadManager::kPosixEsrch);
#endif
}

NYXORA_TEST(thread_manager_scope_is_nested_and_thread_local) {
    nyxora::runtime::TlsRegistry registry;
    nyxora::runtime::GuestThreadManager first(registry);
    nyxora::runtime::GuestThreadManager second(registry);
    NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current() == nullptr);
    {
        nyxora::runtime::ScopedGuestThreadManager outer(first);
        NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current() == &first);
        {
            nyxora::runtime::ScopedGuestThreadManager inner(second);
            NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current() == &second);
        }
        NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current() == &first);
    }
    NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current() == nullptr);
}


NYXORA_TEST(thread_manager_exposes_root_and_nested_current_handles) {
    nyxora::runtime::TlsRegistry registry;
    nyxora::runtime::GuestThreadManager manager(registry);
    NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current_handle() == 0);
    {
        nyxora::runtime::ScopedGuestThreadManager root(manager);
        NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current() == &manager);
        NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current_handle() == manager.root_handle());
        {
            constexpr nyxora::GuestAddress child = 0x1234;
            nyxora::runtime::ScopedGuestThreadManager nested(&manager, child);
            NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current_handle() == child);
        }
        NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current_handle() == manager.root_handle());
    }
    NYXORA_CHECK(nyxora::runtime::GuestThreadManager::current_handle() == 0);
}

NYXORA_TEST(thread_manager_detach_prevents_join_and_duplicate_detach) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::TlsRegistry registry;
    nyxora::runtime::GuestThreadManager manager(registry);
    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    const std::array<std::byte, 1> start_code{std::byte{0xc3}};
    NYXORA_CHECK(code->copy(0, start_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, start_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    nyxora::GuestAddress handle = 0;
    NYXORA_CHECK(manager.create(
                     &handle, 0,
                     reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), 0,
                     64 * 1024) == 0);
    NYXORA_CHECK(manager.detach(handle) == 0);
    NYXORA_CHECK(manager.detach(handle) == nyxora::runtime::GuestThreadManager::kPosixEinval);
    NYXORA_CHECK(manager.join(handle, nullptr) == nyxora::runtime::GuestThreadManager::kPosixEinval);
    NYXORA_CHECK(manager.detach(manager.root_handle()) ==
                 nyxora::runtime::GuestThreadManager::kPosixEinval);
#endif
}
