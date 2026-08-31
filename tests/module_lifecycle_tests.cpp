#include "test.hpp"
#include "sce_fixture.hpp"
#include "nyxora/gpu/null_backend.hpp"
#include "nyxora/runtime/runtime.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace {

template <typename T>
T read_at(const std::vector<std::byte>& bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("unable to write test ELF fixture");
    }
}

std::vector<std::byte> add_needed(std::vector<std::byte> bytes,
                                  std::span<const std::string_view> names) {
    using namespace nyxora::loader;
    auto string_size = read_at<std::uint64_t>(
        bytes, test_fixture::SceImageLayout::dynamic_offset + 16 + 8);
    std::size_t dynamic_index = 17;
    for (const auto name : names) {
        const auto offset = string_size;
        std::memcpy(bytes.data() + test_fixture::SceImageLayout::dynamic_data_offset + string_size,
                    name.data(), name.size());
        string_size += name.size();
        bytes[test_fixture::SceImageLayout::dynamic_data_offset + string_size++] = std::byte{0};
        test_fixture::put_dynamic(bytes, dynamic_index++, kDynamicNeeded, offset);
    }
    test_fixture::put_dynamic(bytes, dynamic_index++, kDynamicNull, 0);
    test_fixture::put_dynamic(bytes, 1, kDynamicSceStringTableSize, string_size);

    const auto dynamic_header = std::size_t{64 + 56};
    const auto dynamic_size = static_cast<std::uint64_t>(dynamic_index * 16);
    test_fixture::put(bytes, dynamic_header + 32, dynamic_size);
    test_fixture::put(bytes, dynamic_header + 40, dynamic_size);
    return bytes;
}

std::vector<std::byte> simple_dependency_elf() {
    using namespace nyxora::loader;
    std::vector<std::byte> bytes(0x101);
    bytes[0] = std::byte{0x7f};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'L'};
    bytes[3] = std::byte{'F'};
    bytes[4] = std::byte{2};
    bytes[5] = std::byte{1};
    bytes[6] = std::byte{1};
    test_fixture::put(bytes, 16, kTypeDyn);
    test_fixture::put(bytes, 18, kMachineX86_64);
    test_fixture::put(bytes, 20, std::uint32_t{1});
    test_fixture::put(bytes, 24, std::uint64_t{0});
    test_fixture::put(bytes, 32, std::uint64_t{64});
    test_fixture::put(bytes, 52, std::uint16_t{64});
    test_fixture::put(bytes, 54, std::uint16_t{56});
    test_fixture::put(bytes, 56, std::uint16_t{1});
    test_fixture::put(bytes, 64, kProgramLoad);
    test_fixture::put(bytes, 68, std::uint32_t{5});
    test_fixture::put(bytes, 72, std::uint64_t{0x100});
    test_fixture::put(bytes, 80, std::uint64_t{0});
    test_fixture::put(bytes, 96, std::uint64_t{1});
    test_fixture::put(bytes, 104, std::uint64_t{0x1000});
    test_fixture::put(bytes, 112, std::uint64_t{0x1000});
    bytes[0x100] = std::byte{0xc3};
    return bytes;
}

std::vector<std::byte> with_process_param(std::vector<std::byte> bytes,
                                          std::uint64_t virtual_address = 0x1060,
                                          std::uint64_t size = 0x20) {
    test_fixture::put(bytes, 56, std::uint16_t{5});
    const auto header = std::size_t{64 + 4 * 56};
    test_fixture::put(bytes, header, nyxora::loader::kProgramSceProcParam);
    test_fixture::put(bytes, header + 4, std::uint32_t{4});
    test_fixture::put(bytes, header + 8,
                      std::uint64_t{test_fixture::SceImageLayout::load_offset + 0x60});
    test_fixture::put(bytes, header + 16, virtual_address);
    test_fixture::put(bytes, header + 24, std::uint64_t{0});
    test_fixture::put(bytes, header + 32, size);
    test_fixture::put(bytes, header + 40, size);
    test_fixture::put(bytes, header + 48, std::uint64_t{8});
    return bytes;
}

