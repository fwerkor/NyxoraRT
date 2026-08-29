#include "test.hpp"
#include "sce_fixture.hpp"
#include "nyxora/gpu/null_backend.hpp"
#include "nyxora/runtime/runtime.hpp"

#include <cstdint>
#include <cstring>
#include <memory>

namespace {

nyxora::runtime::SymbolKey runtime_import_key() {
    return nyxora::runtime::SymbolKey{
        .nid = "impNid",
        .library = "libTest",
        .module = "modTest",
        .library_version = 3,
        .module_major = 1,
        .module_minor = 2,
        .kind = nyxora::runtime::SymbolKind::function,
    };
}

std::uint64_t runtime_read_u64(const nyxora::runtime::Runtime& runtime,
                               nyxora::GuestAddress address) {
    const auto bytes = runtime.memory().view(address, sizeof(std::uint64_t));
    if (bytes.size() != sizeof(std::uint64_t)) {
        throw std::runtime_error("test read is outside runtime memory");
    }
    std::uint64_t value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

} // namespace

NYXORA_TEST(runtime_loads_sce_metadata_assigns_tls_and_links_known_imports) {
    constexpr nyxora::GuestAddress base = 0x300000000ULL;
    constexpr nyxora::GuestAddress hle_address = 0x777788889999aaaaULL;

    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    NYXORA_CHECK(runtime.symbols().register_symbol(
        runtime_import_key(), nyxora::runtime::SymbolBinding{hle_address, "fixture HLE", true}));

    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    auto module = runtime.load_image(image, "fixture.elf", base);

    NYXORA_CHECK(module.dynamic.has_value());
    NYXORA_CHECK(module.tls.has_value());
    NYXORA_CHECK(module.tls_module_id == 1);
    NYXORA_CHECK(module.relocations.applied == 3);
    NYXORA_CHECK(module.relocations.unresolved.empty());
    NYXORA_CHECK(runtime_read_u64(runtime, base + 0x1000) == base + 0x1040);
    NYXORA_CHECK(runtime_read_u64(runtime, base + 0x1008) == hle_address);
    NYXORA_CHECK(runtime_read_u64(runtime, base + 0x1010) == 1);
}

NYXORA_TEST(runtime_can_relink_after_late_hle_registration) {
    constexpr nyxora::GuestAddress base = 0x400000000ULL;
    constexpr nyxora::GuestAddress hle_address = 0x5555666677778888ULL;

    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    auto module = runtime.load_image(image, "fixture.elf", base);

    NYXORA_CHECK(module.relocations.unresolved.size() == 1);
    NYXORA_CHECK(runtime.symbols().register_symbol(
        runtime_import_key(), nyxora::runtime::SymbolBinding{hle_address, "late HLE", true}));

    const auto report = runtime.relink(module);
    NYXORA_CHECK(report.unresolved.empty());
    NYXORA_CHECK(runtime_read_u64(runtime, base + 0x1008) == hle_address);
}

NYXORA_TEST(runtime_can_load_and_execute_synthetic_sce_entry_natively) {
#if defined(__x86_64__) || defined(_M_X64)
    constexpr nyxora::GuestAddress hle_address = 0x4444555566667777ULL;
    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 3U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();

    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    NYXORA_CHECK(runtime.symbols().register_symbol(
        runtime_import_key(), nyxora::runtime::SymbolBinding{hle_address, "native HLE", true}));

    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    const auto module = runtime.load_image(image, "native-fixture.elf", base);
    NYXORA_CHECK(module.relocations.unresolved.empty());
    NYXORA_CHECK(module.entry == base + 0x1040);

    using Entry = int (*)();
    auto entry = reinterpret_cast<Entry>(module.entry);
    NYXORA_CHECK(entry() == 42);
#endif
}
