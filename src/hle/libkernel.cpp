#include "nyxora/hle/libkernel.hpp"
#include "nyxora/runtime/fault.hpp"
#include "nyxora/runtime/thread_manager.hpp"
#include "nyxora/runtime/kernel_services.hpp"

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

runtime::KernelServices* kernel_services() {
    auto* manager = runtime::GuestThreadManager::current();
    return manager == nullptr ? nullptr : manager->kernel_services();
}

std::uint64_t signed_result(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

std::uint64_t kernel_error_result(std::uint32_t value) {
    return signed_result(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

std::uint64_t orbis_pthread_result(int result) {
    if (result == 0) {
        return 0;
    }
    const auto value = 0x80020000U + static_cast<std::uint32_t>(result);
    return signed_result(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

std::uint64_t direct_memory_size() {
    auto* services = kernel_services();
    return services == nullptr ? 0 : services->direct_memory_size();
}

std::uint64_t kernel_mprotect(std::uint64_t address, std::uint64_t size,
                              std::uint64_t protection) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->mprotect(static_cast<GuestAddress>(address),
                                            static_cast<GuestSize>(size),
                                            static_cast<std::uint32_t>(protection)));
}

std::uint64_t kernel_open(std::uint64_t path, std::uint64_t flags, std::uint64_t mode) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->open_readonly(static_cast<GuestAddress>(path),
                                                 static_cast<std::uint32_t>(flags),
                                                 static_cast<std::uint16_t>(mode)));
}

std::uint64_t kernel_read(std::uint64_t fd, std::uint64_t buffer, std::uint64_t size) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEbadf);
    }
    return signed_result(services->read(static_cast<int>(static_cast<std::int32_t>(fd)),
                                        static_cast<GuestAddress>(buffer),
                                        static_cast<GuestSize>(size)));
}

std::uint64_t kernel_close(std::uint64_t fd) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEbadf);
    }
    return signed_result(services->close(static_cast<int>(static_cast<std::int32_t>(fd))));
}

std::uint64_t posix_mutex_init(std::uint64_t mutex, std::uint64_t attributes) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->mutex_init(
                                     static_cast<GuestAddress>(mutex),
                                     static_cast<GuestAddress>(attributes), 0));
}

std::uint64_t sce_mutex_init(std::uint64_t mutex, std::uint64_t attributes,
                             std::uint64_t name) {
    auto* services = kernel_services();
    const auto result = services == nullptr
                            ? runtime::KernelServices::kPosixEinval
                            : services->mutex_init(static_cast<GuestAddress>(mutex),
                                                   static_cast<GuestAddress>(attributes),
                                                   static_cast<GuestAddress>(name));
    return orbis_pthread_result(result);
}

std::uint64_t posix_mutex_lock(std::uint64_t mutex) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(
                                     services->mutex_lock(static_cast<GuestAddress>(mutex)));
}

std::uint64_t posix_mutex_unlock(std::uint64_t mutex) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(
                                     services->mutex_unlock(static_cast<GuestAddress>(mutex)));
}

std::uint64_t posix_mutex_destroy(std::uint64_t mutex) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(
                                     services->mutex_destroy(static_cast<GuestAddress>(mutex)));
}

std::uint64_t orbis_mutex_lock(std::uint64_t mutex) {
    auto* services = kernel_services();
    return orbis_pthread_result(services == nullptr
                                    ? runtime::KernelServices::kPosixEinval
                                    : services->mutex_lock(static_cast<GuestAddress>(mutex)));
}

std::uint64_t orbis_mutex_unlock(std::uint64_t mutex) {
    auto* services = kernel_services();
    return orbis_pthread_result(services == nullptr
                                    ? runtime::KernelServices::kPosixEinval
                                    : services->mutex_unlock(static_cast<GuestAddress>(mutex)));
}

std::uint64_t orbis_mutex_destroy(std::uint64_t mutex) {
    auto* services = kernel_services();
    return orbis_pthread_result(services == nullptr
                                    ? runtime::KernelServices::kPosixEinval
                                    : services->mutex_destroy(static_cast<GuestAddress>(mutex)));
}

