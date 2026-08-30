#include "nyxora/runtime/hle_registry.hpp"
#include "nyxora/runtime/tls.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace nyxora::runtime {

class HleRegistry::BridgeTable {
public:
    static std::unique_ptr<BridgeTable> create(std::size_t capacity) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        if (capacity == 0) {
            return nullptr;
        }
        constexpr std::size_t data_size = sizeof(std::uint64_t);
        const auto page = static_cast<GuestSize>(memory::NativeArena::page_size());
        if (capacity > std::numeric_limits<GuestSize>::max() / kThunkSize ||
            capacity > std::numeric_limits<GuestSize>::max() / data_size) {
            return nullptr;
        }
        const auto code_size = checked_align_up(static_cast<GuestSize>(capacity * kThunkSize), page);
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
        auto result = std::unique_ptr<BridgeTable>(
            new BridgeTable(std::move(*arena), capacity, *code_size));
        return result->emit_all() ? std::move(result) : nullptr;
#else
        (void)capacity;
        return nullptr;
#endif
    }

    std::optional<GuestAddress> add(GuestAddress function) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        if (function == 0 || used_ >= capacity_) {
            return std::nullopt;
        }
        auto* slot = reinterpret_cast<std::uint64_t*>(arena_.base() + data_offset_ +
                                                      used_ * sizeof(std::uint64_t));
        *slot = function;
        const auto address = arena_.base() + used_ * kThunkSize;
        ++used_;
        return address;
#else
        (void)function;
        return std::nullopt;
#endif
    }

private:
    static constexpr std::size_t kThunkSize = 128;

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    BridgeTable(memory::NativeArena arena, std::size_t capacity, GuestSize data_offset)
        : arena_(std::move(arena)), capacity_(capacity), data_offset_(data_offset) {}
