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
thread_local std::int32_t guest_posix_errno = 0;

std::uint64_t posix_error_address() {
    return reinterpret_cast<std::uint64_t>(&guest_posix_errno);
}

std::uint64_t posix_failure(int error) {
    guest_posix_errno = static_cast<std::int32_t>(error);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(-1));
}

std::uint64_t posix_result(int error) {
    return error == 0 ? 0 : posix_failure(error);
}

std::uint64_t posix_kernel_result(std::int64_t result) {
    if (result >= 0) {
        return static_cast<std::uint64_t>(result);
    }
    const auto encoded = static_cast<std::uint32_t>(result);
    if ((encoded & 0xffff0000U) != 0x80020000U) {
        return posix_failure(runtime::KernelServices::kPosixEinval);
    }
    return posix_failure(static_cast<int>(encoded - 0x80020000U));
}

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

std::uint64_t orbis_errno_result(int result) {
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

std::uint64_t kernel_virtual_query(std::uint64_t address, std::uint64_t flags,
                                   std::uint64_t info, std::uint64_t info_size) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->virtual_query(static_cast<GuestAddress>(address),
                                                 static_cast<int>(static_cast<std::int32_t>(flags)),
                                                 static_cast<GuestAddress>(info),
                                                 static_cast<GuestSize>(info_size)));
}

std::uint64_t kernel_query_memory_protection(std::uint64_t address, std::uint64_t start,
                                             std::uint64_t end, std::uint64_t protection) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->query_memory_protection(
        static_cast<GuestAddress>(address), static_cast<GuestAddress>(start),
        static_cast<GuestAddress>(end), static_cast<GuestAddress>(protection)));
}

std::uint64_t kernel_available_direct_memory(std::uint64_t search_start,
                                             std::uint64_t search_end,
                                             std::uint64_t alignment,
                                             std::uint64_t physical_out,
                                             std::uint64_t size_out) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->available_direct_memory(
        static_cast<std::int64_t>(search_start), static_cast<std::int64_t>(search_end),
        static_cast<GuestSize>(alignment), static_cast<GuestAddress>(physical_out),
        static_cast<GuestAddress>(size_out)));
}

std::uint64_t kernel_allocate_direct_memory(std::uint64_t search_start,
                                            std::uint64_t search_end, std::uint64_t size,
                                            std::uint64_t alignment, std::uint64_t memory_type,
                                            std::uint64_t physical_out) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->allocate_direct_memory(
        static_cast<std::int64_t>(search_start), static_cast<std::int64_t>(search_end),
        static_cast<GuestSize>(size), static_cast<GuestSize>(alignment),
        static_cast<int>(static_cast<std::int32_t>(memory_type)),
        static_cast<GuestAddress>(physical_out)));
}

std::uint64_t kernel_allocate_main_direct_memory(std::uint64_t size, std::uint64_t alignment,
                                                 std::uint64_t memory_type,
                                                 std::uint64_t physical_out) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->allocate_direct_memory(
        0, static_cast<std::int64_t>(services->direct_memory_size()), static_cast<GuestSize>(size),
        static_cast<GuestSize>(alignment), static_cast<int>(static_cast<std::int32_t>(memory_type)),
        static_cast<GuestAddress>(physical_out)));
}

std::uint64_t kernel_release_direct_memory(std::uint64_t physical_address, std::uint64_t size) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->release_direct_memory(static_cast<GuestAddress>(physical_address),
                                                         static_cast<GuestSize>(size), false));
}

std::uint64_t kernel_checked_release_direct_memory(std::uint64_t physical_address,
                                                   std::uint64_t size) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->release_direct_memory(static_cast<GuestAddress>(physical_address),
                                                         static_cast<GuestSize>(size), true));
}

std::uint64_t kernel_map_direct_memory(std::uint64_t address_slot, std::uint64_t size,
                                       std::uint64_t protection, std::uint64_t flags,
                                       std::uint64_t physical_address, std::uint64_t alignment) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->map_direct_memory(
        static_cast<GuestAddress>(address_slot), static_cast<GuestSize>(size),
        static_cast<std::uint32_t>(protection), static_cast<std::uint32_t>(flags),
        static_cast<std::int64_t>(physical_address), static_cast<GuestSize>(alignment)));
}

std::uint64_t kernel_map_named_direct_memory(std::uint64_t address_slot, std::uint64_t size,
                                             std::uint64_t protection, std::uint64_t flags,
                                             std::uint64_t physical_address,
                                             std::uint64_t alignment, std::uint64_t name) {
    if (name == 0) {
        return kernel_error_result(runtime::KernelServices::kErrorEfault);
    }
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->map_direct_memory(
        static_cast<GuestAddress>(address_slot), static_cast<GuestSize>(size),
        static_cast<std::uint32_t>(protection), static_cast<std::uint32_t>(flags),
        static_cast<std::int64_t>(physical_address), static_cast<GuestSize>(alignment),
        static_cast<GuestAddress>(name)));
}

std::uint64_t kernel_direct_memory_query(std::uint64_t physical_address, std::uint64_t flags,
                                         std::uint64_t info, std::uint64_t info_size) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->direct_memory_query(
        static_cast<std::int64_t>(physical_address),
        static_cast<int>(static_cast<std::int32_t>(flags)), static_cast<GuestAddress>(info),
        static_cast<GuestSize>(info_size)));
}

std::uint64_t kernel_map_flexible_memory(std::uint64_t address_slot, std::uint64_t size,
                                         std::uint64_t protection, std::uint64_t flags) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->map_flexible_memory(
        static_cast<GuestAddress>(address_slot), static_cast<GuestSize>(size),
        static_cast<std::uint32_t>(protection), static_cast<std::uint32_t>(flags)));
}