std::uint64_t posix_cond_init(std::uint64_t cond, std::uint64_t attributes) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->cond_init(
                                     static_cast<GuestAddress>(cond),
                                     static_cast<GuestAddress>(attributes)));
}

std::uint64_t sce_cond_init(std::uint64_t cond, std::uint64_t attributes,
                            std::uint64_t name) {
    auto* services = kernel_services();
    const auto result = services == nullptr
                            ? runtime::KernelServices::kPosixEinval
                            : services->cond_init(static_cast<GuestAddress>(cond),
                                                  static_cast<GuestAddress>(attributes),
                                                  static_cast<GuestAddress>(name));
    return orbis_pthread_result(result);
}

std::uint64_t posix_cond_destroy(std::uint64_t cond) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(
                                     services->cond_destroy(static_cast<GuestAddress>(cond)));
}

std::uint64_t posix_cond_wait(std::uint64_t cond, std::uint64_t mutex) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->cond_wait(
                                     static_cast<GuestAddress>(cond),
                                     static_cast<GuestAddress>(mutex)));
}

std::uint64_t posix_cond_signal(std::uint64_t cond) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(
                                     services->cond_signal(static_cast<GuestAddress>(cond)));
}

std::uint64_t posix_cond_broadcast(std::uint64_t cond) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(
                                     services->cond_broadcast(static_cast<GuestAddress>(cond)));
}

std::uint64_t orbis_cond_destroy(std::uint64_t cond) {
    return orbis_pthread_result(static_cast<int>(posix_cond_destroy(cond)));
}

std::uint64_t orbis_cond_wait(std::uint64_t cond, std::uint64_t mutex) {
    return orbis_pthread_result(static_cast<int>(posix_cond_wait(cond, mutex)));
}

std::uint64_t orbis_cond_signal(std::uint64_t cond) {
    return orbis_pthread_result(static_cast<int>(posix_cond_signal(cond)));
}

std::uint64_t orbis_cond_broadcast(std::uint64_t cond) {
    return orbis_pthread_result(static_cast<int>(posix_cond_broadcast(cond)));
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

[[noreturn]] void pthread_exit(std::uint64_t status) noexcept {
    runtime::terminate_guest_execution(status);
}

std::uint64_t pthread_attr_init(std::uint64_t attribute) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->thread_attr_init(
                                     static_cast<GuestAddress>(attribute)));
}

std::uint64_t pthread_attr_destroy(std::uint64_t attribute) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->thread_attr_destroy(
                                     static_cast<GuestAddress>(attribute)));
}

std::uint64_t pthread_attr_get_stack_size(std::uint64_t attribute, std::uint64_t output) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->thread_attr_get_stack_size(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<GuestAddress>(output)));
}

std::uint64_t pthread_attr_set_stack_size(std::uint64_t attribute, std::uint64_t stack_size) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->thread_attr_set_stack_size(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<GuestSize>(stack_size)));
}

std::uint64_t pthread_attr_get_detach_state(std::uint64_t attribute, std::uint64_t output) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->thread_attr_get_detach_state(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<GuestAddress>(output)));
}

std::uint64_t pthread_attr_set_detach_state(std::uint64_t attribute, std::uint64_t detach_state) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->thread_attr_set_detach_state(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<int>(detach_state)));
}

std::uint64_t orbis_attr_init(std::uint64_t attribute) {
    return orbis_pthread_result(static_cast<int>(pthread_attr_init(attribute)));
}

std::uint64_t orbis_attr_destroy(std::uint64_t attribute) {
    return orbis_pthread_result(static_cast<int>(pthread_attr_destroy(attribute)));
}

std::uint64_t orbis_attr_get_stack_size(std::uint64_t attribute, std::uint64_t output) {
    return orbis_pthread_result(static_cast<int>(pthread_attr_get_stack_size(attribute, output)));
}

std::uint64_t orbis_attr_set_stack_size(std::uint64_t attribute, std::uint64_t stack_size) {
    return orbis_pthread_result(static_cast<int>(pthread_attr_set_stack_size(attribute, stack_size)));
}

std::uint64_t orbis_attr_get_detach_state(std::uint64_t attribute, std::uint64_t output) {
    return orbis_pthread_result(static_cast<int>(pthread_attr_get_detach_state(attribute, output)));
}

