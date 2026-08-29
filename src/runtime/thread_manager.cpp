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
    if (handle == 0) {
        return kPosixEsrch;
    }
    if (handle == root_handle()) {
        return kPosixEinval;
    }

    std::unique_ptr<Record> record;
    {
        std::scoped_lock lock(mutex_);
        const auto it = threads_.find(handle);
        if (it == threads_.end()) {
            return kPosixEsrch;
        }
        if (it->second->detached) {
            return kPosixEinval;
        }
        record = std::move(it->second);
        threads_.erase(it);
    }

    if (!record->thread) {
        return kPosixEinval;
    }

    GuestInvocationResult result;
    try {
        result = record->thread->join();
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
    if (it->second->detached) {
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