std::uint64_t kernel_map_named_flexible_memory(std::uint64_t address_slot, std::uint64_t size,
                                               std::uint64_t protection, std::uint64_t flags,
                                               std::uint64_t name) {
    if (name == 0) {
        return kernel_error_result(runtime::KernelServices::kErrorEfault);
    }
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->map_flexible_memory(
        static_cast<GuestAddress>(address_slot), static_cast<GuestSize>(size),
        static_cast<std::uint32_t>(protection), static_cast<std::uint32_t>(flags),
        static_cast<GuestAddress>(name)));
}

std::uint64_t kernel_available_flexible_memory(std::uint64_t size_out) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->available_flexible_memory_size_to(
        static_cast<GuestAddress>(size_out)));
}

std::uint64_t kernel_configured_flexible_memory(std::uint64_t size_out) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->configured_flexible_memory_size_to(
        static_cast<GuestAddress>(size_out)));
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

std::uint64_t posix_mprotect(std::uint64_t address, std::uint64_t size,
                               std::uint64_t protection) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return posix_failure(runtime::KernelServices::kPosixEinval);
    }
    return posix_kernel_result(services->mprotect(static_cast<GuestAddress>(address),
                                                  static_cast<GuestSize>(size),
                                                  static_cast<std::uint32_t>(protection)));
}

std::uint64_t posix_mmap(std::uint64_t address, std::uint64_t size, std::uint64_t protection,
                         std::uint64_t flags, std::uint64_t fd, std::uint64_t offset) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return posix_failure(runtime::KernelServices::kPosixEinval);
    }
    return posix_kernel_result(services->map_memory(
        static_cast<GuestAddress>(address), static_cast<GuestSize>(size),
        static_cast<std::uint32_t>(protection), static_cast<std::uint32_t>(flags),
        static_cast<int>(static_cast<std::int32_t>(fd)), static_cast<std::int64_t>(offset)));
}

std::uint64_t kernel_mmap(std::uint64_t address, std::uint64_t size, std::uint64_t protection,
                          std::uint64_t flags, std::uint64_t fd, std::uint64_t offset,
                          std::uint64_t result_address) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->map_memory_to(
        static_cast<GuestAddress>(address), static_cast<GuestSize>(size),
        static_cast<std::uint32_t>(protection), static_cast<std::uint32_t>(flags),
        static_cast<int>(static_cast<std::int32_t>(fd)), static_cast<std::int64_t>(offset),
        static_cast<GuestAddress>(result_address)));
}

std::uint64_t posix_munmap(std::uint64_t address, std::uint64_t size) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return posix_failure(runtime::KernelServices::kPosixEinval);
    }
    return posix_kernel_result(services->unmap_memory(static_cast<GuestAddress>(address),
                                                      static_cast<GuestSize>(size)));
}

std::uint64_t kernel_munmap(std::uint64_t address, std::uint64_t size) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->unmap_memory(static_cast<GuestAddress>(address),
                                                static_cast<GuestSize>(size)));
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

std::uint64_t posix_open(std::uint64_t path, std::uint64_t flags, std::uint64_t mode) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return posix_failure(runtime::KernelServices::kPosixEinval);
    }
    return posix_kernel_result(services->open_readonly(static_cast<GuestAddress>(path),
                                                     static_cast<std::uint32_t>(flags),
                                                     static_cast<std::uint16_t>(mode)));
}

std::uint64_t posix_read(std::uint64_t fd, std::uint64_t buffer, std::uint64_t size) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return posix_failure(runtime::KernelServices::kPosixEbadf);
    }
    return posix_kernel_result(services->read(static_cast<int>(static_cast<std::int32_t>(fd)),
                                            static_cast<GuestAddress>(buffer),
                                            static_cast<GuestSize>(size)));
}

std::uint64_t posix_close(std::uint64_t fd) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return posix_failure(runtime::KernelServices::kPosixEbadf);
    }
    return posix_kernel_result(services->close(static_cast<int>(static_cast<std::int32_t>(fd))));
}

std::uint64_t posix_lseek(std::uint64_t fd, std::uint64_t offset, std::uint64_t whence) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return posix_failure(runtime::KernelServices::kPosixEbadf);
    }
    return posix_kernel_result(services->seek(static_cast<int>(static_cast<std::int32_t>(fd)),
                                            static_cast<std::int64_t>(offset),
                                            static_cast<int>(static_cast<std::int32_t>(whence))));
}

std::uint64_t kernel_lseek(std::uint64_t fd, std::uint64_t offset, std::uint64_t whence) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEbadf);
    }
    return signed_result(services->seek(static_cast<int>(static_cast<std::int32_t>(fd)),
                                        static_cast<std::int64_t>(offset),
                                        static_cast<int>(static_cast<std::int32_t>(whence))));
}

std::uint64_t posix_stat(std::uint64_t path, std::uint64_t stat) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return posix_failure(runtime::KernelServices::kPosixEinval);
    }
    return posix_kernel_result(services->stat_path(static_cast<GuestAddress>(path),
                                                 static_cast<GuestAddress>(stat)));
}

std::uint64_t kernel_stat(std::uint64_t path, std::uint64_t stat) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEinval);
    }
    return signed_result(services->stat_path(static_cast<GuestAddress>(path),
                                             static_cast<GuestAddress>(stat)));
}

std::uint64_t posix_fstat(std::uint64_t fd, std::uint64_t stat) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return posix_failure(runtime::KernelServices::kPosixEbadf);
    }
    return posix_kernel_result(services->fstat(static_cast<int>(static_cast<std::int32_t>(fd)),
                                             static_cast<GuestAddress>(stat)));
}

std::uint64_t kernel_fstat(std::uint64_t fd, std::uint64_t stat) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return kernel_error_result(runtime::KernelServices::kErrorEbadf);
    }
    return signed_result(services->fstat(static_cast<int>(static_cast<std::int32_t>(fd)),
                                         static_cast<GuestAddress>(stat)));
}

std::uint64_t mutex_attr_init(std::uint64_t attribute) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->mutex_attr_init(
                                     static_cast<GuestAddress>(attribute)));
}