struct TempTree {
    std::filesystem::path root;

    TempTree() {
        root = std::filesystem::temp_directory_path() /
               ("nyxora-module-lifecycle-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(root);
    }

    ~TempTree() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

std::array<std::byte, 21> trace_function(nyxora::GuestAddress trace, std::uint8_t value) {
    std::array<std::byte, 21> code{
        std::byte{0x48}, std::byte{0xb8},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0x0f}, std::byte{0xb6}, std::byte{0x08},
        std::byte{0xc6}, std::byte{0x44}, std::byte{0x08}, std::byte{0x01}, std::byte{0},
        std::byte{0xfe}, std::byte{0x00}, std::byte{0xc3},
    };
    std::memcpy(code.data() + 2, &trace, sizeof(trace));
    code[17] = static_cast<std::byte>(value);
    return code;
}

void write_pointer_array(nyxora::runtime::Runtime& runtime, nyxora::GuestAddress address,
                         std::span<const nyxora::GuestAddress> values) {
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(values.data()), values.size_bytes());
    NYXORA_CHECK(runtime.memory().write(address, bytes));
}

} // namespace

NYXORA_TEST(runtime_load_program_discovers_needed_files_and_skips_core_hle_module_file) {
#if defined(__x86_64__) || defined(_M_X64)
    TempTree tree;
    const std::array<std::string_view, 2> needed{"libdep.prx", "libkernel.prx"};
    const auto main_bytes = add_needed(test_fixture::sce_dynamic_elf(), needed);
    const auto dependency_bytes = simple_dependency_elf();
    const auto main_path = tree.root / "main.elf";
    const auto dependency_path = tree.root / "libdep.prx";
    write_file(main_path, main_bytes);
    write_file(dependency_path, dependency_bytes);

    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 12U);
    NYXORA_CHECK(memory.has_value());
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    auto program = runtime.load_program(main_path);

    NYXORA_CHECK(program.modules.size() == 2);
    NYXORA_CHECK(program.main_module == 0);
    NYXORA_CHECK(program.modules[0].entry >= runtime.memory().native_base());
    NYXORA_CHECK(program.modules[0].entry < runtime.memory().native_base() +
                                                  runtime.memory().native_size());
    NYXORA_CHECK(program.modules[0].path.filename() == "main.elf");
    NYXORA_CHECK(program.modules[1].path.filename() == "libdep.prx");
    NYXORA_CHECK(program.dependencies[0].size() == 1);
    NYXORA_CHECK(program.dependencies[0][0] == 1);
    NYXORA_CHECK(program.dependencies[1].empty());
#endif
}

NYXORA_TEST(runtime_load_program_discovers_cycles_before_two_phase_linking) {
#if defined(__x86_64__) || defined(_M_X64)
    TempTree tree;
    const std::array<std::string_view, 1> main_needed{"libdep.prx"};
    const std::array<std::string_view, 1> dep_needed{"main.elf"};
    const auto main_bytes = add_needed(test_fixture::sce_dynamic_elf(), main_needed);
    const auto dependency_bytes = add_needed(test_fixture::sce_dynamic_elf(), dep_needed);
    const auto main_path = tree.root / "main.elf";
    write_file(main_path, main_bytes);
    write_file(tree.root / "libdep.prx", dependency_bytes);

    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 16U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    auto program = runtime.load_program(main_path, base);

    NYXORA_CHECK(program.modules.size() == 2);
    NYXORA_CHECK(program.dependencies[0] == std::vector<std::size_t>{1});
    NYXORA_CHECK(program.dependencies[1] == std::vector<std::size_t>{0});
    NYXORA_CHECK(program.modules[0].tls_module_id != program.modules[1].tls_module_id);
#endif
}

