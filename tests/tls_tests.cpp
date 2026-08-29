#include "test.hpp"
#include "sce_fixture.hpp"
#include "nyxora/gpu/null_backend.hpp"
#include "nyxora/memory/native_arena.hpp"
#include "nyxora/runtime/native_thread.hpp"
#include "nyxora/runtime/runtime.hpp"
#include "nyxora/runtime/tls.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

NYXORA_TEST(tls_context_copies_initial_image_and_zeroes_bss) {
    nyxora::runtime::TlsRegistry registry;
    const std::array<std::byte, 4> initial{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
    };
    NYXORA_CHECK(registry.register_module(7, 32, 16, initial));

    auto context = nyxora::runtime::GuestThreadContext::create(registry);
    NYXORA_CHECK(context.has_value());
    const auto* data = static_cast<const std::byte*>(context->tls_address(7));
    NYXORA_CHECK(data != nullptr);
    NYXORA_CHECK((reinterpret_cast<std::uintptr_t>(data) & 31U) == 0);
    NYXORA_CHECK(data[0] == std::byte{1});
    NYXORA_CHECK(data[3] == std::byte{4});
    NYXORA_CHECK(data[4] == std::byte{0});
    NYXORA_CHECK(data[15] == std::byte{0});
    NYXORA_CHECK(context->tls_address(7, 15) != nullptr);
    NYXORA_CHECK(context->tls_address(7, 16) == nullptr);
}

NYXORA_TEST(scoped_guest_thread_context_restores_previous_context) {
    nyxora::runtime::TlsRegistry registry;
    const std::array<std::byte, 1> initial{std::byte{9}};
    NYXORA_CHECK(registry.register_module(1, 8, 8, initial));
    auto first = nyxora::runtime::GuestThreadContext::create(registry);
    auto second = nyxora::runtime::GuestThreadContext::create(registry);
    NYXORA_CHECK(first.has_value());
    NYXORA_CHECK(second.has_value());
    NYXORA_CHECK(nyxora::runtime::ScopedGuestThreadContext::current() == nullptr);

    {
        nyxora::runtime::ScopedGuestThreadContext first_scope(*first);
        NYXORA_CHECK(nyxora::runtime::ScopedGuestThreadContext::current() == &*first);
        {
            nyxora::runtime::ScopedGuestThreadContext second_scope(*second);
            NYXORA_CHECK(nyxora::runtime::ScopedGuestThreadContext::current() == &*second);
        }
        NYXORA_CHECK(nyxora::runtime::ScopedGuestThreadContext::current() == &*first);
    }
    NYXORA_CHECK(nyxora::runtime::ScopedGuestThreadContext::current() == nullptr);
}

NYXORA_TEST(runtime_registers_pt_tls_template_for_new_threads) {
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    const auto module = runtime.load_image(image, "tls-fixture.elf", 0x500000000ULL);
    NYXORA_CHECK(module.tls_module_id == 1);
    NYXORA_CHECK(runtime.tls_registry().size() == 1);

    auto thread = runtime.create_thread_context();
    NYXORA_CHECK(thread.has_value());
    const auto* tls = static_cast<const std::byte*>(thread->tls_address(module.tls_module_id));
    NYXORA_CHECK(tls != nullptr);
    NYXORA_CHECK(tls[0] == std::byte{0x80});
    NYXORA_CHECK(tls[15] == std::byte{0x8f});
    NYXORA_CHECK(tls[16] == std::byte{0});
    NYXORA_CHECK(tls[31] == std::byte{0});
}

NYXORA_TEST(guest_tcb_and_dtv_describe_registered_tls_modules) {
    nyxora::runtime::TlsRegistry registry;
    const std::array<std::byte, 2> first{std::byte{0x11}, std::byte{0x12}};
    const std::array<std::byte, 1> third{std::byte{0x31}};
    NYXORA_CHECK(registry.register_module(1, 16, 8, first));
    NYXORA_CHECK(registry.register_module(3, 32, 8, third));

    auto context = nyxora::runtime::GuestThreadContext::create(registry);
    NYXORA_CHECK(context.has_value());
    NYXORA_CHECK(context->tcb()->self == context->tcb());
    NYXORA_CHECK(context->tcb()->dtv == context->dtv().data());
    NYXORA_CHECK(context->tcb()->canary != 0);
    NYXORA_CHECK(context->dtv().size() == 5);
    NYXORA_CHECK(context->dtv()[0].counter == registry.generation());
    NYXORA_CHECK(context->dtv()[1].counter == 3);
    NYXORA_CHECK(context->dtv()[2].pointer == context->tls_address(1));
    NYXORA_CHECK(context->dtv()[3].pointer == nullptr);
    NYXORA_CHECK(context->dtv()[4].pointer == context->tls_address(3));
}

NYXORA_TEST(scoped_guest_segment_binds_and_restores_linux_gs_base) {
#if defined(__linux__) && defined(__x86_64__)
    nyxora::runtime::TlsRegistry registry;
    const std::array<std::byte, 1> initial{std::byte{0x44}};
    NYXORA_CHECK(registry.register_module(1, 16, 8, initial));
    auto context = nyxora::runtime::GuestThreadContext::create(registry);
    NYXORA_CHECK(context.has_value());

    const auto before = nyxora::runtime::ScopedGuestSegment::current_base();
    NYXORA_CHECK(before.has_value());
    {
        nyxora::runtime::ScopedGuestSegment binding(*context);
        NYXORA_CHECK(binding.active());
        const auto during = nyxora::runtime::ScopedGuestSegment::current_base();
        NYXORA_CHECK(during.has_value());
        NYXORA_CHECK(*during == reinterpret_cast<std::uintptr_t>(context->tcb()));
    }
    const auto after = nyxora::runtime::ScopedGuestSegment::current_base();
    NYXORA_CHECK(after == before);
#endif
}

NYXORA_TEST(linux_guest_code_reads_tcb_through_gs) {
#if defined(__linux__) && defined(__x86_64__)
    nyxora::runtime::TlsRegistry registry;
    const std::array<std::byte, 1> initial{std::byte{0x55}};
    NYXORA_CHECK(registry.register_module(1, 16, 8, initial));
    auto context = nyxora::runtime::GuestThreadContext::create(registry);
    NYXORA_CHECK(context.has_value());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(code.has_value());
    NYXORA_CHECK(code->protect(0, page,
                               nyxora::memory::Protection::read |
                                   nyxora::memory::Protection::write));
    // mov rax, qword ptr gs:[0]; ret
    const std::array<std::byte, 10> guest_code{
        std::byte{0x65}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xc3},
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
    nyxora::runtime::ScopedGuestThreadContext context_scope(*context);
    nyxora::runtime::ScopedGuestSegment segment_scope(*context);
    const auto result = trampoline->invoke(
        reinterpret_cast<nyxora::GuestAddress>(code->host_pointer()), stack->top());
    NYXORA_CHECK(result == reinterpret_cast<std::uint64_t>(context->tcb()));
#endif
}
