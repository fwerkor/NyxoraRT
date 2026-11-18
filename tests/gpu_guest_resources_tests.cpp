#include "test.hpp"
#include "nyxora/gpu/guest_resources.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

constexpr auto rw = nyxora::memory::Protection::read | nyxora::memory::Protection::write;

} // namespace

NYXORA_TEST(gpu_reader_reads_adjoining_regions_and_rejects_holes) {
    nyxora::memory::GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(0x1000, 4, rw, "a"));
    NYXORA_CHECK(memory.map(0x1004, 4, rw, "b"));
    const std::array<std::byte, 4> first{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array<std::byte, 4> second{std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    NYXORA_CHECK(memory.write(0x1000, first));
    NYXORA_CHECK(memory.write(0x1004, second));
    nyxora::gpu::GuestMemoryReader reader(memory);
    const auto copy = reader.snapshot(0x1002, 6);
    NYXORA_CHECK(copy && copy->size() == 6);
    NYXORA_CHECK((*copy)[0] == std::byte{3});
    NYXORA_CHECK((*copy)[5] == std::byte{8});
    NYXORA_CHECK(reader.snapshot(0x1008, 0).has_value());
    NYXORA_CHECK(!reader.snapshot(0x1002, 7));
    NYXORA_CHECK(memory.unmap(0x1004, 4));
    NYXORA_CHECK(!reader.snapshot(0x1002, 3));
}

NYXORA_TEST(gpu_reader_checks_permissions_commitment_and_overflow) {
    nyxora::memory::GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(0x2000, 8, rw, "writable"));
    NYXORA_CHECK(memory.map(0x3000, 8, nyxora::memory::Protection::none, "noaccess"));
    NYXORA_CHECK(memory.map(nyxora::memory::RegionInfo{
        .base = 0x4000,
        .size = 8,
        .protection = nyxora::memory::Protection::read,
        .name = "uncommitted",
        .committed = false,
    }));
    nyxora::gpu::GuestMemoryReader reader(memory);
    NYXORA_CHECK(reader.snapshot(0x2000, 8));
    NYXORA_CHECK(!reader.snapshot(0x3000, 1));
    NYXORA_CHECK(!reader.snapshot(0x4000, 1));
    NYXORA_CHECK(!reader.snapshot(0, 1));
    NYXORA_CHECK(!reader.snapshot(0x2000, nyxora::gpu::max_guest_gpu_snapshot_bytes + 1));
    NYXORA_CHECK(!reader.snapshot(std::numeric_limits<nyxora::GuestAddress>::max(), 2));
    NYXORA_CHECK(memory.protect(0x2000, 8, nyxora::memory::Protection::write));
    NYXORA_CHECK(!reader.snapshot(0x2000, 1));
}

NYXORA_TEST(gpu_reader_supports_native_mapped_memory_and_protection) {
    using nyxora::memory::GuestAddressSpace;
    using nyxora::memory::NativeArena;
    using nyxora::memory::Protection;
    const auto page = static_cast<nyxora::GuestSize>(NativeArena::page_size());
    auto memory = GuestAddressSpace::reserve_native(page * 3);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();
    NYXORA_CHECK(memory->map(base, page * 2, rw, "native"));
    const std::array<std::byte, 2> data{std::byte{0x12}, std::byte{0x34}};
    NYXORA_CHECK(memory->write(base + page - 1, data));
    nyxora::gpu::GuestMemoryReader reader(*memory);
    auto copy = reader.snapshot(base + page - 1, 2);
    NYXORA_CHECK(copy && (*copy)[0] == data[0] && (*copy)[1] == data[1]);
    NYXORA_CHECK(memory->protect_range(base + page, page, Protection::none));
    NYXORA_CHECK(!reader.snapshot(base + page - 1, 2));
}

