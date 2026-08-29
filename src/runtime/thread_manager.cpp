#include "nyxora/runtime/thread_manager.hpp"

#include <new>
#include <system_error>
#include <utility>

namespace nyxora::runtime {
namespace {
thread_local GuestThreadManager* current_manager = nullptr;
}

int GuestThreadManager::create(GuestAddress* handle_out, GuestAddress attributes,
                               GuestAddress start_routine, GuestAddress argument,
                               GuestSize stack_size) {
    if (handle_out == nullptr || start_routine == 0 || attributes != 0) {
        return kPosixEinval;
    }

    std::optional<GuestThread> thread;
    try {
        thread = GuestThread::start(tls_registry_, start_routine, stack_size, argument, 0, 0, this);
    } catch (const std::bad_alloc&) {
        return kPosixEagain;
    } catch (const std::system_error&) {
        return kPosixEagain;
    }
    if (!thread) {
        return kPosixEagain;
    }

    std::unique_ptr<Record> record;
    try {
        record = std::make_unique<Record>(std::move(*thread));
    } catch (const std::bad_alloc&) {
        return kPosixEagain;
    }
    const auto handle = reinterpret_cast<GuestAddress>(record.get());
    try {
        std::scoped_lock lock(mutex_);
        const auto [_, inserted] = threads_.emplace(handle, std::move(record));
        if (!inserted) {
            return kPosixEagain;
        }
    } catch (const std::bad_alloc&) {
        return kPosixEagain;
    }
    *handle_out = handle;
    return 0;
}

int GuestThreadManager::join(GuestAddress handle, GuestAddress* return_value) {
    if (handle == 0) {
        return kPosixEsrch;
    }

    std::unique_ptr<Record> record;
    {
        std::scoped_lock lock(mutex_);
        const auto it = threads_.find(handle);
        if (it == threads_.end()) {
            return kPosixEsrch;
        }
        record = std::move(it->second);
        threads_.erase(it);
    }

    GuestInvocationResult result;
    try {
        result = record->thread.join();
    } catch (const std::exception&) {
        // C++ exceptions must not unwind through a generated guest ABI frame.
        return kPosixEinval;
    }
    if (!result.completed()) {
        return kPosixEinval;
    }
    if (return_value != nullptr) {
        *return_value = static_cast<GuestAddress>(result.value);
    }
    return 0;
}

std::size_t GuestThreadManager::size() const noexcept {
    std::scoped_lock lock(mutex_);
    return threads_.size();
}

GuestThreadManager* GuestThreadManager::current() noexcept {
    return current_manager;
}

ScopedGuestThreadManager::ScopedGuestThreadManager(GuestThreadManager* manager) noexcept
    : previous_(current_manager) {
    current_manager = manager;
}

ScopedGuestThreadManager::~ScopedGuestThreadManager() {
    current_manager = previous_;
}

} // namespace nyxora::runtime
