#include "asteria/loader/elf64.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace asteria::loader {
namespace {

#pragma pack(push, 1)
struct RawElfHeader {
    std::uint8_t ident[16];
    std::uint16_t type;
    std::uint16_t machine;
    std::uint32_t version;
    std::uint64_t entry;
    std::uint64_t program_header_offset;
    std::uint64_t section_header_offset;
    std::uint32_t flags;
    std::uint16_t header_size;
    std::uint16_t program_header_entry_size;
    std::uint16_t program_header_count;
    std::uint16_t section_header_entry_size;
    std::uint16_t section_header_count;
    std::uint16_t section_name_index;
};

struct RawProgramHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t virtual_address;
    std::uint64_t physical_address;
    std::uint64_t file_size;
    std::uint64_t memory_size;
    std::uint64_t alignment;
};
#pragma pack(pop)

static_assert(sizeof(RawElfHeader) == 64);
static_assert(sizeof(RawProgramHeader) == 56);

template <typename T>
T read_object(std::span<const std::byte> bytes, std::size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        throw std::runtime_error("ELF structure lies outside the input image");
    }
    T result{};
    std::memcpy(&result, bytes.data() + offset, sizeof(T));
    return result;
}

bool range_fits(std::uint64_t offset, std::uint64_t size, std::size_t image_size) {
    if (offset > image_size) {
        return false;
    }
    return size <= static_cast<std::uint64_t>(image_size) - offset;
}

} // namespace

Elf64Image Elf64Image::from_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("unable to open ELF image: " + path.string());
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("unable to determine ELF image size");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), end)) {
        throw std::runtime_error("unable to read ELF image");
    }
    return from_bytes(std::move(bytes));
}

Elf64Image Elf64Image::from_bytes(std::vector<std::byte> bytes) {
    const auto view = std::span<const std::byte>(bytes);
    const auto header = read_object<RawElfHeader>(view, 0);

    const std::array<std::uint8_t, 4> magic{0x7f, 'E', 'L', 'F'};
    if (!std::equal(magic.begin(), magic.end(), header.ident)) {
        throw std::runtime_error("input is not an ELF image");
    }
    if (header.ident[4] != 2 || header.ident[5] != 1 || header.ident[6] != 1) {
        throw std::runtime_error("only 64-bit little-endian ELF v1 images are supported");
    }
    if (header.machine != kMachineX86_64) {
        throw std::runtime_error("ELF image is not x86-64");
    }
    if (header.program_header_entry_size != sizeof(RawProgramHeader)) {
        throw std::runtime_error("unexpected ELF program-header entry size");
    }

    const auto table_size = static_cast<std::uint64_t>(header.program_header_count) *
                            sizeof(RawProgramHeader);
    if (!range_fits(header.program_header_offset, table_size, bytes.size())) {
        throw std::runtime_error("ELF program-header table is outside the input image");
    }

    Elf64Image image;
    image.type_ = header.type;
    image.entry_ = header.entry;
    image.bytes_ = std::move(bytes);
    image.program_headers_.reserve(header.program_header_count);

    const auto stable_view = std::span<const std::byte>(image.bytes_);
    for (std::uint16_t index = 0; index < header.program_header_count; ++index) {
        const auto offset = header.program_header_offset +
                            static_cast<std::uint64_t>(index) * sizeof(RawProgramHeader);
        const auto raw = read_object<RawProgramHeader>(stable_view, static_cast<std::size_t>(offset));
        if (raw.file_size > raw.memory_size) {
            throw std::runtime_error("ELF segment file size exceeds memory size");
        }
        if (raw.file_size != 0 && !range_fits(raw.offset, raw.file_size, image.bytes_.size())) {
            throw std::runtime_error("ELF segment data is outside the input image");
        }
        image.program_headers_.push_back(ProgramHeader{
            .type = raw.type,
            .flags = raw.flags,
            .offset = raw.offset,
            .virtual_address = raw.virtual_address,
            .physical_address = raw.physical_address,
            .file_size = raw.file_size,
            .memory_size = raw.memory_size,
            .alignment = raw.alignment,
        });
    }
    return image;
}

} // namespace asteria::loader
