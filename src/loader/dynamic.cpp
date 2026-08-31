#include "nyxora/loader/dynamic.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace nyxora::loader {
namespace {

#pragma pack(push, 1)
struct RawDynamicEntry {
    std::int64_t tag;
    std::uint64_t value;
};

struct RawSymbol {
    std::uint32_t name_offset;
    std::uint8_t info;
    std::uint8_t other;
    std::uint16_t section_index;
    std::uint64_t value;
    std::uint64_t size;
};

struct RawRelocation {
    std::uint64_t offset;
    std::uint64_t info;
    std::int64_t addend;
};
#pragma pack(pop)

static_assert(sizeof(RawDynamicEntry) == 16);
static_assert(sizeof(RawSymbol) == 24);
static_assert(sizeof(RawRelocation) == 24);

template <typename T>
T read_object(std::span<const std::byte> bytes, std::size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        throw std::runtime_error("dynamic ELF structure lies outside its table");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

bool checked_add(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result) {
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

std::span<const std::byte> file_segment(const Elf64Image& image, const ProgramHeader& header) {
    if (header.offset > image.bytes().size() ||
        header.file_size > static_cast<std::uint64_t>(image.bytes().size()) - header.offset) {
        throw std::runtime_error("ELF segment is outside the input image");
    }
    return image.bytes().subspan(static_cast<std::size_t>(header.offset),
                                 static_cast<std::size_t>(header.file_size));
}

const ProgramHeader* find_header(const Elf64Image& image, std::uint32_t type) {
    const auto headers = image.program_headers();
    const auto it = std::find_if(headers.begin(), headers.end(),
                                 [type](const ProgramHeader& header) { return header.type == type; });
    return it == headers.end() ? nullptr : &*it;
}

std::span<const std::byte> virtual_file_span(const Elf64Image& image, std::uint64_t address,
                                             std::uint64_t size) {
    std::uint64_t end{};
    if (!checked_add(address, size, end)) {
        throw std::runtime_error("dynamic virtual-address range overflows");
    }
    for (const auto& header : image.program_headers()) {
        if (header.type != kProgramLoad && header.type != kProgramSceRelro) {
            continue;
        }
        std::uint64_t file_end{};
        if (!checked_add(header.virtual_address, header.file_size, file_end)) {
            continue;
        }
        if (address < header.virtual_address || end > file_end) {
            continue;
        }
        const auto inside = address - header.virtual_address;
        const auto file_offset = header.offset + inside;
        return image.bytes().subspan(static_cast<std::size_t>(file_offset),
                                     static_cast<std::size_t>(size));
    }
    throw std::runtime_error("dynamic virtual address is not backed by an ELF load segment");
}

std::optional<std::uint64_t> first_value(std::span<const DynamicEntry> entries, std::int64_t tag) {
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [tag](const DynamicEntry& entry) { return entry.tag == tag; });
    if (it == entries.end()) {
        return std::nullopt;
    }
    return it->value;
}

std::vector<std::uint64_t> all_values(std::span<const DynamicEntry> entries, std::int64_t tag) {
    std::vector<std::uint64_t> result;
    for (const auto& entry : entries) {
        if (entry.tag == tag) {
            result.push_back(entry.value);
        }
    }
    return result;
}

std::optional<std::uint64_t> exclusive_value(std::span<const DynamicEntry> entries,
                                             std::int64_t sce_tag, std::int64_t generic_tag,
                                             bool& sce) {
    const auto sce_value = first_value(entries, sce_tag);
    const auto generic_value = first_value(entries, generic_tag);
    if (sce_value && generic_value) {
        throw std::runtime_error("ELF mixes SCE and generic dynamic tags for the same table");
    }
    if (sce_value) {
        sce = true;
        return sce_value;
    }
    return generic_value;
}

std::string string_at(const std::string& table, std::uint64_t offset) {
    if (offset >= table.size()) {
        throw std::runtime_error("dynamic string offset is outside the string table");
    }
    const auto begin = table.data() + static_cast<std::size_t>(offset);
    const auto remaining = table.size() - static_cast<std::size_t>(offset);
    const auto* end = static_cast<const char*>(std::memchr(begin, '\0', remaining));
    if (end == nullptr) {
        throw std::runtime_error("dynamic string is not NUL terminated");
    }
    return std::string(begin, end);
}

std::span<const std::byte> resolve_table(const Elf64Image& image,
                                         const ProgramHeader* sce_dynamic_data,
                                         std::span<const DynamicEntry> entries,
                                         std::int64_t sce_pointer_tag,
                                         std::int64_t generic_pointer_tag,
                                         std::uint64_t size, bool& sce_layout) {
    bool sce_pointer = false;
    const auto pointer = exclusive_value(entries, sce_pointer_tag, generic_pointer_tag, sce_pointer);
    if (!pointer) {
        return {};
    }
    if (sce_pointer) {
        if (sce_dynamic_data == nullptr) {
            throw std::runtime_error("SCE dynamic table references missing PT_SCE_DYNLIBDATA");
        }
        const auto data = file_segment(image, *sce_dynamic_data);
        if (*pointer > data.size() || size > static_cast<std::uint64_t>(data.size()) - *pointer) {
            throw std::runtime_error("SCE dynamic table lies outside PT_SCE_DYNLIBDATA");
        }
        sce_layout = true;
        return data.subspan(static_cast<std::size_t>(*pointer), static_cast<std::size_t>(size));
    }
    return virtual_file_span(image, *pointer, size);
}

std::vector<Relocation> parse_relocations(std::span<const std::byte> bytes,
                                          std::uint64_t entry_size) {
    if (bytes.empty()) {
        return {};
    }
    if (entry_size != sizeof(RawRelocation) || bytes.size() % entry_size != 0) {
        throw std::runtime_error("unsupported ELF RELA entry size");
    }
    std::vector<Relocation> result;
    result.reserve(bytes.size() / sizeof(RawRelocation));
    for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(RawRelocation)) {
        const auto raw = read_object<RawRelocation>(bytes, offset);
        result.push_back(Relocation{raw.offset, raw.info, raw.addend});
    }
    return result;
}

void append_modules(DynamicInfo& info, std::span<const DynamicEntry> entries, std::int64_t tag,
                    bool imports) {
    for (const auto packed : all_values(entries, tag)) {
        ModuleReference reference;
        reference.id = static_cast<std::uint16_t>((packed >> 48U) & 0xffffU);
        reference.version_major = static_cast<std::uint8_t>((packed >> 40U) & 0xffU);
        reference.version_minor = static_cast<std::uint8_t>((packed >> 32U) & 0xffU);
        reference.encoded_id = encode_sce_id(reference.id);
        reference.name = string_at(info.string_table, packed & 0xffffffffU);
        (imports ? info.import_modules : info.export_modules).push_back(std::move(reference));
    }
}

void append_libraries(DynamicInfo& info, std::span<const DynamicEntry> entries, std::int64_t tag,
                      bool imports) {
    for (const auto packed : all_values(entries, tag)) {
        LibraryReference reference;
        reference.id = static_cast<std::uint16_t>((packed >> 48U) & 0xffffU);
        reference.version = static_cast<std::uint16_t>((packed >> 32U) & 0xffffU);
        reference.encoded_id = encode_sce_id(reference.id);
        reference.name = string_at(info.string_table, packed & 0xffffffffU);
        (imports ? info.import_libraries : info.export_libraries).push_back(std::move(reference));
    }
}

} // namespace

