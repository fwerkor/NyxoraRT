#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "nyxora/base/types.hpp"
#include "nyxora/runtime/guest_thread.hpp"
#include "nyxora/runtime/tls.hpp"

namespace nyxora::runtime {

class KernelServices;

class GuestThreadManager {
public:
    static constexpr int kPosixEsrch = 3;
    static constexpr int kPosixEdeadlk = 11;
    static constexpr int kPosixEinval = 22;
    static constexpr int kPosixEagain = 35;
    static constexpr int kPosixEnotsup = 45;
    static constexpr int kPosixEtimedout = 60;

    explicit GuestThreadManager(const TlsRegistry& tls_registry, KernelServices* kernel_services = nullptr)
        : tls_registry_(tls_registry), kernel_services_(kernel_services) {}
    ~GuestThreadManager();

    GuestThreadManager(const GuestThreadManager&) = delete;
    GuestThreadManager& operator=(const GuestThreadManager&) = delete;

    int create(GuestAddress* handle_out, GuestAddress attributes, GuestAddress start_routine,
               GuestAddress argument, GuestSize stack_size = 1024 * 1024);
    int join(GuestAddress handle, GuestAddress* return_value);
    int timed_join(GuestAddress handle, GuestAddress* return_value,
                   GuestAddress absolute_timeout_address);
    int detach(GuestAddress handle);

    [[nodiscard]] std::size_t size();
    [[nodiscard]] GuestAddress root_handle() const noexcept {
        return reinterpret_cast<GuestAddress>(this);
    }
    [[nodiscard]] static GuestThreadManager* current() noexcept;
    [[nodiscard]] static GuestAddress current_handle() noexcept;
    [[nodiscard]] KernelServices* kernel_services() noexcept { return kernel_services_; }
    [[nodiscard]] const KernelServices* kernel_services() const noexcept { return kernel_services_; }

private:
    friend class ScopedGuestThreadManager;

    struct Record {
        std::optional<GuestThread> thread;
        bool detached{};
        bool join_claimed{};
    };

    int join_impl(GuestAddress handle, GuestAddress* return_value,
                  std::optional<std::chrono::system_clock::time_point> deadline);
    void reap_finished_detached_locked();

    const TlsRegistry& tls_registry_;
    KernelServices* kernel_services_{};
    mutable std::mutex mutex_;
    std::unordered_map<GuestAddress, std::unique_ptr<Record>> threads_;
    bool shutting_down_{};
};

class ScopedGuestThreadManager {
public:
    explicit ScopedGuestThreadManager(GuestThreadManager* manager,
                                      GuestAddress thread_handle = 0) noexcept;
    explicit ScopedGuestThreadManager(GuestThreadManager& manager) noexcept
        : ScopedGuestThreadManager(&manager, manager.root_handle()) {}
    ~ScopedGuestThreadManager();

    ScopedGuestThreadManager(const ScopedGuestThreadManager&) = delete;
    ScopedGuestThreadManager& operator=(const ScopedGuestThreadManager&) = delete;

private:
    GuestThreadManager* previous_{};
    GuestAddress previous_handle_{};
};

} // namespace nyxora::runtime
