#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyxora/base/types.hpp"
#include "nyxora/gpu/backend.hpp"
#include "nyxora/loader/dynamic.hpp"
#include "nyxora/loader/elf64.hpp"
#include "nyxora/memory/guest_address_space.hpp"
#include "nyxora/runtime/linker.hpp"
#include "nyxora/runtime/fault.hpp"
#include "nyxora/runtime/guest_thread.hpp"
#include "nyxora/runtime/late_imports.hpp"
#include "nyxora/runtime/kernel_services.hpp"
#include "nyxora/runtime/native_thread.hpp"
#include "nyxora/runtime/hle_registry.hpp"
#include "nyxora/runtime/symbol_registry.hpp"
#include "nyxora/runtime/tls.hpp"
#include "nyxora/runtime/thread_manager.hpp"

namespace nyxora::runtime {

struct LoadedModule {
    std::filesystem::path path;
    GuestAddress base{};
    GuestAddress entry{};
    std::vector<memory::RegionInfo> segments;
    std::optional<loader::DynamicInfo> dynamic;
    std::optional<loader::TlsSegment> tls;
    std::uint32_t tls_module_id{};
    RelocationReport relocations;
};

class Runtime {
public:
    explicit Runtime(std::unique_ptr<gpu::Backend> gpu_backend);
    Runtime(std::unique_ptr<gpu::Backend> gpu_backend, memory::GuestAddressSpace memory);

    LoadedModule load_elf(const std::filesystem::path& path, GuestAddress base = 0);
    LoadedModule load_image(const loader::Elf64Image& image, std::filesystem::path path,
                            GuestAddress base = 0);
    RelocationReport relink(LoadedModule& module);
    [[nodiscard]] std::uint64_t invoke_entry(const LoadedModule& module,
                                             GuestSize stack_size = 1024 * 1024,
                                             std::uint64_t arg0 = 0,
                                             std::uint64_t arg1 = 0,
                                             std::uint64_t arg2 = 0);
    [[nodiscard]] GuestThread start_thread(const LoadedModule& module,
                                           GuestSize stack_size = 1024 * 1024,
                                           std::uint64_t arg0 = 0,
                                           std::uint64_t arg1 = 0,
                                           std::uint64_t arg2 = 0);

    [[nodiscard]] memory::GuestAddressSpace& memory() noexcept { return memory_; }
    [[nodiscard]] const memory::GuestAddressSpace& memory() const noexcept { return memory_; }
    [[nodiscard]] SymbolRegistry& symbols() noexcept { return symbols_; }
    [[nodiscard]] HleRegistry& hle() noexcept { return *hle_; }
    [[nodiscard]] const HleRegistry& hle() const noexcept { return *hle_; }
    [[nodiscard]] const TlsRegistry& tls_registry() const noexcept { return tls_registry_; }
    [[nodiscard]] GuestThreadManager& thread_manager() noexcept { return thread_manager_; }
    [[nodiscard]] const GuestThreadManager& thread_manager() const noexcept { return thread_manager_; }
    [[nodiscard]] LateImportTable* late_imports() noexcept {
        return late_imports_ ? &*late_imports_ : nullptr;
    }
    [[nodiscard]] const LateImportTable* late_imports() const noexcept {
        return late_imports_ ? &*late_imports_ : nullptr;
    }
    [[nodiscard]] std::optional<GuestThreadContext> create_thread_context() const {
        return GuestThreadContext::create(tls_registry_);
    }
    [[nodiscard]] gpu::Backend& gpu() noexcept { return *gpu_; }
    [[nodiscard]] bool set_guest_root(const std::filesystem::path& root) {
        return kernel_services_.set_guest_root(root);
    }
    [[nodiscard]] bool set_flexible_memory_size(GuestSize size) {
        return kernel_services_.set_flexible_memory_size(size);
    }

private:
    memory::GuestAddressSpace memory_;
    SymbolRegistry symbols_;
    TlsRegistry tls_registry_;
    KernelServices kernel_services_;
    GuestThreadManager thread_manager_;
    std::optional<LateImportTable> late_imports_;
    std::unique_ptr<HleRegistry> hle_;
    std::unique_ptr<gpu::Backend> gpu_;
    std::uint32_t next_tls_module_id_{1};
};

} // namespace nyxora::runtime
