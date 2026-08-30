#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "nyxora/base/types.hpp"
#include "nyxora/memory/guest_address_space.hpp"

namespace nyxora::runtime {

class KernelServices {
public:
    static constexpr std::uint32_t kErrorEnoent = 0x80020002U;
    static constexpr std::uint32_t kErrorEbadf = 0x80020009U;
    static constexpr std::uint32_t kErrorEnomem = 0x8002000cU;
    static constexpr std::uint32_t kErrorEacces = 0x8002000dU;
    static constexpr std::uint32_t kErrorEfault = 0x8002000eU;
    static constexpr std::uint32_t kErrorEinval = 0x80020016U;
    static constexpr std::uint32_t kErrorEnotty = 0x80020019U;

    static constexpr int kPosixEperm = 1;
    static constexpr int kPosixEbadf = 9;
    static constexpr int kPosixEdeadlk = 11;
    static constexpr int kPosixEnomem = 12;
    static constexpr int kPosixEbusy = 16;
    static constexpr int kPosixEinval = 22;
    static constexpr int kPosixEnotty = 25;
    static constexpr int kPosixEtimedout = 60;
    static constexpr int kPosixEagain = 35;
    static constexpr int kPosixEoverflow = 84;

    explicit KernelServices(memory::GuestAddressSpace& memory);

    [[nodiscard]] bool set_guest_root(const std::filesystem::path& root);
    [[nodiscard]] bool guest_root_configured() const noexcept { return !guest_root_.empty(); }
    [[nodiscard]] std::uint64_t direct_memory_size() const noexcept;
    [[nodiscard]] std::int64_t mprotect(GuestAddress address, GuestSize size,
                                        std::uint32_t protection);

    [[nodiscard]] std::int64_t open_readonly(GuestAddress path_address, std::uint32_t flags,
                                             std::uint16_t mode);
    [[nodiscard]] std::int64_t read(int fd, GuestAddress buffer, GuestSize size);
    [[nodiscard]] std::int64_t seek(int fd, std::int64_t offset, int whence);
    [[nodiscard]] std::int64_t stat_path(GuestAddress path_address, GuestAddress stat_address);
    [[nodiscard]] std::int64_t fstat(int fd, GuestAddress stat_address);
    [[nodiscard]] std::int64_t close(int fd);

    struct ThreadAttributes {
        GuestSize stack_size{1024 * 1024};
        bool detached{};
    };
    static constexpr GuestSize kDefaultThreadStackSize = 1024 * 1024;
    static constexpr GuestSize kMinimumThreadStackSize = 16 * 1024;

    [[nodiscard]] int thread_attr_init(GuestAddress slot_address);
    [[nodiscard]] int thread_attr_destroy(GuestAddress slot_address);
    [[nodiscard]] int thread_attr_get_stack_size(GuestAddress slot_address,
                                                  GuestAddress output_address);
    [[nodiscard]] int thread_attr_set_stack_size(GuestAddress slot_address, GuestSize stack_size);
    [[nodiscard]] int thread_attr_get_detach_state(GuestAddress slot_address,
                                                    GuestAddress output_address);
    [[nodiscard]] int thread_attr_set_detach_state(GuestAddress slot_address, int detach_state);
    [[nodiscard]] int thread_attr_snapshot(GuestAddress slot_address,
                                           ThreadAttributes& attributes) const;

    static constexpr std::uint32_t kMutexTypeErrorCheck = 1;
    static constexpr std::uint32_t kMutexTypeRecursive = 2;
    static constexpr std::uint32_t kMutexTypeNormal = 3;
    static constexpr std::uint32_t kMutexTypeAdaptive = 4;

    [[nodiscard]] int mutex_attr_init(GuestAddress slot_address);
    [[nodiscard]] int mutex_attr_destroy(GuestAddress slot_address);
    [[nodiscard]] int mutex_attr_get_type(GuestAddress slot_address, GuestAddress output_address);
    [[nodiscard]] int mutex_attr_set_type(GuestAddress slot_address, std::uint32_t type);
    [[nodiscard]] int mutex_attr_get_pshared(GuestAddress slot_address, GuestAddress output_address);
    [[nodiscard]] int mutex_attr_set_pshared(GuestAddress slot_address, int pshared);

