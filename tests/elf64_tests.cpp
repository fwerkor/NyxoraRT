#include "test.hpp"
#include "nyxora/loader/elf64.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace {

template <typename T>
void put(std::vector<std::byte>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

bool rejects(std::vector<std::byte> bytes) {
    try {
        (void)nyxora::loader::Elf64Image::from_bytes(std::move(bytes));
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}
std::vector<std::byte> minimal_elf() {
    std::vector<std::byte> bytes(64 + 56 + 4);
    auto put16 = [&](std::size_t o, std::uint16_t v) { std::memcpy(bytes.data() + o, &v, 2); };
    auto put32 = [&](std::size_t o, std::uint32_t v) { std::memcpy(bytes.data() + o, &v, 4); };
    auto put64 = [&](std::size_t o, std::uint64_t v) { std::memcpy(bytes.data() + o, &v, 8); };
    bytes[0] = std::byte{0x7f}; bytes[1] = std::byte{'E'}; bytes[2] = std::byte{'L'}; bytes[3] = std::byte{'F'};
    bytes[4] = std::byte{2}; bytes[5] = std::byte{1}; bytes[6] = std::byte{1}; bytes[7] = std::byte{9};
    put16(16, nyxora::loader::kTypeSceDynExec); put16(18, nyxora::loader::kMachineX86_64);
    put32(20, 1); put64(24, 0x120); put64(32, 64); put16(52, 64); put16(54, 56); put16(56, 1);
    put32(64, nyxora::loader::kProgramLoad); put32(68, 5); put64(72, 120); put64(80, 0x1000);
    put64(96, 4); put64(104, 8); put64(112, 0x1000);
    bytes[120] = std::byte{1}; bytes[121] = std::byte{2}; bytes[122] = std::byte{3}; bytes[123] = std::byte{4};
    return bytes;
}
}

NYXORA_TEST(parses_sce_x86_64_elf) {
    auto image = nyxora::loader::Elf64Image::from_bytes(minimal_elf());
    NYXORA_CHECK(image.is_sce_dynamic());
    NYXORA_CHECK(image.entry() == 0x120);
    NYXORA_CHECK(image.program_headers().size() == 1);
    NYXORA_CHECK(image.program_headers()[0].memory_size == 8);
}

NYXORA_TEST(rejects_bad_elf_magic) {
    auto bytes = minimal_elf();
    bytes[0] = std::byte{0};
    bool threw = false;
    try { (void)nyxora::loader::Elf64Image::from_bytes(std::move(bytes)); }
    catch (const std::runtime_error&) { threw = true; }
    NYXORA_CHECK(threw);
}


NYXORA_TEST(elf_rejects_structural_header_and_segment_errors) {
    NYXORA_CHECK(rejects({}));

    auto wrong_class = minimal_elf();
    wrong_class[4] = std::byte{1};
    NYXORA_CHECK(rejects(std::move(wrong_class)));

    auto wrong_machine = minimal_elf();
    put(wrong_machine, 18, std::uint16_t{0});
    NYXORA_CHECK(rejects(std::move(wrong_machine)));

    auto wrong_ph_size = minimal_elf();
    put(wrong_ph_size, 54, std::uint16_t{55});
    NYXORA_CHECK(rejects(std::move(wrong_ph_size)));

    auto table_outside = minimal_elf();
    put(table_outside, 32, std::uint64_t{0x10000});
    NYXORA_CHECK(rejects(std::move(table_outside)));

    auto file_exceeds_memory = minimal_elf();
    put(file_exceeds_memory, 64 + 32, std::uint64_t{9});
    put(file_exceeds_memory, 64 + 40, std::uint64_t{8});
    NYXORA_CHECK(rejects(std::move(file_exceeds_memory)));

    auto data_outside = minimal_elf();
    put(data_outside, 64 + 8, std::uint64_t{0x10000});
    NYXORA_CHECK(rejects(std::move(data_outside)));
}

NYXORA_TEST(elf_from_file_reads_valid_image_and_rejects_missing_path) {
    const auto root = std::filesystem::temp_directory_path() / "nyxora-elf-file-test";
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{root};

    const auto path = root / "minimal.elf";
    const auto bytes = minimal_elf();
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    const auto image = nyxora::loader::Elf64Image::from_file(path);
    NYXORA_CHECK(image.entry() == 0x120);
    NYXORA_CHECK(image.program_headers().size() == 1);

    bool missing = false;
    try {
        (void)nyxora::loader::Elf64Image::from_file(root / "missing.elf");
    } catch (const std::runtime_error&) {
        missing = true;
    }
    NYXORA_CHECK(missing);
}
