#include "test.hpp"
#include "nyxora/memory/native_arena.hpp"
#include "nyxora/runtime/late_imports.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

NYXORA_TEST(late_import_thunk_records_unresolved_calls_and_tail_jumps_after_binding) {
#if defined(__x86_64__) || defined(_M_X64)
    auto table = nyxora::runtime::LateImportTable::create(4);
    NYXORA_CHECK(table.has_value());

    const nyxora::runtime::SymbolKey key{
        .nid = "testNid",
        .library = "libTest",
        .module = "modTest",
        .library_version = 1,
        .module_major = 1,
        .module_minor = 0,
        .kind = nyxora::runtime::SymbolKind::function,
    };
    constexpr nyxora::GuestAddress patch_address = 0x12340000;
    const auto thunk = table->get_or_create(patch_address, key, "testNid#A#B");
    NYXORA_CHECK(thunk.has_value());
    NYXORA_CHECK(table->size() == 1);

    using Function = std::uint64_t (*)();
    const auto unresolved = reinterpret_cast<Function>(*thunk);
    NYXORA_CHECK(unresolved() == 0);
    NYXORA_CHECK(table->call_count(0) == 1);
    NYXORA_CHECK(unresolved() == 0);
    NYXORA_CHECK(table->call_count(0) == 2);

    const auto page = nyxora::memory::NativeArena::page_size();
    auto target_code = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(target_code.has_value());
    NYXORA_CHECK(target_code->protect(0, page,
                                      nyxora::memory::Protection::read |
                                          nyxora::memory::Protection::write));
    const std::array<std::byte, 11> target{
        std::byte{0x48}, std::byte{0xb8}, std::byte{0x4d}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xc3},
    };
    NYXORA_CHECK(target_code->copy(0, target));
    NYXORA_CHECK(target_code->flush_instruction_cache(0, target.size()));
    NYXORA_CHECK(target_code->protect(0, page,
                                      nyxora::memory::Protection::read |
                                          nyxora::memory::Protection::execute));

    NYXORA_CHECK(table->bind_patch(
        patch_address, reinterpret_cast<nyxora::GuestAddress>(target_code->host_pointer())));
    NYXORA_CHECK(unresolved() == 77);
    NYXORA_CHECK(table->call_count(0) == 2);
#endif
}

NYXORA_TEST(late_import_table_reuses_thunk_for_same_patch_site) {
#if defined(__x86_64__) || defined(_M_X64)
    auto table = nyxora::runtime::LateImportTable::create(2);
    NYXORA_CHECK(table.has_value());
    const nyxora::runtime::SymbolKey key{
        .nid = "same",
        .library = "lib",
        .module = "mod",
        .library_version = 1,
        .module_major = 1,
        .module_minor = 1,
        .kind = nyxora::runtime::SymbolKind::function,
    };
    const auto first = table->get_or_create(0x1000, key, "same#A#B");
    const auto second = table->get_or_create(0x1000, key, "same#A#B");
    NYXORA_CHECK(first.has_value());
    NYXORA_CHECK(second == first);
    NYXORA_CHECK(table->size() == 1);
#endif
}
