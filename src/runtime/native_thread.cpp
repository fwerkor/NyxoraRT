#include "nyxora/runtime/native_thread.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace nyxora::runtime {
namespace {


#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
void emit_u32(std::vector<std::byte>& code, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    code.insert(code.end(), bytes.begin(), bytes.end());
}

void emit_movdqu_rsp(std::vector<std::byte>& code, bool load, unsigned xmm, std::uint32_t offset) {
    code.push_back(std::byte{0xf3});
    if (xmm >= 8) {
        code.push_back(std::byte{0x44});
    }
    code.push_back(std::byte{0x0f});
    code.push_back(load ? std::byte{0x6f} : std::byte{0x7f});
    const auto reg = static_cast<std::uint8_t>(xmm & 7U);
    code.push_back(static_cast<std::byte>(0x84U | (reg << 3U)));
    code.push_back(std::byte{0x24});
    emit_u32(code, offset);
}
#endif

std::vector<std::byte> build_entry_trampoline() {
    std::vector<std::byte> code;
#if defined(__x86_64__) || defined(_M_X64)
#if defined(_WIN32)
    // Host ABI: Microsoft x64. Guest ABI: SysV x86-64.
    // Save the fifth host argument before moving to the guest stack.
    const std::byte prefix[] = {
        std::byte{0x4c}, std::byte{0x8b}, std::byte{0x54}, std::byte{0x24}, std::byte{0x28}, // mov r10,[rsp+0x28]
        std::byte{0x55},                                                                   // push rbp
        std::byte{0x53},                                                                   // push rbx
        std::byte{0x57},                                                                   // push rdi
        std::byte{0x56},                                                                   // push rsi
        std::byte{0x41}, std::byte{0x54},                                                 // push r12
        std::byte{0x41}, std::byte{0x55},                                                 // push r13
        std::byte{0x41}, std::byte{0x56},                                                 // push r14
        std::byte{0x41}, std::byte{0x57},                                                 // push r15
        std::byte{0x48}, std::byte{0x81}, std::byte{0xec}, std::byte{0xa0}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00},                                                 // sub rsp,0xa0
    };
    code.insert(code.end(), std::begin(prefix), std::end(prefix));
    for (unsigned xmm = 6; xmm <= 15; ++xmm) {
        emit_movdqu_rsp(code, false, xmm, static_cast<std::uint32_t>((xmm - 6U) * 16U));
    }
    const std::byte switch_stack[] = {
        std::byte{0x49}, std::byte{0x89}, std::byte{0xe4},                               // mov r12,rsp
        std::byte{0x49}, std::byte{0x89}, std::byte{0xcb},                               // mov r11,rcx
        std::byte{0x49}, std::byte{0x89}, std::byte{0xd6},                               // mov r14,rdx
        std::byte{0x4c}, std::byte{0x89}, std::byte{0xc7},                               // mov rdi,r8
        std::byte{0x4c}, std::byte{0x89}, std::byte{0xce},                               // mov rsi,r9
        std::byte{0x4c}, std::byte{0x89}, std::byte{0xd2},                               // mov rdx,r10
        std::byte{0x4c}, std::byte{0x89}, std::byte{0xf4},                               // mov rsp,r14
        std::byte{0x48}, std::byte{0x83}, std::byte{0xe4}, std::byte{0xf0},              // and rsp,-16
        std::byte{0x48}, std::byte{0x83}, std::byte{0xec}, std::byte{0x10},              // sub rsp,16
        std::byte{0x48}, std::byte{0xc7}, std::byte{0x04}, std::byte{0x24}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},                               // mov qword [rsp],0
        std::byte{0x48}, std::byte{0xc7}, std::byte{0x44}, std::byte{0x24}, std::byte{0x08},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},              // mov qword [rsp+8],0
        std::byte{0x48}, std::byte{0x89}, std::byte{0xe5},                               // mov rbp,rsp
        std::byte{0x41}, std::byte{0xff}, std::byte{0xd3},                               // call r11
        std::byte{0x4c}, std::byte{0x89}, std::byte{0xe4},                               // mov rsp,r12
    };
    code.insert(code.end(), std::begin(switch_stack), std::end(switch_stack));
    for (unsigned xmm = 6; xmm <= 15; ++xmm) {
        emit_movdqu_rsp(code, true, xmm, static_cast<std::uint32_t>((xmm - 6U) * 16U));
    }
    const std::byte suffix[] = {
        std::byte{0x48}, std::byte{0x81}, std::byte{0xc4}, std::byte{0xa0}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00},                                                 // add rsp,0xa0
        std::byte{0x41}, std::byte{0x5f},                                                 // pop r15
        std::byte{0x41}, std::byte{0x5e},                                                 // pop r14
        std::byte{0x41}, std::byte{0x5d},                                                 // pop r13
        std::byte{0x41}, std::byte{0x5c},                                                 // pop r12
        std::byte{0x5e},                                                                   // pop rsi
        std::byte{0x5f},                                                                   // pop rdi
        std::byte{0x5b},                                                                   // pop rbx
        std::byte{0x5d},                                                                   // pop rbp
        std::byte{0xc3},                                                                   // ret
    };
    code.insert(code.end(), std::begin(suffix), std::end(suffix));
