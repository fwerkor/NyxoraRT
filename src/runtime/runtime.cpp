#include "nyxora/runtime/runtime.hpp"
#include "nyxora/hle/libkernel.hpp"
#include "nyxora/runtime/cpu_patches.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

GuestAddress checked_address(GuestAddress base, std::uint64_t offset);
bool overlaps(GuestAddress base, GuestSize size, const memory::RegionInfo& region);

bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    if (relative.empty()) {
        return path == root;
    }
    return !relative.is_absolute() && *relative.begin() != "..";
}

bool has_dynamic_segment(const loader::Elf64Image& image) {
    return std::any_of(image.program_headers().begin(), image.program_headers().end(),
                       [](const loader::ProgramHeader& header) {
                           return header.type == loader::kProgramDynamic && header.file_size != 0;
                       });
}

std::vector<std::string_view> split_search_path(std::string_view value) {
    std::vector<std::string_view> components;
    while (!value.empty()) {
        const auto separator = value.find(':');
        const auto component = value.substr(0, separator);
        if (!component.empty()) {
            components.push_back(component);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        value.remove_prefix(separator + 1U);
    }
    return components;
}

void replace_all(std::string& value, std::string_view token, std::string_view replacement) {
    std::size_t offset = 0;
    while ((offset = value.find(token, offset)) != std::string::npos) {
        value.replace(offset, token.size(), replacement);
        offset += replacement.size();
    }
}

std::filesystem::path search_directory(const std::filesystem::path& root,
                                       const std::filesystem::path& module_path,
                                       std::string_view component) {
    auto expanded = std::string(component);
    const bool uses_origin = component.find("$ORIGIN") != std::string_view::npos ||
                             component.find("${ORIGIN}") != std::string_view::npos;
    const auto origin = module_path.parent_path().generic_string();
    replace_all(expanded, "${ORIGIN}", origin);
    replace_all(expanded, "$ORIGIN", origin);
    if (expanded.find('$') != std::string::npos) {
        throw std::runtime_error("unsupported dynamic search-path token: " +
                                 std::string(component));
    }

    std::filesystem::path directory;
    if (expanded == "/app0" || expanded.starts_with("/app0/")) {
        const auto relative = expanded.size() == 5 ? std::string_view{}
                                                  : std::string_view(expanded).substr(6);
        directory = root / std::filesystem::path(relative);
    } else {
        directory = std::filesystem::path(expanded);
        if (directory.is_absolute()) {
            if (!uses_origin || !path_is_within(root, directory.lexically_normal())) {
                throw std::runtime_error("absolute dynamic search path is outside guest /app0: " +
                                         std::string(component));
            }
        } else {
            if (!expanded.empty() && expanded.front() == '/') {
                throw std::runtime_error("absolute dynamic search path is outside guest /app0: " +
                                         std::string(component));
            }
            directory = root / directory;
        }
    }

    directory = directory.lexically_normal();
    if (!path_is_within(root, directory)) {
        throw std::runtime_error("dynamic search path escapes the program root: " +
                                 std::string(component));
    }
    return directory;
}

std::optional<std::filesystem::path> resolve_dependency_path(
    const std::filesystem::path& root, const std::filesystem::path& module_path,
    const loader::DynamicInfo& dynamic, const std::filesystem::path& dependency_name) {
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(module_path.parent_path() / dependency_name);

    if (!dependency_name.has_parent_path()) {
        const std::string_view search =
            !dynamic.runpath.empty() ? std::string_view(dynamic.runpath)
                                     : std::string_view(dynamic.rpath);
        for (const auto component : split_search_path(search)) {
            candidates.push_back(search_directory(root, module_path, component) / dependency_name);
        }
    }

    std::unordered_set<std::string> seen;
    for (auto candidate : candidates) {
        candidate = candidate.lexically_normal();
        if (!path_is_within(root, candidate)) {
            throw std::runtime_error("DT_NEEDED dependency escapes the program root: " +
                                     dependency_name.string());
        }
        if (!seen.emplace(candidate.generic_string()).second) {
            continue;
        }

        std::error_code error;
        const auto canonical = std::filesystem::canonical(candidate, error);
        if (error) {
            continue;
        }
        if (!path_is_within(root, canonical)) {
            throw std::runtime_error("DT_NEEDED dependency escapes the program root: " +
                                     dependency_name.string());
        }
        if (std::filesystem::is_regular_file(canonical, error) && !error) {
            return canonical;
        }
    }
    return std::nullopt;
}

struct ModuleFootprint {
    std::uint64_t first{};
    std::uint64_t end{};
};

std::optional<ModuleFootprint> module_footprint(const loader::Elf64Image& image) {
    ModuleFootprint result{std::numeric_limits<std::uint64_t>::max(), 0};
    bool found = false;
    for (const auto& segment : image.program_headers()) {
        if (!is_loadable_segment(segment.type) || segment.memory_size == 0) {
            continue;
        }
        if (segment.virtual_address > std::numeric_limits<std::uint64_t>::max() -
                                          segment.memory_size) {
            throw std::runtime_error("ELF loadable segment range overflows");
        }
        result.first = std::min(result.first, segment.virtual_address);
        result.end = std::max(result.end, segment.virtual_address + segment.memory_size);
        found = true;
    }
    return found ? std::optional{result} : std::nullopt;
}

bool image_fits_at(const memory::GuestAddressSpace& memory, const loader::Elf64Image& image,
                   GuestAddress base) {
    const auto regions = memory.regions();
    GuestAddress native_end{};
    if (memory.native_backed()) {
        if (memory.native_base() > std::numeric_limits<GuestAddress>::max() - memory.native_size()) {
            return false;
        }
        native_end = memory.native_base() + memory.native_size();
    }
    for (const auto& segment : image.program_headers()) {
        if (!is_loadable_segment(segment.type) || segment.memory_size == 0) {
            continue;
        }
        if (base > std::numeric_limits<GuestAddress>::max() - segment.virtual_address) {
            return false;
        }
        const auto address = base + segment.virtual_address;
        if (address > std::numeric_limits<GuestAddress>::max() - segment.memory_size) {
            return false;
        }
        const auto end = address + segment.memory_size;
        if (memory.native_backed() &&
            (address < memory.native_base() || end > native_end)) {
            return false;
        }
        if (std::any_of(regions.begin(), regions.end(), [&](const auto& region) {
                return overlaps(address, segment.memory_size, region);
            })) {
            return false;
        }
    }
    return true;
}

std::vector<GuestAddress> function_array(const memory::GuestAddressSpace& memory,
                                         const LoadedModule& module, std::uint64_t address,
                                         std::uint64_t byte_size, bool reverse) {
    if (byte_size == 0) {
        return {};
    }
    if (address == 0 || byte_size % sizeof(GuestAddress) != 0) {
        throw std::runtime_error("ELF lifecycle function array has an invalid size or address");
    }
    const auto array_address = checked_address(module.base, address);
    std::vector<GuestAddress> functions;
    functions.reserve(static_cast<std::size_t>(byte_size / sizeof(GuestAddress)));
    for (std::uint64_t offset = 0; offset < byte_size; offset += sizeof(GuestAddress)) {
        const auto bytes = memory.view(checked_address(array_address, offset), sizeof(GuestAddress));
        if (bytes.size() != sizeof(GuestAddress)) {
            throw std::runtime_error("ELF lifecycle function array is outside mapped guest memory");
        }
        GuestAddress function{};
        std::memcpy(&function, bytes.data(), sizeof(function));
        if (function != 0 && function != std::numeric_limits<GuestAddress>::max()) {
            functions.push_back(function);
        }
    }
    if (reverse) {
        std::reverse(functions.begin(), functions.end());
    }
    return functions;
}

void invoke_lifecycle_function(const memory::GuestAddressSpace& memory,
                               const EntryTrampoline& trampoline, GuestAddress stack_top,
                               GuestAddress function) {
    const auto* region = memory.find(function);
    if (region == nullptr || !memory::has(region->protection, memory::Protection::execute)) {
        throw std::runtime_error("module lifecycle function is not mapped executable guest memory");
    }
    const auto result = invoke_guest_captured(trampoline, function, stack_top, 0, 0, 0);
    if (result.fault) {
        throw GuestFaultException(*result.fault);
    }
}

void validate_native_entry(const memory::GuestAddressSpace& memory,
                           const LoadedModule& module) {
    if (!memory.native_backed()) {
        throw std::runtime_error("native entry invocation requires a native-backed guest address space");
    }
    const auto* entry_region = memory.find(module.entry);
    if (entry_region == nullptr ||
        !memory::has(entry_region->protection, memory::Protection::execute)) {
        throw std::runtime_error("module entry is not mapped executable guest memory");
    }
}

struct PendingSegment {
    GuestAddress base{};
    GuestSize size{};
    GuestSize file_size{};
    memory::Protection final_protection{memory::Protection::none};
};

bool overlaps(GuestAddress base, GuestSize size, const memory::RegionInfo& region) {
    if (size == 0 || region.size == 0 ||
        base > std::numeric_limits<GuestAddress>::max() - size ||
        region.base > std::numeric_limits<GuestAddress>::max() - region.size) {
        return false;
    }
    return base < region.base + region.size && region.base < base + size;
}

std::optional<TcbPatchArena> try_map_cpu_patch_page(memory::GuestAddressSpace& memory,
                                                     std::span<const PendingSegment> segments,
                                                     std::string name) {
    if (segments.empty()) {
        return std::nullopt;
    }
    const auto page = static_cast<GuestSize>(memory::NativeArena::page_size());
    if (std::none_of(segments.begin(), segments.end(), [](const auto& segment) {
            return memory::has(segment.final_protection, memory::Protection::execute);
        })) {
        return std::nullopt;
    }
    GuestAddress module_begin = std::numeric_limits<GuestAddress>::max();
    GuestAddress module_end = 0;
    for (const auto& segment : segments) {
        module_begin = std::min(module_begin, segment.base);
        if (segment.base > std::numeric_limits<GuestAddress>::max() - segment.size) {
            return std::nullopt;
        }
        module_end = std::max(module_end, segment.base + segment.size);
    }

    auto candidate = checked_align_up(module_end, page);
    if (!candidate) {
        return std::nullopt;
    }
    const auto regions = memory.regions();
    while (true) {
        if (memory.native_backed()) {
            const auto native_begin = memory.native_base();
            if (native_begin > std::numeric_limits<GuestAddress>::max() - memory.native_size()) {
                break;
            }
            const auto native_end = native_begin + memory.native_size();
            if (*candidate < native_begin || *candidate > native_end || page > native_end - *candidate) {
                break;
            }
        }
        const auto blocking = std::find_if(regions.begin(), regions.end(), [&](const auto& region) {
            return overlaps(*candidate, page, region);
        });
        if (blocking == regions.end()) {
            const auto forward = *candidate >= module_begin ? *candidate - module_begin
                                                             : module_begin - *candidate;
            if (forward <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) &&
                memory.map(*candidate, page,
                           memory::Protection::read | memory::Protection::write, name)) {
                return TcbPatchArena{.base = *candidate, .size = page, .used = 0};
            }
            break;
        }
        if (blocking->base > std::numeric_limits<GuestAddress>::max() - blocking->size) {
            break;
        }
        candidate = checked_align_up(blocking->base + blocking->size, page);
        if (!candidate) {
            break;
        }
    }

    if (module_begin < page) {
        return std::nullopt;
    }
    GuestAddress downward = module_begin / page * page - page;
    while (true) {
        if (memory.native_backed()) {
            const auto native_begin = memory.native_base();
            if (downward < native_begin) {
                break;
            }
        }
        const auto blocking = std::find_if(regions.begin(), regions.end(), [&](const auto& region) {
            return overlaps(downward, page, region);
        });
        if (blocking == regions.end()) {
            const auto distance = module_end >= downward ? module_end - downward : downward - module_end;
            if (distance <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) &&
                memory.map(downward, page,
                           memory::Protection::read | memory::Protection::write, name)) {
                return TcbPatchArena{.base = downward, .size = page, .used = 0};
            }
            break;
        }
        if (blocking->base < page) {
            break;
        }
        downward = blocking->base / page * page - page;
    }
    return std::nullopt;
}

} // namespace