NYXORA_TEST(runtime_load_program_rejects_missing_and_root_escape_dependencies_before_mapping) {
    TempTree tree;
    const auto main_path = tree.root / "main.elf";
    {
        const std::array<std::string_view, 1> missing{"missing.prx"};
        const auto bytes = add_needed(test_fixture::sce_dynamic_elf(), missing);
        write_file(main_path, bytes);
        nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
        bool rejected = false;
        try {
            (void)runtime.load_program(main_path, 0x500000000ULL);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        NYXORA_CHECK(rejected);
        NYXORA_CHECK(runtime.memory().regions().empty());
    }

    const auto outside = tree.root.parent_path() / "nyxora-outside-dependency.prx";
    write_file(outside, simple_dependency_elf());
    struct OutsideCleanup {
        std::filesystem::path path;
        ~OutsideCleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } outside_cleanup{outside};
    const std::array<std::string_view, 1> escaping{"../nyxora-outside-dependency.prx"};
    const auto escaping_bytes = add_needed(test_fixture::sce_dynamic_elf(), escaping);
    write_file(main_path, escaping_bytes);
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    bool rejected = false;
    try {
        (void)runtime.load_program(main_path, 0x500000000ULL);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    NYXORA_CHECK(rejected);
    NYXORA_CHECK(runtime.memory().regions().empty());
}

NYXORA_TEST(runtime_records_only_mapped_process_parameter_ranges) {
    constexpr nyxora::GuestAddress base = 0x520000000ULL;
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    const auto image = nyxora::loader::Elf64Image::from_bytes(
        with_process_param(test_fixture::sce_dynamic_elf()));
    const auto module = runtime.load_image(image, "process-param.elf", base);
    NYXORA_CHECK(module.process_param == base + 0x1060);
    NYXORA_CHECK(module.process_param_size == 0x20);

    nyxora::runtime::Runtime invalid_runtime(std::make_unique<nyxora::gpu::NullBackend>());
    const auto invalid_image = nyxora::loader::Elf64Image::from_bytes(
        with_process_param(test_fixture::sce_dynamic_elf(), 0x9000, 0x20));
    bool rejected = false;
    try {
        (void)invalid_runtime.load_image(invalid_image, "bad-process-param.elf", base);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    NYXORA_CHECK(rejected);
}

NYXORA_TEST(runtime_program_lifecycle_obeys_dependency_and_array_order_once) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto page = static_cast<nyxora::GuestSize>(nyxora::memory::NativeArena::page_size());
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 6U);
    NYXORA_CHECK(memory.has_value());
    const auto arena = memory->native_base();
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));

    const auto main_base = arena;
    const auto main_data = arena + page;
    const auto dep_base = arena + page * 2U;
    const auto dep_data = arena + page * 3U;
    const auto trace = arena + page * 4U;
    NYXORA_CHECK(runtime.memory().map(main_base, page,
                                      nyxora::memory::Protection::read |
                                          nyxora::memory::Protection::write,
                                      "main-code"));
    NYXORA_CHECK(runtime.memory().map(main_data, page,
                                      nyxora::memory::Protection::read |
                                          nyxora::memory::Protection::write,
                                      "main-data"));
    NYXORA_CHECK(runtime.memory().map(dep_base, page,
                                      nyxora::memory::Protection::read |
                                          nyxora::memory::Protection::write,
                                      "dep-code"));
    NYXORA_CHECK(runtime.memory().map(dep_data, page,
                                      nyxora::memory::Protection::read |
                                          nyxora::memory::Protection::write,
                                      "dep-data"));
    NYXORA_CHECK(runtime.memory().map(trace, page,
                                      nyxora::memory::Protection::read |
                                          nyxora::memory::Protection::write,
                                      "lifecycle-trace"));

    auto emit = [&](nyxora::GuestAddress base, std::size_t slot, std::uint8_t value) {
        const auto address = base + 0x20 + slot * 0x20;
        const auto code = trace_function(trace, value);
        NYXORA_CHECK(runtime.memory().write(address, code));
        return address;
    };

    const auto main_preinit = emit(main_base, 0, 1);
    const auto main_init = emit(main_base, 1, 4);
    const auto main_init_a = emit(main_base, 2, 5);
    const auto main_init_b = emit(main_base, 3, 6);
    const auto main_fini_a = emit(main_base, 4, 7);
    const auto main_fini_b = emit(main_base, 5, 8);
    const auto main_fini = emit(main_base, 6, 9);
    const auto dep_init = emit(dep_base, 0, 2);
    const auto dep_init_array = emit(dep_base, 1, 3);
    const auto dep_fini_array = emit(dep_base, 2, 10);
    const auto dep_fini = emit(dep_base, 3, 11);

    const std::array<nyxora::GuestAddress, 1> main_preinit_array{main_preinit};
    const std::array<nyxora::GuestAddress, 2> main_init_array{main_init_a, main_init_b};
    const std::array<nyxora::GuestAddress, 2> main_fini_array{main_fini_a, main_fini_b};
    const std::array<nyxora::GuestAddress, 1> dep_init_functions{dep_init_array};
    const std::array<nyxora::GuestAddress, 1> dep_fini_functions{dep_fini_array};
    write_pointer_array(runtime, main_data, main_preinit_array);
    write_pointer_array(runtime, main_data + 0x20, main_init_array);
    write_pointer_array(runtime, main_data + 0x40, main_fini_array);
    write_pointer_array(runtime, dep_data, dep_init_functions);
    write_pointer_array(runtime, dep_data + 0x20, dep_fini_functions);

    NYXORA_CHECK(runtime.memory().protect(main_base, page,
                                          nyxora::memory::Protection::read |
                                              nyxora::memory::Protection::execute));
    NYXORA_CHECK(runtime.memory().protect(dep_base, page,
                                          nyxora::memory::Protection::read |
                                              nyxora::memory::Protection::execute));

    nyxora::loader::DynamicInfo main_dynamic;
    main_dynamic.preinit_array = main_data - main_base;
    main_dynamic.preinit_array_size = sizeof(main_preinit_array);
    main_dynamic.init = main_init - main_base;
    main_dynamic.init_array = main_data + 0x20 - main_base;
    main_dynamic.init_array_size = sizeof(main_init_array);
    main_dynamic.fini_array = main_data + 0x40 - main_base;
    main_dynamic.fini_array_size = sizeof(main_fini_array);
    main_dynamic.fini = main_fini - main_base;

    nyxora::loader::DynamicInfo dep_dynamic;
    dep_dynamic.init = dep_init - dep_base;
    dep_dynamic.init_array = dep_data - dep_base;
    dep_dynamic.init_array_size = sizeof(dep_init_functions);
    dep_dynamic.fini_array = dep_data + 0x20 - dep_base;
    dep_dynamic.fini_array_size = sizeof(dep_fini_functions);
    dep_dynamic.fini = dep_fini - dep_base;

    nyxora::runtime::LoadedProgram program;
    program.modules.resize(2);
    program.modules[0].path = "main.elf";
    program.modules[0].base = main_base;
    program.modules[0].dynamic = main_dynamic;
    program.modules[1].path = "libdep.prx";
    program.modules[1].base = dep_base;
    program.modules[1].dynamic = dep_dynamic;
    program.dependencies = {{1}, {}};
    program.main_module = 0;

    runtime.initialize_program(program, 64 * 1024);
    runtime.initialize_program(program, 64 * 1024);
    NYXORA_CHECK(program.initialized_modules == std::vector<std::size_t>({1, 0}));

    runtime.finalize_program(program, 64 * 1024);
    runtime.finalize_program(program, 64 * 1024);
    NYXORA_CHECK(program.finalized);
    NYXORA_CHECK(program.initialized_modules.empty());

    const std::array<std::uint8_t, 11> expected{1, 2, 3, 4, 5, 6, 8, 7, 9, 10, 11};
    const auto trace_bytes = runtime.memory().view(trace, expected.size() + 1U);
    NYXORA_CHECK(trace_bytes.size() == expected.size() + 1U);
    NYXORA_CHECK(static_cast<std::uint8_t>(trace_bytes[0]) == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        NYXORA_CHECK(static_cast<std::uint8_t>(trace_bytes[index + 1]) == expected[index]);
    }
