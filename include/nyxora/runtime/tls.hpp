#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nyxora::runtime {

struct TlsModuleImage {
    std::uint32_t module_id{};
    std::size_t alignment{1};
    std::size_t memory_size{};
    std::vector<std::byte> initial_image;
};

class TlsRegistry {
public:
    bool register_module(std::uint32_t module_id, std::size_t alignment, std::size_t memory_size,
                         std::span<const std::byte> initial_image);

    [[nodiscard]] const TlsModuleImage* find(std::uint32_t module_id) const noexcept;
    [[nodiscard]] std::span<const TlsModuleImage> modules() const noexcept { return modules_; }
    [[nodiscard]] std::size_t size() const noexcept { return modules_.size(); }

private:
    std::vector<TlsModuleImage> modules_;
};

class GuestThreadContext {
public:
    GuestThreadContext() = default;

    [[nodiscard]] static std::optional<GuestThreadContext> create(const TlsRegistry& registry);
    bool synchronize(const TlsRegistry& registry);

    [[nodiscard]] void* tls_address(std::uint32_t module_id, std::size_t offset = 0) noexcept;
    [[nodiscard]] const void* tls_address(std::uint32_t module_id,
                                          std::size_t offset = 0) const noexcept;
    [[nodiscard]] std::size_t tls_module_count() const noexcept { return blocks_.size(); }

private:
    struct Block {
        std::uint32_t module_id{};
        std::size_t memory_size{};
        std::size_t aligned_offset{};
        std::vector<std::byte> storage;
    };

    bool add_module(const TlsModuleImage& module);
    [[nodiscard]] Block* find_block(std::uint32_t module_id) noexcept;
    [[nodiscard]] const Block* find_block(std::uint32_t module_id) const noexcept;

    std::vector<Block> blocks_;
};

class ScopedGuestThreadContext {
public:
    explicit ScopedGuestThreadContext(GuestThreadContext& context) noexcept;
    ~ScopedGuestThreadContext();

    ScopedGuestThreadContext(const ScopedGuestThreadContext&) = delete;
    ScopedGuestThreadContext& operator=(const ScopedGuestThreadContext&) = delete;

    [[nodiscard]] static GuestThreadContext* current() noexcept;

private:
    GuestThreadContext* previous_{};
};

} // namespace nyxora::runtime