std::uint64_t orbis_attr_set_detach_state(std::uint64_t attribute, std::uint64_t detach_state) {
    return orbis_pthread_result(static_cast<int>(pthread_attr_set_detach_state(attribute, detach_state)));
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
    (void)registry.register_no_arg(key("pO96TwzOm5E"), direct_memory_size,
                                   "sceKernelGetDirectMemorySize");
    (void)registry.register_function(key("vSMAm3cxYTY"),
                                     reinterpret_cast<GuestAddress>(&kernel_mprotect),
                                     "sceKernelMprotect");
    (void)registry.register_function(key("1G3lF1Gg1k8"),
                                     reinterpret_cast<GuestAddress>(&kernel_open),
                                     "sceKernelOpen");
    (void)registry.register_function(key("Cg4srZ6TKbU"),
                                     reinterpret_cast<GuestAddress>(&kernel_read),
                                     "sceKernelRead");
    (void)registry.register_function(key("UK2Tl2DWUns"),
                                     reinterpret_cast<GuestAddress>(&kernel_close),
                                     "sceKernelClose");

    const auto create_address = reinterpret_cast<GuestAddress>(&pthread_create);
    const auto join_address = reinterpret_cast<GuestAddress>(&pthread_join);
    const auto self_address = reinterpret_cast<GuestAddress>(&pthread_self);
    const auto detach_address = reinterpret_cast<GuestAddress>(&pthread_detach);
    const auto exit_address = reinterpret_cast<GuestAddress>(&pthread_exit);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("OxhIB8LB-PQ", library), create_address,
                                         "pthread_create");
        (void)registry.register_function(key("h9CcP3J0oVM", library), join_address,
                                         "pthread_join");
        (void)registry.register_function(key("EotR8a3ASf4", library), self_address,
                                         "pthread_self");
        (void)registry.register_function(key("+U1R4WtXvoc", library), detach_address,
                                         "pthread_detach");
        (void)registry.register_function(key("FJrT5LuUBAU", library), exit_address,
                                         "pthread_exit");
    }
    (void)registry.register_function(key("3kg7rT0NQIs"), exit_address, "scePthreadExit");

    const auto attr_init_address = reinterpret_cast<GuestAddress>(&pthread_attr_init);
    const auto attr_destroy_address = reinterpret_cast<GuestAddress>(&pthread_attr_destroy);
    const auto attr_get_stack_size_address = reinterpret_cast<GuestAddress>(&pthread_attr_get_stack_size);
    const auto attr_set_stack_size_address = reinterpret_cast<GuestAddress>(&pthread_attr_set_stack_size);
    const auto attr_get_detach_state_address =
        reinterpret_cast<GuestAddress>(&pthread_attr_get_detach_state);
    const auto attr_set_detach_state_address =
        reinterpret_cast<GuestAddress>(&pthread_attr_set_detach_state);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("wtkt-teR1so", library), attr_init_address,
                                         "pthread_attr_init");
        (void)registry.register_function(key("zHchY8ft5pk", library), attr_destroy_address,
                                         "pthread_attr_destroy");
        (void)registry.register_function(key("0qOtCR-ZHck", library), attr_get_stack_size_address,
                                         "pthread_attr_getstacksize");
        (void)registry.register_function(key("2Q0z6rnBrTE", library), attr_set_stack_size_address,
                                         "pthread_attr_setstacksize");
        (void)registry.register_function(key("VUT1ZSrHT0I", library), attr_get_detach_state_address,
                                         "pthread_attr_getdetachstate");
        (void)registry.register_function(key("E+tyo3lp5Lw", library), attr_set_detach_state_address,
                                         "pthread_attr_setdetachstate");
    }
    (void)registry.register_function(key("nsYoNRywwNg"),
                                     reinterpret_cast<GuestAddress>(&orbis_attr_init),
                                     "scePthreadAttrInit");
    (void)registry.register_function(key("62KCwEMmzcM"),
                                     reinterpret_cast<GuestAddress>(&orbis_attr_destroy),
                                     "scePthreadAttrDestroy");
    (void)registry.register_function(key("-fA+7ZlGDQs"),
                                     reinterpret_cast<GuestAddress>(&orbis_attr_get_stack_size),
                                     "scePthreadAttrGetstacksize");
    (void)registry.register_function(key("UTXzJbWhhTE"),
                                     reinterpret_cast<GuestAddress>(&orbis_attr_set_stack_size),
                                     "scePthreadAttrSetstacksize");
    (void)registry.register_function(key("JaRMy+QcpeU"),
                                     reinterpret_cast<GuestAddress>(&orbis_attr_get_detach_state),
                                     "scePthreadAttrGetdetachstate");
    (void)registry.register_function(key("-Wreprtu0Qs"),
                                     reinterpret_cast<GuestAddress>(&orbis_attr_set_detach_state),
                                     "scePthreadAttrSetdetachstate");

    (void)registry.register_function(key("cmo1RIYva9o"),
                                     reinterpret_cast<GuestAddress>(&sce_mutex_init),
                                     "scePthreadMutexInit");
    (void)registry.register_function(key("9UK1vLZQft4"),
                                     reinterpret_cast<GuestAddress>(&orbis_mutex_lock),
                                     "scePthreadMutexLock");
    (void)registry.register_function(key("tn3VlD0hG60"),
                                     reinterpret_cast<GuestAddress>(&orbis_mutex_unlock),
                                     "scePthreadMutexUnlock");
    (void)registry.register_function(key("2Of0f+3mhhE"),
                                     reinterpret_cast<GuestAddress>(&orbis_mutex_destroy),
                                     "scePthreadMutexDestroy");

    const auto mutex_init_address = reinterpret_cast<GuestAddress>(&posix_mutex_init);
    const auto mutex_lock_address = reinterpret_cast<GuestAddress>(&posix_mutex_lock);
    const auto mutex_unlock_address = reinterpret_cast<GuestAddress>(&posix_mutex_unlock);
    const auto mutex_destroy_address = reinterpret_cast<GuestAddress>(&posix_mutex_destroy);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("ttHNfU+qDBU", library), mutex_init_address,
                                         "pthread_mutex_init");
        (void)registry.register_function(key("7H0iTOciTLo", library), mutex_lock_address,
                                         "pthread_mutex_lock");
        (void)registry.register_function(key("2Z+PpY6CaJg", library), mutex_unlock_address,
                                         "pthread_mutex_unlock");
        (void)registry.register_function(key("ltCfaGr2JGE", library), mutex_destroy_address,
                                         "pthread_mutex_destroy");
    }

    const auto cond_init_address = reinterpret_cast<GuestAddress>(&posix_cond_init);
    const auto cond_destroy_address = reinterpret_cast<GuestAddress>(&posix_cond_destroy);
    const auto cond_wait_address = reinterpret_cast<GuestAddress>(&posix_cond_wait);
    const auto cond_signal_address = reinterpret_cast<GuestAddress>(&posix_cond_signal);
    const auto cond_broadcast_address = reinterpret_cast<GuestAddress>(&posix_cond_broadcast);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("0TyVk4MSLt0", library), cond_init_address,
                                         "pthread_cond_init");
        (void)registry.register_function(key("RXXqi4CtF8w", library), cond_destroy_address,
                                         "pthread_cond_destroy");
        (void)registry.register_function(key("Op8TBGY5KHg", library), cond_wait_address,
                                         "pthread_cond_wait");
        (void)registry.register_function(key("2MOy+rUfuhQ", library), cond_signal_address,
                                         "pthread_cond_signal");
        (void)registry.register_function(key("mkx2fVhNMsg", library), cond_broadcast_address,
                                         "pthread_cond_broadcast");
    }
    (void)registry.register_function(key("2Tb92quprl0"),
                                     reinterpret_cast<GuestAddress>(&sce_cond_init),
                                     "scePthreadCondInit");
    (void)registry.register_function(key("g+PZd2hiacg"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_destroy),
                                     "scePthreadCondDestroy");
    (void)registry.register_function(key("WKAXJ4XBPQ4"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_wait),
                                     "scePthreadCondWait");
    (void)registry.register_function(key("kDh-NfxgMtE"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_signal),
                                     "scePthreadCondSignal");
    (void)registry.register_function(key("JGgj7Uvrl+A"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_broadcast),
                                     "scePthreadCondBroadcast");
}

} // namespace nyxora::hle::libkernel
