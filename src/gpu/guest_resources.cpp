#include "nyxora/gpu/guest_resources.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace nyxora::gpu {
namespace {

std::uint32_t word_at(std::span<const std::byte> bytes, std::size_t pos, std::size_t count) {
    std::uint32_t word = 0;
    for (std::size_t i = 0; i < count; ++i) {
        word |= std::to_integer<std::uint32_t>(bytes[pos + i]) << (8U * i);
    }
    return word;
}

} // namespace

std::optional<std::vector<std::byte>>
GuestMemoryReader::snapshot(GuestAddress address, GuestSize size) const {
    if (size > max_guest_gpu_snapshot_bytes ||
        address > std::numeric_limits<GuestAddress>::max() - size) {
        return std::nullopt;
    }
    std::vector<std::byte> copy(static_cast<std::size_t>(size));
    std::size_t consumed = 0;
    while (consumed < copy.size()) {
        const auto cursor = address + consumed;
        const auto* region = memory_.find(cursor);
        if (!region || !region->committed ||
            !memory::has(region->protection, memory::Protection::read)) {
            return std::nullopt;
        }
        const auto offset = cursor - region->base;
        if (offset >= region->size) {
            return std::nullopt;
        }
        const auto length = std::min<GuestSize>(copy.size() - consumed, region->size - offset);
        const auto view = memory_.view(cursor, length);
        if (view.size() != length) {
            return std::nullopt;
        }
        std::memcpy(copy.data() + consumed, view.data(), view.size());
        consumed += view.size();
    }
    return copy;
}

std::optional<ResolvedIndexBuffer>
resolve_index_buffer(const GuestMemoryReader& reader, const pm4::DrawIndex2& draw) {
    if (!draw.index_type || draw.index_count > draw.max_size) {
        return std::nullopt;
    }
    // AMD VGT_INDEX_TYPE encodings (Mesa register definitions).
    const auto width = [&]() -> std::optional<IndexElementWidth> {
        switch (*draw.index_type) {
        case 0:
            return IndexElementWidth::u16;
        case 1:
            return IndexElementWidth::u32;
        case 2:
            return IndexElementWidth::u8;
        default:
            return std::nullopt;
        }
    }();
    if (!width) {
        return std::nullopt;
    }
    const auto bytes_per_index = static_cast<std::size_t>(*width);
    if (draw.index_address % bytes_per_index != 0 ||
        draw.index_count > max_guest_gpu_snapshot_bytes / bytes_per_index) {
        return std::nullopt;
    }
    const auto bytes = reader.snapshot(
        draw.index_address, static_cast<GuestSize>(draw.index_count) * bytes_per_index);
    if (!bytes) {
        return std::nullopt;
    }
    ResolvedIndexBuffer result{.width = *width, .indices = {}};
    result.indices.reserve(draw.index_count);
    for (std::size_t offset = 0; offset < bytes->size(); offset += bytes_per_index) {
        result.indices.push_back(word_at(*bytes, offset, bytes_per_index));
    }
    return result;
}

std::optional<std::vector<std::uint32_t>>
snapshot_shader_words(const GuestMemoryReader& reader, const pm4::ShaderProgram& program,
                      std::size_t word_count) {
    if (program.address % sizeof(std::uint32_t) != 0 ||
        word_count == 0 || word_count > max_guest_gpu_snapshot_bytes / sizeof(std::uint32_t)) {
        return std::nullopt;
    }
    const auto bytes = reader.snapshot(program.address, word_count * sizeof(std::uint32_t));
    if (!bytes) {
        return std::nullopt;
    }
    std::vector<std::uint32_t> words;
    words.reserve(word_count);
    for (std::size_t offset = 0; offset < bytes->size(); offset += sizeof(std::uint32_t)) {
        words.push_back(word_at(*bytes, offset, sizeof(std::uint32_t)));
    }
    return words;
}

} // namespace nyxora::gpu
