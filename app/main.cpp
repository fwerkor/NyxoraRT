#include <exception>
#include <iostream>
#include <memory>

#include "asteria/gpu/null_backend.hpp"
#include "asteria/runtime/runtime.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: asteria <x86-64-elf>\n";
        return 2;
    }

    try {
        asteria::runtime::Runtime runtime(std::make_unique<asteria::gpu::NullBackend>());
        const auto module = runtime.load_elf(argv[1]);
        std::cout << "loaded " << module.path.filename().string() << "\n"
                  << "entry: 0x" << std::hex << module.entry << "\n"
                  << "load segments: " << std::dec << module.segments.size() << "\n";
    } catch (const std::exception& error) {
        std::cerr << "asteria: " << error.what() << '\n';
        return 1;
    }
}
