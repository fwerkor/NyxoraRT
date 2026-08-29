#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nyxora/base/types.hpp"
#include "nyxora/loader/dynamic.hpp"
#include "nyxora/memory/guest_address_space.hpp"
#include "nyxora/runtime/late_imports.hpp"
#include "nyxora/runtime/symbol_registry.hpp"

namespace nyxora::runtime {

struct UnresolvedRelocation {
    GuestAddress patch_address{};
    std::uint32_t type{};
    std::uint32_t symbol_index{};
    std::string symbol_name;
    bool weak{};
    bool plt{};
    GuestAddress thunk_address{};
};

struct RelocationReport {
    std::size_t applied{};
    std::size_t late_thunks{};
    std::vector<UnresolvedRelocation> unresolved;
};

class RuntimeLinker {
public:
    RuntimeLinker(memory::GuestAddressSpace& memory, SymbolRegistry& symbols,
                  LateImportTable* late_imports = nullptr)
        : memory_(memory), symbols_(symbols), late_imports_(late_imports) {}

    std::size_t register_exports(GuestAddress module_base, const loader::DynamicInfo& dynamic);
    RelocationReport relocate(GuestAddress module_base, std::uint32_t tls_module_id,
                              const loader::DynamicInfo& dynamic);

private:
    [[nodiscard]] std::optional<SymbolKey> make_symbol_key(const loader::DynamicInfo& dynamic,
                                                           const loader::DynamicSymbol& symbol) const;
    [[nodiscard]] std::optional<GuestAddress>
    resolve_symbol(GuestAddress module_base, const loader::DynamicInfo& dynamic,
                   const loader::DynamicSymbol& symbol) const;

    memory::GuestAddressSpace& memory_;
    SymbolRegistry& symbols_;
    LateImportTable* late_imports_{};
};

} // namespace nyxora::runtime
