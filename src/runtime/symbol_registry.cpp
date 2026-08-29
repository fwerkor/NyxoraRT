#include "nyxora/runtime/symbol_registry.hpp"

#include <functional>

namespace nyxora::runtime {
namespace {
void hash_combine(std::size_t& seed, std::size_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}
}

std::size_t SymbolKeyHash::operator()(const SymbolKey& key) const noexcept {
    std::size_t seed = 0;
    hash_combine(seed, std::hash<std::string>{}(key.nid));
    hash_combine(seed, std::hash<std::string>{}(key.library));
    hash_combine(seed, std::hash<std::string>{}(key.module));
    hash_combine(seed, key.library_version);
    hash_combine(seed, key.module_major);
    hash_combine(seed, key.module_minor);
    hash_combine(seed, static_cast<std::size_t>(key.kind));
    return seed;
}

bool SymbolRegistry::register_symbol(SymbolKey key, SymbolBinding binding) {
    auto [it, inserted] = symbols_.emplace(key, binding);
    if (inserted) {
        return true;
    }
    if (it->second.hle && !binding.hle) {
        it->second = std::move(binding);
        return true;
    }
    return false;
}

std::optional<SymbolBinding> SymbolRegistry::resolve(const SymbolKey& key) const {
    const auto it = symbols_.find(key);
    if (it == symbols_.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace nyxora::runtime
