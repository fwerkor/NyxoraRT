#include "nyxora/runtime/runtime.hpp"
#include "nyxora/hle/libkernel.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nyxora::runtime {
namespace {

memory::Protection segment_protection(std::uint32_t flags) {
    memory::Protection protection = memory::Protection::none;
    if ((flags & 0x4U) != 0) {
        protection = protection | memory::Protection::read;
    }
    if ((flags & 0x2U) != 0) {
        protection = protection | memory::Protection::write;
    }
    if ((flags & 0x1U) != 0) {
        protection = protection | memory::Protection::execute;
    }
    return protection;
}

GuestAddress checked_address(GuestAddress base, std::uint64_t offset) {
    if (base > std::numeric_limits<GuestAddress>::max() - offset) {
        throw std::runtime_error("guest module address overflows");
    }
    return base + offset;
}

bool is_loadable_segment(std::uint32_t type) {
    return type == loader::kProgramLoad || type == loader::kProgramSceRelro;
}

} // namespace

Runtime::Runtime(std::unique_ptr<gpu::Backend> gpu_backend)
    : Runtime(std::move(gpu_backend), memory::GuestAddressSpace{}) {}

Runtime::Runtime(std::unique_ptr<gpu::Backend> gpu_backend, memory::GuestAddressSpace memory)
    : memory_(std::move(memory)), late_imports_(LateImportTable::create()),
      gpu_(std::move(gpu_backend)) {
    if (!gpu_) {
        throw std::invalid_argument("Runtime requires a GPU backend");
    }
    hle_ = std::make_unique<HleRegistry>(symbols_);
    hle::libkernel::register_core(*hle_);
}

LoadedModule Runtime::load_elf(const std::filesystem::path& path, GuestAddress base) {
    const auto image = loader::Elf64Image::from_file(path);
    return load_image(image, path, base);
}

LoadedModule Runtime::load_image(const loader::Elf64Image& image, std::filesystem::path path,
                                 GuestAddress base) {
    LoadedModule module{
        .path = std::move(path),
        .base = base,
        .entry = checked_address(base, image.entry()),
        .segments = {},
        .dynamic = std::nullopt,
        .tls = image.tls(),
        .tls_module_id = 0,
        .relocations = {},
    };

    for (const auto& segment : image.program_headers()) {
        if (!is_loadable_segment(segment.type) || segment.memory_size == 0) {
            continue;
        }
        const auto guest_base = checked_address(base, segment.virtual_address);
        const auto final_protection = segment_protection(segment.flags);
        const auto load_protection = memory::has(final_protection, memory::Protection::execute)
                                         ? memory::Protection::read | memory::Protection::write
                                         : final_protection | memory::Protection::write;
        if (!memory_.map(guest_base, segment.memory_size, load_protection,
                         module.path.filename().string())) {
            throw std::runtime_error("unable to map loadable ELF segment");
        }

        if (segment.file_size != 0) {
            const auto bytes = image.bytes().subspan(static_cast<std::size_t>(segment.offset),
                                                     static_cast<std::size_t>(segment.file_size));
            if (!memory_.write(guest_base, bytes)) {
                throw std::runtime_error("unable to copy ELF segment into guest memory");
            }
        }
        if (segment.memory_size > segment.file_size &&
            !memory_.zero(checked_address(guest_base, segment.file_size),
                          segment.memory_size - segment.file_size)) {
            throw std::runtime_error("unable to initialize ELF BSS range");
        }
        if (!memory_.protect(guest_base, segment.memory_size, final_protection)) {
            throw std::runtime_error("unable to apply ELF segment protection");
        }
        module.segments.push_back(*memory_.find(guest_base));
    }

    if (module.tls) {
        if (next_tls_module_id_ == 0) {
            throw std::runtime_error("TLS module identifier space exhausted");
        }
        module.tls_module_id = next_tls_module_id_++;

        const auto tls_header = std::find_if(
            image.program_headers().begin(), image.program_headers().end(),
            [](const loader::ProgramHeader& header) { return header.type == loader::kProgramTls; });
        if (tls_header == image.program_headers().end()) {
            throw std::runtime_error("PT_TLS metadata is missing its program header");
        }
        const auto initial = image.bytes().subspan(static_cast<std::size_t>(tls_header->offset),
                                                   static_cast<std::size_t>(tls_header->file_size));
        if (tls_header->memory_size > std::numeric_limits<std::size_t>::max() ||
            tls_header->alignment > std::numeric_limits<std::size_t>::max() ||
            !tls_registry_.register_module(
                module.tls_module_id, static_cast<std::size_t>(tls_header->alignment),
                static_cast<std::size_t>(tls_header->memory_size), initial)) {
            throw std::runtime_error("unable to register PT_TLS module image");
        }
    }

    if (image.is_sce_dynamic()) {
        module.dynamic = loader::parse_dynamic_info(image);
    }
    if (module.dynamic) {
        RuntimeLinker linker(memory_, symbols_, late_imports_ ? &*late_imports_ : nullptr);
        (void)linker.register_exports(module.base, *module.dynamic);
        module.relocations = linker.relocate(module.base, module.tls_module_id, *module.dynamic);
    }

    return module;
}

RelocationReport Runtime::relink(LoadedModule& module) {
    if (!module.dynamic) {
        module.relocations = {};
        return module.relocations;
    }
    RuntimeLinker linker(memory_, symbols_, late_imports_ ? &*late_imports_ : nullptr);
    (void)linker.register_exports(module.base, *module.dynamic);
    module.relocations = linker.relocate(module.base, module.tls_module_id, *module.dynamic);
    return module.relocations;
}

std::uint64_t Runtime::invoke_entry(const LoadedModule& module, GuestSize stack_size,
                                    std::uint64_t arg0, std::uint64_t arg1,
                                    std::uint64_t arg2) {
    if (!memory_.native_backed()) {
        throw std::runtime_error("native entry invocation requires a native-backed guest address space");
    }
    const auto* entry_region = memory_.find(module.entry);
    if (entry_region == nullptr ||
        !memory::has(entry_region->protection, memory::Protection::execute)) {
        throw std::runtime_error("module entry is not mapped executable guest memory");
    }

    auto stack = GuestStack::create(stack_size);
    auto trampoline = EntryTrampoline::create();
    auto thread = create_thread_context();
    if (!stack || !trampoline || !thread) {
        throw std::runtime_error("unable to initialize native guest thread execution state");
    }

    ScopedGuestThreadContext context_scope(*thread);
    return trampoline->invoke(module.entry, stack->top(), arg0, arg1, arg2);
}

} // namespace nyxora::runtime