std::uint64_t mutex_attr_destroy(std::uint64_t attribute) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->mutex_attr_destroy(
                                     static_cast<GuestAddress>(attribute)));
}

std::uint64_t mutex_attr_get_type(std::uint64_t attribute, std::uint64_t output) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->mutex_attr_get_type(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<GuestAddress>(output)));
}

std::uint64_t mutex_attr_set_type(std::uint64_t attribute, std::uint64_t type) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->mutex_attr_set_type(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<std::uint32_t>(type)));
}

std::uint64_t mutex_attr_get_pshared(std::uint64_t attribute, std::uint64_t output) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->mutex_attr_get_pshared(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<GuestAddress>(output)));
}

std::uint64_t mutex_attr_set_pshared(std::uint64_t attribute, std::uint64_t pshared) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->mutex_attr_set_pshared(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<int>(pshared)));
}

std::uint64_t orbis_mutex_attr_init(std::uint64_t attribute) {
    return orbis_errno_result(static_cast<int>(mutex_attr_init(attribute)));
}

std::uint64_t orbis_mutex_attr_destroy(std::uint64_t attribute) {
    return orbis_errno_result(static_cast<int>(mutex_attr_destroy(attribute)));
}

std::uint64_t orbis_mutex_attr_get_type(std::uint64_t attribute, std::uint64_t output) {
    return orbis_errno_result(static_cast<int>(mutex_attr_get_type(attribute, output)));
}

std::uint64_t orbis_mutex_attr_set_type(std::uint64_t attribute, std::uint64_t type) {
    return orbis_errno_result(static_cast<int>(mutex_attr_set_type(attribute, type)));
}

std::uint64_t orbis_mutex_attr_get_pshared(std::uint64_t attribute, std::uint64_t output) {
    return orbis_errno_result(static_cast<int>(mutex_attr_get_pshared(attribute, output)));
}

std::uint64_t orbis_mutex_attr_set_pshared(std::uint64_t attribute, std::uint64_t pshared) {
    return orbis_errno_result(static_cast<int>(mutex_attr_set_pshared(attribute, pshared)));
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
    return orbis_errno_result(result);
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
    return orbis_errno_result(services == nullptr
                                    ? runtime::KernelServices::kPosixEinval
                                    : services->mutex_lock(static_cast<GuestAddress>(mutex)));
}

std::uint64_t orbis_mutex_unlock(std::uint64_t mutex) {
    auto* services = kernel_services();
    return orbis_errno_result(services == nullptr
                                    ? runtime::KernelServices::kPosixEinval
                                    : services->mutex_unlock(static_cast<GuestAddress>(mutex)));
}

std::uint64_t orbis_mutex_destroy(std::uint64_t mutex) {
    auto* services = kernel_services();
    return orbis_errno_result(services == nullptr
                                    ? runtime::KernelServices::kPosixEinval
                                    : services->mutex_destroy(static_cast<GuestAddress>(mutex)));
}

std::uint64_t cond_attr_init(std::uint64_t attribute) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->cond_attr_init(
                                     static_cast<GuestAddress>(attribute)));
}

std::uint64_t cond_attr_destroy(std::uint64_t attribute) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->cond_attr_destroy(
                                     static_cast<GuestAddress>(attribute)));
}

std::uint64_t cond_attr_get_clock(std::uint64_t attribute, std::uint64_t output) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->cond_attr_get_clock(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<GuestAddress>(output)));
}

std::uint64_t cond_attr_set_clock(std::uint64_t attribute, std::uint64_t clock_id) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->cond_attr_set_clock(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<std::uint32_t>(clock_id)));
}

std::uint64_t cond_attr_get_pshared(std::uint64_t attribute, std::uint64_t output) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->cond_attr_get_pshared(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<GuestAddress>(output)));
}

std::uint64_t cond_attr_set_pshared(std::uint64_t attribute, std::uint64_t pshared) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->cond_attr_set_pshared(
                                     static_cast<GuestAddress>(attribute),
                                     static_cast<int>(pshared)));
}

std::uint64_t orbis_cond_attr_init(std::uint64_t attribute) {
    return orbis_errno_result(static_cast<int>(cond_attr_init(attribute)));
}

std::uint64_t orbis_cond_attr_destroy(std::uint64_t attribute) {
    return orbis_errno_result(static_cast<int>(cond_attr_destroy(attribute)));
}

std::uint64_t orbis_cond_attr_get_clock(std::uint64_t attribute, std::uint64_t output) {
    return orbis_errno_result(static_cast<int>(cond_attr_get_clock(attribute, output)));
}

std::uint64_t orbis_cond_attr_set_clock(std::uint64_t attribute, std::uint64_t clock_id) {
    return orbis_errno_result(static_cast<int>(cond_attr_set_clock(attribute, clock_id)));
}

std::uint64_t orbis_cond_attr_get_pshared(std::uint64_t attribute, std::uint64_t output) {
    return orbis_errno_result(static_cast<int>(cond_attr_get_pshared(attribute, output)));
}

std::uint64_t orbis_cond_attr_set_pshared(std::uint64_t attribute, std::uint64_t pshared) {
    return orbis_errno_result(static_cast<int>(cond_attr_set_pshared(attribute, pshared)));
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
    return orbis_errno_result(result);
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


std::uint64_t posix_cond_timed_wait(std::uint64_t cond, std::uint64_t mutex,
                                    std::uint64_t absolute_timeout) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->cond_timed_wait(
                                     static_cast<GuestAddress>(cond),
                                     static_cast<GuestAddress>(mutex),
                                     static_cast<GuestAddress>(absolute_timeout)));
}

std::uint64_t posix_cond_reltimed_wait(std::uint64_t cond, std::uint64_t mutex,
                                       std::uint64_t microseconds) {
    auto* services = kernel_services();
    return services == nullptr ? runtime::KernelServices::kPosixEinval
                               : static_cast<std::uint64_t>(services->cond_reltimed_wait(
                                     static_cast<GuestAddress>(cond),
                                     static_cast<GuestAddress>(mutex), microseconds));
}