Runtime::Runtime(std::unique_ptr<gpu::Backend> gpu_backend)
    : Runtime(std::move(gpu_backend), memory::GuestAddressSpace{}) {}

Runtime::Runtime(std::unique_ptr<gpu::Backend> gpu_backend, memory::GuestAddressSpace memory)
    : memory_(std::move(memory)), kernel_services_(memory_),
      thread_manager_(tls_registry_, &kernel_services_), late_imports_(LateImportTable::create()),
      gpu_(std::move(gpu_backend)) {
    if (!gpu_) {
        throw std::invalid_argument("Runtime requires a GPU backend");
    }
    hle_ = std::make_unique<HleRegistry>(symbols_);
    hle::libkernel::register_core(*hle_);
}

LoadedModule Runtime::load_elf(const std::filesystem::path& path, GuestAddress base) {
    const auto image = loader::Elf64Image::from_file(path);
    if (!kernel_services_.guest_root_configured()) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path, error);
        if (error || !kernel_services_.set_guest_root(absolute.parent_path())) {
            throw std::runtime_error("unable to configure guest /app0 root");
        }
    }
    return load_image(image, path, base);
}

GuestAddress Runtime::find_module_base(const loader::Elf64Image& image) const {
    const auto footprint = module_footprint(image);
    if (!footprint) {
        throw std::runtime_error("ELF image has no loadable segments");
    }
    const auto page = static_cast<GuestSize>(memory::NativeArena::page_size());
    GuestAddress candidate{};
    GuestAddress limit = std::numeric_limits<GuestAddress>::max();
    if (memory_.native_backed()) {
        const auto aligned = checked_align_up(memory_.native_base(), page);
        if (!aligned || memory_.native_base() >
                            std::numeric_limits<GuestAddress>::max() - memory_.native_size()) {
            throw std::runtime_error("native guest arena cannot place another module");
        }
        candidate = *aligned;
        limit = memory_.native_base() + memory_.native_size();
    } else {
        candidate = 0x100000000ULL;
        for (const auto& region : memory_.regions()) {
            if (region.base <= std::numeric_limits<GuestAddress>::max() - region.size) {
                candidate = std::max(candidate, region.base + region.size);
            }
        }
        const auto aligned = checked_align_up(candidate, page);
        if (!aligned) {
            throw std::runtime_error("guest address space cannot place another module");
        }
        candidate = *aligned;
    }

    const auto span = footprint->end - footprint->first;
    while (candidate >= footprint->first) {
        if (candidate > std::numeric_limits<GuestAddress>::max() - span ||
            candidate + span > limit) {
            break;
        }
        const auto base = candidate - footprint->first;
        if (image_fits_at(memory_, image, base)) {
            return base;
        }
        if (candidate > std::numeric_limits<GuestAddress>::max() - page) {
            break;
        }
        candidate += page;
    }
    throw std::runtime_error("guest address space has no free range for dependency module");
}

