#include "test.hpp"
#include "nyxora/runtime/symbol_registry.hpp"

NYXORA_TEST(symbol_registry_keys_include_library_and_module) {
    using namespace nyxora::runtime;
    SymbolRegistry registry;
    SymbolKey key{"abc", "libKernel", "libkernel", 1, 1, 0, SymbolKind::function};
    NYXORA_CHECK(registry.register_symbol(key, {0x1234, "sceExample", true}));
    auto result = registry.resolve(key);
    NYXORA_CHECK(result.has_value());
    NYXORA_CHECK(result->address == 0x1234);
    auto different = key;
    different.library = "other";
    NYXORA_CHECK(!registry.resolve(different).has_value());
}