std::uint64_t orbis_cond_reltimed_wait(std::uint64_t cond, std::uint64_t mutex,
                                       std::uint64_t microseconds) {
    return orbis_errno_result(
        static_cast<int>(posix_cond_reltimed_wait(cond, mutex, microseconds)));
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
    return orbis_errno_result(static_cast<int>(posix_cond_destroy(cond)));
}

std::uint64_t orbis_cond_wait(std::uint64_t cond, std::uint64_t mutex) {
    return orbis_errno_result(static_cast<int>(posix_cond_wait(cond, mutex)));
}

std::uint64_t orbis_cond_signal(std::uint64_t cond) {
    return orbis_errno_result(static_cast<int>(posix_cond_signal(cond)));
}

std::uint64_t orbis_cond_broadcast(std::uint64_t cond) {
    return orbis_errno_result(static_cast<int>(posix_cond_broadcast(cond)));
}

std::uint64_t posix_nanosleep(std::uint64_t request, std::uint64_t remaining) {
    auto* services = kernel_services();
    return services == nullptr
               ? posix_failure(runtime::KernelServices::kPosixEinval)
               : posix_result(services->nanosleep(static_cast<GuestAddress>(request),
                                                  static_cast<GuestAddress>(remaining)));
}

std::uint64_t sce_kernel_nanosleep(std::uint64_t request, std::uint64_t remaining) {
    auto* services = kernel_services();
    return orbis_errno_result(
        services == nullptr ? runtime::KernelServices::kPosixEinval
                            : services->nanosleep(static_cast<GuestAddress>(request),
                                                  static_cast<GuestAddress>(remaining)));
}

std::uint64_t posix_usleep(std::uint64_t microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<std::uint32_t>(microseconds)));
    return 0;
}

std::uint64_t sce_kernel_usleep(std::uint64_t microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<std::uint32_t>(microseconds)));
    return 0;
}

std::uint64_t posix_sleep(std::uint64_t seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(static_cast<std::uint32_t>(seconds)));
    return 0;
}

std::uint64_t sce_kernel_sleep(std::uint64_t seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(static_cast<std::uint32_t>(seconds)));
    return 0;
}

std::uint64_t guest_yield() {
    std::this_thread::yield();
    return 0;
}

std::uint64_t posix_sem_init(std::uint64_t semaphore, std::uint64_t pshared,
                             std::uint64_t value) {
    auto* services = kernel_services();
    if (services == nullptr) {
        return posix_failure(runtime::KernelServices::kPosixEinval);
    }
    return posix_result(services->sem_init(static_cast<GuestAddress>(semaphore),
                                          static_cast<int>(pshared),
                                          static_cast<std::uint32_t>(value)));
}

std::uint64_t posix_sem_destroy(std::uint64_t semaphore) {
    auto* services = kernel_services();
    return services == nullptr
               ? posix_failure(runtime::KernelServices::kPosixEinval)
               : posix_result(services->sem_destroy(static_cast<GuestAddress>(semaphore)));
}

std::uint64_t posix_sem_wait(std::uint64_t semaphore) {
    auto* services = kernel_services();
    return services == nullptr
               ? posix_failure(runtime::KernelServices::kPosixEinval)
               : posix_result(services->sem_wait(static_cast<GuestAddress>(semaphore)));
}

std::uint64_t posix_sem_try_wait(std::uint64_t semaphore) {
    auto* services = kernel_services();
    return services == nullptr
               ? posix_failure(runtime::KernelServices::kPosixEinval)
               : posix_result(services->sem_try_wait(static_cast<GuestAddress>(semaphore)));
}

std::uint64_t posix_sem_timed_wait(std::uint64_t semaphore, std::uint64_t absolute_timeout) {
    auto* services = kernel_services();
    return services == nullptr
               ? posix_failure(runtime::KernelServices::kPosixEinval)
               : posix_result(services->sem_timed_wait(static_cast<GuestAddress>(semaphore),
                                                       static_cast<GuestAddress>(absolute_timeout)));
}

std::uint64_t posix_sem_reltimed_wait(std::uint64_t semaphore, std::uint64_t microseconds) {
    auto* services = kernel_services();
    return services == nullptr
               ? posix_failure(runtime::KernelServices::kPosixEinval)
               : posix_result(services->sem_reltimed_wait(static_cast<GuestAddress>(semaphore),
                                                          microseconds));
}

std::uint64_t posix_sem_post(std::uint64_t semaphore) {
    auto* services = kernel_services();
    return services == nullptr
               ? posix_failure(runtime::KernelServices::kPosixEinval)
               : posix_result(services->sem_post(static_cast<GuestAddress>(semaphore)));
}

std::uint64_t posix_sem_get_value(std::uint64_t semaphore, std::uint64_t output) {
    auto* services = kernel_services();
    return services == nullptr
               ? posix_failure(runtime::KernelServices::kPosixEinval)
               : posix_result(services->sem_get_value(static_cast<GuestAddress>(semaphore),
                                                      static_cast<GuestAddress>(output)));
}

std::uint64_t sce_pthread_sem_init(std::uint64_t semaphore, std::uint64_t flag,
                                   std::uint64_t value, std::uint64_t name) {
    (void)name;
    if (flag != 0) {
        return orbis_errno_result(runtime::KernelServices::kPosixEinval);
    }
    auto* services = kernel_services();
    return orbis_errno_result(
        services == nullptr ? runtime::KernelServices::kPosixEinval
                            : services->sem_init(static_cast<GuestAddress>(semaphore), 0,
                                                 static_cast<std::uint32_t>(value)));
}

