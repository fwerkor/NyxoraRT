#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nyxora::runtime {

union GuestDtvEntry {
    std::size_t counter;
    std::byte* pointer;
};

struct GuestTcb {
    GuestTcb* self{};
    GuestDtvEntry* dtv{};
    void* thread{};
    void* spare[2]{};
    std::uint64_t canary{};
    void* fiber{};
    std::uint64_t reserved{};
};
static_assert(sizeof(GuestTcb) == 0x40);

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
    [[nodiscard]] std::size_t max_module_id() const noexcept { return max_module_id_; }
    [[nodiscard]] std::size_t generation() const noexcept { return generation_; }

private:
    std::vector<TlsModuleImage> modules_;
    std::size_t max_module_id_{};
    std::size_t generation_{};
};

class GuestThreadContext {
public:
    GuestThreadContext() = default;
    GuestThreadContext(const GuestThreadContext&) = delete;
    GuestThreadContext& operator=(const GuestThreadContext&) = delete;
    GuestThreadContext(GuestThreadContext&& other) noexcept;
    GuestThreadContext& operator=(GuestThreadContext&& other) noexcept;

    [[nodiscard]] static std::optional<GuestThreadContext> create(const TlsRegistry& registry);
    bool synchronize(const TlsRegistry& registry);

    [[nodiscard]] void* tls_address(std::uint32_t module_id, std::size_t offset = 0) noexcept;
    [[nodiscard]] const void* tls_address(std::uint32_t module_id,
                                          std::size_t offset = 0) const noexcept;
    [[nodiscard]] GuestTcb* tcb() noexcept { return &tcb_; }
    [[nodiscard]] const GuestTcb* tcb() const noexcept { return &tcb_; }
    [[nodiscard]] std::span<const GuestDtvEntry> dtv() const noexcept { return dtv_; }
    [[nodiscard]] std::size_t tls_module_count() const noexcept { return blocks_.size(); }

private:
    struct Block {
        std::uint32_t module_id{};
        std::size_t memory_size{};
        std::size_t aligned_offset{};
        std::vector<std::byte> storage;
    };

    bool add_module(const TlsModuleImage& module);
    void rebuild_dtv(const TlsRegistry& registry);
    void rebind_tcb() noexcept;
    [[nodiscard]] Block* find_block(std::uint32_t module_id) noexcept;
    [[nodiscard]] const Block* find_block(std::uint32_t module_id) const noexcept;

    std::vector<Block> blocks_;
    std::vector<GuestDtvEntry> dtv_;
    GuestTcb tcb_{};
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

[[nodiscard]] std::optional<std::uint32_t> windows_guest_tcb_teb_offset() noexcept;
[[nodiscard]] std::optional<std::uint32_t> windows_host_stack_teb_offset() noexcept;

class ScopedGuestSegment {
public:
    explicit ScopedGuestSegment(GuestThreadContext& context);
    ~ScopedGuestSegment();

    ScopedGuestSegment(const ScopedGuestSegment&) = delete;
    ScopedGuestSegment& operator=(const ScopedGuestSegment&) = delete;

    [[nodiscard]] static bool supported() noexcept;
    [[nodiscard]] static std::optional<std::uintptr_t> current_base() noexcept;
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
#if defined(__linux__) && defined(__x86_64__)
    std::uintptr_t previous_base_{};
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    void* previous_windows_tcb_{};
#endif
    bool active_{};
};

} // namespace nyxora::runtime
