#include "test.hpp"
#include "nyxora/memory/native_arena.hpp"
#include "nyxora/runtime/kernel_services.hpp"
#include "nyxora/runtime/thread_manager.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

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


NYXORA_TEST(thread_manager_timed_join_timeout_preserves_handle_for_later_join) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress timespec_address = 0x120000;
    NYXORA_CHECK(memory.map(timespec_address, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "timed-join"));
    NYXORA_CHECK(memory.zero(timespec_address, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    nyxora::runtime::TlsRegistry registry;
    nyxora::runtime::GuestThreadManager manager(registry, &services);

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    // cmp byte ptr [rdi],0; je -5; mov rax,rdi; ret
    const std::array<std::byte, 9> start_code{
        std::byte{0x80}, std::byte{0x3f}, std::byte{0x00}, std::byte{0x74}, std::byte{0xfb},
        std::byte{0x48}, std::byte{0x89}, std::byte{0xf8}, std::byte{0xc3},
    };
    NYXORA_CHECK(code->copy(0, start_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, start_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    std::atomic<std::uint8_t> gate{0};
    nyxora::GuestAddress handle = 0;
    NYXORA_CHECK(manager.create(
                     &handle, 0,
                     reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()),
                     reinterpret_cast<nyxora::GuestAddress>(&gate), 64 * 1024) == 0);

    const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(5);
    const auto since_epoch = deadline.time_since_epoch();
    const auto whole_seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    const std::array<std::int64_t, 2> guest_deadline{
        whole_seconds.count(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch - whole_seconds).count(),
    };
    NYXORA_CHECK(memory.write(
        timespec_address,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(guest_deadline.data()),
                                   sizeof(guest_deadline))));

    NYXORA_CHECK(manager.timed_join(handle, nullptr, timespec_address) ==
                 nyxora::runtime::GuestThreadManager::kPosixEtimedout);
    NYXORA_CHECK(manager.size() == 1);
    gate.store(1, std::memory_order_release);
    nyxora::GuestAddress result = 0;
    NYXORA_CHECK(manager.join(handle, &result) == 0);
    NYXORA_CHECK(result == reinterpret_cast<nyxora::GuestAddress>(&gate));
    NYXORA_CHECK(manager.size() == 0);
#endif
}

NYXORA_TEST(thread_manager_join_claim_rejects_detach_and_second_joiner) {
#if defined(__x86_64__) || defined(_M_X64)
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress future_timespec = 0x130000;
    constexpr nyxora::GuestAddress past_timespec = future_timespec + 0x20;
    NYXORA_CHECK(memory.map(future_timespec, 0x1000,
                            nyxora::memory::Protection::read |
                                nyxora::memory::Protection::write,
                            "join-claim"));
    NYXORA_CHECK(memory.zero(future_timespec, 0x1000));
    nyxora::runtime::KernelServices services(memory);
    nyxora::runtime::TlsRegistry registry;
    nyxora::runtime::GuestThreadManager manager(registry, &services);

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    const std::array<std::byte, 9> start_code{
        std::byte{0x80}, std::byte{0x3f}, std::byte{0x00}, std::byte{0x74}, std::byte{0xfb},
        std::byte{0x48}, std::byte{0x89}, std::byte{0xf8}, std::byte{0xc3},
    };
    NYXORA_CHECK(code->copy(0, start_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, start_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    std::atomic<std::uint8_t> gate{0};
    nyxora::GuestAddress handle = 0;
    NYXORA_CHECK(manager.create(
                     &handle, 0,
                     reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()),
                     reinterpret_cast<nyxora::GuestAddress>(&gate), 64 * 1024) == 0);

    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
    const auto since_epoch = deadline.time_since_epoch();
    const auto whole_seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    const std::array<std::int64_t, 2> guest_deadline{
        whole_seconds.count(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch - whole_seconds).count(),
    };
    const std::array<std::int64_t, 2> expired_deadline{0, 0};
    NYXORA_CHECK(memory.write(
        future_timespec,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(guest_deadline.data()),
                                   sizeof(guest_deadline))));
    NYXORA_CHECK(memory.write(
        past_timespec,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(expired_deadline.data()),
                                   sizeof(expired_deadline))));

    std::atomic<bool> join_started{false};
    std::atomic<int> join_result{-1};
    std::thread joiner([&] {
        join_started.store(true, std::memory_order_release);
        join_result.store(manager.timed_join(handle, nullptr, future_timespec),
                          std::memory_order_release);
    });
    while (!join_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    bool saw_claim = false;
    int detach_result = -1;
    for (int attempt = 0; attempt < 100'000 && !saw_claim; ++attempt) {
        const auto second_join = manager.timed_join(handle, nullptr, past_timespec);
        if (second_join == nyxora::runtime::GuestThreadManager::kPosixEnotsup) {
            saw_claim = true;
            detach_result = manager.detach(handle);
        } else if (second_join != nyxora::runtime::GuestThreadManager::kPosixEtimedout) {
            break;
        }
        std::this_thread::yield();
    }

    gate.store(1, std::memory_order_release);
    joiner.join();
    NYXORA_CHECK(saw_claim);
    NYXORA_CHECK(detach_result == nyxora::runtime::GuestThreadManager::kPosixEinval);
    NYXORA_CHECK(join_result.load(std::memory_order_acquire) == 0);
    NYXORA_CHECK(manager.size() == 0);
#endif
}