    [[nodiscard]] int mutex_init(GuestAddress slot_address, GuestAddress attributes,
                                 GuestAddress name_address);
    [[nodiscard]] int mutex_lock(GuestAddress slot_address);
    [[nodiscard]] int mutex_unlock(GuestAddress slot_address);
    [[nodiscard]] int mutex_destroy(GuestAddress slot_address);

    [[nodiscard]] int cond_attr_init(GuestAddress slot_address);
    [[nodiscard]] int cond_attr_destroy(GuestAddress slot_address);
    [[nodiscard]] int cond_attr_get_clock(GuestAddress slot_address, GuestAddress output_address);
    [[nodiscard]] int cond_attr_set_clock(GuestAddress slot_address, std::uint32_t clock_id);
    [[nodiscard]] int cond_attr_get_pshared(GuestAddress slot_address, GuestAddress output_address);
    [[nodiscard]] int cond_attr_set_pshared(GuestAddress slot_address, int pshared);

    [[nodiscard]] int cond_init(GuestAddress slot_address, GuestAddress attributes,
                                GuestAddress name_address = 0);
    [[nodiscard]] int cond_destroy(GuestAddress slot_address);
    [[nodiscard]] int cond_wait(GuestAddress slot_address, GuestAddress mutex_slot_address);
    [[nodiscard]] int cond_timed_wait(GuestAddress slot_address, GuestAddress mutex_slot_address,
                                      GuestAddress absolute_timeout_address);
    [[nodiscard]] int cond_reltimed_wait(GuestAddress slot_address, GuestAddress mutex_slot_address,
                                         std::uint64_t microseconds);
    [[nodiscard]] int cond_signal(GuestAddress slot_address);
    [[nodiscard]] int cond_broadcast(GuestAddress slot_address);

    static constexpr std::uint32_t kSemaphoreValueMax = 0x7fffffffU;
    [[nodiscard]] int sem_init(GuestAddress slot_address, int pshared, std::uint32_t value);
    [[nodiscard]] int sem_destroy(GuestAddress slot_address);
    [[nodiscard]] int sem_wait(GuestAddress slot_address);
    [[nodiscard]] int sem_try_wait(GuestAddress slot_address);
    [[nodiscard]] int sem_timed_wait(GuestAddress slot_address, GuestAddress absolute_timeout_address);
    [[nodiscard]] int sem_reltimed_wait(GuestAddress slot_address, std::uint64_t microseconds);
    [[nodiscard]] int sem_post(GuestAddress slot_address);
    [[nodiscard]] int sem_get_value(GuestAddress slot_address, GuestAddress output_address);

    [[nodiscard]] int nanosleep(GuestAddress request_address, GuestAddress remaining_address);
    [[nodiscard]] int realtime_deadline(
        GuestAddress timespec_address, std::chrono::system_clock::time_point& deadline) const;

private:
    struct FileRecord {
        FileRecord(std::ifstream file_in, std::filesystem::path path_in)
            : file(std::move(file_in)), path(std::move(path_in)) {}
        std::mutex mutex;
        std::ifstream file;
        std::filesystem::path path;
    };

    struct MutexAttributes {
        std::uint32_t type{kMutexTypeErrorCheck};
        int pshared{};
    };

    struct MutexRecord {
        std::mutex mutex;
        std::condition_variable condition;
        bool locked{};
        std::thread::id owner;
        std::size_t recursion_depth{};
        std::size_t waiters{};
        std::uint32_t type{kMutexTypeErrorCheck};
    };

    struct CondWaiter {
        std::mutex mutex;
        std::condition_variable condition;
        bool signaled{};
    };

    struct CondAttributes {
        std::uint32_t clock_id{};
        int pshared{};
    };

    struct CondRecord {
        std::mutex mutex;
        std::deque<std::shared_ptr<CondWaiter>> waiters;
        std::uint32_t clock_id{};
    };

    struct GuestTimespec {
        std::int64_t seconds{};
        std::int64_t nanoseconds{};
    };

    enum class CondWaitKind {
        infinite,
        relative,
        absolute,
    };

    struct SemaphoreRecord {
        std::mutex mutex;
        std::condition_variable condition;
        std::uint32_t value{};
        std::size_t waiters{};
    };

    enum class SemaphoreWaitKind {
        infinite,
        relative,
        absolute,
    };

