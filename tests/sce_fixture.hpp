#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "nyxora/loader/dynamic.hpp"

namespace test_fixture {

struct SceImageLayout {
    static constexpr std::size_t dynamic_offset = 0x200;
    static constexpr std::size_t load_offset = 0x400;
    static constexpr std::size_t dynamic_data_offset = 0x600;
    static constexpr std::uint64_t load_virtual_address = 0x1000;
    static constexpr std::size_t dynamic_data_size = 0x300;
};

template <typename T>
void put(std::vector<std::byte>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

inline std::uint32_t append_string(std::vector<std::byte>& bytes, std::size_t table_base,
                                   std::size_t& cursor, std::string_view value) {
    const auto offset = static_cast<std::uint32_t>(cursor);
    std::memcpy(bytes.data() + table_base + cursor, value.data(), value.size());
    cursor += value.size();
    bytes[table_base + cursor++] = std::byte{0};
    return offset;
}

inline void put_dynamic(std::vector<std::byte>& bytes, std::size_t index, std::int64_t tag,
                        std::uint64_t value) {
    const auto offset = SceImageLayout::dynamic_offset + index * 16;
    put(bytes, offset, tag);
    put(bytes, offset + 8, value);
}

inline void put_symbol(std::vector<std::byte>& bytes, std::size_t index, std::uint32_t name,
                       std::uint8_t info, std::uint16_t section, std::uint64_t value,
                       std::uint64_t size = 0) {
    const auto offset = SceImageLayout::dynamic_data_offset + 0x80 + index * 24;
    put(bytes, offset, name);
    put(bytes, offset + 4, info);
    put(bytes, offset + 5, std::uint8_t{0});
    put(bytes, offset + 6, section);
    put(bytes, offset + 8, value);
    put(bytes, offset + 16, size);
}

inline void put_relocation(std::vector<std::byte>& bytes, std::size_t table_offset,
                           std::size_t index, std::uint64_t patch_offset,
                           std::uint32_t symbol, std::uint32_t type, std::int64_t addend) {
    const auto offset = SceImageLayout::dynamic_data_offset + table_offset + index * 24;
    const auto info = (static_cast<std::uint64_t>(symbol) << 32U) | type;
    put(bytes, offset, patch_offset);
    put(bytes, offset + 8, info);
    put(bytes, offset + 16, addend);
}

inline std::vector<std::byte> sce_dynamic_elf() {
    using namespace nyxora::loader;

    std::vector<std::byte> bytes(0x900);

    bytes[0] = std::byte{0x7f};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'L'};
    bytes[3] = std::byte{'F'};
    bytes[4] = std::byte{2};
    bytes[5] = std::byte{1};
    bytes[6] = std::byte{1};
    bytes[7] = std::byte{9};
    put(bytes, 16, kTypeSceDynExec);
    put(bytes, 18, kMachineX86_64);
    put(bytes, 20, std::uint32_t{1});
    put(bytes, 24, std::uint64_t{0x1040});
    put(bytes, 32, std::uint64_t{64});
    put(bytes, 52, std::uint16_t{64});
    put(bytes, 54, std::uint16_t{56});
    put(bytes, 56, std::uint16_t{4});

    auto put_program = [&](std::size_t index, std::uint32_t type, std::uint32_t flags,
                           std::uint64_t offset, std::uint64_t vaddr, std::uint64_t file_size,
                           std::uint64_t memory_size, std::uint64_t alignment) {
        const auto ph = 64 + index * 56;
        put(bytes, ph, type);
        put(bytes, ph + 4, flags);
        put(bytes, ph + 8, offset);
        put(bytes, ph + 16, vaddr);
        put(bytes, ph + 24, std::uint64_t{0});
        put(bytes, ph + 32, file_size);
        put(bytes, ph + 40, memory_size);
        put(bytes, ph + 48, alignment);
    };

    put_program(0, kProgramLoad, 5, SceImageLayout::load_offset,
                SceImageLayout::load_virtual_address, 0x100, 0x100, 0x1000);
    put_program(1, kProgramDynamic, 4, SceImageLayout::dynamic_offset, 0, 16 * 18, 16 * 18, 8);
    put_program(2, kProgramSceDynlibData, 4, SceImageLayout::dynamic_data_offset, 0,
                SceImageLayout::dynamic_data_size, SceImageLayout::dynamic_data_size, 16);
    put_program(3, kProgramTls, 4, SceImageLayout::load_offset + 0x80, 0x1080, 16, 32, 16);

    for (std::size_t i = 0; i < 0x100; ++i) {
        bytes[SceImageLayout::load_offset + i] = static_cast<std::byte>(i & 0xffU);
    }

    const std::byte entry_code[] = {std::byte{0xb8}, std::byte{0x2a}, std::byte{0x00},
                                    std::byte{0x00}, std::byte{0x00}, std::byte{0xc3}};
    std::memcpy(bytes.data() + SceImageLayout::load_offset + 0x40, entry_code, sizeof(entry_code));

    std::size_t string_cursor = 1;
    const auto library_name = append_string(bytes, SceImageLayout::dynamic_data_offset,
                                            string_cursor, "libTest");
    const auto module_name = append_string(bytes, SceImageLayout::dynamic_data_offset,
                                           string_cursor, "modTest");
    const auto import_name = append_string(bytes, SceImageLayout::dynamic_data_offset,
                                           string_cursor, "impNid#A#B");
    const auto export_name = append_string(bytes, SceImageLayout::dynamic_data_offset,
                                           string_cursor, "expNid#A#B");
    const auto filename = append_string(bytes, SceImageLayout::dynamic_data_offset,
                                        string_cursor, "fixture.elf");

    put_symbol(bytes, 0, 0, 0, 0, 0);
    put_symbol(bytes, 1, import_name,
               static_cast<std::uint8_t>((kSymbolBindGlobal << 4U) | kSymbolTypeFunction), 0, 0);
    put_symbol(bytes, 2, export_name,
               static_cast<std::uint8_t>((kSymbolBindGlobal << 4U) | kSymbolTypeFunction), 1,
               0x1040);

    put_relocation(bytes, 0x100, 0, 0x1000, 0, kRelocationX86_64Relative, 0x1040);
    put_relocation(bytes, 0x100, 1, 0x1010, 0, kRelocationX86_64DtpMod64, 0);
    put_relocation(bytes, 0x130, 0, 0x1008, 1, kRelocationX86_64JumpSlot, 0);

    const auto module_packed = (std::uint64_t{1} << 48U) | (std::uint64_t{1} << 40U) |
                               (std::uint64_t{2} << 32U) | module_name;
    const auto library_packed = (std::uint64_t{0} << 48U) | (std::uint64_t{3} << 32U) |
                                library_name;

    std::size_t dyn = 0;
    put_dynamic(bytes, dyn++, kDynamicSceStringTable, 0);
    put_dynamic(bytes, dyn++, kDynamicSceStringTableSize, string_cursor);
    put_dynamic(bytes, dyn++, kDynamicSceSymbolTable, 0x80);
    put_dynamic(bytes, dyn++, kDynamicSceSymbolEntrySize, 24);
    put_dynamic(bytes, dyn++, kDynamicSceSymbolTableSize, 72);
    put_dynamic(bytes, dyn++, kDynamicSceRela, 0x100);
    put_dynamic(bytes, dyn++, kDynamicSceRelaSize, 48);
    put_dynamic(bytes, dyn++, kDynamicSceRelaEntrySize, 24);
    put_dynamic(bytes, dyn++, kDynamicSceJumpRel, 0x130);
    put_dynamic(bytes, dyn++, kDynamicScePltRelSize, 24);
    put_dynamic(bytes, dyn++, kDynamicScePltRel, kDynamicRela);
    put_dynamic(bytes, dyn++, kDynamicSceNeededModule, module_packed);
    put_dynamic(bytes, dyn++, kDynamicSceImportLib, library_packed);
    put_dynamic(bytes, dyn++, kDynamicSceModuleInfo, module_packed);
    put_dynamic(bytes, dyn++, kDynamicSceExportLib, library_packed);
    put_dynamic(bytes, dyn++, kDynamicSceOriginalFilename, filename);
    put_dynamic(bytes, dyn++, kDynamicRelaCount, 1);
    put_dynamic(bytes, dyn++, kDynamicNull, 0);

    return bytes;
}

} // namespace test_fixture
