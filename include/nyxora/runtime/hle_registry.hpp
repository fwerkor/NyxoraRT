#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyxora/base/types.hpp"
#include "nyxora/memory/native_arena.hpp"
#include "nyxora/runtime/symbol_registry.hpp"

namespace nyxora::runtime {

using NoArgHleFunction = std::uint64_t (*)();

class HleRegistry {
public:
    explicit HleRegistry(SymbolRegistry& symbols);
    ~HleRegistry();

    bool register_no_arg(SymbolKey key, NoArgHleFunction function, std::string debug_name);
    [[nodiscard]] std::size_t size() const noexcept { return registered_; }

private:
    class NoArgBridgeTable;

    SymbolRegistry& symbols_;
    std::unique_ptr<NoArgBridgeTable> bridges_;
    std::size_t registered_{};
};

} // namespace nyxora::runtime
