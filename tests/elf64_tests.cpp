#include "test.hpp"
#include "nyxora/loader/elf64.hpp"

#include <cstring>
#include <vector>

namespace {
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
