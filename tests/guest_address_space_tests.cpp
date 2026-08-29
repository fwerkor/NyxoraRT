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