LoadedProgram Runtime::load_program(const std::filesystem::path& path, GuestAddress base) {
    std::error_code error;
    const auto main_path = std::filesystem::canonical(path, error);
    if (error || !std::filesystem::is_regular_file(main_path, error) || error) {
        throw std::runtime_error("unable to open program ELF image: " + path.string());
    }
    const auto root = main_path.parent_path();
    if (!kernel_services_.guest_root_configured() && !kernel_services_.set_guest_root(root)) {
        throw std::runtime_error("unable to configure guest /app0 root");
    }

    struct PendingModule {
        std::filesystem::path path;
        loader::Elf64Image image;
        std::optional<loader::DynamicInfo> dynamic;
        std::vector<std::size_t> dependencies;
    };
    std::vector<PendingModule> pending;
    std::unordered_map<std::string, std::size_t> indices;

    std::function<std::size_t(const std::filesystem::path&)> discover;
    discover = [&](const std::filesystem::path& requested) -> std::size_t {
        std::error_code canonical_error;
        const auto canonical = std::filesystem::canonical(requested, canonical_error);
        if (canonical_error || !path_is_within(root, canonical)) {
            throw std::runtime_error("program dependency escapes or is missing from the program root: " +
                                     requested.string());
        }
        const auto key = canonical.generic_string();
        if (const auto it = indices.find(key); it != indices.end()) {
            return it->second;
        }

        auto image = loader::Elf64Image::from_file(canonical);
        auto dynamic = has_dynamic_segment(image) ? loader::parse_dynamic_info(image) : std::nullopt;
        const auto index = pending.size();
        indices.emplace(key, index);
        pending.push_back(PendingModule{canonical, std::move(image), std::move(dynamic), {}});

        if (pending[index].dynamic) {
            for (const auto& needed : pending[index].dynamic->needed) {
                const std::filesystem::path dependency_name(needed);
                if (dependency_name.empty() || dependency_name.is_absolute() || needed.front() == '/') {
                    throw std::runtime_error("unsupported absolute or empty DT_NEEDED dependency: " +
                                             needed);
                }
                const auto dependency_path =
                    resolve_dependency_path(root, canonical, *pending[index].dynamic, dependency_name);
                if (!dependency_path) {
                    if (hle::libkernel::provides_module(dependency_name.filename().string())) {
                        continue;
                    }
                    throw std::runtime_error("missing DT_NEEDED dependency: " + needed);
                }
                const auto dependency_index = discover(*dependency_path);
                pending[index].dependencies.push_back(dependency_index);
            }
        }
        return index;
    };

    const auto main_index = discover(main_path);
    LoadedProgram program;
    program.main_module = main_index;
    program.modules.reserve(pending.size());
    program.dependencies.resize(pending.size());
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const auto module_base = index == main_index && base != 0
                                     ? base
                                     : find_module_base(pending[index].image);
        program.modules.push_back(
            map_image(pending[index].image, pending[index].path, module_base));
        program.dependencies[index] = pending[index].dependencies;
    }

    RuntimeLinker linker(memory_, symbols_, late_imports_ ? &*late_imports_ : nullptr);
    for (const auto& module : program.modules) {
        if (module.dynamic) {
            (void)linker.register_exports(module.base, *module.dynamic);
        }
    }
    for (auto& module : program.modules) {
        if (module.dynamic) {
            module.relocations = linker.relocate(module.base, module.tls_module_id, *module.dynamic);
        }
    }
    return program;
}

