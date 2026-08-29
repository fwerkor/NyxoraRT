#include "test.hpp"
#include "sce_fixture.hpp"
#include "nyxora/gpu/null_backend.hpp"
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
