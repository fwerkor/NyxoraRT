#include "nyxora/runtime/tls.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>

#if defined(__linux__) && defined(__x86_64__)
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nyxora::runtime {
namespace {

thread_local GuestThreadContext* current_context = nullptr;

bool valid_alignment(std::size_t alignment) noexcept {
    return alignment != 0 && (alignment & (alignment - 1U)) == 0;
}

std::optional<std::size_t> aligned_offset(const void* storage, std::size_t alignment) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(storage);
    if (base > std::numeric_limits<std::uintptr_t>::max() - (alignment - 1U)) {
        return std::nullopt;
    }
    const auto aligned = (base + alignment - 1U) & ~(static_cast<std::uintptr_t>(alignment) - 1U);
    return static_cast<std::size_t>(aligned - base);
}

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
// Windows x64 exposes the first 64 Win32 TLS slots inline in the TEB at GS:[0x1480].
// Only those slots can replace a guest FS:[0] access in-place without a trampoline.
constexpr DWORD kDirectTebTlsSlotCount = 64;
constexpr std::uint32_t kTebTlsSlotsOffset = 0x1480;
static_assert(sizeof(void*) == 8);
std::once_flag windows_tcb_slot_once;
DWORD windows_tcb_slot = TLS_OUT_OF_INDEXES;

DWORD get_windows_tcb_slot() noexcept {
    std::call_once(windows_tcb_slot_once, [] { windows_tcb_slot = ::TlsAlloc(); });
    return windows_tcb_slot;
}
#endif

#if defined(__linux__) && defined(__x86_64__)
bool get_gs_base(std::uintptr_t& base) noexcept {
    unsigned long value = 0;
    if (::syscall(SYS_arch_prctl, ARCH_GET_GS, &value) != 0) {
        return false;
    }
    base = static_cast<std::uintptr_t>(value);
    return true;
}

bool set_gs_base(std::uintptr_t base) noexcept {
    return ::syscall(SYS_arch_prctl, ARCH_SET_GS, static_cast<unsigned long>(base)) == 0;
}
#endif

} // namespace

bool TlsRegistry::register_module(std::uint32_t module_id, std::size_t alignment,
                                  std::size_t memory_size,
                                  std::span<const std::byte> initial_image) {
    if (module_id == 0 || memory_size == 0 || initial_image.size() > memory_size) {
        return false;
    }
    if (alignment == 0) {
        alignment = 1;
    }
    if (!valid_alignment(alignment) || find(module_id) != nullptr) {
        return false;
    }

    TlsModuleImage module;
    module.module_id = module_id;
    module.alignment = alignment;
    module.memory_size = memory_size;
    module.initial_image.assign(initial_image.begin(), initial_image.end());
    modules_.push_back(std::move(module));
    max_module_id_ = std::max(max_module_id_, static_cast<std::size_t>(module_id));
    ++generation_;
    return true;
}

const TlsModuleImage* TlsRegistry::find(std::uint32_t module_id) const noexcept {
    const auto it = std::find_if(modules_.begin(), modules_.end(), [module_id](const auto& module) {
        return module.module_id == module_id;
    });
    return it == modules_.end() ? nullptr : &*it;
}

GuestThreadContext::GuestThreadContext(GuestThreadContext&& other) noexcept
    : blocks_(std::move(other.blocks_)), dtv_(std::move(other.dtv_)), tcb_(other.tcb_) {
    rebind_tcb();
}

GuestThreadContext& GuestThreadContext::operator=(GuestThreadContext&& other) noexcept {
    if (this != &other) {
        blocks_ = std::move(other.blocks_);
        dtv_ = std::move(other.dtv_);
        tcb_ = other.tcb_;
        rebind_tcb();
    }
    return *this;
}

std::optional<GuestThreadContext> GuestThreadContext::create(const TlsRegistry& registry) {
    GuestThreadContext context;
    if (!context.synchronize(registry)) {
        return std::nullopt;
    }
    return context;
}

bool GuestThreadContext::synchronize(const TlsRegistry& registry) {
    for (const auto& module : registry.modules()) {
        if (find_block(module.module_id) == nullptr && !add_module(module)) {
            return false;
        }
    }
    rebuild_dtv(registry);
    return true;
}

bool GuestThreadContext::add_module(const TlsModuleImage& module) {
    if (module.memory_size > std::numeric_limits<std::size_t>::max() - (module.alignment - 1U)) {
        return false;
    }

    Block block;
    block.module_id = module.module_id;
    block.memory_size = module.memory_size;
    block.storage.resize(module.memory_size + module.alignment - 1U);
    const auto offset = aligned_offset(block.storage.data(), module.alignment);
    if (!offset || *offset > block.storage.size() ||
        module.memory_size > block.storage.size() - *offset) {
        return false;
    }
    block.aligned_offset = *offset;
    if (!module.initial_image.empty()) {
        std::memcpy(block.storage.data() + block.aligned_offset, module.initial_image.data(),
                    module.initial_image.size());
    }
    blocks_.push_back(std::move(block));
    return true;
}

