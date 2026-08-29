#include "test.hpp"
#include "asteria/runtime/symbol_registry.hpp"

ASTERIA_TEST(symbol_registry_keys_include_library_and_module) {
    using namespace asteria::runtime;
    SymbolRegistry registry;
    SymbolKey key{"abc", "libKernel", "libkernel", 1, 1, 0, SymbolKind::function};
    ASTERIA_CHECK(registry.register_symbol(key, {0x1234, "sceExample", true}));
    auto result = registry.resolve(key);
    ASTERIA_CHECK(result.has_value());
    ASTERIA_CHECK(result->address == 0x1234);
    auto different = key;
    different.library = "other";
    ASTERIA_CHECK(!registry.resolve(different).has_value());
}
