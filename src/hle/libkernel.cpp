#include "nyxora/hle/libkernel.hpp"

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

runtime::SymbolKey key(const char* nid) {
    return runtime::SymbolKey{
        .nid = nid,
        .library = "libkernel",
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
}

} // namespace nyxora::hle::libkernel
