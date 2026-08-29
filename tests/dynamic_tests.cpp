#include "test.hpp"
#include "sce_fixture.hpp"
#include "nyxora/loader/dynamic.hpp"

#include <stdexcept>

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
