#include "nyxora/runtime/linker.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace nyxora::runtime {
namespace {

std::optional<std::array<std::string_view, 3>> split_symbol_name(std::string_view value) {
    const auto first = value.find('#');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const auto second = value.find('#', first + 1);
    if (second == std::string_view::npos || value.find('#', second + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    return std::array<std::string_view, 3>{value.substr(0, first),
                                           value.substr(first + 1, second - first - 1),
                                           value.substr(second + 1)};
}

SymbolKind symbol_kind(std::uint8_t type) {
    switch (type) {
    case loader::kSymbolTypeFunction:
        return SymbolKind::function;
    case loader::kSymbolTypeObject:
        return SymbolKind::object;
    case loader::kSymbolTypeTls:
        return SymbolKind::tls;
    case loader::kSymbolTypeNoType:
        return SymbolKind::no_type;
    default:
        return SymbolKind::unknown;
    }
}

const loader::LibraryReference* find_library(const loader::DynamicInfo& dynamic,
                                              std::string_view encoded_id) {
    for (const auto& library : dynamic.import_libraries) {
        if (library.encoded_id == encoded_id) {
            return &library;
        }
    }
    for (const auto& library : dynamic.export_libraries) {
        if (library.encoded_id == encoded_id) {
            return &library;
        }
    }
    return nullptr;
}

const loader::ModuleReference* find_module(const loader::DynamicInfo& dynamic,
                                            std::string_view encoded_id) {
    for (const auto& module : dynamic.import_modules) {
        if (module.encoded_id == encoded_id) {
            return &module;
        }
    }
    for (const auto& module : dynamic.export_modules) {
        if (module.encoded_id == encoded_id) {
            return &module;
        }
    }
    return nullptr;
}

std::optional<std::uint64_t> add_unsigned(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

std::optional<std::uint64_t> add_signed(std::uint64_t lhs, std::int64_t rhs) {
    if (rhs >= 0) {
        return add_unsigned(lhs, static_cast<std::uint64_t>(rhs));
    }
    const auto magnitude = static_cast<std::uint64_t>(-(rhs + 1)) + 1U;
    if (lhs < magnitude) {
        return std::nullopt;
    }
    return lhs - magnitude;
}

bool patch_u64(memory::GuestAddressSpace& memory, GuestAddress address, std::uint64_t value) {
    std::array<std::byte, sizeof(value)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return memory.patch(address, bytes);
}

} // namespace

std::optional<SymbolKey> RuntimeLinker::make_symbol_key(const loader::DynamicInfo& dynamic,
                                                         const loader::DynamicSymbol& symbol) const {
    const auto parts = split_symbol_name(symbol.name);
    if (!parts || (*parts)[0].empty() || (*parts)[1].empty() || (*parts)[2].empty()) {
        return std::nullopt;
    }
    const auto* library = find_library(dynamic, (*parts)[1]);
    const auto* module = find_module(dynamic, (*parts)[2]);
    if (library == nullptr || module == nullptr) {
        return std::nullopt;
    }
    const auto kind = symbol_kind(symbol.type());
    if (kind == SymbolKind::unknown) {
        return std::nullopt;
    }
    return SymbolKey{
        .nid = std::string((*parts)[0]),
        .library = library->name,
        .module = module->name,
        .library_version = library->version,
        .module_major = module->version_major,
        .module_minor = module->version_minor,
        .kind = kind,
    };
}

std::size_t RuntimeLinker::register_exports(GuestAddress module_base,
                                            const loader::DynamicInfo& dynamic) {
    std::size_t registered = 0;
    for (const auto& symbol : dynamic.symbols) {
        if (symbol.value == 0 ||
            (symbol.binding() != loader::kSymbolBindGlobal &&
             symbol.binding() != loader::kSymbolBindWeak)) {
            continue;
        }
        const auto key = make_symbol_key(dynamic, symbol);
        if (!key) {
            continue;
        }
        const auto address = add_unsigned(module_base, symbol.value);
        if (!address) {
            throw std::runtime_error("export symbol address overflows guest address space");
        }
        if (symbols_.register_symbol(*key, SymbolBinding{*address, symbol.name, false})) {
            ++registered;
        }
    }
    return registered;
}

std::optional<GuestAddress> RuntimeLinker::resolve_symbol(
    GuestAddress module_base, const loader::DynamicInfo& dynamic,
    const loader::DynamicSymbol& symbol) const {
    if (symbol.binding() == loader::kSymbolBindLocal || symbol.value != 0) {
        return add_unsigned(module_base, symbol.value);
    }
    if (symbol.binding() != loader::kSymbolBindGlobal &&
        symbol.binding() != loader::kSymbolBindWeak) {
        return std::nullopt;
    }
    const auto key = make_symbol_key(dynamic, symbol);
    if (!key) {
        return std::nullopt;
    }
    const auto binding = symbols_.resolve(*key);
    return binding ? std::optional<GuestAddress>{binding->address} : std::nullopt;
}

RelocationReport RuntimeLinker::relocate(GuestAddress module_base, std::uint32_t tls_module_id,
                                         const loader::DynamicInfo& dynamic) {
    RelocationReport report;

    const auto relocate_table = [&](std::span<const loader::Relocation> relocations, bool plt) {
        for (const auto& relocation : relocations) {
            const auto patch_address = add_unsigned(module_base, relocation.offset);
            if (!patch_address) {
                throw std::runtime_error("relocation patch address overflows guest address space");
            }

            std::optional<std::uint64_t> value;
            std::optional<SymbolKey> key;
            bool weak = false;
            bool unresolved = false;
            GuestAddress thunk_address = 0;
            std::string symbol_name;

            switch (relocation.type()) {
            case loader::kRelocationX86_64Relative:
                value = add_signed(module_base, relocation.addend);
                break;
            case loader::kRelocationX86_64DtpMod64:
                if (tls_module_id != 0) {
                    value = tls_module_id;
                } else {
                    unresolved = true;
                }
                break;
            case loader::kRelocationX86_64_64:
            case loader::kRelocationX86_64GlobDat:
            case loader::kRelocationX86_64JumpSlot: {
                const auto symbol_index = relocation.symbol_index();
                if (symbol_index >= dynamic.symbols.size()) {
                    throw std::runtime_error("relocation symbol index is outside the dynamic symbol table");
                }
                const auto& symbol = dynamic.symbols[symbol_index];
                symbol_name = symbol.name;
                weak = symbol.binding() == loader::kSymbolBindWeak;
                key = make_symbol_key(dynamic, symbol);
                const auto resolved = resolve_symbol(module_base, dynamic, symbol);
                if (resolved) {
                    const auto addend = relocation.type() == loader::kRelocationX86_64_64
                                            ? relocation.addend
                                            : std::int64_t{0};
                    value = add_signed(*resolved, addend);
                    if (value && late_imports_ != nullptr) {
                        (void)late_imports_->bind_patch(*patch_address, *value);
                    }
                } else {
                    unresolved = true;
                    const bool callable = symbol.type() == loader::kSymbolTypeFunction ||
                                          symbol.type() == loader::kSymbolTypeNoType;
                    if (callable && key && late_imports_ != nullptr) {
                        const auto thunk =
                            late_imports_->get_or_create(*patch_address, *key, symbol.name);
                        if (thunk) {
                            value = *thunk;
                            thunk_address = *thunk;
                            ++report.late_thunks;
                        }
                    }
                }
                break;
            }
            default:
                throw std::runtime_error("unsupported x86-64 relocation type");
            }

            if (unresolved) {
                report.unresolved.push_back(UnresolvedRelocation{
                    .patch_address = *patch_address,
                    .type = relocation.type(),
                    .symbol_index = relocation.symbol_index(),
                    .symbol_name = symbol_name,
                    .weak = weak,
                    .plt = plt,
                    .thunk_address = thunk_address,
                });
            }
            if (!value) {
                continue;
            }
            if (!patch_u64(memory_, *patch_address, *value)) {
                throw std::runtime_error("relocation target is outside mapped guest memory");
            }
            ++report.applied;
        }
    };

    relocate_table(dynamic.relocations, false);
    relocate_table(dynamic.plt_relocations, true);
    return report;
}

} // namespace nyxora::runtime
