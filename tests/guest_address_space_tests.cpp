#include "test.hpp"
#include "nyxora/memory/guest_address_space.hpp"

#include <array>

NYXORA_TEST(guest_address_space_rejects_overlap_and_enforces_write_protection) {
    using namespace nyxora::memory;
    GuestAddressSpace memory;
    NYXORA_CHECK(memory.map(0x1000, 0x1000, Protection::read | Protection::write, "code"));
    NYXORA_CHECK(!memory.map(0x1800, 0x1000, Protection::read, "overlap"));
    const std::array data{std::byte{1}, std::byte{2}};
    NYXORA_CHECK(memory.write(0x1000, data));
    NYXORA_CHECK(memory.protect(0x1000, 0x1000, Protection::read | Protection::execute));
    NYXORA_CHECK(!memory.write(0x1000, data));
    NYXORA_CHECK(memory.view(0x1000, 2)[1] == std::byte{2});
}


NYXORA_TEST(guest_address_space_protect_range_splits_region_and_preserves_bytes) {
    using nyxora::memory::Protection;
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0x10000;
    constexpr nyxora::GuestSize page = 0x4000;
    NYXORA_CHECK(memory.map(base, page * 3,
                            Protection::read | Protection::write, "range"));
    const std::array<std::byte, 4> marker{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    NYXORA_CHECK(memory.write(base + page + 8, marker));
    NYXORA_CHECK(memory.protect_range(base + page, page, Protection::read));

    const auto regions = memory.regions();
    NYXORA_CHECK(regions.size() == 3);
    NYXORA_CHECK(regions[0].base == base);
    NYXORA_CHECK(regions[0].size == page);
    NYXORA_CHECK(regions[1].base == base + page);
    NYXORA_CHECK(regions[1].size == page);
    NYXORA_CHECK(regions[1].protection == Protection::read);
    NYXORA_CHECK(regions[2].base == base + page * 2);
    NYXORA_CHECK(regions[2].size == page);
    const auto bytes = memory.view(base + page + 8, marker.size());
    NYXORA_CHECK(std::equal(bytes.begin(), bytes.end(), marker.begin(), marker.end()));
    NYXORA_CHECK(!memory.write(base + page + 8, marker));
    NYXORA_CHECK(memory.write(base + page * 2 + 8, marker));
}


NYXORA_TEST(guest_address_space_unmap_range_splits_regions_and_tolerates_holes) {
    using nyxora::memory::Protection;
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0x10000;
    constexpr nyxora::GuestSize page = 0x1000;
    NYXORA_CHECK(memory.map(base, page * 3, Protection::read | Protection::write, "first"));
    NYXORA_CHECK(memory.map(base + page * 4, page * 2,
                            Protection::read | Protection::write, "second"));
    const std::array<std::byte, 1> left{std::byte{0x11}};
    const std::array<std::byte, 1> right{std::byte{0x22}};
    NYXORA_CHECK(memory.write(base, left));
    NYXORA_CHECK(memory.write(base + page * 5, right));

    NYXORA_CHECK(memory.unmap_range(base + page, page * 4));
    const auto regions = memory.regions();
    NYXORA_CHECK(regions.size() == 2);
    NYXORA_CHECK(regions[0].base == base);
    NYXORA_CHECK(regions[0].size == page);
    NYXORA_CHECK(regions[1].base == base + page * 5);
    NYXORA_CHECK(regions[1].size == page);
    NYXORA_CHECK(memory.find(base + page) == nullptr);
    NYXORA_CHECK(memory.find(base + page * 4) == nullptr);
    NYXORA_CHECK(memory.view(base, 1)[0] == left[0]);
    NYXORA_CHECK(memory.view(base + page * 5, 1)[0] == right[0]);
    NYXORA_CHECK(memory.unmap_range(base + page * 2, page));
}

NYXORA_TEST(native_guest_address_space_unmap_range_releases_subrange_for_reuse) {
    using nyxora::memory::Protection;
    const auto page = static_cast<nyxora::GuestSize>(nyxora::memory::NativeArena::page_size());
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 4);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();
    NYXORA_CHECK(memory->map(base, page * 3, Protection::read | Protection::write, "native"));
    NYXORA_CHECK(memory->unmap_range(base + page, page));
    NYXORA_CHECK(memory->find(base) != nullptr);
    NYXORA_CHECK(memory->find(base + page) == nullptr);
    NYXORA_CHECK(memory->find(base + page * 2) != nullptr);
    NYXORA_CHECK(memory->map(base + page, page, Protection::read | Protection::write, "reused"));
    const std::array<std::byte, 1> value{std::byte{0x5a}};
    NYXORA_CHECK(memory->write(base + page, value));
    NYXORA_CHECK(memory->view(base + page, 1)[0] == value[0]);
}

NYXORA_TEST(guest_address_space_exact_unmap_requires_matching_region) {
    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0x9000;
    constexpr nyxora::GuestSize size = 0x1000;
    NYXORA_CHECK(memory.map(base, size, nyxora::memory::Protection::read, "exact"));
    NYXORA_CHECK(!memory.unmap(base, size / 2));
    NYXORA_CHECK(memory.find(base) != nullptr);
    NYXORA_CHECK(memory.unmap(base, size));
    NYXORA_CHECK(memory.find(base) == nullptr);
    NYXORA_CHECK(!memory.unmap(base, size));
}

NYXORA_TEST(guest_address_space_preserves_semantic_metadata_across_splits) {
    using nyxora::memory::Protection;
    using nyxora::memory::RegionInfo;
    using nyxora::memory::RegionKind;

    nyxora::memory::GuestAddressSpace memory;
    constexpr nyxora::GuestAddress base = 0x20000;
    constexpr nyxora::GuestSize page = 0x4000;
    RegionInfo direct{
        .base = base,
        .size = page * 3,
        .protection = Protection::read | Protection::write,
        .name = "direct-meta",
        .offset = 0x8000,
        .memory_type = 11,
        .kind = RegionKind::direct,
        .committed = true,
        .auxiliary_protection = 0x30,
    };
    NYXORA_CHECK(memory.map(std::move(direct)));
    NYXORA_CHECK(memory.protect_range(base + page, page, Protection::read));

    auto regions = memory.regions();
    NYXORA_CHECK(regions.size() == 3);
    for (std::size_t index = 0; index < regions.size(); ++index) {
        NYXORA_CHECK(regions[index].kind == RegionKind::direct);
        NYXORA_CHECK(regions[index].memory_type == 11);
        NYXORA_CHECK(regions[index].name == "direct-meta");
        NYXORA_CHECK(regions[index].auxiliary_protection == 0x30);
        NYXORA_CHECK(regions[index].offset == 0x8000 + page * index);
    }
    NYXORA_CHECK(regions[1].protection == Protection::read);

    NYXORA_CHECK(memory.unmap_range(base + page, page));
    regions = memory.regions();
    NYXORA_CHECK(regions.size() == 2);
    NYXORA_CHECK(regions[0].offset == 0x8000);
    NYXORA_CHECK(regions[1].base == base + page * 2);
    NYXORA_CHECK(regions[1].offset == 0x8000 + page * 2);
}
