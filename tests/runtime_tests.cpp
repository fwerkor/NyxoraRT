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

namespace {

nyxora::runtime::SymbolKey runtime_import_key() {
    return nyxora::runtime::SymbolKey{
        .nid = "impNid",
        .library = "libTest",
        .module = "modTest",
        .library_version = 3,
        .module_major = 1,
        .module_minor = 2,
        .kind = nyxora::runtime::SymbolKind::function,
    };
}

std::uint64_t runtime_read_u64(const nyxora::runtime::Runtime& runtime,
                               nyxora::GuestAddress address) {
    const auto bytes = runtime.memory().view(address, sizeof(std::uint64_t));
    if (bytes.size() != sizeof(std::uint64_t)) {
        throw std::runtime_error("test read is outside runtime memory");
    }
    std::uint64_t value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

} // namespace

NYXORA_TEST(runtime_loads_sce_metadata_assigns_tls_and_links_known_imports) {
    constexpr nyxora::GuestAddress base = 0x300000000ULL;
    constexpr nyxora::GuestAddress hle_address = 0x777788889999aaaaULL;

    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    NYXORA_CHECK(runtime.symbols().register_symbol(
        runtime_import_key(), nyxora::runtime::SymbolBinding{hle_address, "fixture HLE", true}));

    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    auto module = runtime.load_image(image, "fixture.elf", base);

    NYXORA_CHECK(module.dynamic.has_value());
    NYXORA_CHECK(module.tls.has_value());
    NYXORA_CHECK(module.tls_module_id == 1);
    NYXORA_CHECK(module.relocations.applied == 3);
    NYXORA_CHECK(module.relocations.unresolved.empty());
    NYXORA_CHECK(runtime_read_u64(runtime, base + 0x1000) == base + 0x1040);
    NYXORA_CHECK(runtime_read_u64(runtime, base + 0x1008) == hle_address);
    NYXORA_CHECK(runtime_read_u64(runtime, base + 0x1010) == 1);
}

NYXORA_TEST(runtime_can_relink_after_late_hle_registration) {
    constexpr nyxora::GuestAddress base = 0x400000000ULL;
    constexpr nyxora::GuestAddress hle_address = 0x5555666677778888ULL;

    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    auto module = runtime.load_image(image, "fixture.elf", base);

    NYXORA_CHECK(module.relocations.unresolved.size() == 1);
    NYXORA_CHECK(runtime.symbols().register_symbol(
        runtime_import_key(), nyxora::runtime::SymbolBinding{hle_address, "late HLE", true}));

    const auto report = runtime.relink(module);
    NYXORA_CHECK(report.unresolved.empty());
    NYXORA_CHECK(runtime_read_u64(runtime, base + 0x1008) == hle_address);
}

NYXORA_TEST(runtime_can_load_and_execute_synthetic_sce_entry_natively) {
#if defined(__x86_64__) || defined(_M_X64)
    constexpr nyxora::GuestAddress hle_address = 0x4444555566667777ULL;
    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 3U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();

    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    NYXORA_CHECK(runtime.symbols().register_symbol(
        runtime_import_key(), nyxora::runtime::SymbolBinding{hle_address, "native HLE", true}));

    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    const auto module = runtime.load_image(image, "native-fixture.elf", base);
    NYXORA_CHECK(module.relocations.unresolved.empty());
    NYXORA_CHECK(module.entry == base + 0x1040);

    using Entry = int (*)();
    auto entry = reinterpret_cast<Entry>(module.entry);
    NYXORA_CHECK(entry() == 42);
#endif
}

NYXORA_TEST(runtime_installs_callable_late_import_and_rebinds_it) {
#if defined(__x86_64__) || defined(_M_X64)
    constexpr nyxora::GuestAddress base = 0x600000000ULL;
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    auto module = runtime.load_image(image, "late-fixture.elf", base);

    NYXORA_CHECK(module.relocations.unresolved.size() == 1);
    NYXORA_CHECK(module.relocations.late_thunks == 1);
    const auto thunk = module.relocations.unresolved[0].thunk_address;
    NYXORA_CHECK(thunk != 0);
    NYXORA_CHECK(runtime_read_u64(runtime, base + 0x1008) == thunk);
    const auto* table = runtime.late_imports();
    NYXORA_CHECK(table != nullptr);
    NYXORA_CHECK(table->size() == 1);

    using Function = std::uint64_t (*)();
    NYXORA_CHECK(reinterpret_cast<Function>(thunk)() == 0);
    NYXORA_CHECK(table->call_count(0) == 1);

    const auto page = nyxora::memory::NativeArena::page_size();
    auto target = nyxora::memory::NativeArena::reserve(page);
    NYXORA_CHECK(target.has_value());
    NYXORA_CHECK(target->protect(0, page,
                                 nyxora::memory::Protection::read |
                                     nyxora::memory::Protection::write));
    const std::array<std::byte, 11> return_91{
        std::byte{0x48}, std::byte{0xb8}, std::byte{0x5b}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xc3},
    };
    NYXORA_CHECK(target->copy(0, return_91));
    NYXORA_CHECK(target->flush_instruction_cache(0, return_91.size()));
    NYXORA_CHECK(target->protect(0, page,
                                 nyxora::memory::Protection::read |
                                     nyxora::memory::Protection::execute));
    const auto target_address =
        reinterpret_cast<nyxora::GuestAddress>(target->host_pointer());
    NYXORA_CHECK(runtime.symbols().register_symbol(
        runtime_import_key(), nyxora::runtime::SymbolBinding{target_address, "late target", true}));

