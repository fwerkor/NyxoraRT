#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "asteria/base/types.hpp"

namespace asteria::runtime {

enum class SymbolKind : std::uint8_t { unknown, function, object, tls, no_type };

struct SymbolKey {
    std::string nid;
    std::string library;
    std::string module;
    std::uint16_t library_version{};
    std::uint16_t module_major{};
    std::uint16_t module_minor{};
    SymbolKind kind{SymbolKind::unknown};

    bool operator==(const SymbolKey&) const = default;
};

struct SymbolBinding {
    GuestAddress address{};
    std::string debug_name;
    bool hle{};
};

struct SymbolKeyHash {
    std::size_t operator()(const SymbolKey& key) const noexcept;
};

class SymbolRegistry {
public:
    bool register_symbol(SymbolKey key, SymbolBinding binding);
    [[nodiscard]] std::optional<SymbolBinding> resolve(const SymbolKey& key) const;
    [[nodiscard]] std::size_t size() const noexcept { return symbols_.size(); }

private:
    std::unordered_map<SymbolKey, SymbolBinding, SymbolKeyHash> symbols_;
};

} // namespace asteria::runtime
