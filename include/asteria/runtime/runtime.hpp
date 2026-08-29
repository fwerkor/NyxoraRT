#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "asteria/base/types.hpp"
#include "asteria/gpu/backend.hpp"
#include "asteria/loader/elf64.hpp"
#include "asteria/memory/guest_address_space.hpp"
#include "asteria/runtime/symbol_registry.hpp"

namespace asteria::runtime {

struct LoadedModule {
    std::filesystem::path path;
    GuestAddress base{};
    GuestAddress entry{};
    std::vector<memory::RegionInfo> segments;
};

class Runtime {
public:
    explicit Runtime(std::unique_ptr<gpu::Backend> gpu_backend);

    LoadedModule load_elf(const std::filesystem::path& path, GuestAddress base = 0);

    [[nodiscard]] memory::GuestAddressSpace& memory() noexcept { return memory_; }
    [[nodiscard]] SymbolRegistry& symbols() noexcept { return symbols_; }
    [[nodiscard]] gpu::Backend& gpu() noexcept { return *gpu_; }

private:
    memory::GuestAddressSpace memory_;
    SymbolRegistry symbols_;
    std::unique_ptr<gpu::Backend> gpu_;
};

} // namespace asteria::runtime