#endif

    bool emit_all() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        const auto host_stack_teb_offset = windows_host_stack_teb_offset();
        if (!host_stack_teb_offset) {
            return false;
        }
        for (std::size_t index = 0; index < capacity_; ++index) {
            const auto code_address = arena_.base() + index * kThunkSize;
            const auto slot_address = arena_.base() + data_offset_ + index * sizeof(std::uint64_t);
            std::array<std::byte, kThunkSize> code{};
            code.fill(std::byte{0x90});
            std::size_t offset = 0;
            auto append = [&](std::span<const std::byte> bytes) {
                if (bytes.size() > code.size() - offset) {
                    return false;
                }
                std::memcpy(code.data() + offset, bytes.data(), bytes.size());
                offset += bytes.size();
                return true;
            };
            auto append_u32 = [&](std::uint32_t value) {
                const auto bytes = std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(&value), sizeof(value));
                return append(bytes);
            };

            const std::byte save_guest_stack[] = {
                std::byte{0x41}, std::byte{0x54},                                           // push r12
                std::byte{0x41}, std::byte{0x55},                                           // push r13
                std::byte{0x41}, std::byte{0x56},                                           // push r14
                std::byte{0x4c}, std::byte{0x8d}, std::byte{0x64}, std::byte{0x24},
                std::byte{0x18},                                                           // lea r12,[rsp+0x18]
                std::byte{0x4d}, std::byte{0x89}, std::byte{0xc5},                         // mov r13,r8
                std::byte{0x4d}, std::byte{0x89}, std::byte{0xce},                         // mov r14,r9
                std::byte{0x49}, std::byte{0x89}, std::byte{0xd2},                         // mov r10,rdx
                std::byte{0x49}, std::byte{0x89}, std::byte{0xcb},                         // mov r11,rcx
                std::byte{0x48}, std::byte{0x89}, std::byte{0xf9},                         // mov rcx,rdi
                std::byte{0x48}, std::byte{0x89}, std::byte{0xf2},                         // mov rdx,rsi
                std::byte{0x4d}, std::byte{0x89}, std::byte{0xd0},                         // mov r8,r10
                std::byte{0x4d}, std::byte{0x89}, std::byte{0xd9},                         // mov r9,r11
                std::byte{0x4d}, std::byte{0x8b}, std::byte{0x54}, std::byte{0x24},
                std::byte{0x08},                                                           // mov r10,[r12+8]
                std::byte{0x65}, std::byte{0x4c}, std::byte{0x8b}, std::byte{0x1c},
                std::byte{0x25},                                                           // mov r11,gs:[disp32]
            };
            if (!append(save_guest_stack) || !append_u32(*host_stack_teb_offset)) {
                return false;
            }
            const std::byte switch_host_stack[] = {
                std::byte{0x4c}, std::byte{0x89}, std::byte{0xdc},                         // mov rsp,r11
                std::byte{0x48}, std::byte{0x83}, std::byte{0xec}, std::byte{0x38},       // shadow + args 4-6
                std::byte{0x4c}, std::byte{0x89}, std::byte{0x6c}, std::byte{0x24},
                std::byte{0x20},                                                           // mov [rsp+0x20],r13
                std::byte{0x4c}, std::byte{0x89}, std::byte{0x74}, std::byte{0x24},
                std::byte{0x28},                                                           // mov [rsp+0x28],r14
                std::byte{0x4c}, std::byte{0x89}, std::byte{0x54}, std::byte{0x24},
                std::byte{0x30},                                                           // mov [rsp+0x30],r10
                std::byte{0x48}, std::byte{0x8b}, std::byte{0x05},                         // mov rax,[rip+rel32]
            };
            if (!append(switch_host_stack)) {
                return false;
            }
            const auto rip_after_target_load = code_address + offset + sizeof(std::int32_t);
            const auto displacement = static_cast<std::int64_t>(slot_address) -
                                      static_cast<std::int64_t>(rip_after_target_load);
            if (displacement < std::numeric_limits<std::int32_t>::min() ||
                displacement > std::numeric_limits<std::int32_t>::max()) {
                return false;
            }
            const auto rel = static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
            if (!append_u32(rel)) {
                return false;
            }
            const std::byte finish[] = {
                std::byte{0xff}, std::byte{0xd0},                                           // call rax
                std::byte{0x4c}, std::byte{0x89}, std::byte{0xe4},                         // mov rsp,r12
                std::byte{0x48}, std::byte{0x83}, std::byte{0xec}, std::byte{0x18},       // reach saved regs
                std::byte{0x41}, std::byte{0x5e},                                           // pop r14
                std::byte{0x41}, std::byte{0x5d},                                           // pop r13
                std::byte{0x41}, std::byte{0x5c},                                           // pop r12
                std::byte{0xc3},                                                             // ret
            };
            if (!append(finish) || !arena_.copy(index * kThunkSize, code)) {
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
    auto bridge = BridgeTable::create(kBridgeChunkCapacity);
    if (bridge) {
        bridges_.push_back(std::move(bridge));
    }
#endif
}

bool HleRegistry::register_function(SymbolKey key, GuestAddress function, std::string debug_name) {
    if (function == 0 || key.kind != SymbolKind::function) {
        return false;
    }

    GuestAddress address{};
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (bridges_.empty()) {
        return false;
    }
    auto bridge = bridges_.back()->add(function);
    if (!bridge) {
        auto next = BridgeTable::create(kBridgeChunkCapacity);
        if (!next) {
            return false;
        }
        bridge = next->add(function);
        if (!bridge) {
            return false;
        }
        bridges_.push_back(std::move(next));
    }
    address = *bridge;
#elif defined(__x86_64__)
    address = function;
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

bool HleRegistry::register_no_arg(SymbolKey key, NoArgHleFunction function,
                                  std::string debug_name) {
    return function != nullptr &&
           register_function(std::move(key), reinterpret_cast<GuestAddress>(function),
                             std::move(debug_name));
}

} // namespace nyxora::runtime
