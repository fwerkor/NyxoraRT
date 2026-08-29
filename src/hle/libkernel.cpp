#include "nyxora/hle/libkernel.hpp"
#include "nyxora/runtime/thread_manager.hpp"

#include <chrono>
#include <cstdint>

namespace nyxora::hle::libkernel {
namespace {

using Clock = std::chrono::steady_clock;
const auto process_start = Clock::now();

std::uint64_t process_time_us() {
    const auto elapsed = Clock::now() - process_start;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

std::uint64_t process_time_counter() {
    const auto elapsed = Clock::now() - process_start;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

std::uint64_t process_time_counter_frequency() {
    return 1'000'000'000ULL;
}

std::uint64_t current_cpu() {
    return 0;
}

std::uint64_t pthread_create(std::uint64_t thread_out, std::uint64_t attributes,
                             std::uint64_t start_routine, std::uint64_t argument) {
    auto* manager = runtime::GuestThreadManager::current();
    if (manager == nullptr || thread_out == 0) {
        return runtime::GuestThreadManager::kPosixEinval;
    }
    return static_cast<std::uint64_t>(manager->create(
        reinterpret_cast<GuestAddress*>(thread_out), static_cast<GuestAddress>(attributes),
        static_cast<GuestAddress>(start_routine), static_cast<GuestAddress>(argument)));
}

std::uint64_t pthread_join(std::uint64_t thread, std::uint64_t return_value) {
    auto* manager = runtime::GuestThreadManager::current();
    if (manager == nullptr) {
        return runtime::GuestThreadManager::kPosixEinval;
    }
    return static_cast<std::uint64_t>(manager->join(
        static_cast<GuestAddress>(thread),
        return_value == 0 ? nullptr : reinterpret_cast<GuestAddress*>(return_value)));
}

std::uint64_t pthread_self() {
    return static_cast<std::uint64_t>(runtime::GuestThreadManager::current_handle());
}

std::uint64_t pthread_detach(std::uint64_t thread) {
    auto* manager = runtime::GuestThreadManager::current();
    if (manager == nullptr) {
        return runtime::GuestThreadManager::kPosixEinval;
    }
    return static_cast<std::uint64_t>(manager->detach(static_cast<GuestAddress>(thread)));
}

runtime::SymbolKey key(const char* nid, const char* library = "libkernel") {
    return runtime::SymbolKey{
        .nid = nid,
        .library = library,
        .module = "libkernel",
        .library_version = 1,
        .module_major = 1,
        .module_minor = 1,
        .kind = runtime::SymbolKind::function,
    };
}

} // namespace

void register_core(runtime::HleRegistry& registry) {
    (void)registry.register_no_arg(key("4J2sUJmuHZQ"), process_time_us,
                                   "sceKernelGetProcessTime");
    (void)registry.register_no_arg(key("fgxnMeTNUtY"), process_time_counter,
                                   "sceKernelGetProcessTimeCounter");
    (void)registry.register_no_arg(key("BNowx2l588E"), process_time_counter_frequency,
                                   "sceKernelGetProcessTimeCounterFrequency");
    (void)registry.register_no_arg(key("g0VTBxfJyu0"), current_cpu,
                                   "sceKernelGetCurrentCpu");

    const auto create_address = reinterpret_cast<GuestAddress>(&pthread_create);
    const auto join_address = reinterpret_cast<GuestAddress>(&pthread_join);
    const auto self_address = reinterpret_cast<GuestAddress>(&pthread_self);
    const auto detach_address = reinterpret_cast<GuestAddress>(&pthread_detach);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("OxhIB8LB-PQ", library), create_address,
                                         "pthread_create");
        (void)registry.register_function(key("h9CcP3J0oVM", library), join_address,
                                         "pthread_join");
        (void)registry.register_function(key("EotR8a3ASf4", library), self_address,
                                         "pthread_self");
        (void)registry.register_function(key("+U1R4WtXvoc", library), detach_address,
                                         "pthread_detach");
    }
}

} // namespace nyxora::hle::libkernel
