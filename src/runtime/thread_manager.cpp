#include "nyxora/runtime/thread_manager.hpp"
#include "nyxora/runtime/kernel_services.hpp"

#include <new>
#include <system_error>
#include <utility>

namespace nyxora::runtime {
namespace {
thread_local GuestThreadManager* current_manager = nullptr;
thread_local GuestAddress current_thread_handle = 0;
}

GuestThreadManager::~GuestThreadManager() {
    std::unordered_map<GuestAddress, std::unique_ptr<Record>> threads;
    {
        std::scoped_lock lock(mutex_);
        shutting_down_ = true;
        threads.swap(threads_);
    }
    // Records are destroyed outside the manager lock. GuestThread destruction joins any
    // remaining host worker, while a worker that re-enters this manager observes shutdown.
}

int GuestThreadManager::create(GuestAddress* handle_out, GuestAddress attributes,
                               GuestAddress start_routine, GuestAddress argument,
                               GuestSize stack_size) {
    if (handle_out == nullptr || start_routine == 0) {
        return kPosixEinval;
    }

    bool detached = false;
    if (attributes != 0) {
        if (kernel_services_ == nullptr) {
            return kPosixEinval;
        }
        KernelServices::ThreadAttributes resolved;
        const auto result = kernel_services_->thread_attr_snapshot(attributes, resolved);
        if (result != 0) {
            return result;
        }
        stack_size = resolved.stack_size;
        detached = resolved.detached;
    }

    std::unique_ptr<Record> record;
    try {
        record = std::make_unique<Record>();
    } catch (const std::bad_alloc&) {
        return kPosixEagain;
    }
    const auto handle = reinterpret_cast<GuestAddress>(record.get());

    std::unique_lock lock(mutex_);
    if (shutting_down_) {
        return kPosixEagain;
    }
    reap_finished_detached_locked();
    auto [it, inserted] = threads_.emplace(handle, std::move(record));
    if (!inserted) {
        return kPosixEagain;
    }

    std::optional<GuestThread> thread;
    try {
        thread = GuestThread::start(tls_registry_, start_routine, stack_size, argument, 0, 0,
                                    this, handle);
    } catch (const std::bad_alloc&) {
        threads_.erase(it);
        return kPosixEagain;
    } catch (const std::system_error&) {
        threads_.erase(it);
        return kPosixEagain;
    }
    if (!thread) {
        threads_.erase(it);
        return kPosixEagain;
    }

    it->second->thread.emplace(std::move(*thread));
    it->second->detached = detached;
    lock.unlock();
    *handle_out = handle;
    return 0;
}

int GuestThreadManager::join(GuestAddress handle, GuestAddress* return_value) {
    return join_impl(handle, return_value, std::nullopt);
}

int GuestThreadManager::timed_join(GuestAddress handle, GuestAddress* return_value,
                                   GuestAddress absolute_timeout_address) {
    if (kernel_services_ == nullptr) {
        return kPosixEinval;
    }
    std::chrono::system_clock::time_point deadline;
    const auto deadline_result =
        kernel_services_->realtime_deadline(absolute_timeout_address, deadline);
    if (deadline_result != 0) {
        return deadline_result;
    }
    return join_impl(handle, return_value, deadline);
}

int GuestThreadManager::join_impl(
    GuestAddress handle, GuestAddress* return_value,
    std::optional<std::chrono::system_clock::time_point> deadline) {
    if (handle == 0) {
        return kPosixEinval;
    }
    const auto current = current_handle();
    if (current != 0 && handle == current) {
        return kPosixEdeadlk;
    }
    if (handle == root_handle()) {
        return kPosixEinval;
    }

    Record* claimed_record = nullptr;
    GuestThread* thread = nullptr;
    {
        std::scoped_lock lock(mutex_);
        const auto it = threads_.find(handle);
        if (it == threads_.end()) {
            return kPosixEsrch;
        }
        if (it->second->detached) {
            return kPosixEinval;
        }
        if (it->second->join_claimed) {
            return kPosixEnotsup;
        }
        if (!it->second->thread) {
            return kPosixEinval;
        }
        it->second->join_claimed = true;
        claimed_record = it->second.get();
        thread = &*it->second->thread;
    }

    if (deadline && !thread->wait_until(*deadline)) {
        std::scoped_lock lock(mutex_);
        const auto it = threads_.find(handle);
        if (it != threads_.end() && it->second.get() == claimed_record) {
            it->second->join_claimed = false;
        }
        return kPosixEtimedout;
    }

    GuestInvocationResult result;
    try {
        result = thread->join();
    } catch (...) {
        // Host exceptions must never unwind through an HLE call frame. Guest-visible thread
        // failures are reported through the pthread error convention instead.
        std::scoped_lock lock(mutex_);
        const auto it = threads_.find(handle);
        if (it != threads_.end() && it->second.get() == claimed_record) {
            threads_.erase(it);
        }
        return kPosixEinval;
    }

    {
        std::scoped_lock lock(mutex_);
        const auto it = threads_.find(handle);
        if (it == threads_.end() || it->second.get() != claimed_record) {
            return kPosixEsrch;
        }
        threads_.erase(it);
    }

    if (!result.completed()) {
        return kPosixEinval;
    }
    if (return_value != nullptr) {
        *return_value = static_cast<GuestAddress>(result.value);
    }
    return 0;
}

int GuestThreadManager::detach(GuestAddress handle) {
    if (handle == 0) {
        return kPosixEsrch;
    }
    if (handle == root_handle()) {
        return kPosixEinval;
    }

    std::scoped_lock lock(mutex_);
    const auto it = threads_.find(handle);
    if (it == threads_.end()) {
        return kPosixEsrch;
    }
    if (it->second->detached || it->second->join_claimed) {
        return kPosixEinval;
    }
    it->second->detached = true;
    return 0;
}

void GuestThreadManager::reap_finished_detached_locked() {
    for (auto it = threads_.begin(); it != threads_.end();) {
        const auto& record = *it->second;
        if (record.detached && record.thread && record.thread->finished()) {
            it = threads_.erase(it);
        } else {
            ++it;
        }
    }
}

std::size_t GuestThreadManager::size() {
    std::scoped_lock lock(mutex_);
    reap_finished_detached_locked();
    return threads_.size();
}

GuestThreadManager* GuestThreadManager::current() noexcept {
    return current_manager;
}

GuestAddress GuestThreadManager::current_handle() noexcept {
    return current_thread_handle;
}

ScopedGuestThreadManager::ScopedGuestThreadManager(GuestThreadManager* manager,
                                                   GuestAddress thread_handle) noexcept
    : previous_(current_manager), previous_handle_(current_thread_handle) {
    current_manager = manager;
    current_thread_handle = thread_handle;
}

ScopedGuestThreadManager::~ScopedGuestThreadManager() {
    current_manager = previous_;
    current_thread_handle = previous_handle_;
}

} // namespace nyxora::runtime