LoadedModule Runtime::map_image(const loader::Elf64Image& image, std::filesystem::path path,
                                GuestAddress base) {
    LoadedModule module{
        .path = std::move(path),
        .base = base,
        .entry = checked_address(base, image.entry()),
        .segments = {},
        .dynamic = std::nullopt,
        .tls = image.tls(),
        .tls_module_id = 0,
        .process_param = 0,
        .process_param_size = 0,
        .relocations = {},
    };

    std::vector<PendingSegment> pending_segments;
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
        pending_segments.push_back(PendingSegment{
            .base = guest_base,
            .size = segment.memory_size,
            .file_size = segment.file_size,
            .final_protection = final_protection,
        });
    }

    const auto policy = host_tcb_patch_policy();
    std::optional<TcbPatchArena> patch_arena;
    if (policy.mode == TcbPatchMode::fs_to_windows_teb) {
        patch_arena = try_map_cpu_patch_page(memory_, pending_segments,
                                             module.path.filename().string() + ".cpu-patches");
    }

    for (const auto& segment : pending_segments) {
        if (memory::has(segment.final_protection, memory::Protection::execute) &&
            segment.file_size != 0) {
            const auto patches = patch_tcb_accesses(memory_, segment.base, segment.file_size, policy,
                                                    patch_arena ? &*patch_arena : nullptr);
            if (!patches) {
                throw std::runtime_error("unable to inspect executable segment for CPU patches");
            }
            if (patches->unsupported != 0) {
                throw std::runtime_error("executable segment contains unsupported guest TCB accesses");
            }
        }
    }

    if (patch_arena) {
        if (patch_arena->used == 0) {
            if (!memory_.unmap(patch_arena->base, patch_arena->size)) {
                throw std::runtime_error("unable to release unused CPU patch arena");
            }
            patch_arena.reset();
        } else if (!memory_.protect(patch_arena->base, patch_arena->size,
                                    memory::Protection::read | memory::Protection::execute)) {
            throw std::runtime_error("unable to protect CPU patch arena");
        }
    }

    for (const auto& segment : pending_segments) {
        if (!memory_.protect(segment.base, segment.size, segment.final_protection)) {
            throw std::runtime_error("unable to apply ELF segment protection");
        }
        module.segments.push_back(*memory_.find(segment.base));
    }

    for (const auto& header : image.program_headers()) {
        if (header.type != loader::kProgramSceProcParam || header.memory_size == 0) {
            continue;
        }
        if (module.process_param != 0) {
            throw std::runtime_error("ELF contains multiple PT_SCE_PROCPARAM segments");
        }
        const auto address = checked_address(base, header.virtual_address);
        const auto* region = memory_.find(address);
        if (region == nullptr || address > std::numeric_limits<GuestAddress>::max() -
                                             header.memory_size ||
            address + header.memory_size > region->base + region->size) {
            throw std::runtime_error("PT_SCE_PROCPARAM is outside mapped guest memory");
        }
        module.process_param = address;
        module.process_param_size = header.memory_size;
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

    if (has_dynamic_segment(image)) {
        module.dynamic = loader::parse_dynamic_info(image);
    }

    return module;
}

