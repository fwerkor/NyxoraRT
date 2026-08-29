#pragma once

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

    static constexpr int kPosixEperm = 1;
    static constexpr int kPosixEdeadlk = 11;
    static constexpr int kPosixEnomem = 12;
    static constexpr int kPosixEbusy = 16;
    static constexpr int kPosixEinval = 22;

    explicit KernelServices(memory::GuestAddressSpace& memory);

    [[nodiscard]] bool set_guest_root(const std::filesystem::path& root);
    [[nodiscard]] bool guest_root_configured() const noexcept { return !guest_root_.empty(); }
    [[nodiscard]] std::uint64_t direct_memory_size() const noexcept;
    [[nodiscard]] std::int64_t mprotect(GuestAddress address, GuestSize size,
                                        std::uint32_t protection);

    [[nodiscard]] std::int64_t open_readonly(GuestAddress path_address, std::uint32_t flags,
                                             std::uint16_t mode);
    [[nodiscard]] std::int64_t read(int fd, GuestAddress buffer, GuestSize size);
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

    [[nodiscard]] int mutex_init(GuestAddress slot_address, GuestAddress attributes,
                                 GuestAddress name_address);
    [[nodiscard]] int mutex_lock(GuestAddress slot_address);
    [[nodiscard]] int mutex_unlock(GuestAddress slot_address);
    [[nodiscard]] int mutex_destroy(GuestAddress slot_address);

    [[nodiscard]] int cond_init(GuestAddress slot_address, GuestAddress attributes,
                                GuestAddress name_address = 0);
    [[nodiscard]] int cond_destroy(GuestAddress slot_address);
    [[nodiscard]] int cond_wait(GuestAddress slot_address, GuestAddress mutex_slot_address);
    [[nodiscard]] int cond_signal(GuestAddress slot_address);
    [[nodiscard]] int cond_broadcast(GuestAddress slot_address);

private:
    struct FileRecord {
        explicit FileRecord(std::ifstream file_in) : file(std::move(file_in)) {}
        std::mutex mutex;
        std::ifstream file;
    };

    struct MutexRecord {
        std::mutex mutex;
        std::condition_variable condition;
        bool locked{};
        std::thread::id owner;
        std::size_t waiters{};
    };

    struct CondWaiter {
        std::mutex mutex;
        std::condition_variable condition;
        bool signaled{};
    };

    struct CondRecord {
        std::mutex mutex;
        std::deque<std::shared_ptr<CondWaiter>> waiters;
    };

    [[nodiscard]] static std::int64_t error(std::uint32_t value) noexcept;
    [[nodiscard]] bool read_guest_u64(GuestAddress address, std::uint64_t& value) const;
    [[nodiscard]] bool write_guest_u64(GuestAddress address, std::uint64_t value);
    [[nodiscard]] bool write_guest_u32(GuestAddress address, std::uint32_t value);
    [[nodiscard]] bool read_guest_c_string(GuestAddress address, std::string& value) const;
    [[nodiscard]] bool guest_writable(GuestAddress address, GuestSize size) const noexcept;
    [[nodiscard]] std::filesystem::path resolve_guest_path(const std::string& guest_path,
                                                           bool& allowed) const;
    [[nodiscard]] std::shared_ptr<MutexRecord> mutex_for_slot_locked(GuestAddress slot_address,
                                                                     bool create_static,
                                                                     int& error_out);
    [[nodiscard]] std::uint64_t allocate_mutex_handle_locked();
    [[nodiscard]] std::uint64_t allocate_thread_attr_handle_locked();
    [[nodiscard]] std::shared_ptr<CondRecord> cond_for_slot_locked(GuestAddress slot_address,
                                                                   bool create_static,
                                                                   int& error_out);
    [[nodiscard]] std::uint64_t allocate_cond_handle_locked();

    memory::GuestAddressSpace& memory_;
    std::filesystem::path guest_root_;

    mutable std::mutex files_mutex_;
    int next_fd_{3};
    std::unordered_map<int, std::shared_ptr<FileRecord>> files_;

    mutable std::mutex thread_attrs_mutex_;
    std::uint64_t next_thread_attr_handle_{0x20000};
    std::unordered_map<std::uint64_t, ThreadAttributes> thread_attrs_;

    mutable std::mutex mutexes_mutex_;
    std::uint64_t next_mutex_handle_{0x10000};
    std::unordered_map<std::uint64_t, std::shared_ptr<MutexRecord>> mutexes_;

    mutable std::mutex conds_mutex_;
    std::uint64_t next_cond_handle_{0x30000};
    std::unordered_map<std::uint64_t, std::shared_ptr<CondRecord>> conds_;
};

} // namespace nyxora::runtime
