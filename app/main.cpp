#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "nyxora/gpu/null_backend.hpp"
#include "nyxora/gpu/pm4_trace.hpp"
#include "nyxora/runtime/runtime.hpp"

namespace {

void usage() {
    std::cerr << "usage: nyxora <x86-64-elf>\n"
              << "       nyxora --pm4-replay <graphics|compute> <trace.pm4>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        usage();
        return 2;
    }

    try {
        if (argc == 4) {
            if (std::string_view(argv[1]) != "--pm4-replay") {
                usage();
                return 2;
            }
            const auto queue_name = std::string_view(argv[2]);
            if (queue_name != "graphics" && queue_name != "compute") {
                usage();
                return 2;
            }

            std::ifstream file(argv[3], std::ios::binary);
            if (!file) {
                throw std::runtime_error("could not open PM4 trace file");
            }
            const auto words = nyxora::gpu::pm4::read_binary_trace(file);
            const auto queue = queue_name == "graphics" ? nyxora::gpu::pm4::QueueType::graphics
                                                         : nyxora::gpu::pm4::QueueType::compute;
            nyxora::gpu::pm4::Processor processor(queue);
            const auto submission = processor.process(words);
            nyxora::gpu::pm4::write_submission_json(
                std::cout, submission, queue,
                queue == nyxora::gpu::pm4::QueueType::compute
                    ? std::optional<std::uint32_t>{0}
                    : std::nullopt);
            return 0;
        }

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