NYXORA_TEST(gpu_index_reader_resolves_16_32_and_8_bit_elements) {
    nyxora::memory::GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(0x1000, 256, rw, "index"));
    const std::array<std::byte, 8> data{
        std::byte{0x34}, std::byte{0x12}, std::byte{0xff}, std::byte{0x7f},
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
    };
    NYXORA_CHECK(memory.write(0x1000, data));
    nyxora::gpu::GuestMemoryReader reader(memory);
    nyxora::gpu::pm4::DrawIndex2 draw{};
    draw.index_address = 0x1000;
    draw.max_size = 10;
    draw.index_count = 2;
    draw.index_type = 0;
    auto result = nyxora::gpu::resolve_index_buffer(reader, draw);
    NYXORA_CHECK(result && result->width == nyxora::gpu::IndexElementWidth::u16);
    NYXORA_CHECK(result->indices == std::vector<std::uint32_t>({0x1234, 0x7fff}));

    draw.index_type = 1;
    result = nyxora::gpu::resolve_index_buffer(reader, draw);
    NYXORA_CHECK(result && result->width == nyxora::gpu::IndexElementWidth::u32);
    NYXORA_CHECK(result->indices == std::vector<std::uint32_t>({0x7fff1234, 0x01020304}));

    draw.index_type = 2;
    draw.index_count = 4;
    result = nyxora::gpu::resolve_index_buffer(reader, draw);
    NYXORA_CHECK(result && result->width == nyxora::gpu::IndexElementWidth::u8);
    NYXORA_CHECK(result->indices == std::vector<std::uint32_t>({0x34, 0x12, 0xff, 0x7f}));
}

NYXORA_TEST(gpu_index_reader_rejects_unmapped_unaligned_and_excessive_draws) {
    nyxora::memory::GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(0x1000, 64, rw, "index"));
    nyxora::gpu::GuestMemoryReader reader(memory);
    nyxora::gpu::pm4::DrawIndex2 draw{};
    draw.index_type = 1;
    draw.index_address = 0x1000;
    draw.max_size = 16;
    draw.index_count = 5;
    NYXORA_CHECK(nyxora::gpu::resolve_index_buffer(reader, draw));
    draw.index_address = 0x1001;
    NYXORA_CHECK(!nyxora::gpu::resolve_index_buffer(reader, draw));
    draw.index_address = 0x1000;
    draw.index_count = 17;
    NYXORA_CHECK(!nyxora::gpu::resolve_index_buffer(reader, draw));
    draw.index_count = 4;
    draw.index_address = 0x2000;
    NYXORA_CHECK(!nyxora::gpu::resolve_index_buffer(reader, draw));
    draw.index_address = 0x1000;
    draw.index_type = 3;
    NYXORA_CHECK(!nyxora::gpu::resolve_index_buffer(reader, draw));
    draw.index_type.reset();
    NYXORA_CHECK(!nyxora::gpu::resolve_index_buffer(reader, draw));
    draw.index_type = 1;
    draw.index_count = 2'000'000;
    draw.max_size = 2'000'000;
    NYXORA_CHECK(!nyxora::gpu::resolve_index_buffer(reader, draw));
    draw.index_count = 0;
    NYXORA_CHECK(nyxora::gpu::resolve_index_buffer(reader, draw)->indices.empty());
}

NYXORA_TEST(gpu_shader_reader_returns_exact_little_endian_words) {
    nyxora::memory::GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(0x4000, 16, rw, "shader"));
    const std::array<std::byte, 8> bytes{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x80}, std::byte{0xbf},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x81}, std::byte{0xbf},
    };
    NYXORA_CHECK(memory.write(0x4000, bytes));
    nyxora::gpu::GuestMemoryReader reader(memory);
    nyxora::gpu::pm4::ShaderProgram shader{.stage = nyxora::gpu::pm4::ShaderStage::compute,
                                            .address = 0x4000};
    const auto words = nyxora::gpu::snapshot_shader_words(reader, shader, 2);
    NYXORA_CHECK(words && *words == std::vector<std::uint32_t>({0xbf800000, 0xbf810000}));
    shader.address = 0x4001;
    NYXORA_CHECK(!nyxora::gpu::snapshot_shader_words(reader, shader, 2));
    shader.address = 0x4000;
    NYXORA_CHECK(!nyxora::gpu::snapshot_shader_words(reader, shader, 0));
    NYXORA_CHECK(!nyxora::gpu::snapshot_shader_words(reader, shader, 5));
    NYXORA_CHECK(!nyxora::gpu::snapshot_shader_words(
        reader, shader, nyxora::gpu::max_guest_gpu_snapshot_bytes / 4 + 1));
}
