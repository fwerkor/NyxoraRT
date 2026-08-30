#include "test.hpp"
#include "sce_fixture.hpp"
#include "nyxora/gpu/null_backend.hpp"
#include "nyxora/memory/guest_address_space.hpp"
#include "nyxora/runtime/guest_thread.hpp"
#include "nyxora/runtime/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <memory>

NYXORA_TEST(guest_thread_executes_and_joins_with_return_value) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 3U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();

    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    const auto module = runtime.load_image(image, "thread-fixture.elf", base);

    auto thread = runtime.start_thread(module, 64 * 1024);
    NYXORA_CHECK(thread.joinable());
    const auto result = thread.join();
    NYXORA_CHECK(result.completed());
    NYXORA_CHECK(result.value == 42);
    NYXORA_CHECK(!thread.joinable());
#endif
}

NYXORA_TEST(guest_thread_reports_guest_fault_on_posix_x86_64) {
#if !defined(_WIN32) && defined(__x86_64__)
    nyxora::runtime::TlsRegistry registry;
    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    const std::array<std::byte, 9> guest_code{
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xc3},
    };
    NYXORA_CHECK(code->copy(0, guest_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, guest_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    auto thread = nyxora::runtime::GuestThread::start(
        registry, reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), 64 * 1024);
    NYXORA_CHECK(thread.has_value());
    const auto result = thread->join();
    NYXORA_CHECK(!result.completed());
    NYXORA_CHECK(result.fault.has_value());
    NYXORA_CHECK(result.fault->kind == nyxora::runtime::GuestFaultKind::access_violation);
#endif
}

NYXORA_TEST(linux_guest_threads_receive_distinct_tcb_bases) {
#if defined(__linux__) && defined(__x86_64__)
    nyxora::runtime::TlsRegistry registry;
    const std::array<std::byte, 1> initial{std::byte{0x66}};
    NYXORA_CHECK(registry.register_module(1, 16, 8, initial));

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    const std::array<std::byte, 10> guest_code{
        std::byte{0x65}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xc3},
    };
    NYXORA_CHECK(code->copy(0, guest_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, guest_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));

    const auto main_gs = nyxora::runtime::ScopedGuestSegment::current_base();
    auto first = nyxora::runtime::GuestThread::start(
        registry, reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), 64 * 1024);
    auto second = nyxora::runtime::GuestThread::start(
        registry, reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), 64 * 1024);
    NYXORA_CHECK(first.has_value());
    NYXORA_CHECK(second.has_value());
    const auto first_result = first->join();
    const auto second_result = second->join();
    NYXORA_CHECK(first_result.completed());
    NYXORA_CHECK(second_result.completed());
    NYXORA_CHECK(first_result.value != 0);
    NYXORA_CHECK(second_result.value != 0);
    NYXORA_CHECK(first_result.value != second_result.value);
    NYXORA_CHECK(nyxora::runtime::ScopedGuestSegment::current_base() == main_gs);
#endif
}

NYXORA_TEST(guest_thread_empty_state_and_move_assignment_are_well_defined) {
    nyxora::runtime::GuestThread empty;
    NYXORA_CHECK(!empty.joinable());
    NYXORA_CHECK(!empty.finished());
    NYXORA_CHECK(!empty.wait_until(std::chrono::system_clock::now()));
    bool empty_join_rejected = false;
    try {
        (void)empty.join();
    } catch (const std::runtime_error&) {
        empty_join_rejected = true;
    }
    NYXORA_CHECK(empty_join_rejected);

#if defined(__x86_64__) || defined(_M_X64)
    nyxora::runtime::TlsRegistry registry;
    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    const std::array<std::byte, 6> return_code{
        std::byte{0xb8}, std::byte{0x2b}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xc3}};
    NYXORA_CHECK(code->copy(0, return_code));
    NYXORA_CHECK(code->flush_instruction_cache(0, return_code.size()));
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::execute));
    auto source = nyxora::runtime::GuestThread::start(
        registry, reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), 64 * 1024);
    NYXORA_CHECK(source.has_value());
    nyxora::runtime::GuestThread destination;
    destination = std::move(*source);
    NYXORA_CHECK(destination.joinable());
    const auto result = destination.join();
    NYXORA_CHECK(result.completed());
    NYXORA_CHECK(result.value == 43);
#endif
}