#endif
}

NYXORA_TEST(runtime_program_lifecycle_preserves_completed_dependencies_after_init_failure) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto page = static_cast<nyxora::GuestSize>(nyxora::memory::NativeArena::page_size());
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 5U);
    NYXORA_CHECK(memory.has_value());
    const auto arena = memory->native_base();
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));

    const auto dep_base = arena;
    const auto main_base = arena + page;
    const auto nonexec = arena + page * 2U;
    const auto trace = arena + page * 3U;
    NYXORA_CHECK(runtime.memory().map(dep_base, page,
                                      nyxora::memory::Protection::read |
                                          nyxora::memory::Protection::write,
                                      "dep-code"));
    NYXORA_CHECK(runtime.memory().map(main_base, page,
                                      nyxora::memory::Protection::read |
                                          nyxora::memory::Protection::write,
                                      "main-code"));
    NYXORA_CHECK(runtime.memory().map(nonexec, page, nyxora::memory::Protection::read,
                                      "nonexec-init"));
    NYXORA_CHECK(runtime.memory().map(trace, page,
                                      nyxora::memory::Protection::read |
                                          nyxora::memory::Protection::write,
                                      "failure-trace"));

    const auto dep_init = dep_base + 0x20;
    const auto dep_fini = dep_base + 0x60;
    const auto dep_init_code = trace_function(trace, 1);
    const auto dep_fini_code = trace_function(trace, 2);
    NYXORA_CHECK(runtime.memory().write(dep_init, dep_init_code));
    NYXORA_CHECK(runtime.memory().write(dep_fini, dep_fini_code));
    NYXORA_CHECK(runtime.memory().protect(dep_base, page,
                                          nyxora::memory::Protection::read |
                                              nyxora::memory::Protection::execute));
    NYXORA_CHECK(runtime.memory().protect(main_base, page,
                                          nyxora::memory::Protection::read |
                                              nyxora::memory::Protection::execute));

    nyxora::runtime::LoadedProgram program;
    program.modules.resize(2);
    program.modules[0].base = main_base;
    program.modules[0].dynamic.emplace();
    program.modules[0].dynamic->init = nonexec - main_base;
    program.modules[1].base = dep_base;
    program.modules[1].dynamic.emplace();
    program.modules[1].dynamic->init = dep_init - dep_base;
    program.modules[1].dynamic->fini = dep_fini - dep_base;
    program.dependencies = {{1}, {}};
    program.main_module = 0;

    bool rejected = false;
    try {
        runtime.initialize_program(program, 64 * 1024);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    NYXORA_CHECK(rejected);
    NYXORA_CHECK(program.initialization_failed);
    NYXORA_CHECK(program.initialized_modules == std::vector<std::size_t>{1});

    rejected = false;
    try {
        runtime.initialize_program(program, 64 * 1024);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    NYXORA_CHECK(rejected);
    NYXORA_CHECK(program.initialized_modules == std::vector<std::size_t>{1});

    runtime.finalize_program(program, 64 * 1024);
    NYXORA_CHECK(program.finalized);
    NYXORA_CHECK(program.initialized_modules.empty());
    const auto trace_bytes = runtime.memory().view(trace, 3);
    NYXORA_CHECK(trace_bytes.size() == 3);
    NYXORA_CHECK(static_cast<std::uint8_t>(trace_bytes[0]) == 2);
    NYXORA_CHECK(static_cast<std::uint8_t>(trace_bytes[1]) == 1);
    NYXORA_CHECK(static_cast<std::uint8_t>(trace_bytes[2]) == 2);
#endif
}
