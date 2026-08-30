#include "test.hpp"
#include "sce_fixture.hpp"
#include "nyxora/loader/dynamic.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

NYXORA_TEST(parses_sce_dynamic_tables_and_tls_metadata) {
    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    const auto dynamic = nyxora::loader::parse_dynamic_info(image);

    NYXORA_CHECK(dynamic.has_value());
    NYXORA_CHECK(dynamic->sce_layout);
    NYXORA_CHECK(dynamic->symbols.size() == 3);
    NYXORA_CHECK(dynamic->symbols[1].name == "impNid#A#B");
    NYXORA_CHECK(dynamic->symbols[2].name == "expNid#A#B");
    NYXORA_CHECK(dynamic->relocations.size() == 2);
    NYXORA_CHECK(dynamic->plt_relocations.size() == 1);
    NYXORA_CHECK(dynamic->plt_relocations[0].symbol_index() == 1);
    NYXORA_CHECK(dynamic->plt_relocations[0].type() == nyxora::loader::kRelocationX86_64JumpSlot);
    NYXORA_CHECK(dynamic->import_modules.size() == 1);
    NYXORA_CHECK(dynamic->import_modules[0].encoded_id == "B");
    NYXORA_CHECK(dynamic->import_modules[0].version_major == 1);
    NYXORA_CHECK(dynamic->import_modules[0].version_minor == 2);
    NYXORA_CHECK(dynamic->import_libraries.size() == 1);
    NYXORA_CHECK(dynamic->import_libraries[0].encoded_id == "A");
    NYXORA_CHECK(dynamic->import_libraries[0].version == 3);
    NYXORA_CHECK(dynamic->original_filename == "fixture.elf");
    NYXORA_CHECK(dynamic->relative_count == 1);

    NYXORA_CHECK(image.tls().has_value());
    NYXORA_CHECK(image.tls()->virtual_address == 0x1080);
    NYXORA_CHECK(image.tls()->file_size == 16);
    NYXORA_CHECK(image.tls()->memory_size == 32);
    NYXORA_CHECK(image.tls()->alignment == 16);
}

NYXORA_TEST(sce_id_encoding_matches_symbol_identifier_format) {
    NYXORA_CHECK(nyxora::loader::encode_sce_id(0) == "A");
    NYXORA_CHECK(nyxora::loader::encode_sce_id(63) == "-");
    NYXORA_CHECK(nyxora::loader::encode_sce_id(64) == "BA");
    NYXORA_CHECK(nyxora::loader::encode_sce_id(4096) == "BAA");
}

NYXORA_TEST(rejects_out_of_bounds_sce_dynamic_table) {
    auto bytes = test_fixture::sce_dynamic_elf();
    test_fixture::put_dynamic(bytes, 1, nyxora::loader::kDynamicSceStringTableSize, 0x1000);
    const auto image = nyxora::loader::Elf64Image::from_bytes(std::move(bytes));

    bool threw = false;
    try {
        (void)nyxora::loader::parse_dynamic_info(image);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    NYXORA_CHECK(threw);
}

namespace {
std::vector<std::byte> generic_dynamic_elf(std::uint64_t string_address = 0x1050) {
    auto bytes = test_fixture::sce_dynamic_elf();
    constexpr std::int64_t ignored_tag = 0x777777;
    for (std::size_t i = 0; i < 17; ++i) {
        test_fixture::put_dynamic(bytes, i, ignored_tag, 0);
    }
    test_fixture::put_dynamic(bytes, 0, nyxora::loader::kDynamicStringTable, string_address);
    test_fixture::put_dynamic(bytes, 1, nyxora::loader::kDynamicStringTableSize, 8);
    test_fixture::put_dynamic(bytes, 2, nyxora::loader::kDynamicNeeded, 0);
    test_fixture::put_dynamic(bytes, 3, nyxora::loader::kDynamicSoName, 4);
    test_fixture::put_dynamic(bytes, 17, nyxora::loader::kDynamicNull, 0);
    constexpr char strings[8] = {'l', 'i', 'b', '\0', 's', 'o', '\0', '\0'};
    std::memcpy(bytes.data() + test_fixture::SceImageLayout::load_offset + 0x50, strings,
                sizeof(strings));
    return bytes;
}

bool dynamic_rejects(std::vector<std::byte> bytes) {
    try {
        const auto image = nyxora::loader::Elf64Image::from_bytes(std::move(bytes));
        (void)nyxora::loader::parse_dynamic_info(image);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}
} // namespace

NYXORA_TEST(parses_generic_dynamic_string_table_from_load_segment) {
    const auto image = nyxora::loader::Elf64Image::from_bytes(generic_dynamic_elf());
    const auto dynamic = nyxora::loader::parse_dynamic_info(image);
    NYXORA_CHECK(dynamic.has_value());
    NYXORA_CHECK(!dynamic->sce_layout);
    NYXORA_CHECK(dynamic->needed.size() == 1);
    NYXORA_CHECK(dynamic->needed[0] == "lib");
    NYXORA_CHECK(dynamic->so_name == "so");
}

NYXORA_TEST(dynamic_parser_rejects_missing_terminator_and_conflicting_layout_tags) {
    auto no_null = test_fixture::sce_dynamic_elf();
    test_fixture::put_dynamic(no_null, 17, 0x777777, 0);
    NYXORA_CHECK(dynamic_rejects(std::move(no_null)));

    auto mixed = test_fixture::sce_dynamic_elf();
    test_fixture::put_dynamic(mixed, 16, nyxora::loader::kDynamicStringTable, 0x1050);
    NYXORA_CHECK(dynamic_rejects(std::move(mixed)));

    auto no_size = test_fixture::sce_dynamic_elf();
    test_fixture::put_dynamic(no_size, 1, 0x777777, 0);
    NYXORA_CHECK(dynamic_rejects(std::move(no_size)));
}

NYXORA_TEST(dynamic_parser_rejects_invalid_relocation_symbol_and_virtual_ranges) {
    auto bad_rela_entry = test_fixture::sce_dynamic_elf();
    test_fixture::put_dynamic(bad_rela_entry, 7, nyxora::loader::kDynamicSceRelaEntrySize, 8);
    NYXORA_CHECK(dynamic_rejects(std::move(bad_rela_entry)));

    auto bad_plt_kind = test_fixture::sce_dynamic_elf();
    test_fixture::put_dynamic(bad_plt_kind, 10, nyxora::loader::kDynamicScePltRel, 0);
    NYXORA_CHECK(dynamic_rejects(std::move(bad_plt_kind)));

    auto bad_symbol_size = test_fixture::sce_dynamic_elf();
    test_fixture::put_dynamic(bad_symbol_size, 3, nyxora::loader::kDynamicSceSymbolEntrySize, 8);
    NYXORA_CHECK(dynamic_rejects(std::move(bad_symbol_size)));

    auto missing_segment = generic_dynamic_elf(0x900000);
    NYXORA_CHECK(dynamic_rejects(std::move(missing_segment)));

    auto overflowing = generic_dynamic_elf(std::numeric_limits<std::uint64_t>::max() - 3U);
    NYXORA_CHECK(dynamic_rejects(std::move(overflowing)));
}