void GuestThreadContext::rebuild_dtv(const TlsRegistry& registry) {
    dtv_.assign(registry.max_module_id() + 2U, GuestDtvEntry{});
    dtv_[0].counter = registry.generation();
    dtv_[1].counter = registry.max_module_id();
    for (auto& block : blocks_) {
        if (block.module_id < dtv_.size() - 1U) {
            dtv_[block.module_id + 1U].pointer = block.storage.data() + block.aligned_offset;
        }
    }

    rebind_tcb();
}

void GuestThreadContext::rebind_tcb() noexcept {
    tcb_.self = &tcb_;
    tcb_.dtv = dtv_.empty() ? nullptr : dtv_.data();
    tcb_.canary = 0x9e3779b97f4a7c15ULL ^ reinterpret_cast<std::uintptr_t>(&tcb_);
}

GuestThreadContext::Block* GuestThreadContext::find_block(std::uint32_t module_id) noexcept {
    const auto it = std::find_if(blocks_.begin(), blocks_.end(), [module_id](const auto& block) {
        return block.module_id == module_id;
    });
    return it == blocks_.end() ? nullptr : &*it;
}

const GuestThreadContext::Block* GuestThreadContext::find_block(std::uint32_t module_id) const noexcept {
    const auto it = std::find_if(blocks_.begin(), blocks_.end(), [module_id](const auto& block) {
        return block.module_id == module_id;
    });
    return it == blocks_.end() ? nullptr : &*it;
}

void* GuestThreadContext::tls_address(std::uint32_t module_id, std::size_t offset) noexcept {
    auto* block = find_block(module_id);
    if (block == nullptr || offset >= block->memory_size) {
        return nullptr;
    }
    return block->storage.data() + block->aligned_offset + offset;
}

const void* GuestThreadContext::tls_address(std::uint32_t module_id, std::size_t offset) const noexcept {
    const auto* block = find_block(module_id);
    if (block == nullptr || offset >= block->memory_size) {
        return nullptr;
    }
    return block->storage.data() + block->aligned_offset + offset;
}

ScopedGuestThreadContext::ScopedGuestThreadContext(GuestThreadContext& context) noexcept
    : previous_(current_context) {
    current_context = &context;
}

ScopedGuestThreadContext::~ScopedGuestThreadContext() {
    current_context = previous_;
}

GuestThreadContext* ScopedGuestThreadContext::current() noexcept {
    return current_context;
}

std::optional<std::uint32_t> windows_guest_tcb_teb_offset() noexcept {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const auto slot = get_windows_tcb_slot();
    if (slot == TLS_OUT_OF_INDEXES || slot >= kDirectTebTlsSlotCount) {
        return std::nullopt;
    }
    return kTebTlsSlotsOffset + slot * static_cast<std::uint32_t>(sizeof(void*));
#else
    return std::nullopt;
#endif
}

bool ScopedGuestSegment::supported() noexcept {
#if defined(__linux__) && defined(__x86_64__)
    return true;
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return windows_guest_tcb_teb_offset().has_value();
#else
    return false;
#endif
}

std::optional<std::uintptr_t> ScopedGuestSegment::current_base() noexcept {
#if defined(__linux__) && defined(__x86_64__)
    std::uintptr_t base = 0;
    if (!get_gs_base(base)) {
        return std::nullopt;
    }
    return base;
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const auto slot = get_windows_tcb_slot();
    if (slot == TLS_OUT_OF_INDEXES || slot >= kDirectTebTlsSlotCount) {
        return std::nullopt;
    }
    return reinterpret_cast<std::uintptr_t>(::TlsGetValue(slot));
#else
    return std::nullopt;
#endif
}

ScopedGuestSegment::ScopedGuestSegment(GuestThreadContext& context) {
#if defined(__linux__) && defined(__x86_64__)
    if (!get_gs_base(previous_base_) ||
        !set_gs_base(reinterpret_cast<std::uintptr_t>(context.tcb()))) {
        throw std::runtime_error("unable to bind guest TCB to GS base");
    }
    active_ = true;
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const auto slot = get_windows_tcb_slot();
    if (slot == TLS_OUT_OF_INDEXES || slot >= kDirectTebTlsSlotCount) {
        throw std::runtime_error("unable to allocate an inline Windows guest TCB TLS slot");
    }
    previous_windows_tcb_ = ::TlsGetValue(slot);
    if (::TlsSetValue(slot, context.tcb()) == 0) {
        throw std::runtime_error("unable to bind guest TCB to the Windows TLS slot");
    }
    active_ = true;
#else
    (void)context;
#endif
}

ScopedGuestSegment::~ScopedGuestSegment() {
#if defined(__linux__) && defined(__x86_64__)
    if (active_ && !set_gs_base(previous_base_)) {
        std::terminate();
    }
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (active_ && ::TlsSetValue(get_windows_tcb_slot(), previous_windows_tcb_) == 0) {
        std::terminate();
    }
#endif
}

} // namespace nyxora::runtime
