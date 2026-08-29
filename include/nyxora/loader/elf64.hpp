#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nyxora::loader {

inline constexpr std::uint16_t kMachineX86_64 = 62;
inline constexpr std::uint16_t kTypeExec = 2;
inline constexpr std::uint16_t kTypeDyn = 3;
inline constexpr std::uint16_t kTypeSceDynExec = 0xfe10;
inline constexpr std::uint16_t kTypeSceDynamic = 0xfe18;
inline constexpr std::uint32_t kProgramLoad = 1;
inline constexpr std::uint32_t kProgramDynamic = 2;
inline constexpr std::uint32_t kProgramTls = 7;
inline constexpr std::uint32_t kProgramSceDynlibData = 0x61000000;
inline constexpr std::uint32_t kProgramSceProcParam = 0x61000001;
inline constexpr std::uint32_t kProgramSceRelro = 0x61000010;

struct TlsSegment {
    std::uint64_t virtual_address{};
    std::uint64_t file_size{};
    std::uint64_t memory_size{};
    std::uint64_t alignment{};
};

struct ProgramHeader {
    std::uint32_t type{};
    std::uint32_t flags{};
    std::uint64_t offset{};
    std::uint64_t virtual_address{};
    std::uint64_t physical_address{};
    std::uint64_t file_size{};
    std::uint64_t memory_size{};
    std::uint64_t alignment{};
};

class Elf64Image {
public:
    static Elf64Image from_file(const std::filesystem::path& path);
    static Elf64Image from_bytes(std::vector<std::byte> bytes);

    [[nodiscard]] std::uint16_t type() const noexcept { return type_; }
    [[nodiscard]] std::uint64_t entry() const noexcept { return entry_; }
    [[nodiscard]] std::span<const ProgramHeader> program_headers() const noexcept {
        return program_headers_;
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] const std::optional<TlsSegment>& tls() const noexcept { return tls_; }
    [[nodiscard]] bool is_sce_dynamic() const noexcept {
        return type_ == kTypeSceDynExec || type_ == kTypeSceDynamic;
    }

private:
    std::vector<std::byte> bytes_;
    std::vector<ProgramHeader> program_headers_;
    std::uint16_t type_{};
    std::uint64_t entry_{};
    std::optional<TlsSegment> tls_;
};

} // namespace nyxora::loader