std::string encode_sce_id(std::uint16_t id) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result;
    if (id < 0x40U) {
        result.push_back(alphabet[id]);
    } else if (id < 0x1000U) {
        result.push_back(alphabet[(id >> 6U) & 0x3fU]);
        result.push_back(alphabet[id & 0x3fU]);
    } else {
        result.push_back(alphabet[(id >> 12U) & 0x3fU]);
        result.push_back(alphabet[(id >> 6U) & 0x3fU]);
        result.push_back(alphabet[id & 0x3fU]);
    }
    return result;
}

std::optional<DynamicInfo> parse_dynamic_info(const Elf64Image& image) {
    const auto* dynamic_header = find_header(image, kProgramDynamic);
    if (dynamic_header == nullptr || dynamic_header->file_size == 0) {
        return std::nullopt;
    }

    const auto dynamic_bytes = file_segment(image, *dynamic_header);
    if (dynamic_bytes.size() < sizeof(RawDynamicEntry)) {
        throw std::runtime_error("ELF PT_DYNAMIC is too small");
    }

    DynamicInfo info;
    bool saw_null = false;
    for (std::size_t offset = 0; offset + sizeof(RawDynamicEntry) <= dynamic_bytes.size();
         offset += sizeof(RawDynamicEntry)) {
        const auto raw = read_object<RawDynamicEntry>(dynamic_bytes, offset);
        if (raw.tag == kDynamicNull) {
            saw_null = true;
            break;
        }
        info.entries.push_back(DynamicEntry{raw.tag, raw.value});
    }
    if (!saw_null) {
        throw std::runtime_error("ELF PT_DYNAMIC has no DT_NULL terminator");
    }

    const auto entries = std::span<const DynamicEntry>(info.entries);
    const auto* sce_dynamic_data = find_header(image, kProgramSceDynlibData);

    bool string_table_is_sce = false;
    const auto string_table_pointer =
        exclusive_value(entries, kDynamicSceStringTable, kDynamicStringTable, string_table_is_sce);
    const auto string_table_size = first_value(entries, string_table_is_sce ? kDynamicSceStringTableSize
                                                                            : kDynamicStringTableSize)
                                       .value_or(0);
    if (string_table_pointer) {
        if (string_table_size == 0) {
            throw std::runtime_error("dynamic string table has no size");
        }
        std::span<const std::byte> strings;
        if (string_table_is_sce) {
            if (sce_dynamic_data == nullptr) {
                throw std::runtime_error("SCE string table requires PT_SCE_DYNLIBDATA");
            }
            const auto data = file_segment(image, *sce_dynamic_data);
            if (*string_table_pointer > data.size() ||
                string_table_size > static_cast<std::uint64_t>(data.size()) - *string_table_pointer) {
                throw std::runtime_error("SCE string table lies outside PT_SCE_DYNLIBDATA");
            }
            strings = data.subspan(static_cast<std::size_t>(*string_table_pointer),
                                   static_cast<std::size_t>(string_table_size));
            info.sce_layout = true;
        } else {
            strings = virtual_file_span(image, *string_table_pointer, string_table_size);
        }
        info.string_table.assign(reinterpret_cast<const char*>(strings.data()), strings.size());
    }

    const auto rela_size = first_value(entries, first_value(entries, kDynamicSceRela).has_value()
                                                    ? kDynamicSceRelaSize
                                                    : kDynamicRelaSize)
                               .value_or(0);
    if (rela_size != 0) {
        const auto rela_entry_size =
            first_value(entries, first_value(entries, kDynamicSceRela).has_value()
                                     ? kDynamicSceRelaEntrySize
                                     : kDynamicRelaEntrySize)
                .value_or(0);
        auto rela_bytes = resolve_table(image, sce_dynamic_data, entries, kDynamicSceRela,
                                        kDynamicRela, rela_size, info.sce_layout);
        info.relocations = parse_relocations(rela_bytes, rela_entry_size);
    }

    const auto plt_size = first_value(entries, first_value(entries, kDynamicSceJumpRel).has_value()
                                                   ? kDynamicScePltRelSize
                                                   : kDynamicPltRelSize)
                              .value_or(0);
    if (plt_size != 0) {
        const auto plt_kind =
            first_value(entries, first_value(entries, kDynamicSceJumpRel).has_value()
                                     ? kDynamicScePltRel
                                     : kDynamicPltRel)
                .value_or(0);
        if (plt_kind != static_cast<std::uint64_t>(kDynamicRela)) {
            throw std::runtime_error("only RELA PLT relocations are supported");
        }
        auto plt_bytes = resolve_table(image, sce_dynamic_data, entries, kDynamicSceJumpRel,
                                       kDynamicJumpRel, plt_size, info.sce_layout);
        info.plt_relocations = parse_relocations(plt_bytes, sizeof(RawRelocation));
    }

    std::size_t symbol_count = 0;
    const bool sce_symbols = first_value(entries, kDynamicSceSymbolTable).has_value();
    const auto symbol_entry_size =
        first_value(entries, sce_symbols ? kDynamicSceSymbolEntrySize : kDynamicSymbolEntrySize)
            .value_or(0);
    if (sce_symbols) {
        const auto symbol_bytes = first_value(entries, kDynamicSceSymbolTableSize).value_or(0);
        if (symbol_bytes != 0) {
            if (symbol_entry_size != sizeof(RawSymbol) || symbol_bytes % symbol_entry_size != 0) {
                throw std::runtime_error("unsupported SCE symbol-table entry size");
            }
            symbol_count = static_cast<std::size_t>(symbol_bytes / symbol_entry_size);
        }
    } else if (first_value(entries, kDynamicSymbolTable)) {
        if (symbol_entry_size != 0 && symbol_entry_size != sizeof(RawSymbol)) {
            throw std::runtime_error("unsupported ELF symbol-table entry size");
        }
        if (const auto hash_pointer = first_value(entries, kDynamicHash)) {
            const auto hash_header = virtual_file_span(image, *hash_pointer, 8);
            std::uint32_t nchain{};
            std::memcpy(&nchain, hash_header.data() + 4, sizeof(nchain));
            symbol_count = nchain;
        } else {
            for (const auto& relocation : info.relocations) {
                symbol_count = std::max(symbol_count,
                                        static_cast<std::size_t>(relocation.symbol_index()) + 1U);
            }
            for (const auto& relocation : info.plt_relocations) {
                symbol_count = std::max(symbol_count,
                                        static_cast<std::size_t>(relocation.symbol_index()) + 1U);
            }
        }
    }

    if (symbol_count != 0) {
        const auto total_symbol_bytes = symbol_count * sizeof(RawSymbol);
        auto symbol_bytes = resolve_table(image, sce_dynamic_data, entries, kDynamicSceSymbolTable,
                                          kDynamicSymbolTable, total_symbol_bytes, info.sce_layout);
        info.symbols.reserve(symbol_count);
        for (std::size_t index = 0; index < symbol_count; ++index) {
            const auto raw = read_object<RawSymbol>(symbol_bytes, index * sizeof(RawSymbol));
            DynamicSymbol symbol{raw.name_offset, raw.info, raw.other, raw.section_index,
                                 raw.value, raw.size, {}};
            if (raw.name_offset != 0) {
                if (info.string_table.empty()) {
                    throw std::runtime_error("ELF symbol name requires a string table");
                }
                symbol.name = string_at(info.string_table, raw.name_offset);
            }
            info.symbols.push_back(std::move(symbol));
        }
    }

    if (!info.string_table.empty()) {
        for (const auto offset : all_values(entries, kDynamicNeeded)) {
            info.needed.push_back(string_at(info.string_table, offset));
        }
        if (const auto offset = first_value(entries, kDynamicSoName)) {
            info.so_name = string_at(info.string_table, *offset);
        }
        if (const auto offset = first_value(entries, kDynamicRPath)) {
            info.rpath = string_at(info.string_table, *offset);
        }
        if (const auto offset = first_value(entries, kDynamicRunPath)) {
            info.runpath = string_at(info.string_table, *offset);
        }
        auto filename = first_value(entries, kDynamicSceOriginalFilename);
        if (!filename) {
            filename = first_value(entries, kDynamicSceOriginalFilename1);
        }
        if (filename) {
            info.original_filename = string_at(info.string_table, *filename);
        }

        append_modules(info, entries, kDynamicSceNeededModule, true);
        append_modules(info, entries, kDynamicSceNeededModule1, true);
        append_modules(info, entries, kDynamicSceModuleInfo, false);
        append_modules(info, entries, kDynamicSceModuleInfo1, false);
        append_libraries(info, entries, kDynamicSceImportLib, true);
        append_libraries(info, entries, kDynamicSceImportLib1, true);
        append_libraries(info, entries, kDynamicSceExportLib, false);
        append_libraries(info, entries, kDynamicSceExportLib1, false);
    }

    info.init = first_value(entries, kDynamicInit).value_or(0);
    info.fini = first_value(entries, kDynamicFini).value_or(0);
    info.init_array = first_value(entries, kDynamicInitArray).value_or(0);
    info.init_array_size = first_value(entries, kDynamicInitArraySize).value_or(0);
    info.fini_array = first_value(entries, kDynamicFiniArray).value_or(0);
    info.fini_array_size = first_value(entries, kDynamicFiniArraySize).value_or(0);
    info.preinit_array = first_value(entries, kDynamicPreinitArray).value_or(0);
    info.preinit_array_size = first_value(entries, kDynamicPreinitArraySize).value_or(0);
    auto plt_got = first_value(entries, kDynamicScePltGot);
    if (!plt_got) {
        plt_got = first_value(entries, kDynamicPltGot);
    }
    info.plt_got = plt_got.value_or(0);
    info.relative_count = first_value(entries, kDynamicRelaCount).value_or(0);

    return info;
}

} // namespace nyxora::loader