    const auto report = runtime.relink(module);
    NYXORA_CHECK(report.unresolved.empty());
    NYXORA_CHECK(runtime_read_u64(runtime, base + 0x1008) == target_address);
    NYXORA_CHECK(reinterpret_cast<Function>(thunk)() == 91);
    NYXORA_CHECK(table->call_count(0) == 1);
#endif
}

NYXORA_TEST(runtime_invoke_entry_uses_native_guest_thread_path) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 3U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();

    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    NYXORA_CHECK(runtime.symbols().register_symbol(
        runtime_import_key(), nyxora::runtime::SymbolBinding{0x12345678, "unused HLE", true}));
    const auto image = nyxora::loader::Elf64Image::from_bytes(test_fixture::sce_dynamic_elf());
    const auto module = runtime.load_image(image, "invoke-fixture.elf", base);

    NYXORA_CHECK(nyxora::runtime::ScopedGuestThreadContext::current() == nullptr);
    NYXORA_CHECK(runtime.invoke_entry(module, 64 * 1024) == 42);
    auto guest_thread = runtime.start_thread(module, 64 * 1024);
    const auto guest_result = guest_thread.join();
    NYXORA_CHECK(!guest_result.fault.has_value());
    NYXORA_CHECK(guest_result.value == 42);
    NYXORA_CHECK(nyxora::runtime::ScopedGuestThreadContext::current() == nullptr);
#endif
}


NYXORA_TEST(runtime_rewrites_fs_tcb_access_and_executes_it_on_supported_x64_hosts) {
#if (defined(__linux__) && defined(__x86_64__)) || (defined(_WIN32) && defined(_M_X64))
    auto bytes = test_fixture::sce_dynamic_elf();
    const std::array<std::byte, 10> entry_code{
        std::byte{0x64}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xc3},
    };
    std::memcpy(bytes.data() + test_fixture::SceImageLayout::load_offset + 0x40,
                entry_code.data(), entry_code.size());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 3U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    const auto image = nyxora::loader::Elf64Image::from_bytes(std::move(bytes));
    const auto module = runtime.load_image(image, "fs-tcb.elf", base);

    const auto patched = runtime.memory().view(module.entry, entry_code.size());
    NYXORA_CHECK(patched.size() == entry_code.size());
    NYXORA_CHECK(patched[0] == std::byte{0x65});
    const auto result = runtime.invoke_entry(module, 64 * 1024);
    NYXORA_CHECK(result != 0);
