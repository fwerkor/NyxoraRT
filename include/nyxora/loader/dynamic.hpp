#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nyxora/loader/elf64.hpp"

namespace nyxora::loader {

inline constexpr std::int64_t kDynamicNull = 0;
inline constexpr std::int64_t kDynamicNeeded = 1;
inline constexpr std::int64_t kDynamicPltRelSize = 2;
inline constexpr std::int64_t kDynamicPltGot = 3;
inline constexpr std::int64_t kDynamicHash = 4;
inline constexpr std::int64_t kDynamicStringTable = 5;
inline constexpr std::int64_t kDynamicSymbolTable = 6;
inline constexpr std::int64_t kDynamicRela = 7;
inline constexpr std::int64_t kDynamicRelaSize = 8;
inline constexpr std::int64_t kDynamicRelaEntrySize = 9;
inline constexpr std::int64_t kDynamicStringTableSize = 10;
inline constexpr std::int64_t kDynamicSymbolEntrySize = 11;
inline constexpr std::int64_t kDynamicInit = 12;
inline constexpr std::int64_t kDynamicFini = 13;
inline constexpr std::int64_t kDynamicSoName = 14;
inline constexpr std::int64_t kDynamicRPath = 15;
inline constexpr std::int64_t kDynamicPltRel = 20;
inline constexpr std::int64_t kDynamicDebug = 21;
inline constexpr std::int64_t kDynamicTextRel = 22;
inline constexpr std::int64_t kDynamicJumpRel = 23;
inline constexpr std::int64_t kDynamicInitArray = 25;
inline constexpr std::int64_t kDynamicFiniArray = 26;
inline constexpr std::int64_t kDynamicInitArraySize = 27;
inline constexpr std::int64_t kDynamicFiniArraySize = 28;
inline constexpr std::int64_t kDynamicRunPath = 29;
inline constexpr std::int64_t kDynamicFlags = 30;
inline constexpr std::int64_t kDynamicPreinitArray = 32;
inline constexpr std::int64_t kDynamicPreinitArraySize = 33;
inline constexpr std::int64_t kDynamicRelaCount = 0x6ffffff9;

inline constexpr std::int64_t kDynamicSceFingerprint = 0x61000007;
inline constexpr std::int64_t kDynamicSceOriginalFilename = 0x61000009;
inline constexpr std::int64_t kDynamicSceModuleInfo = 0x6100000d;
inline constexpr std::int64_t kDynamicSceNeededModule = 0x6100000f;
inline constexpr std::int64_t kDynamicSceModuleAttr = 0x61000011;
inline constexpr std::int64_t kDynamicSceExportLib = 0x61000013;
inline constexpr std::int64_t kDynamicSceImportLib = 0x61000015;
inline constexpr std::int64_t kDynamicSceExportLibAttr = 0x61000017;
inline constexpr std::int64_t kDynamicSceImportLibAttr = 0x61000019;
inline constexpr std::int64_t kDynamicSceHash = 0x61000025;
inline constexpr std::int64_t kDynamicScePltGot = 0x61000027;
inline constexpr std::int64_t kDynamicSceJumpRel = 0x61000029;
inline constexpr std::int64_t kDynamicScePltRel = 0x6100002b;
inline constexpr std::int64_t kDynamicScePltRelSize = 0x6100002d;
inline constexpr std::int64_t kDynamicSceRela = 0x6100002f;
inline constexpr std::int64_t kDynamicSceRelaSize = 0x61000031;
inline constexpr std::int64_t kDynamicSceRelaEntrySize = 0x61000033;
inline constexpr std::int64_t kDynamicSceStringTable = 0x61000035;
inline constexpr std::int64_t kDynamicSceStringTableSize = 0x61000037;
inline constexpr std::int64_t kDynamicSceSymbolTable = 0x61000039;
inline constexpr std::int64_t kDynamicSceSymbolEntrySize = 0x6100003b;
inline constexpr std::int64_t kDynamicSceHashSize = 0x6100003d;
inline constexpr std::int64_t kDynamicSceSymbolTableSize = 0x6100003f;
inline constexpr std::int64_t kDynamicSceOriginalFilename1 = 0x61000041;
inline constexpr std::int64_t kDynamicSceModuleInfo1 = 0x61000043;
inline constexpr std::int64_t kDynamicSceNeededModule1 = 0x61000045;
inline constexpr std::int64_t kDynamicSceExportLib1 = 0x61000047;
inline constexpr std::int64_t kDynamicSceImportLib1 = 0x61000049;

inline constexpr std::uint32_t kRelocationX86_64_64 = 1;
inline constexpr std::uint32_t kRelocationX86_64GlobDat = 6;
inline constexpr std::uint32_t kRelocationX86_64JumpSlot = 7;
inline constexpr std::uint32_t kRelocationX86_64Relative = 8;
inline constexpr std::uint32_t kRelocationX86_64DtpMod64 = 16;

inline constexpr std::uint8_t kSymbolBindLocal = 0;
inline constexpr std::uint8_t kSymbolBindGlobal = 1;
inline constexpr std::uint8_t kSymbolBindWeak = 2;
inline constexpr std::uint8_t kSymbolTypeNoType = 0;
inline constexpr std::uint8_t kSymbolTypeObject = 1;
inline constexpr std::uint8_t kSymbolTypeFunction = 2;
inline constexpr std::uint8_t kSymbolTypeTls = 6;

struct DynamicEntry {
    std::int64_t tag{};
    std::uint64_t value{};
};

struct DynamicSymbol {
    std::uint32_t name_offset{};
    std::uint8_t info{};
    std::uint8_t other{};
    std::uint16_t section_index{};
    std::uint64_t value{};
    std::uint64_t size{};
    std::string name;

