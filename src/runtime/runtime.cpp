#include "nyxora/runtime/runtime.hpp"

#include <algorithm>
#include <stdexcept>

namespace nyxora::runtime {
namespace {
memory::Protection segment_protection(std::uint32_t flags) {
    memory::Protection protection = memory::Protection::none;
    if ((flags & 0x4U) != 0) protection = protection | memory::Protection::read;
    if ((flags & 0x2U) != 0) protection = protection | memory::Protection::write;
    if ((flags & 0x1U) != 0) protection = protection | memory::Protection::execute;
    return protection;
}
}

Runtime::Runtime(std::unique_ptr<gpu::Backend> gpu_backend) : gpu_(std::move(gpu_backend)) {
    if (!gpu_) {
        throw std::invalid_argument("Runtime requires a GPU backend");
    }
}

LoadedModule Runtime::load_elf(const std::filesystem::path& path, GuestAddress base) {
    const auto image = loader::Elf64Image::from_file(path);
    LoadedModule module{.path = path, .base = base, .entry = base + image.entry(), .segments = {}};

    for (const auto& segment : image.program_headers()) {
        if (segment.type != loader::kProgramLoad || segment.memory_size == 0) {
            continue;
        }
        const auto guest_base = base + segment.virtual_address;
        const auto final_protection = segment_protection(segment.flags);
        const auto load_protection = final_protection | memory::Protection::write;
        if (!memory_.map(guest_base, segment.memory_size, load_protection,
                         path.filename().string())) {
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
            !memory_.zero(guest_base + segment.file_size, segment.memory_size - segment.file_size)) {
            throw std::runtime_error("unable to initialize ELF BSS range");
        }
        if (!memory_.protect(guest_base, segment.memory_size, final_protection)) {
            throw std::runtime_error("unable to apply ELF segment protection");
        }
        module.segments.push_back(*memory_.find(guest_base));
    }
    return module;
}

} // namespace nyxora::runtime