std::uint64_t sce_pthread_sem_destroy(std::uint64_t semaphore) {
    auto* services = kernel_services();
    return orbis_errno_result(
        services == nullptr ? runtime::KernelServices::kPosixEinval
                            : services->sem_destroy(static_cast<GuestAddress>(semaphore)));
}

std::uint64_t sce_pthread_sem_wait(std::uint64_t semaphore) {
    auto* services = kernel_services();
    return orbis_errno_result(
        services == nullptr ? runtime::KernelServices::kPosixEinval
                            : services->sem_wait(static_cast<GuestAddress>(semaphore)));
}

std::uint64_t sce_pthread_sem_try_wait(std::uint64_t semaphore) {
    auto* services = kernel_services();
    return orbis_errno_result(
        services == nullptr ? runtime::KernelServices::kPosixEinval
                            : services->sem_try_wait(static_cast<GuestAddress>(semaphore)));
}

std::uint64_t sce_pthread_sem_timed_wait(std::uint64_t semaphore, std::uint64_t microseconds) {
    auto* services = kernel_services();
    return orbis_errno_result(
        services == nullptr ? runtime::KernelServices::kPosixEinval
                            : services->sem_reltimed_wait(static_cast<GuestAddress>(semaphore),
                                                         microseconds));
}

std::uint64_t sce_pthread_sem_post(std::uint64_t semaphore) {
    auto* services = kernel_services();
    return orbis_errno_result(
        services == nullptr ? runtime::KernelServices::kPosixEinval
                            : services->sem_post(static_cast<GuestAddress>(semaphore)));
}

