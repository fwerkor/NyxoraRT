#pragma once

#include "nyxora/gpu/pm4.hpp"
#include "nyxora/memory/guest_address_space.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace nyxora::gpu {

inline constexpr std::size_t max_guest_gpu_snapshot_bytes = 4U * 1024U * 1024U;

// Non-owning view of the runtime's tracked guest VM. This interface captures a
// bounded, stable copy for offline inspection; it does not emulate GPU caches.
class GuestMemoryReader {
public:
    explicit GuestMemoryReader(const memory::GuestAddressSpace& memory) : memory_(memory) {}
    [[nodiscard]] std::optional<std::vector<std::byte>>
    snapshot(GuestAddress address, GuestSize size) const;

private:
    const memory::GuestAddressSpace& memory_;
};

enum class IndexElementWidth : std::uint8_t { u8 = 1, u16 = 2, u32 = 4 };

struct ResolvedIndexBuffer {
    IndexElementWidth width{};
    std::vector<std::uint32_t> indices;
};

// A CPU-side diagnostic snapshot, not a GPU draw or synchronization point.
[[nodiscard]] std::optional<ResolvedIndexBuffer>
resolve_index_buffer(const GuestMemoryReader& reader, const pm4::DrawIndex2& draw);

[[nodiscard]] std::optional<std::vector<std::uint32_t>>
snapshot_shader_words(const GuestMemoryReader& reader, const pm4::ShaderProgram& program,
                      std::size_t word_count);

} // namespace nyxora::gpu