void Runtime::link_module(LoadedModule& module) {
    if (!module.dynamic) {
        module.relocations = {};
        return;
    }
    RuntimeLinker linker(memory_, symbols_, late_imports_ ? &*late_imports_ : nullptr);
    (void)linker.register_exports(module.base, *module.dynamic);
    module.relocations = linker.relocate(module.base, module.tls_module_id, *module.dynamic);
}

LoadedModule Runtime::load_image(const loader::Elf64Image& image, std::filesystem::path path,
                                 GuestAddress base) {
    auto module = map_image(image, std::move(path), base);
    link_module(module);
    return module;
}

RelocationReport Runtime::relink(LoadedModule& module) {
    link_module(module);
    return module.relocations;
}

void Runtime::initialize_program(LoadedProgram& program, GuestSize stack_size) {
    if (program.finalized) {
        throw std::runtime_error("cannot initialize a finalized program");
    }
    if (program.initialization_failed) {
        throw std::runtime_error("cannot retry a partially failed program initialization");
    }
    if (program.modules.empty() || program.main_module >= program.modules.size() ||
        program.dependencies.size() != program.modules.size()) {
        throw std::runtime_error("loaded program has invalid module graph metadata");
    }

    std::vector<std::uint8_t> state(program.modules.size());
    std::vector<std::size_t> order;
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
        if (index >= program.modules.size()) {
            throw std::runtime_error("loaded program dependency index is out of range");
        }
        if (state[index] == 2) {
            return;
        }
        if (state[index] == 1) {
            return;
        }
        state[index] = 1;
        for (const auto dependency : program.dependencies[index]) {
            visit(dependency);
        }
        state[index] = 2;
        order.push_back(index);
    };
    visit(program.main_module);

    auto stack = GuestStack::create(stack_size);
    auto trampoline = EntryTrampoline::create();
    auto thread = create_thread_context();
    if (!stack || !trampoline || !thread) {
        throw std::runtime_error("unable to initialize module lifecycle execution state");
    }
    ScopedGuestThreadManager manager_scope(thread_manager_);
    ScopedGuestThreadContext context_scope(*thread);
    ScopedGuestSegment segment_scope(*thread);
    try {
        if (!program.preinitialized) {
            const auto& main = program.modules[program.main_module];
            if (main.dynamic) {
                for (const auto function :
                     function_array(memory_, main, main.dynamic->preinit_array,
                                    main.dynamic->preinit_array_size, false)) {
                    invoke_lifecycle_function(memory_, *trampoline, stack->top(), function);
                }
            }
            program.preinitialized = true;
        }

        std::unordered_set<std::size_t> initialized(program.initialized_modules.begin(),
                                                    program.initialized_modules.end());
        for (const auto index : order) {
            if (initialized.contains(index)) {
                continue;
            }
            const auto& module = program.modules[index];
            if (module.dynamic) {
                if (module.dynamic->init != 0) {
                    invoke_lifecycle_function(memory_, *trampoline, stack->top(),
                                              checked_address(module.base, module.dynamic->init));
                }
                for (const auto function :
                     function_array(memory_, module, module.dynamic->init_array,
                                    module.dynamic->init_array_size, false)) {
                    invoke_lifecycle_function(memory_, *trampoline, stack->top(), function);
                }
            }
            program.initialized_modules.push_back(index);
            initialized.insert(index);
        }
    } catch (...) {
        program.initialization_failed = true;
        throw;
    }
}

