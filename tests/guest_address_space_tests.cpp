#include "test.hpp"
#include "asteria/memory/guest_address_space.hpp"

#include <array>

ASTERIA_TEST(guest_address_space_rejects_overlap_and_enforces_write_protection) {
    using namespace asteria::memory;
    GuestAddressSpace memory;
    ASTERIA_CHECK(memory.map(0x1000, 0x1000, Protection::read | Protection::write, "code"));
    ASTERIA_CHECK(!memory.map(0x1800, 0x1000, Protection::read, "overlap"));
    const std::array data{std::byte{1}, std::byte{2}};
    ASTERIA_CHECK(memory.write(0x1000, data));
    ASTERIA_CHECK(memory.protect(0x1000, 0x1000, Protection::read | Protection::execute));
    ASTERIA_CHECK(!memory.write(0x1000, data));
    ASTERIA_CHECK(memory.view(0x1000, 2)[1] == std::byte{2});
}