#endif
}


NYXORA_TEST(runtime_executes_nonzero_fs_tcb_mov_on_supported_x64_hosts) {
#if (defined(__linux__) && defined(__x86_64__)) || (defined(_WIN32) && defined(_M_X64))
    auto bytes = test_fixture::sce_dynamic_elf();
    const std::array<std::byte, 10> entry_code{
        std::byte{0x64}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xc3},
    };
    std::memcpy(bytes.data() + test_fixture::SceImageLayout::load_offset + 0x40,
                entry_code.data(), entry_code.size());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 3U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    const auto image = nyxora::loader::Elf64Image::from_bytes(std::move(bytes));
    const auto module = runtime.load_image(image, "fs-tcb-thread.elf", base);

    const auto patched = runtime.memory().view(module.entry, entry_code.size());
    NYXORA_CHECK(patched.size() == entry_code.size());
#if defined(_WIN32)
    NYXORA_CHECK(patched[0] == std::byte{0xe9});
#else
    NYXORA_CHECK(patched[0] == std::byte{0x65});
#endif
    NYXORA_CHECK(runtime.invoke_entry(module, 64 * 1024) == 0);
#endif
}

NYXORA_TEST(runtime_preserves_cmp_flags_through_nonzero_tcb_thunk) {
#if (defined(__linux__) && defined(__x86_64__)) || (defined(_WIN32) && defined(_M_X64))
    auto bytes = test_fixture::sce_dynamic_elf();
    const std::array<std::byte, 25> entry_code{
        // mov rax, fs:[8]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // cmp rax, fs:[8]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x3b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // sete al; movzx eax,al; ret
        std::byte{0x0f}, std::byte{0x94}, std::byte{0xc0},
        std::byte{0x0f}, std::byte{0xb6}, std::byte{0xc0}, std::byte{0xc3},
    };
    std::memcpy(bytes.data() + test_fixture::SceImageLayout::load_offset + 0x40,
                entry_code.data(), entry_code.size());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 3U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    const auto image = nyxora::loader::Elf64Image::from_bytes(std::move(bytes));
    const auto module = runtime.load_image(image, "fs-tcb-cmp.elf", base);
    NYXORA_CHECK(runtime.invoke_entry(module, 64 * 1024) == 1);
#endif
}

NYXORA_TEST(runtime_preserves_xor_flags_through_nonzero_tcb_thunk) {
#if (defined(__linux__) && defined(__x86_64__)) || (defined(_WIN32) && defined(_M_X64))
    auto bytes = test_fixture::sce_dynamic_elf();
    const std::array<std::byte, 25> entry_code{
        // mov rax, fs:[8]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // xor rax, fs:[8]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x33}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // sete al; movzx eax,al; ret
        std::byte{0x0f}, std::byte{0x94}, std::byte{0xc0},
        std::byte{0x0f}, std::byte{0xb6}, std::byte{0xc0}, std::byte{0xc3},
    };
    std::memcpy(bytes.data() + test_fixture::SceImageLayout::load_offset + 0x40,
                entry_code.data(), entry_code.size());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 3U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    const auto image = nyxora::loader::Elf64Image::from_bytes(std::move(bytes));
    const auto module = runtime.load_image(image, "fs-tcb-xor.elf", base);
    NYXORA_CHECK(runtime.invoke_entry(module, 64 * 1024) == 1);
#endif
}