void Runtime::finalize_program(LoadedProgram& program, GuestSize stack_size) {
    if (program.finalized) {
        return;
    }
    if (program.finalization_failed) {
        throw std::runtime_error("cannot retry a partially failed program finalization");
    }
    if (program.modules.empty() || program.dependencies.size() != program.modules.size()) {
        throw std::runtime_error("loaded program has invalid module graph metadata");
    }
    if (program.initialized_modules.empty()) {
        program.finalized = true;
        return;
    }

    auto stack = GuestStack::create(stack_size);
    auto trampoline = EntryTrampoline::create();
    auto thread = create_thread_context();
    if (!stack || !trampoline || !thread) {
        throw std::runtime_error("unable to initialize module lifecycle execution state");
    }
    ScopedGuestThreadManager manager_scope(thread_manager_);
    ScopedGuestThreadContext context_scope(*thread);
    ScopedGuestSegment segment_scope(*thread);
    try {
        while (!program.initialized_modules.empty()) {
            const auto index = program.initialized_modules.back();
            if (index >= program.modules.size()) {
                throw std::runtime_error("loaded program initialization state is invalid");
            }
            const auto& module = program.modules[index];
            if (module.dynamic) {
                for (const auto function :
                     function_array(memory_, module, module.dynamic->fini_array,
                                    module.dynamic->fini_array_size, true)) {
                    invoke_lifecycle_function(memory_, *trampoline, stack->top(), function);
                }
                if (module.dynamic->fini != 0) {
                    invoke_lifecycle_function(memory_, *trampoline, stack->top(),
                                              checked_address(module.base, module.dynamic->fini));
                }
            }
            program.initialized_modules.pop_back();
        }
        program.finalized = true;
    } catch (...) {
        program.finalization_failed = true;
        throw;
    }
}

