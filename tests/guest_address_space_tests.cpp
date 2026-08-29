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
