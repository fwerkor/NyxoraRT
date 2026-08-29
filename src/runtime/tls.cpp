#include "nyxora/runtime/tls.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

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
    return true;
}

const TlsModuleImage* TlsRegistry::find(std::uint32_t module_id) const noexcept {
    const auto it = std::find_if(modules_.begin(), modules_.end(), [module_id](const auto& module) {
        return module.module_id == module_id;
    });
    return it == modules_.end() ? nullptr : &*it;
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

} // namespace nyxora::runtime