#else
    // Host and guest both use the SysV x86-64 ABI.
    const std::byte bytes[] = {
        std::byte{0x55},                                                                   // push rbp
        std::byte{0x53},                                                                   // push rbx
        std::byte{0x41}, std::byte{0x54},                                                 // push r12
        std::byte{0x41}, std::byte{0x55},                                                 // push r13
        std::byte{0x41}, std::byte{0x56},                                                 // push r14
        std::byte{0x41}, std::byte{0x57},                                                 // push r15
        std::byte{0x49}, std::byte{0x89}, std::byte{0xfb},                               // mov r11,rdi
        std::byte{0x49}, std::byte{0x89}, std::byte{0xf6},                               // mov r14,rsi
        std::byte{0x48}, std::byte{0x89}, std::byte{0xd7},                               // mov rdi,rdx
        std::byte{0x48}, std::byte{0x89}, std::byte{0xce},                               // mov rsi,rcx
        std::byte{0x4c}, std::byte{0x89}, std::byte{0xc2},                               // mov rdx,r8
        std::byte{0x49}, std::byte{0x89}, std::byte{0xe4},                               // mov r12,rsp
        std::byte{0x4c}, std::byte{0x89}, std::byte{0xf4},                               // mov rsp,r14
        std::byte{0x48}, std::byte{0x83}, std::byte{0xe4}, std::byte{0xf0},              // and rsp,-16
        std::byte{0x48}, std::byte{0x83}, std::byte{0xec}, std::byte{0x10},              // sub rsp,16
        std::byte{0x48}, std::byte{0xc7}, std::byte{0x04}, std::byte{0x24}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},                               // mov qword [rsp],0
        std::byte{0x48}, std::byte{0xc7}, std::byte{0x44}, std::byte{0x24}, std::byte{0x08},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},              // mov qword [rsp+8],0
        std::byte{0x48}, std::byte{0x89}, std::byte{0xe5},                               // mov rbp,rsp
        std::byte{0x41}, std::byte{0xff}, std::byte{0xd3},                               // call r11
        std::byte{0x4c}, std::byte{0x89}, std::byte{0xe4},                               // mov rsp,r12
        std::byte{0x41}, std::byte{0x5f},                                                // pop r15
        std::byte{0x41}, std::byte{0x5e},                                                // pop r14
        std::byte{0x41}, std::byte{0x5d},                                                // pop r13
        std::byte{0x41}, std::byte{0x5c},                                                // pop r12
        std::byte{0x5b},                                                                  // pop rbx
        std::byte{0x5d},                                                                  // pop rbp
        std::byte{0xc3},                                                                  // ret
    };
    code.assign(std::begin(bytes), std::end(bytes));
#endif
#endif
    return code;
}

} // namespace

std::optional<GuestStack> GuestStack::create(GuestSize usable_size, std::size_t guard_pages) {
    const auto page = static_cast<GuestSize>(memory::NativeArena::page_size());
    const auto rounded_usable = checked_align_up(usable_size, page);
    if (!rounded_usable || guard_pages > std::numeric_limits<GuestSize>::max() / page) {
        return std::nullopt;
    }
    const auto guard_size = static_cast<GuestSize>(guard_pages) * page;
    if (guard_size > std::numeric_limits<GuestSize>::max() - *rounded_usable) {
        return std::nullopt;
    }
    auto arena = memory::NativeArena::reserve(guard_size + *rounded_usable);
    if (!arena || !arena->protect(guard_size, *rounded_usable,
                                  memory::Protection::read | memory::Protection::write)) {
        return std::nullopt;
    }
    const auto base = arena->base() + guard_size;
    return GuestStack(std::move(*arena), base, *rounded_usable);
}

bool EntryTrampoline::supported() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return true;
#else
    return false;
#endif
}

std::optional<EntryTrampoline> EntryTrampoline::create() {
    if (!supported()) {
        return std::nullopt;
    }
    const auto code = build_entry_trampoline();
    if (code.empty()) {
        return std::nullopt;
    }
    const auto page = memory::NativeArena::page_size();
    auto arena = memory::NativeArena::reserve(page);
    if (!arena || !arena->protect(0, page,
                                  memory::Protection::read | memory::Protection::write) ||
        !arena->copy(0, code) || !arena->flush_instruction_cache(0, code.size()) ||
        !arena->protect(0, page, memory::Protection::read | memory::Protection::execute)) {
        return std::nullopt;
    }
    return EntryTrampoline(std::move(*arena));
}

std::uint64_t EntryTrampoline::invoke(GuestAddress entry, GuestAddress stack_top,
                                      std::uint64_t arg0, std::uint64_t arg1,
                                      std::uint64_t arg2) const {
    if (!code_ || entry == 0 || stack_top == 0) {
        throw std::runtime_error("entry trampoline is not initialized");
    }
    using HostThunk = std::uint64_t (*)(GuestAddress, GuestAddress, std::uint64_t, std::uint64_t,
                                        std::uint64_t);
    const auto function = reinterpret_cast<HostThunk>(const_cast<void*>(code_.host_pointer()));
    return function(entry, stack_top, arg0, arg1, arg2);
}

} // namespace nyxora::runtime
