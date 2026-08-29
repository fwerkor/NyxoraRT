#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "nyxora/base/types.hpp"
#include "nyxora/gpu/backend.hpp"
#include "nyxora/loader/elf64.hpp"
#include "nyxora/memory/guest_address_space.hpp"
#include "nyxora/runtime/symbol_registry.hpp"

namespace nyxora::runtime {

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

} // namespace nyxora::runtime