    [[nodiscard]] std::uint8_t binding() const noexcept { return info >> 4U; }
    [[nodiscard]] std::uint8_t type() const noexcept { return info & 0x0fU; }
};

struct Relocation {
    std::uint64_t offset{};
    std::uint64_t info{};
    std::int64_t addend{};

    [[nodiscard]] std::uint32_t symbol_index() const noexcept {
        return static_cast<std::uint32_t>(info >> 32U);
    }
    [[nodiscard]] std::uint32_t type() const noexcept {
        return static_cast<std::uint32_t>(info & 0xffffffffU);
    }
};

struct ModuleReference {
    std::uint16_t id{};
    std::uint8_t version_major{};
    std::uint8_t version_minor{};
    std::string encoded_id;
    std::string name;
};

struct LibraryReference {
    std::uint16_t id{};
    std::uint16_t version{};
    std::string encoded_id;
    std::string name;
};

struct DynamicInfo {
    bool sce_layout{};
    std::vector<DynamicEntry> entries;
    std::string string_table;
    std::vector<DynamicSymbol> symbols;
    std::vector<Relocation> relocations;
    std::vector<Relocation> plt_relocations;
    std::vector<std::string> needed;
    std::string so_name;
    std::string rpath;
    std::string runpath;
    std::string original_filename;
    std::vector<ModuleReference> import_modules;
    std::vector<ModuleReference> export_modules;
    std::vector<LibraryReference> import_libraries;
    std::vector<LibraryReference> export_libraries;
    std::uint64_t init{};
    std::uint64_t fini{};
    std::uint64_t init_array{};
    std::uint64_t init_array_size{};
    std::uint64_t fini_array{};
    std::uint64_t fini_array_size{};
    std::uint64_t preinit_array{};
    std::uint64_t preinit_array_size{};
    std::uint64_t plt_got{};
    std::uint64_t relative_count{};
};

[[nodiscard]] std::string encode_sce_id(std::uint16_t id);
[[nodiscard]] std::optional<DynamicInfo> parse_dynamic_info(const Elf64Image& image);

} // namespace nyxora::loader