    [[nodiscard]] static std::int64_t error(std::uint32_t value) noexcept;
    [[nodiscard]] bool read_guest_u64(GuestAddress address, std::uint64_t& value) const;
    [[nodiscard]] bool write_guest_u64(GuestAddress address, std::uint64_t value);
    [[nodiscard]] bool write_guest_u32(GuestAddress address, std::uint32_t value);
    [[nodiscard]] bool read_guest_timespec(GuestAddress address, GuestTimespec& value) const;
    [[nodiscard]] bool write_guest_timespec(GuestAddress address, const GuestTimespec& value);
    [[nodiscard]] bool read_guest_c_string(GuestAddress address, std::string& value) const;
    [[nodiscard]] bool guest_writable(GuestAddress address, GuestSize size) const noexcept;
    [[nodiscard]] std::filesystem::path resolve_guest_path(const std::string& guest_path,
                                                           bool& allowed) const;
    [[nodiscard]] std::int64_t write_file_stat(const std::filesystem::path& path,
                                               GuestAddress stat_address);
    [[nodiscard]] int mutex_attr_snapshot(GuestAddress slot_address, MutexAttributes& attributes) const;
    [[nodiscard]] std::uint64_t allocate_mutex_attr_handle_locked();
    [[nodiscard]] std::shared_ptr<MutexRecord> mutex_for_slot_locked(GuestAddress slot_address,
                                                                     bool create_static,
                                                                     int& error_out);
    [[nodiscard]] std::uint64_t allocate_mutex_handle_locked();
    [[nodiscard]] std::uint64_t allocate_thread_attr_handle_locked();
    [[nodiscard]] int cond_attr_snapshot(GuestAddress slot_address, CondAttributes& attributes) const;
    [[nodiscard]] std::uint64_t allocate_cond_attr_handle_locked();
    [[nodiscard]] std::shared_ptr<CondRecord> cond_for_slot_locked(GuestAddress slot_address,
                                                                   bool create_static,
                                                                   int& error_out);
    [[nodiscard]] std::uint64_t allocate_cond_handle_locked();
    [[nodiscard]] int cond_wait_impl(GuestAddress slot_address, GuestAddress mutex_slot_address,
                                     CondWaitKind kind, const GuestTimespec& absolute_timeout,
                                     std::uint64_t relative_microseconds);
    [[nodiscard]] std::uint64_t allocate_semaphore_handle_locked();
    [[nodiscard]] std::shared_ptr<SemaphoreRecord> semaphore_for_slot_locked(
        GuestAddress slot_address, int& error_out);
    [[nodiscard]] int sem_wait_impl(GuestAddress slot_address, SemaphoreWaitKind kind,
                                    const GuestTimespec& absolute_timeout,
                                    std::uint64_t relative_microseconds);

    memory::GuestAddressSpace& memory_;
    std::filesystem::path guest_root_;

    mutable std::mutex files_mutex_;
    int next_fd_{3};
    std::unordered_map<int, std::shared_ptr<FileRecord>> files_;

    mutable std::mutex thread_attrs_mutex_;
    std::uint64_t next_thread_attr_handle_{0x20000};
    std::unordered_map<std::uint64_t, ThreadAttributes> thread_attrs_;

    mutable std::mutex mutex_attrs_mutex_;
    std::uint64_t next_mutex_attr_handle_{0x60000};
    std::unordered_map<std::uint64_t, MutexAttributes> mutex_attrs_;

    mutable std::mutex mutexes_mutex_;
    std::uint64_t next_mutex_handle_{0x10000};
    std::unordered_map<std::uint64_t, std::shared_ptr<MutexRecord>> mutexes_;

    mutable std::mutex cond_attrs_mutex_;
    std::uint64_t next_cond_attr_handle_{0x30000};
    std::unordered_map<std::uint64_t, CondAttributes> cond_attrs_;

    mutable std::mutex conds_mutex_;
    std::uint64_t next_cond_handle_{0x40000};
    std::unordered_map<std::uint64_t, std::shared_ptr<CondRecord>> conds_;

    mutable std::mutex semaphores_mutex_;
    std::uint64_t next_semaphore_handle_{0x50000};
    std::unordered_map<std::uint64_t, std::shared_ptr<SemaphoreRecord>> semaphores_;
};

} // namespace nyxora::runtime
