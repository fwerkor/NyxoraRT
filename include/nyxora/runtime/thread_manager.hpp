#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "nyxora/base/types.hpp"
#include "nyxora/runtime/guest_thread.hpp"
#include "nyxora/runtime/tls.hpp"

namespace nyxora::runtime {

class GuestThreadManager {
public:
    static constexpr int kPosixEsrch = 3;
    static constexpr int kPosixEinval = 22;
    static constexpr int kPosixEagain = 35;

    explicit GuestThreadManager(const TlsRegistry& tls_registry) : tls_registry_(tls_registry) {}

    GuestThreadManager(const GuestThreadManager&) = delete;
    GuestThreadManager& operator=(const GuestThreadManager&) = delete;

    int create(GuestAddress* handle_out, GuestAddress attributes, GuestAddress start_routine,
               GuestAddress argument, GuestSize stack_size = 1024 * 1024);
    int join(GuestAddress handle, GuestAddress* return_value);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] static GuestThreadManager* current() noexcept;

private:
    friend class ScopedGuestThreadManager;

    struct Record {
        explicit Record(GuestThread thread_in) : thread(std::move(thread_in)) {}
        GuestThread thread;
    };

    const TlsRegistry& tls_registry_;
    mutable std::mutex mutex_;
    std::unordered_map<GuestAddress, std::unique_ptr<Record>> threads_;
};

class ScopedGuestThreadManager {
public:
    explicit ScopedGuestThreadManager(GuestThreadManager* manager) noexcept;
    explicit ScopedGuestThreadManager(GuestThreadManager& manager) noexcept
        : ScopedGuestThreadManager(&manager) {}
    ~ScopedGuestThreadManager();

    ScopedGuestThreadManager(const ScopedGuestThreadManager&) = delete;
    ScopedGuestThreadManager& operator=(const ScopedGuestThreadManager&) = delete;

private:
    GuestThreadManager* previous_{};
};

} // namespace nyxora::runtime
