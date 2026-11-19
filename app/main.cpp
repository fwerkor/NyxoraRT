#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "nyxora/gpu/null_backend.hpp"
#include "nyxora/gpu/pm4_trace.hpp"
#include "nyxora/gpu/shader/isa.hpp"
#include "nyxora/gpu/shader/scalar_decode.hpp"
#include "nyxora/gpu/shader/scalar_cfg.hpp"
#include "nyxora/gpu/shader/scalar_exec.hpp"
#include "nyxora/gpu/shader/spirv.hpp"
#include "nyxora/runtime/runtime.hpp"

namespace {

void usage() {
    std::cerr << "usage: nyxora <x86-64-elf>\n"
              << "       nyxora --pm4-replay <graphics|compute> <trace.pm4>\n"
              << "       nyxora --shader-sopp <shader.bin>\n"
              << "       nyxora --shader-scalar <shader.bin>\n"
              << "       nyxora --shader-cfg <shader.bin>\n"
              << "       nyxora --shader-exec <shader.bin>\n"
              << "       nyxora --shader-spv <shader.bin> <output.spv>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3 && argc != 4) {
        usage();
        return 2;
    }

    try {
        if (argc == 3) {
            const auto mode = std::string_view(argv[1]);
            if (mode != "--shader-sopp" && mode != "--shader-scalar" &&
                mode != "--shader-cfg" && mode != "--shader-exec") {
                usage();
                return 2;
            }
            std::ifstream file(argv[2], std::ios::binary);
            if (!file) {
                throw std::runtime_error("could not open shader bytecode file");
            }
            const auto words = nyxora::gpu::pm4::read_binary_trace(file, 65536U);
            if (mode == "--shader-sopp") {
                const auto program = nyxora::gpu::shader::decode_sopp_program(words);
                if (!program) {
                    throw std::runtime_error("unsupported or invalid SOPP-only shader program");
                }
                nyxora::gpu::shader::write_sopp_json(std::cout, *program);
                return 0;
            }
            const auto program = nyxora::gpu::shader::decode_scalar_program(words);
            if (!program) {
                throw std::runtime_error("unsupported or invalid scalar shader program");
            }
            if (mode == "--shader-scalar") {
                nyxora::gpu::shader::write_scalar_program_json(std::cout, *program);
                return 0;
            }
            const auto cfg = nyxora::gpu::shader::build_scalar_cfg(*program);
            if (!cfg) {
                throw std::runtime_error("invalid scalar control-flow graph");
            }
            if (mode == "--shader-exec") {
                const auto state = nyxora::gpu::shader::execute_scalar_subset(*cfg);
                if (!state) {
                    throw std::runtime_error("scalar program cannot execute in reference mode");
                }
                nyxora::gpu::shader::write_scalar_state_json(std::cout, *state);
            } else {
                nyxora::gpu::shader::write_scalar_cfg_json(std::cout, *cfg);
            }
            return 0;
        }
        if (argc == 4) {
            if (std::string_view(argv[1]) == "--shader-spv") {
                std::ifstream file(argv[2], std::ios::binary);
                if (!file) {
                    throw std::runtime_error("could not open shader bytecode file");
                }
                const auto words = nyxora::gpu::pm4::read_binary_trace(file, 65536U);
                const auto program = nyxora::gpu::shader::decode_scalar_program(words);
                if (!program) {
                    throw std::runtime_error("unsupported or invalid scalar shader program");
                }
                const auto cfg = nyxora::gpu::shader::build_scalar_cfg(*program);
                const auto spirv = cfg ? nyxora::gpu::shader::compile_scalar_diagnostic_spirv(*cfg)
                                       : std::nullopt;
                if (!spirv) {
                    throw std::runtime_error("program cannot be lowered to diagnostic SPIR-V");
                }
                std::ofstream output(argv[3], std::ios::binary | std::ios::trunc);
                if (!output) {
                    throw std::runtime_error("could not create SPIR-V output file");
                }
                for (const auto word : *spirv) {
                    const std::array<char, 4> bytes{
                        static_cast<char>(word & 0xffU),
                        static_cast<char>((word >> 8U) & 0xffU),
                        static_cast<char>((word >> 16U) & 0xffU),
                        static_cast<char>((word >> 24U) & 0xffU),
                    };
                    output.write(bytes.data(), bytes.size());
                }
                if (!output.flush()) {
                    throw std::runtime_error("failed to write SPIR-V output file");
                }
                std::cout << "wrote " << spirv->size() << " SPIR-V dwords\n";
                return 0;
            }
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