std::uint64_t Runtime::invoke_entry(const LoadedModule& module, GuestSize stack_size,
                                    std::uint64_t arg0, std::uint64_t arg1,
                                    std::uint64_t arg2) {
    validate_native_entry(memory_, module);

    auto stack = GuestStack::create(stack_size);
    auto trampoline = EntryTrampoline::create();
    auto thread = create_thread_context();
    if (!stack || !trampoline || !thread) {
        throw std::runtime_error("unable to initialize native guest thread execution state");
    }

    ScopedGuestThreadManager manager_scope(thread_manager_);
    ScopedGuestThreadContext context_scope(*thread);
    ScopedGuestSegment segment_scope(*thread);
    const auto result = invoke_guest_captured(*trampoline, module.entry, stack->top(),
                                              arg0, arg1, arg2);
    if (result.fault) {
        throw GuestFaultException(*result.fault);
    }
    return result.value;
}

GuestThread Runtime::start_thread(const LoadedModule& module, GuestSize stack_size,
                                  std::uint64_t arg0, std::uint64_t arg1,
                                  std::uint64_t arg2) {
    validate_native_entry(memory_, module);
    auto thread = GuestThread::start(tls_registry_, module.entry, stack_size, arg0, arg1, arg2,
                                     &thread_manager_);
    if (!thread) {
        throw std::runtime_error("unable to initialize guest thread execution state");
    }
    return std::move(*thread);
}

} // namespace nyxora::runtime
