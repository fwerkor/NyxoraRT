#include "nyxora/runtime/hle_registry.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nyxora::runtime {

class HleRegistry::NoArgBridgeTable {
public:
    static std::unique_ptr<NoArgBridgeTable> create(std::size_t capacity) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        if (capacity == 0) {
            return nullptr;
        }
        constexpr std::size_t thunk_size = 32;
        constexpr std::size_t data_size = sizeof(std::uint64_t);
        const auto page = static_cast<GuestSize>(memory::NativeArena::page_size());
        if (capacity > std::numeric_limits<GuestSize>::max() / thunk_size ||
            capacity > std::numeric_limits<GuestSize>::max() / data_size) {
            return nullptr;
        }
        const auto code_size = checked_align_up(static_cast<GuestSize>(capacity * thunk_size), page);
        const auto slot_region_size = checked_align_up(static_cast<GuestSize>(capacity * data_size), page);
        if (!code_size || !slot_region_size ||
            *code_size > std::numeric_limits<GuestSize>::max() - *slot_region_size) {
            return nullptr;
        }
        auto arena = memory::NativeArena::reserve(*code_size + *slot_region_size);
        if (!arena ||
            !arena->protect(0, *code_size,
                            memory::Protection::read | memory::Protection::write) ||
            !arena->protect(*code_size, *slot_region_size,
                            memory::Protection::read | memory::Protection::write)) {
            return nullptr;
        }
        auto result = std::unique_ptr<NoArgBridgeTable>(
            new NoArgBridgeTable(std::move(*arena), capacity, *code_size));
        if (!result->emit_all()) {
            return nullptr;
        }
        return result;
#else
        (void)capacity;
        return nullptr;
#endif
    }

    std::optional<GuestAddress> add(NoArgHleFunction function) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        if (function == nullptr || used_ >= capacity_) {
            return std::nullopt;
        }
        auto* slot = reinterpret_cast<std::uint64_t*>(arena_.base() + data_offset_ +
                                                      used_ * sizeof(std::uint64_t));
        *slot = reinterpret_cast<std::uint64_t>(function);
        const auto address = arena_.base() + used_ * kThunkSize;
        ++used_;
        return address;
#else
        (void)function;
        return std::nullopt;
#endif
    }

private:
    static constexpr std::size_t kThunkSize = 32;

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    NoArgBridgeTable(memory::NativeArena arena, std::size_t capacity, GuestSize data_offset)
        : arena_(std::move(arena)), capacity_(capacity), data_offset_(data_offset) {}
#endif

    bool emit_all() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        for (std::size_t index = 0; index < capacity_; ++index) {
            const auto code_address = arena_.base() + index * kThunkSize;
            const auto slot_address = arena_.base() + data_offset_ + index * sizeof(std::uint64_t);
            const auto displacement = static_cast<std::int64_t>(slot_address) -
                                      static_cast<std::int64_t>(code_address + 11U);
            if (displacement < std::numeric_limits<std::int32_t>::min() ||
                displacement > std::numeric_limits<std::int32_t>::max()) {
                return false;
            }
            const auto rel = static_cast<std::int32_t>(displacement);
            std::array<std::byte, kThunkSize> code{};
            code.fill(std::byte{0x90});
            const std::byte prefix[] = {
                std::byte{0x48}, std::byte{0x83}, std::byte{0xec}, std::byte{0x28}, // sub rsp,0x28
                std::byte{0x48}, std::byte{0x8b}, std::byte{0x05},                 // mov rax,[rip+rel32]
            };
            std::memcpy(code.data(), prefix, sizeof(prefix));
            std::memcpy(code.data() + 7, &rel, sizeof(rel));
            const std::byte suffix[] = {
                std::byte{0xff}, std::byte{0xd0},                                 // call rax
                std::byte{0x48}, std::byte{0x83}, std::byte{0xc4}, std::byte{0x28}, // add rsp,0x28
                std::byte{0xc3},                                                   // ret
            };
            std::memcpy(code.data() + 11, suffix, sizeof(suffix));
            if (!arena_.copy(index * kThunkSize, code)) {
                return false;
            }
        }
        const auto code_bytes = capacity_ * kThunkSize;
        return arena_.flush_instruction_cache(0, code_bytes) &&
               arena_.protect(0, data_offset_,
                              memory::Protection::read | memory::Protection::execute);
#else
        return false;
#endif
    }

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    memory::NativeArena arena_;
    std::size_t capacity_{};
    std::size_t used_{};
    GuestSize data_offset_{};
#endif
};

HleRegistry::~HleRegistry() = default;

HleRegistry::HleRegistry(SymbolRegistry& symbols) : symbols_(symbols) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    bridges_ = NoArgBridgeTable::create(64);
#endif
}

bool HleRegistry::register_no_arg(SymbolKey key, NoArgHleFunction function,
                                  std::string debug_name) {
    if (function == nullptr || key.kind != SymbolKind::function) {
        return false;
    }

    GuestAddress address{};
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (!bridges_) {
        return false;
    }
    const auto bridge = bridges_->add(function);
    if (!bridge) {
        return false;
    }
    address = *bridge;
#elif defined(__x86_64__)
    address = reinterpret_cast<GuestAddress>(function);
#else
    return false;
#endif

    if (!symbols_.register_symbol(std::move(key),
                                  SymbolBinding{address, std::move(debug_name), true})) {
        return false;
    }
    ++registered_;
    return true;
}

} // namespace nyxora::runtime
