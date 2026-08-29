#include <exception>
#include <iostream>
#include <memory>

#include "nyxora/gpu/null_backend.hpp"
#include "nyxora/runtime/runtime.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: nyxora <x86-64-elf>\n";
        return 2;
    }

    try {
        nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
        const auto module = runtime.load_elf(argv[1]);
        std::cout << "loaded " << module.path.filename().string() << "\n"
                  << "entry: 0x" << std::hex << module.entry << "\n"
                  << "load segments: " << std::dec << module.segments.size() << "\n";
    } catch (const std::exception& error) {
        std::cerr << "nyxora: " << error.what() << '\n';
        return 1;
    }
}
