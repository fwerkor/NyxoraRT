#include "test.hpp"
#include "nyxora/memory/native_arena.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

NYXORA_TEST(native_arena_reserves_identity_mapped_host_memory) {
    const auto page = nyxora::memory::NativeArena::page_size();
    auto arena = nyxora::memory::NativeArena::reserve(page * 2U);
    NYXORA_CHECK(arena.has_value());
    NYXORA_CHECK(arena->base() != 0);
    NYXORA_CHECK(reinterpret_cast<std::uintptr_t>(arena->host_pointer()) == arena->base());
    NYXORA_CHECK(arena->size() >= page * 2U);

    NYXORA_CHECK(arena->protect(0, page,
                                nyxora::memory::Protection::read |
                                    nyxora::memory::Protection::write));
    const std::array<std::byte, 4> value{std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
                                         std::byte{0x44}};
    NYXORA_CHECK(arena->copy(0, value));
    NYXORA_CHECK(std::memcmp(arena->host_pointer(), value.data(), value.size()) == 0);
}

NYXORA_TEST(native_arena_can_execute_native_x86_64_code) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto page = nyxora::memory::NativeArena::page_size();
    auto arena = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(arena.has_value());
    NYXORA_CHECK(arena->protect(0, page,
                                nyxora::memory::Protection::read |
                                    nyxora::memory::Protection::write));

    const std::array<std::byte, 6> code{
        std::byte{0xb8}, std::byte{0x2a}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xc3},
    };
    NYXORA_CHECK(arena->copy(0, code));
    NYXORA_CHECK(arena->protect(0, page,
                                nyxora::memory::Protection::read |
                                    nyxora::memory::Protection::execute));

    using Function = int (*)();
    auto function = reinterpret_cast<Function>(arena->host_pointer());
    NYXORA_CHECK(function() == 42);
#endif
}