NYXORA_TEST(runtime_fault_capture_remains_active_after_nonzero_tcb_patch) {
#if (defined(__linux__) && defined(__x86_64__)) || (defined(_WIN32) && defined(_M_X64))
    auto bytes = test_fixture::sce_dynamic_elf();
    const std::array<std::byte, 15> entry_code{
        // mov rax, fs:[8]
        std::byte{0x64}, std::byte{0x48}, std::byte{0x8b}, std::byte{0x04}, std::byte{0x25},
        std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        // xor eax,eax; mov rax,[rax]; ret
        std::byte{0x31}, std::byte{0xc0},
        std::byte{0x48}, std::byte{0x8b}, std::byte{0x00}, std::byte{0xc3},
    };
    std::memcpy(bytes.data() + test_fixture::SceImageLayout::load_offset + 0x40,
                entry_code.data(), entry_code.size());

    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 3U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    const auto image = nyxora::loader::Elf64Image::from_bytes(std::move(bytes));
    const auto module = runtime.load_image(image, "fs-tcb-fault.elf", base);

    try {
        (void)runtime.invoke_entry(module, 64 * 1024);
        NYXORA_CHECK(false);
    } catch (const nyxora::runtime::GuestFaultException& fault) {
        NYXORA_CHECK(fault.fault().kind == nyxora::runtime::GuestFaultKind::access_violation);
        NYXORA_CHECK(fault.fault().instruction_pointer == module.entry + 11);
    }
#endif
}

NYXORA_TEST(runtime_rejects_null_backend_and_non_native_entry_invocation) {
    bool null_backend = false;
    try {
        nyxora::runtime::Runtime runtime(nullptr);
    } catch (const std::invalid_argument&) {
        null_backend = true;
    }
    NYXORA_CHECK(null_backend);

    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    nyxora::runtime::LoadedModule module{};
    module.entry = 0x1000;
    bool non_native = false;
    try {
        (void)runtime.invoke_entry(module, 64 * 1024);
    } catch (const std::runtime_error&) {
        non_native = true;
    }
    NYXORA_CHECK(non_native);
}

NYXORA_TEST(runtime_rejects_unmapped_and_nonexecutable_native_entries) {
#if defined(__x86_64__) || defined(_M_X64)
    const auto page = nyxora::memory::NativeArena::page_size();
    auto memory = nyxora::memory::GuestAddressSpace::reserve_native(page * 2U);
    NYXORA_CHECK(memory.has_value());
    const auto base = memory->native_base();
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>(),
                                     std::move(*memory));
    nyxora::runtime::LoadedModule module{};
    module.entry = base;

    bool unmapped = false;
    try {
        (void)runtime.invoke_entry(module, 64 * 1024);
    } catch (const std::runtime_error&) {
        unmapped = true;
    }
    NYXORA_CHECK(unmapped);

    NYXORA_CHECK(runtime.memory().map(base, page, nyxora::memory::Protection::read,
                                      "non-executable"));
    bool non_executable = false;
    try {
        (void)runtime.invoke_entry(module, 64 * 1024);
    } catch (const std::runtime_error&) {
        non_executable = true;
    }
    NYXORA_CHECK(non_executable);
#endif
}

NYXORA_TEST(runtime_relink_without_dynamic_metadata_is_a_noop) {
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    nyxora::runtime::LoadedModule module{};
    module.relocations.applied = 7;
    module.relocations.late_thunks = 3;
    const auto report = runtime.relink(module);
    NYXORA_CHECK(report.applied == 0);
    NYXORA_CHECK(report.late_thunks == 0);
    NYXORA_CHECK(report.unresolved.empty());
}

NYXORA_TEST(runtime_load_elf_configures_app0_root_from_executable_directory) {
    const auto root = std::filesystem::temp_directory_path() / "nyxora-runtime-load-elf";
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{root};

    const auto path = root / "fixture.elf";
    const auto bytes = test_fixture::sce_dynamic_elf();
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    constexpr nyxora::GuestAddress base = 0x710000000ULL;
    nyxora::runtime::Runtime runtime(std::make_unique<nyxora::gpu::NullBackend>());
    const auto module = runtime.load_elf(path, base);
    NYXORA_CHECK(module.base == base);
    NYXORA_CHECK(module.dynamic.has_value());
    NYXORA_CHECK(runtime.set_guest_root(root));
}
