#include "test.hpp"
#include "sce_fixture.hpp"
#include "nyxora/loader/dynamic.hpp"
#include "nyxora/memory/guest_address_space.hpp"
#include "nyxora/runtime/linker.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {

std::uint64_t read_u64(const nyxora::memory::GuestAddressSpace& memory,
                       nyxora::GuestAddress address) {
    const auto bytes = memory.view(address, sizeof(std::uint64_t));
    if (bytes.size() != sizeof(std::uint64_t)) {
        throw std::runtime_error("test read is outside guest memory");
    }
    std::uint64_t value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

nyxora::runtime::SymbolKey import_key() {
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

nyxora::runtime::SymbolKey export_key() {
    auto key = import_key();
    key.nid = "expNid";
    return key;
}

} // namespace

NYXORA_TEST(runtime_linker_applies_relative_tls_and_import_relocations) {
    constexpr nyxora::GuestAddress base = 0x100000000ULL;
    constexpr nyxora::GuestAddress hle_address = 0x123456789abcdef0ULL;

    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    const auto dynamic = nyxora::loader::parse_dynamic_info(image);
    NYXORA_CHECK(dynamic.has_value());

    nyxora::memory::GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(base + 0x1000, 0x100,
                            nyxora::memory::Protection::read | nyxora::memory::Protection::write,
                            "fixture"));
    NYXORA_CHECK(memory.protect(base + 0x1000, 0x100,
                                nyxora::memory::Protection::read |
                                    nyxora::memory::Protection::execute));

    nyxora::runtime::SymbolRegistry symbols;
    NYXORA_CHECK(symbols.register_symbol(
        import_key(), nyxora::runtime::SymbolBinding{hle_address, "HLE impNid", true}));

    nyxora::runtime::RuntimeLinker linker(memory, symbols);
    NYXORA_CHECK(linker.register_exports(base, *dynamic) == 1);

    const auto exported = symbols.resolve(export_key());
    NYXORA_CHECK(exported.has_value());
    NYXORA_CHECK(exported->address == base + 0x1040);
    NYXORA_CHECK(!exported->hle);

    const auto report = linker.relocate(base, 7, *dynamic);
    NYXORA_CHECK(report.applied == 3);
    NYXORA_CHECK(report.unresolved.empty());
    NYXORA_CHECK(read_u64(memory, base + 0x1000) == base + 0x1040);
    NYXORA_CHECK(read_u64(memory, base + 0x1008) == hle_address);
    NYXORA_CHECK(read_u64(memory, base + 0x1010) == 7);
}

NYXORA_TEST(runtime_linker_reports_and_can_retry_unresolved_imports) {
    constexpr nyxora::GuestAddress base = 0x200000000ULL;
    constexpr nyxora::GuestAddress hle_address = 0x1111222233334444ULL;

    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    const auto dynamic = nyxora::loader::parse_dynamic_info(image);
    NYXORA_CHECK(dynamic.has_value());

    nyxora::memory::GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(base + 0x1000, 0x100, nyxora::memory::Protection::read, "fixture"));
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::RuntimeLinker linker(memory, symbols);

    auto report = linker.relocate(base, 9, *dynamic);
    NYXORA_CHECK(report.applied == 2);
    NYXORA_CHECK(report.unresolved.size() == 1);
    NYXORA_CHECK(report.unresolved[0].symbol_name == "impNid#A#B");
    NYXORA_CHECK(report.unresolved[0].plt);
    NYXORA_CHECK(read_u64(memory, base + 0x1008) == 0);

    NYXORA_CHECK(symbols.register_symbol(
        import_key(), nyxora::runtime::SymbolBinding{hle_address, "late HLE", true}));
    report = linker.relocate(base, 9, *dynamic);
    NYXORA_CHECK(report.applied == 3);
    NYXORA_CHECK(report.unresolved.empty());
    NYXORA_CHECK(read_u64(memory, base + 0x1008) == hle_address);
}

NYXORA_TEST(runtime_linker_rejects_overflow_bad_symbol_and_unsupported_relocations) {
    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    const auto parsed = nyxora::loader::parse_dynamic_info(image);
    NYXORA_CHECK(parsed.has_value());
    nyxora::memory::GuestAddressSpace memory;
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::RuntimeLinker linker(memory, symbols);

    auto overflow_export = *parsed;
    overflow_export.symbols[2].value = 0x40;
    bool export_overflow = false;
    try {
        (void)linker.register_exports(std::numeric_limits<std::uint64_t>::max() - 0x20,
                                      overflow_export);
    } catch (const std::runtime_error&) {
        export_overflow = true;
    }
    NYXORA_CHECK(export_overflow);

    auto patch_overflow = *parsed;
    patch_overflow.relocations = {
        nyxora::loader::Relocation{std::numeric_limits<std::uint64_t>::max(),
                                   nyxora::loader::kRelocationX86_64Relative, 0}};
    patch_overflow.plt_relocations.clear();
    bool relocation_overflow = false;
    try {
        (void)linker.relocate(1, 1, patch_overflow);
    } catch (const std::runtime_error&) {
        relocation_overflow = true;
    }
    NYXORA_CHECK(relocation_overflow);

    auto bad_symbol = *parsed;
    bad_symbol.relocations = {
        nyxora::loader::Relocation{0, (std::uint64_t{99} << 32U) |
                                          nyxora::loader::kRelocationX86_64GlobDat,
                                   0}};
    bad_symbol.plt_relocations.clear();
    bool bad_symbol_index = false;
    try {
        (void)linker.relocate(0x1000, 1, bad_symbol);
    } catch (const std::runtime_error&) {
        bad_symbol_index = true;
    }
    NYXORA_CHECK(bad_symbol_index);

    auto unsupported = *parsed;
    unsupported.relocations = {nyxora::loader::Relocation{0, 0xffffU, 0}};
    unsupported.plt_relocations.clear();
    bool unsupported_type = false;
    try {
        (void)linker.relocate(0x1000, 1, unsupported);
    } catch (const std::runtime_error&) {
        unsupported_type = true;
    }
    NYXORA_CHECK(unsupported_type);
}

NYXORA_TEST(runtime_linker_reports_missing_tls_module_and_unmapped_patch_target) {
    nyxora::memory::GuestAddressSpace memory;
    nyxora::runtime::SymbolRegistry symbols;
    nyxora::runtime::RuntimeLinker linker(memory, symbols);
    nyxora::loader::DynamicInfo dynamic;
    dynamic.relocations = {
        nyxora::loader::Relocation{0x20, nyxora::loader::kRelocationX86_64DtpMod64, 0}};

    const auto unresolved = linker.relocate(0x1000, 0, dynamic);
    NYXORA_CHECK(unresolved.applied == 0);
    NYXORA_CHECK(unresolved.unresolved.size() == 1);
    NYXORA_CHECK(unresolved.unresolved[0].type == nyxora::loader::kRelocationX86_64DtpMod64);

    dynamic.relocations = {
        nyxora::loader::Relocation{0, nyxora::loader::kRelocationX86_64Relative, 0}};
    bool unmapped_target = false;
    try {
        (void)linker.relocate(0x1000, 1, dynamic);
    } catch (const std::runtime_error&) {
        unmapped_target = true;
    }
    NYXORA_CHECK(unmapped_target);

    dynamic.relocations = {
        nyxora::loader::Relocation{0, nyxora::loader::kRelocationX86_64Relative, -2}};
    const auto underflow = linker.relocate(1, 1, dynamic);
    NYXORA_CHECK(underflow.applied == 0);
    NYXORA_CHECK(underflow.unresolved.empty());
}