std::uint64_t sce_pthread_sem_get_value(std::uint64_t semaphore, std::uint64_t output) {
    auto* services = kernel_services();
    return orbis_errno_result(
        services == nullptr ? runtime::KernelServices::kPosixEinval
                            : services->sem_get_value(static_cast<GuestAddress>(semaphore),
                                                      static_cast<GuestAddress>(output)));
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

std::uint64_t pthread_timed_join(std::uint64_t thread, std::uint64_t return_value,
                                 std::uint64_t absolute_timeout) {
    auto* manager = runtime::GuestThreadManager::current();
    if (manager == nullptr) {
        return runtime::GuestThreadManager::kPosixEinval;
    }
    return static_cast<std::uint64_t>(manager->timed_join(
        static_cast<GuestAddress>(thread),
        return_value == 0 ? nullptr : reinterpret_cast<GuestAddress*>(return_value),
        static_cast<GuestAddress>(absolute_timeout)));
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
    return orbis_errno_result(static_cast<int>(pthread_attr_init(attribute)));
}

std::uint64_t orbis_attr_destroy(std::uint64_t attribute) {
    return orbis_errno_result(static_cast<int>(pthread_attr_destroy(attribute)));
}

std::uint64_t orbis_attr_get_stack_size(std::uint64_t attribute, std::uint64_t output) {
    return orbis_errno_result(static_cast<int>(pthread_attr_get_stack_size(attribute, output)));
}

std::uint64_t orbis_attr_set_stack_size(std::uint64_t attribute, std::uint64_t stack_size) {
    return orbis_errno_result(static_cast<int>(pthread_attr_set_stack_size(attribute, stack_size)));
}

std::uint64_t orbis_attr_get_detach_state(std::uint64_t attribute, std::uint64_t output) {
    return orbis_errno_result(static_cast<int>(pthread_attr_get_detach_state(attribute, output)));
}

std::uint64_t orbis_attr_set_detach_state(std::uint64_t attribute, std::uint64_t detach_state) {
    return orbis_errno_result(static_cast<int>(pthread_attr_set_detach_state(attribute, detach_state)));
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
    (void)registry.register_no_arg(key("9BcDykPmo1I"), posix_error_address, "__Error");

    const auto nanosleep_address = reinterpret_cast<GuestAddress>(&posix_nanosleep);
    const auto usleep_address = reinterpret_cast<GuestAddress>(&posix_usleep);
    const auto sleep_address = reinterpret_cast<GuestAddress>(&posix_sleep);
    const auto yield_address = reinterpret_cast<GuestAddress>(&guest_yield);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("NhpspxdjEKU", library), nanosleep_address,
                                         "nanosleep");
        (void)registry.register_function(key("yS8U2TGCe1A", library), nanosleep_address,
                                         "nanosleep");
        (void)registry.register_function(key("QcteRwbsnV0", library), usleep_address, "usleep");
        (void)registry.register_function(key("0wu33hunNdE", library), sleep_address, "sleep");
        (void)registry.register_function(key("6XG4B33N09g", library), yield_address,
                                         "sched_yield");
    }
    (void)registry.register_function(key("B5GmVDKwpn0", "libScePosix"), yield_address,
                                     "pthread_yield");
    (void)registry.register_function(key("T72hz6ffq08"), yield_address, "scePthreadYield");
    (void)registry.register_function(key("QvsZxomvUHs"),
                                     reinterpret_cast<GuestAddress>(&sce_kernel_nanosleep),
                                     "sceKernelNanosleep");
    (void)registry.register_function(key("1jfXLRVzisc"),
                                     reinterpret_cast<GuestAddress>(&sce_kernel_usleep),
                                     "sceKernelUsleep");
    (void)registry.register_function(key("-ZR+hG7aDHw"),
                                     reinterpret_cast<GuestAddress>(&sce_kernel_sleep),
                                     "sceKernelSleep");
    (void)registry.register_no_arg(key("pO96TwzOm5E"), direct_memory_size,
                                   "sceKernelGetDirectMemorySize");
    (void)registry.register_function(key("rVjRvHJ0X6c"),
                                     reinterpret_cast<GuestAddress>(&kernel_virtual_query),
                                     "sceKernelVirtualQuery");
    (void)registry.register_function(
        key("WFcfL2lzido"), reinterpret_cast<GuestAddress>(&kernel_query_memory_protection),
        "sceKernelQueryMemoryProtection");
    (void)registry.register_function(
        key("C0f7TJcbfac"), reinterpret_cast<GuestAddress>(&kernel_available_direct_memory),
        "sceKernelAvailableDirectMemorySize");
    (void)registry.register_function(
        key("rTXw65xmLIA"), reinterpret_cast<GuestAddress>(&kernel_allocate_direct_memory),
        "sceKernelAllocateDirectMemory");
    (void)registry.register_function(
        key("B+vc2AO2Zrc"), reinterpret_cast<GuestAddress>(&kernel_allocate_main_direct_memory),
        "sceKernelAllocateMainDirectMemory");
    (void)registry.register_function(
        key("MBuItvba6z8"), reinterpret_cast<GuestAddress>(&kernel_release_direct_memory),
        "sceKernelReleaseDirectMemory");
    (void)registry.register_function(
        key("hwVSPCmp5tM"), reinterpret_cast<GuestAddress>(&kernel_checked_release_direct_memory),
        "sceKernelCheckedReleaseDirectMemory");
    (void)registry.register_function(
        key("L-Q3LEjIbgA"), reinterpret_cast<GuestAddress>(&kernel_map_direct_memory),
        "sceKernelMapDirectMemory");
    (void)registry.register_function(
        key("NcaWUxfMNIQ"), reinterpret_cast<GuestAddress>(&kernel_map_named_direct_memory),
        "sceKernelMapNamedDirectMemory");
    (void)registry.register_function(
        key("BHouLQzh0X0"), reinterpret_cast<GuestAddress>(&kernel_direct_memory_query),
        "sceKernelDirectMemoryQuery");
    (void)registry.register_function(
        key("mL8NDH86iQI"), reinterpret_cast<GuestAddress>(&kernel_map_named_flexible_memory),
        "sceKernelMapNamedFlexibleMemory");
    (void)registry.register_function(
        key("IWIBBdTHit4"), reinterpret_cast<GuestAddress>(&kernel_map_flexible_memory),
        "sceKernelMapFlexibleMemory");
    (void)registry.register_function(
        key("4h6F1LLbTiw"), reinterpret_cast<GuestAddress>(&kernel_map_flexible_memory),
        "sceKernelMapFlexibleMemory");
    (void)registry.register_function(
        key("aNz11fnnzi4"), reinterpret_cast<GuestAddress>(&kernel_available_flexible_memory),
        "sceKernelAvailableFlexibleMemorySize");
    (void)registry.register_function(
        key("n1-v6FgU7MQ"), reinterpret_cast<GuestAddress>(&kernel_configured_flexible_memory),
        "sceKernelConfiguredFlexibleMemorySize");
    (void)registry.register_function(key("vSMAm3cxYTY"),
                                     reinterpret_cast<GuestAddress>(&kernel_mprotect),
                                     "sceKernelMprotect");
    const auto posix_mprotect_address = reinterpret_cast<GuestAddress>(&posix_mprotect);
    const auto posix_mmap_address = reinterpret_cast<GuestAddress>(&posix_mmap);
    const auto posix_munmap_address = reinterpret_cast<GuestAddress>(&posix_munmap);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("YQOfxL4QfeU", library), posix_mprotect_address,
                                         "mprotect");
        (void)registry.register_function(key("BPE9s9vQQXo", library), posix_mmap_address, "mmap");
        (void)registry.register_function(key("UqDGjXA5yUM", library), posix_munmap_address,
                                         "munmap");
    }
    (void)registry.register_function(key("PGhQHd-dzv8"),
                                     reinterpret_cast<GuestAddress>(&kernel_mmap),
                                     "sceKernelMmap");
    (void)registry.register_function(key("cQke9UuBQOk"),
                                     reinterpret_cast<GuestAddress>(&kernel_munmap),
                                     "sceKernelMunmap");
    (void)registry.register_function(key("1G3lF1Gg1k8"),
                                     reinterpret_cast<GuestAddress>(&kernel_open),
                                     "sceKernelOpen");
    (void)registry.register_function(key("Cg4srZ6TKbU"),
                                     reinterpret_cast<GuestAddress>(&kernel_read),
                                     "sceKernelRead");
    (void)registry.register_function(key("UK2Tl2DWUns"),
                                     reinterpret_cast<GuestAddress>(&kernel_close),
                                     "sceKernelClose");

    const auto posix_open_address = reinterpret_cast<GuestAddress>(&posix_open);
    const auto posix_read_address = reinterpret_cast<GuestAddress>(&posix_read);
    const auto posix_close_address = reinterpret_cast<GuestAddress>(&posix_close);
    const auto posix_lseek_address = reinterpret_cast<GuestAddress>(&posix_lseek);
    const auto posix_stat_address = reinterpret_cast<GuestAddress>(&posix_stat);
    const auto posix_fstat_address = reinterpret_cast<GuestAddress>(&posix_fstat);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("wuCroIGjt2g", library), posix_open_address, "open");
        (void)registry.register_function(key("AqBioC2vF3I", library), posix_read_address, "read");
        (void)registry.register_function(key("bY-PO6JhzhQ", library), posix_close_address, "close");
        (void)registry.register_function(key("Oy6IpwgtYOk", library), posix_lseek_address, "lseek");
        (void)registry.register_function(key("E6ao34wPw+U", library), posix_stat_address, "stat");
        (void)registry.register_function(key("mqQMh1zPPT8", library), posix_fstat_address, "fstat");
    }
    (void)registry.register_function(key("6c3rCVE-fTU"), posix_open_address, "open");
    (void)registry.register_function(key("DRuBt2pvICk"), posix_read_address, "read");
    (void)registry.register_function(key("NNtFaKJbPt0"), posix_close_address, "close");
    (void)registry.register_function(key("oib76F-12fk"),
                                     reinterpret_cast<GuestAddress>(&kernel_lseek),
                                     "sceKernelLseek");
    (void)registry.register_function(key("eV9wAD2riIA"),
                                     reinterpret_cast<GuestAddress>(&kernel_stat),
                                     "sceKernelStat");
    (void)registry.register_function(key("kBwCPsYX-m4"),
                                     reinterpret_cast<GuestAddress>(&kernel_fstat),
                                     "sceKernelFstat");

    const auto sem_init_address = reinterpret_cast<GuestAddress>(&posix_sem_init);
    const auto sem_destroy_address = reinterpret_cast<GuestAddress>(&posix_sem_destroy);
    const auto sem_wait_address = reinterpret_cast<GuestAddress>(&posix_sem_wait);
    const auto sem_try_wait_address = reinterpret_cast<GuestAddress>(&posix_sem_try_wait);
    const auto sem_timed_wait_address = reinterpret_cast<GuestAddress>(&posix_sem_timed_wait);
    const auto sem_reltimed_wait_address = reinterpret_cast<GuestAddress>(&posix_sem_reltimed_wait);
    const auto sem_post_address = reinterpret_cast<GuestAddress>(&posix_sem_post);
    const auto sem_get_value_address = reinterpret_cast<GuestAddress>(&posix_sem_get_value);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("pDuPEf3m4fI", library), sem_init_address, "sem_init");
        (void)registry.register_function(key("cDW233RAwWo", library), sem_destroy_address,
                                         "sem_destroy");
        (void)registry.register_function(key("YCV5dGGBcCo", library), sem_wait_address, "sem_wait");
        (void)registry.register_function(key("WBWzsRifCEA", library), sem_try_wait_address,
                                         "sem_trywait");
        (void)registry.register_function(key("w5IHyvahg-o", library), sem_timed_wait_address,
                                         "sem_timedwait");
        (void)registry.register_function(key("4SbrhCozqQU", library), sem_reltimed_wait_address,
                                         "sem_reltimedwait_np");
        (void)registry.register_function(key("IKP8typ0QUk", library), sem_post_address, "sem_post");
        (void)registry.register_function(key("Bq+LRV-N6Hk", library), sem_get_value_address,
                                         "sem_getvalue");
    }
    (void)registry.register_function(key("GEnUkDZoUwY"),
                                     reinterpret_cast<GuestAddress>(&sce_pthread_sem_init),
                                     "scePthreadSemInit");
    (void)registry.register_function(key("Vwc+L05e6oE"),
                                     reinterpret_cast<GuestAddress>(&sce_pthread_sem_destroy),
                                     "scePthreadSemDestroy");
    (void)registry.register_function(key("C36iRE0F5sE"),
                                     reinterpret_cast<GuestAddress>(&sce_pthread_sem_wait),
                                     "scePthreadSemWait");
    (void)registry.register_function(key("H2a+IN9TP0E"),
                                     reinterpret_cast<GuestAddress>(&sce_pthread_sem_try_wait),
                                     "scePthreadSemTrywait");
    (void)registry.register_function(key("fjN6NQHhK8k"),
                                     reinterpret_cast<GuestAddress>(&sce_pthread_sem_timed_wait),
                                     "scePthreadSemTimedwait");
    (void)registry.register_function(key("aishVAiFaYM"),
                                     reinterpret_cast<GuestAddress>(&sce_pthread_sem_post),
                                     "scePthreadSemPost");
    (void)registry.register_function(key("DjpBvGlaWbQ"),
                                     reinterpret_cast<GuestAddress>(&sce_pthread_sem_get_value),
                                     "scePthreadSemGetvalue");

    const auto create_address = reinterpret_cast<GuestAddress>(&pthread_create);
    const auto join_address = reinterpret_cast<GuestAddress>(&pthread_join);
    const auto timed_join_address = reinterpret_cast<GuestAddress>(&pthread_timed_join);
    const auto self_address = reinterpret_cast<GuestAddress>(&pthread_self);
    const auto detach_address = reinterpret_cast<GuestAddress>(&pthread_detach);
    const auto exit_address = reinterpret_cast<GuestAddress>(&pthread_exit);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("OxhIB8LB-PQ", library), create_address,
                                         "pthread_create");
        (void)registry.register_function(key("h9CcP3J0oVM", library), join_address,
                                         "pthread_join");
        (void)registry.register_function(key("PkS44IGrDkM", library), timed_join_address,
                                         "pthread_timedjoin_np");
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

    const auto mutex_attr_init_address = reinterpret_cast<GuestAddress>(&mutex_attr_init);
    const auto mutex_attr_destroy_address = reinterpret_cast<GuestAddress>(&mutex_attr_destroy);
    const auto mutex_attr_get_type_address = reinterpret_cast<GuestAddress>(&mutex_attr_get_type);
    const auto mutex_attr_set_type_address = reinterpret_cast<GuestAddress>(&mutex_attr_set_type);
    const auto mutex_attr_get_pshared_address =
        reinterpret_cast<GuestAddress>(&mutex_attr_get_pshared);
    const auto mutex_attr_set_pshared_address =
        reinterpret_cast<GuestAddress>(&mutex_attr_set_pshared);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("dQHWEsJtoE4", library), mutex_attr_init_address,
                                         "pthread_mutexattr_init");
        (void)registry.register_function(key("HF7lK46xzjY", library), mutex_attr_destroy_address,
                                         "pthread_mutexattr_destroy");
        (void)registry.register_function(key("GZFlI7RhuQo", library), mutex_attr_get_type_address,
                                         "pthread_mutexattr_gettype");
        (void)registry.register_function(key("mDmgMOGVUqg", library), mutex_attr_set_type_address,
                                         "pthread_mutexattr_settype");
        (void)registry.register_function(key("PmL-TwKUzXI", library), mutex_attr_get_pshared_address,
                                         "pthread_mutexattr_getpshared");
        (void)registry.register_function(key("EXv3ztGqtDM", library), mutex_attr_set_pshared_address,
                                         "pthread_mutexattr_setpshared");
    }
    (void)registry.register_function(key("n2MMpvU8igI"),
                                     reinterpret_cast<GuestAddress>(&orbis_mutex_attr_init),
                                     "scePthreadMutexattrInit");
    (void)registry.register_function(key("F8bUHwAG284"),
                                     reinterpret_cast<GuestAddress>(&orbis_mutex_attr_init),
                                     "scePthreadMutexattrInit");
    (void)registry.register_function(key("smWEktiyyG0"),
                                     reinterpret_cast<GuestAddress>(&orbis_mutex_attr_destroy),
                                     "scePthreadMutexattrDestroy");
    (void)registry.register_function(key("gquEhBrS2iw"),
                                     reinterpret_cast<GuestAddress>(&orbis_mutex_attr_get_type),
                                     "scePthreadMutexattrGettype");
    (void)registry.register_function(key("iMp8QpE+XO4"),
                                     reinterpret_cast<GuestAddress>(&orbis_mutex_attr_set_type),
                                     "scePthreadMutexattrSettype");
    (void)registry.register_function(key("losEubHc64c"),
                                     reinterpret_cast<GuestAddress>(&orbis_mutex_attr_get_pshared),
                                     "scePthreadMutexattrGetpshared");
    (void)registry.register_function(key("mxKx9bxXF2I"),
                                     reinterpret_cast<GuestAddress>(&orbis_mutex_attr_set_pshared),
                                     "scePthreadMutexattrSetpshared");

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

    const auto cond_attr_init_address = reinterpret_cast<GuestAddress>(&cond_attr_init);
    const auto cond_attr_destroy_address = reinterpret_cast<GuestAddress>(&cond_attr_destroy);
    const auto cond_attr_get_clock_address = reinterpret_cast<GuestAddress>(&cond_attr_get_clock);
    const auto cond_attr_set_clock_address = reinterpret_cast<GuestAddress>(&cond_attr_set_clock);
    const auto cond_attr_get_pshared_address = reinterpret_cast<GuestAddress>(&cond_attr_get_pshared);
    const auto cond_attr_set_pshared_address = reinterpret_cast<GuestAddress>(&cond_attr_set_pshared);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("mKoTx03HRWA", library), cond_attr_init_address,
                                         "pthread_condattr_init");
        (void)registry.register_function(key("dJcuQVn6-Iw", library), cond_attr_destroy_address,
                                         "pthread_condattr_destroy");
        (void)registry.register_function(key("cTDYxTUNPhM", library), cond_attr_get_clock_address,
                                         "pthread_condattr_getclock");
        (void)registry.register_function(key("EjllaAqAPZo", library), cond_attr_set_clock_address,
                                         "pthread_condattr_setclock");
        (void)registry.register_function(key("h0qUqSuOmC8", library), cond_attr_get_pshared_address,
                                         "pthread_condattr_getpshared");
        (void)registry.register_function(key("3BpP850hBT4", library), cond_attr_set_pshared_address,
                                         "pthread_condattr_setpshared");
    }
    (void)registry.register_function(key("m5-2bsNfv7s"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_attr_init),
                                     "scePthreadCondattrInit");
    (void)registry.register_function(key("waPcxYiR3WA"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_attr_destroy),
                                     "scePthreadCondattrDestroy");
    (void)registry.register_function(key("6qM3kO5S3Oo"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_attr_get_clock),
                                     "scePthreadCondattrGetclock");
    (void)registry.register_function(key("c-bxj027czs"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_attr_set_clock),
                                     "scePthreadCondattrSetclock");
    (void)registry.register_function(key("Dn-DRWi9t54"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_attr_get_pshared),
                                     "scePthreadCondattrGetpshared");
    (void)registry.register_function(key("6xMew9+rZwI"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_attr_set_pshared),
                                     "scePthreadCondattrSetpshared");

    const auto cond_init_address = reinterpret_cast<GuestAddress>(&posix_cond_init);
    const auto cond_destroy_address = reinterpret_cast<GuestAddress>(&posix_cond_destroy);
    const auto cond_wait_address = reinterpret_cast<GuestAddress>(&posix_cond_wait);
    const auto cond_timed_wait_address = reinterpret_cast<GuestAddress>(&posix_cond_timed_wait);
    const auto cond_reltimed_wait_address = reinterpret_cast<GuestAddress>(&posix_cond_reltimed_wait);
    const auto cond_signal_address = reinterpret_cast<GuestAddress>(&posix_cond_signal);
    const auto cond_broadcast_address = reinterpret_cast<GuestAddress>(&posix_cond_broadcast);
    for (const char* library : {"libkernel", "libScePosix"}) {
        (void)registry.register_function(key("0TyVk4MSLt0", library), cond_init_address,
                                         "pthread_cond_init");
        (void)registry.register_function(key("RXXqi4CtF8w", library), cond_destroy_address,
                                         "pthread_cond_destroy");
        (void)registry.register_function(key("Op8TBGY5KHg", library), cond_wait_address,
                                         "pthread_cond_wait");
        (void)registry.register_function(key("27bAgiJmOh0", library), cond_timed_wait_address,
                                         "pthread_cond_timedwait");
        (void)registry.register_function(key("K953PF5u6Pc", library), cond_reltimed_wait_address,
                                         "pthread_cond_reltimedwait_np");
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
    (void)registry.register_function(key("BmMjYxmew1w"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_reltimed_wait),
                                     "scePthreadCondTimedwait");
    (void)registry.register_function(key("kDh-NfxgMtE"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_signal),
                                     "scePthreadCondSignal");
    (void)registry.register_function(key("JGgj7Uvrl+A"),
                                     reinterpret_cast<GuestAddress>(&orbis_cond_broadcast),
                                     "scePthreadCondBroadcast");
}

bool provides_module(std::string_view filename) noexcept {
    return filename == "libkernel.prx" || filename == "libScePosix.prx";
}

} // namespace nyxora::hle::libkernel
