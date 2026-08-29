#include "nyxora/runtime/kernel_services.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace nyxora::runtime {
namespace {

constexpr GuestSize kGuestPageSize = 16 * 1024;
constexpr std::size_t kMaxGuestPath = 1024;
constexpr std::uint32_t kOpenWriteOnly = 0x0001;
constexpr std::uint32_t kOpenReadWrite = 0x0002;
constexpr std::uint32_t kOpenAppend = 0x0008;
constexpr std::uint32_t kOpenCreate = 0x0200;
constexpr std::uint32_t kOpenTruncate = 0x0400;
constexpr std::uint32_t kOpenExclusive = 0x0800;
constexpr std::uint32_t kMutatingOpenFlags =
    kOpenWriteOnly | kOpenReadWrite | kOpenAppend | kOpenCreate | kOpenTruncate | kOpenExclusive;
constexpr std::uint64_t kMutexAdaptiveInitializer = 1;
constexpr std::uint64_t kMutexDestroyed = 2;
constexpr std::uint64_t kCondDestroyed = 1;
constexpr std::uint32_t kClockRealtime = 0;
constexpr std::uint32_t kClockVirtual = 1;
constexpr std::uint32_t kClockProf = 2;
constexpr std::uint32_t kClockMonotonic = 4;

bool supported_cond_clock(std::uint32_t clock_id) noexcept {
    return clock_id == kClockRealtime || clock_id == kClockVirtual || clock_id == kClockProf ||
           clock_id == kClockMonotonic;
}

template <typename Duration>
void split_duration(Duration duration, std::int64_t& seconds, std::int64_t& nanoseconds) {
    const auto whole_seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto remainder = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - whole_seconds);
    seconds = whole_seconds.count();
    nanoseconds = remainder.count();
}

bool clock_now(std::uint32_t clock_id, std::int64_t& seconds, std::int64_t& nanoseconds) {
    if (clock_id == kClockRealtime) {
        split_duration(std::chrono::system_clock::now().time_since_epoch(), seconds, nanoseconds);
        return true;
    }
    if (clock_id == kClockMonotonic) {
        split_duration(std::chrono::steady_clock::now().time_since_epoch(), seconds, nanoseconds);
        return true;
    }
#if defined(_WIN32)
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return false;
    }
    const FILETIME& selected = clock_id == kClockVirtual ? user : kernel;
    if (clock_id != kClockVirtual && clock_id != kClockProf) {
        return false;
    }
    ULARGE_INTEGER ticks{};
    ticks.LowPart = selected.dwLowDateTime;
    ticks.HighPart = selected.dwHighDateTime;
    const auto total_100ns = ticks.QuadPart;
    seconds = static_cast<std::int64_t>(total_100ns / 10'000'000ULL);
    nanoseconds = static_cast<std::int64_t>((total_100ns % 10'000'000ULL) * 100ULL);
    return true;
#else
    if (clock_id != kClockVirtual && clock_id != kClockProf) {
        return false;
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return false;
    }
    const timeval& selected = clock_id == kClockVirtual ? usage.ru_utime : usage.ru_stime;
    seconds = static_cast<std::int64_t>(selected.tv_sec);
    nanoseconds = static_cast<std::int64_t>(selected.tv_usec) * 1000;
    return true;
#endif
}

std::chrono::nanoseconds remaining_until(std::int64_t deadline_seconds,
                                         std::int64_t deadline_nanoseconds,
                                         std::int64_t now_seconds,
                                         std::int64_t now_nanoseconds) {
    if (deadline_seconds < now_seconds ||
        (deadline_seconds == now_seconds && deadline_nanoseconds <= now_nanoseconds)) {
        return std::chrono::nanoseconds::zero();
    }
    auto seconds = deadline_seconds - now_seconds;
    auto nanoseconds = deadline_nanoseconds - now_nanoseconds;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += 1'000'000'000;
    }
    constexpr auto max_ns = std::chrono::nanoseconds::max().count();
    constexpr auto max_seconds = max_ns / 1'000'000'000;
    if (seconds > max_seconds) {
        return std::chrono::nanoseconds::max();
    }
    const auto base = seconds * 1'000'000'000;
    if (nanoseconds > max_ns - base) {
        return std::chrono::nanoseconds::max();
    }
    return std::chrono::nanoseconds(base + nanoseconds);
}

std::chrono::nanoseconds relative_timeout(std::uint64_t microseconds) {
    constexpr auto max_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::nanoseconds::max())
            .count());
    if (microseconds > max_us) {
        return std::chrono::nanoseconds::max();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::microseconds(microseconds));
}

bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto root_it = root.begin();
    auto path_it = path.begin();
    for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || *path_it != *root_it) {
            return false;
        }
    }
    return true;
}

} // namespace

KernelServices::KernelServices(memory::GuestAddressSpace& memory) : memory_(memory) {}

bool KernelServices::set_guest_root(const std::filesystem::path& root) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(root, error);
    if (error || !std::filesystem::is_directory(canonical, error) || error) {
        return false;
    }
    guest_root_ = canonical;
    return true;
}

std::int64_t KernelServices::error(std::uint32_t value) noexcept {
    return static_cast<std::int64_t>(static_cast<std::int32_t>(value));
}

std::uint64_t KernelServices::direct_memory_size() const noexcept {
    return memory_.native_backed() ? memory_.native_size() : 0;
}

std::int64_t KernelServices::mprotect(GuestAddress address, GuestSize size,
                                      std::uint32_t protection) {
    constexpr std::uint32_t known_bits = 0x37U;
    if ((protection & ~known_bits) != 0) {
        return error(kErrorEinval);
    }
    if (size == 0) {
        return 0;
    }

    const auto aligned_address = address / kGuestPageSize * kGuestPageSize;
    const auto prefix = address - aligned_address;
    if (size > std::numeric_limits<GuestSize>::max() - prefix) {
        return error(kErrorEinval);
    }
    const auto aligned_size = checked_align_up(size + prefix, kGuestPageSize);
    if (!aligned_size) {
        return error(kErrorEinval);
    }

    memory::Protection host = memory::Protection::none;
    if ((protection & 0x01U) != 0) {
        host = host | memory::Protection::read;
    }
    if ((protection & 0x02U) != 0) {
        host = host | memory::Protection::read | memory::Protection::write;
    }
    if ((protection & 0x04U) != 0) {
        host = host | memory::Protection::execute;
    }
    try {
        return memory_.protect_range(aligned_address, *aligned_size, host) ? 0 : error(kErrorEnomem);
    } catch (const std::bad_alloc&) {
        return error(kErrorEnomem);
    }
}

bool KernelServices::read_guest_u64(GuestAddress address, std::uint64_t& value) const {
    const auto* region = memory_.find(address);
    if (region == nullptr ||
        (!memory::has(region->protection, memory::Protection::read) &&
         !memory::has(region->protection, memory::Protection::write)) ||
        address > std::numeric_limits<GuestAddress>::max() - sizeof(value) ||
        address + sizeof(value) > region->base + region->size) {
        return false;
    }
    const auto bytes = memory_.view(address, sizeof(value));
    if (bytes.size() != sizeof(value)) {
        return false;
    }
    std::memcpy(&value, bytes.data(), sizeof(value));
    return true;
}

bool KernelServices::write_guest_u64(GuestAddress address, std::uint64_t value) {
    if (!guest_writable(address, sizeof(value))) {
        return false;
    }
    const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value),
                                                  sizeof(value));
    return memory_.write(address, bytes);
}

bool KernelServices::write_guest_u32(GuestAddress address, std::uint32_t value) {
    if (!guest_writable(address, sizeof(value))) {
        return false;
    }
    const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value),
                                                  sizeof(value));
    return memory_.write(address, bytes);
}

bool KernelServices::read_guest_timespec(GuestAddress address, GuestTimespec& value) const {
    if (address == 0 || address > std::numeric_limits<GuestAddress>::max() - sizeof(value)) {
        return false;
    }
    const auto* region = memory_.find(address);
    if (region == nullptr ||
        (!memory::has(region->protection, memory::Protection::read) &&
         !memory::has(region->protection, memory::Protection::write)) ||
        address + sizeof(value) > region->base + region->size) {
        return false;
    }
    const auto bytes = memory_.view(address, sizeof(value));
    if (bytes.size() != sizeof(value)) {
        return false;
    }
    std::memcpy(&value, bytes.data(), sizeof(value));
    return true;
}

bool KernelServices::write_guest_timespec(GuestAddress address, const GuestTimespec& value) {
    if (address == 0 || !guest_writable(address, sizeof(value))) {
        return false;
    }
    return memory_.write(
        address, std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value),
                                           sizeof(value)));
}

bool KernelServices::read_guest_c_string(GuestAddress address, std::string& value) const {
    const auto* region = memory_.find(address);
    if (region == nullptr ||
        (!memory::has(region->protection, memory::Protection::read) &&
         !memory::has(region->protection, memory::Protection::write)) ||
        address < region->base || address >= region->base + region->size) {
        return false;
    }
    const auto available = std::min<GuestSize>(region->base + region->size - address, kMaxGuestPath);
    const auto bytes = memory_.view(address, available);
    const auto terminator = std::find(bytes.begin(), bytes.end(), std::byte{0});
    if (terminator == bytes.end()) {
        return false;
    }
    const auto length = static_cast<std::size_t>(terminator - bytes.begin());
    value.assign(reinterpret_cast<const char*>(bytes.data()), length);
    return true;
}

bool KernelServices::guest_writable(GuestAddress address, GuestSize size) const noexcept {
    if (size == 0) {
        return true;
    }
    const auto* region = memory_.find(address);
    if (region == nullptr || !memory::has(region->protection, memory::Protection::write) ||
        address > std::numeric_limits<GuestAddress>::max() - size) {
        return false;
    }
    return address + size <= region->base + region->size;
}

std::filesystem::path KernelServices::resolve_guest_path(const std::string& guest_path,
                                                         bool& allowed) const {
    allowed = false;
    std::filesystem::path relative;
    if (guest_path == "/app0") {
        relative = ".";
    } else if (guest_path.starts_with("/app0/")) {
        relative = guest_path.substr(6);
    } else if (!guest_path.empty() && guest_path.front() != '/') {
        relative = guest_path;
    } else {
        return {};
    }

    std::error_code error;
    const auto candidate = std::filesystem::canonical(guest_root_ / relative, error);
    if (error || !path_is_within(guest_root_, candidate)) {
        return {};
    }
    allowed = true;
    return candidate;
}

std::int64_t KernelServices::open_readonly(GuestAddress path_address, std::uint32_t flags,
                                           std::uint16_t mode) {
    (void)mode;
    if (guest_root_.empty() || (flags & kMutatingOpenFlags) != 0) {
        return error(kErrorEacces);
    }
    try {
        std::string guest_path;
        if (!read_guest_c_string(path_address, guest_path)) {
            return error(kErrorEfault);
        }
        bool allowed = false;
        const auto host_path = resolve_guest_path(guest_path, allowed);
        if (!allowed) {
            return error(kErrorEnoent);
        }

        std::error_code fs_error;
        if (!std::filesystem::is_regular_file(host_path, fs_error) || fs_error) {
            return error(kErrorEnoent);
        }
        std::ifstream stream(host_path, std::ios::binary);
        if (!stream.is_open()) {
            return error(kErrorEacces);
        }

        auto record = std::make_shared<FileRecord>(std::move(stream));
        std::scoped_lock lock(files_mutex_);
        for (std::size_t attempts = 0;
             attempts < static_cast<std::size_t>(std::numeric_limits<int>::max()); ++attempts) {
            if (next_fd_ < 3 || next_fd_ == std::numeric_limits<int>::max()) {
                next_fd_ = 3;
            }
            const int fd = next_fd_;
            ++next_fd_;
            if (!files_.contains(fd)) {
                files_.emplace(fd, std::move(record));
                return fd;
            }
        }
    } catch (const std::bad_alloc&) {
        return error(kErrorEnomem);
    }
    return error(kErrorEnomem);
}

std::int64_t KernelServices::read(int fd, GuestAddress buffer, GuestSize size) {
    if (!guest_writable(buffer, size)) {
        return error(kErrorEfault);
    }
    std::shared_ptr<FileRecord> record;
    {
        std::scoped_lock lock(files_mutex_);
        const auto it = files_.find(fd);
        if (it == files_.end()) {
            return error(kErrorEbadf);
        }
        record = it->second;
    }
    if (size == 0) {
        return 0;
    }

    constexpr std::size_t chunk_size = 64 * 1024;
    std::vector<std::byte> chunk;
    try {
        chunk.resize(chunk_size);
    } catch (const std::bad_alloc&) {
        return error(kErrorEnomem);
    }

    std::scoped_lock lock(record->mutex);
    GuestSize total = 0;
    while (total < size) {
        const auto request = static_cast<std::size_t>(std::min<GuestSize>(size - total, chunk.size()));
        record->file.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(request));
        const auto count = record->file.gcount();
        if (count < 0) {
            return total == 0 ? error(kErrorEbadf) : static_cast<std::int64_t>(total);
        }
        const auto bytes_read = static_cast<std::size_t>(count);
        if (bytes_read != 0 &&
            !memory_.write(buffer + total,
                           std::span<const std::byte>(chunk.data(), bytes_read))) {
            return total == 0 ? error(kErrorEfault) : static_cast<std::int64_t>(total);
        }
        total += bytes_read;
        if (record->file.bad()) {
            return total == 0 ? error(kErrorEbadf) : static_cast<std::int64_t>(total);
        }
        if (bytes_read < request) {
            break;
        }
    }
    return static_cast<std::int64_t>(total);
}

std::int64_t KernelServices::close(int fd) {
    std::scoped_lock lock(files_mutex_);
    return files_.erase(fd) == 1 ? 0 : error(kErrorEbadf);
}

std::uint64_t KernelServices::allocate_thread_attr_handle_locked() {
    while (next_thread_attr_handle_ == 0 || thread_attrs_.contains(next_thread_attr_handle_)) {
        next_thread_attr_handle_ += 8;
        if (next_thread_attr_handle_ < 0x20000) {
            next_thread_attr_handle_ = 0x20000;
        }
    }
    const auto result = next_thread_attr_handle_;
    next_thread_attr_handle_ += 8;
    return result;
}

int KernelServices::thread_attr_init(GuestAddress slot_address) {
    if (slot_address == 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    std::scoped_lock lock(thread_attrs_mutex_);
    const auto handle = allocate_thread_attr_handle_locked();
    try {
        thread_attrs_.emplace(handle, ThreadAttributes{});
    } catch (const std::bad_alloc&) {
        return kPosixEnomem;
    }
    if (!write_guest_u64(slot_address, handle)) {
        thread_attrs_.erase(handle);
        return kPosixEinval;
    }
    return 0;
}

int KernelServices::thread_attr_destroy(GuestAddress slot_address) {
    if (slot_address == 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    std::scoped_lock lock(thread_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = thread_attrs_.find(handle);
    if (it == thread_attrs_.end()) {
        return kPosixEinval;
    }
    if (!write_guest_u64(slot_address, 0)) {
        return kPosixEinval;
    }
    thread_attrs_.erase(it);
    return 0;
}

int KernelServices::thread_attr_snapshot(GuestAddress slot_address,
                                         ThreadAttributes& attributes) const {
    if (slot_address == 0) {
        return kPosixEinval;
    }
    std::scoped_lock lock(thread_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = thread_attrs_.find(handle);
    if (it == thread_attrs_.end()) {
        return kPosixEinval;
    }
    attributes = it->second;
    return 0;
}

int KernelServices::thread_attr_get_stack_size(GuestAddress slot_address,
                                                GuestAddress output_address) {
    if (!guest_writable(output_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    ThreadAttributes attributes;
    const auto result = thread_attr_snapshot(slot_address, attributes);
    if (result != 0) {
        return result;
    }
    return write_guest_u64(output_address, attributes.stack_size) ? 0 : kPosixEinval;
}

int KernelServices::thread_attr_set_stack_size(GuestAddress slot_address, GuestSize stack_size) {
    if (stack_size < kMinimumThreadStackSize) {
        return kPosixEinval;
    }
    std::scoped_lock lock(thread_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = thread_attrs_.find(handle);
    if (it == thread_attrs_.end()) {
        return kPosixEinval;
    }
    it->second.stack_size = stack_size;
    return 0;
}

int KernelServices::thread_attr_get_detach_state(GuestAddress slot_address,
                                                  GuestAddress output_address) {
    if (!guest_writable(output_address, sizeof(std::uint32_t))) {
        return kPosixEinval;
    }
    ThreadAttributes attributes;
    const auto result = thread_attr_snapshot(slot_address, attributes);
    if (result != 0) {
        return result;
    }
    return write_guest_u32(output_address, attributes.detached ? 1U : 0U) ? 0 : kPosixEinval;
}

int KernelServices::thread_attr_set_detach_state(GuestAddress slot_address, int detach_state) {
    if (detach_state != 0 && detach_state != 1) {
        return kPosixEinval;
    }
    std::scoped_lock lock(thread_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = thread_attrs_.find(handle);
    if (it == thread_attrs_.end()) {
        return kPosixEinval;
    }
    it->second.detached = detach_state != 0;
    return 0;
}

std::uint64_t KernelServices::allocate_mutex_handle_locked() {
    while (next_mutex_handle_ <= kMutexDestroyed || mutexes_.contains(next_mutex_handle_)) {
        next_mutex_handle_ += 8;
        if (next_mutex_handle_ < 0x10000) {
            next_mutex_handle_ = 0x10000;
        }
    }
    const auto result = next_mutex_handle_;
    next_mutex_handle_ += 8;
    return result;
}

std::shared_ptr<KernelServices::MutexRecord>
KernelServices::mutex_for_slot_locked(GuestAddress slot_address, bool create_static,
                                      int& error_out) {
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        error_out = kPosixEinval;
        return {};
    }
    if (handle == kMutexDestroyed) {
        error_out = kPosixEinval;
        return {};
    }
    if (handle == 0 || handle == kMutexAdaptiveInitializer) {
        if (!create_static) {
            error_out = kPosixEperm;
            return {};
        }
        std::shared_ptr<MutexRecord> record;
        try {
            record = std::make_shared<MutexRecord>();
        } catch (const std::bad_alloc&) {
            error_out = kPosixEnomem;
            return {};
        }
        handle = allocate_mutex_handle_locked();
        try {
            mutexes_.emplace(handle, record);
        } catch (const std::bad_alloc&) {
            error_out = kPosixEnomem;
            return {};
        }
        if (!write_guest_u64(slot_address, handle)) {
            mutexes_.erase(handle);
            error_out = kPosixEinval;
            return {};
        }
        return record;
    }
    const auto it = mutexes_.find(handle);
    if (it == mutexes_.end()) {
        error_out = kPosixEinval;
        return {};
    }
    return it->second;
}

int KernelServices::mutex_init(GuestAddress slot_address, GuestAddress attributes,
                               GuestAddress name_address) {
    (void)name_address;
    if (slot_address == 0 || attributes != 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    std::scoped_lock table_lock(mutexes_mutex_);
    std::uint64_t current = 0;
    if (!read_guest_u64(slot_address, current)) {
        return kPosixEinval;
    }
    if (current > kMutexDestroyed) {
        return kPosixEbusy;
    }
    std::shared_ptr<MutexRecord> record;
    try {
        record = std::make_shared<MutexRecord>();
    } catch (const std::bad_alloc&) {
        return kPosixEnomem;
    }
    const auto handle = allocate_mutex_handle_locked();
    try {
        mutexes_.emplace(handle, record);
    } catch (const std::bad_alloc&) {
        return kPosixEnomem;
    }
    if (!write_guest_u64(slot_address, handle)) {
        mutexes_.erase(handle);
        return kPosixEinval;
    }
    return 0;
}

int KernelServices::mutex_lock(GuestAddress slot_address) {
    if (slot_address == 0) {
        return kPosixEinval;
    }
    std::shared_ptr<MutexRecord> record;
    std::unique_lock<std::mutex> state_lock;
    {
        std::unique_lock table_lock(mutexes_mutex_);
        int result = 0;
        record = mutex_for_slot_locked(slot_address, true, result);
        if (!record) {
            return result;
        }
        state_lock = std::unique_lock(record->mutex);
    }

    const auto self = std::this_thread::get_id();
    if (record->locked && record->owner == self) {
        return kPosixEdeadlk;
    }
    ++record->waiters;
    try {
        record->condition.wait(state_lock, [&] { return !record->locked; });
    } catch (const std::system_error&) {
        --record->waiters;
        return kPosixEinval;
    }
    --record->waiters;
    record->locked = true;
    record->owner = self;
    return 0;
}

int KernelServices::mutex_unlock(GuestAddress slot_address) {
    if (slot_address == 0) {
        return kPosixEinval;
    }
    std::shared_ptr<MutexRecord> record;
    std::unique_lock<std::mutex> state_lock;
    {
        std::unique_lock table_lock(mutexes_mutex_);
        int result = 0;
        record = mutex_for_slot_locked(slot_address, false, result);
        if (!record) {
            return result;
        }
        state_lock = std::unique_lock(record->mutex);
    }
    if (!record->locked || record->owner != std::this_thread::get_id()) {
        return kPosixEperm;
    }
    record->locked = false;
    record->owner = {};
    state_lock.unlock();
    record->condition.notify_one();
    return 0;
}

int KernelServices::mutex_destroy(GuestAddress slot_address) {
    if (slot_address == 0) {
        return kPosixEinval;
    }
    std::unique_lock table_lock(mutexes_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    if (handle < kMutexDestroyed) {
        return 0;
    }
    if (handle == kMutexDestroyed) {
        return kPosixEinval;
    }
    const auto it = mutexes_.find(handle);
    if (it == mutexes_.end()) {
        return kPosixEinval;
    }
    auto record = it->second;
    std::unique_lock state_lock(record->mutex);
    if (record->locked || record->waiters != 0) {
        return kPosixEbusy;
    }
    mutexes_.erase(it);
    state_lock.unlock();
    return write_guest_u64(slot_address, kMutexDestroyed) ? 0 : kPosixEinval;
}

std::uint64_t KernelServices::allocate_cond_attr_handle_locked() {
    while (next_cond_attr_handle_ == 0 || cond_attrs_.contains(next_cond_attr_handle_)) {
        next_cond_attr_handle_ += 8;
        if (next_cond_attr_handle_ < 0x30000) {
            next_cond_attr_handle_ = 0x30000;
        }
    }
    const auto result = next_cond_attr_handle_;
    next_cond_attr_handle_ += 8;
    return result;
}

int KernelServices::cond_attr_init(GuestAddress slot_address) {
    if (slot_address == 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    std::scoped_lock lock(cond_attrs_mutex_);
    const auto handle = allocate_cond_attr_handle_locked();
    try {
        cond_attrs_.emplace(handle, CondAttributes{});
    } catch (const std::bad_alloc&) {
        return kPosixEnomem;
    }
    if (!write_guest_u64(slot_address, handle)) {
        cond_attrs_.erase(handle);
        return kPosixEinval;
    }
    return 0;
}

int KernelServices::cond_attr_destroy(GuestAddress slot_address) {
    if (slot_address == 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    std::scoped_lock lock(cond_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = cond_attrs_.find(handle);
    if (it == cond_attrs_.end()) {
        return kPosixEinval;
    }
    if (!write_guest_u64(slot_address, 0)) {
        return kPosixEinval;
    }
    cond_attrs_.erase(it);
    return 0;
}

int KernelServices::cond_attr_snapshot(GuestAddress slot_address, CondAttributes& attributes) const {
    if (slot_address == 0) {
        return kPosixEinval;
    }
    std::scoped_lock lock(cond_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = cond_attrs_.find(handle);
    if (it == cond_attrs_.end()) {
        return kPosixEinval;
    }
    attributes = it->second;
    return 0;
}

int KernelServices::cond_attr_get_clock(GuestAddress slot_address, GuestAddress output_address) {
    if (!guest_writable(output_address, sizeof(std::uint32_t))) {
        return kPosixEinval;
    }
    CondAttributes attributes;
    const auto result = cond_attr_snapshot(slot_address, attributes);
    if (result != 0) {
        return result;
    }
    return write_guest_u32(output_address, attributes.clock_id) ? 0 : kPosixEinval;
}

int KernelServices::cond_attr_set_clock(GuestAddress slot_address, std::uint32_t clock_id) {
    if (!supported_cond_clock(clock_id)) {
        return kPosixEinval;
    }
    std::scoped_lock lock(cond_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = cond_attrs_.find(handle);
    if (it == cond_attrs_.end()) {
        return kPosixEinval;
    }
    it->second.clock_id = clock_id;
    return 0;
}

int KernelServices::cond_attr_get_pshared(GuestAddress slot_address, GuestAddress output_address) {
    if (!guest_writable(output_address, sizeof(std::uint32_t))) {
        return kPosixEinval;
    }
    CondAttributes attributes;
    const auto result = cond_attr_snapshot(slot_address, attributes);
    if (result != 0) {
        return result;
    }
    return write_guest_u32(output_address, static_cast<std::uint32_t>(attributes.pshared)) ? 0
                                                                                            : kPosixEinval;
}

int KernelServices::cond_attr_set_pshared(GuestAddress slot_address, int pshared) {
    if (pshared != 0) {
        return kPosixEinval;
    }
    std::scoped_lock lock(cond_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = cond_attrs_.find(handle);
    if (it == cond_attrs_.end()) {
        return kPosixEinval;
    }
    it->second.pshared = 0;
    return 0;
}

std::uint64_t KernelServices::allocate_cond_handle_locked() {
    while (next_cond_handle_ <= kCondDestroyed || conds_.contains(next_cond_handle_)) {
        next_cond_handle_ += 8;
        if (next_cond_handle_ < 0x40000) {
            next_cond_handle_ = 0x40000;
        }
    }
    const auto result = next_cond_handle_;
    next_cond_handle_ += 8;
    return result;
}

std::shared_ptr<KernelServices::CondRecord>
KernelServices::cond_for_slot_locked(GuestAddress slot_address, bool create_static,
                                     int& error_out) {
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        error_out = kPosixEinval;
        return {};
    }
    if (handle == kCondDestroyed) {
        error_out = kPosixEinval;
        return {};
    }
    if (handle == 0) {
        if (!create_static) {
            error_out = kPosixEinval;
            return {};
        }
        std::shared_ptr<CondRecord> record;
        try {
            record = std::make_shared<CondRecord>();
        } catch (const std::bad_alloc&) {
            error_out = kPosixEnomem;
            return {};
        }
        handle = allocate_cond_handle_locked();
        try {
            conds_.emplace(handle, record);
        } catch (const std::bad_alloc&) {
            error_out = kPosixEnomem;
            return {};
        }
        if (!write_guest_u64(slot_address, handle)) {
            conds_.erase(handle);
            error_out = kPosixEinval;
            return {};
        }
        return record;
    }
    const auto it = conds_.find(handle);
    if (it == conds_.end()) {
        error_out = kPosixEinval;
        return {};
    }
    return it->second;
}

int KernelServices::cond_init(GuestAddress slot_address, GuestAddress attributes,
                              GuestAddress name_address) {
    (void)name_address;
    if (slot_address == 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    CondAttributes resolved_attributes;
    if (attributes != 0) {
        const auto result = cond_attr_snapshot(attributes, resolved_attributes);
        if (result != 0) {
            return result;
        }
    }
    std::shared_ptr<CondRecord> record;
    try {
        record = std::make_shared<CondRecord>();
    } catch (const std::bad_alloc&) {
        return kPosixEnomem;
    }
    record->clock_id = resolved_attributes.clock_id;
    std::scoped_lock lock(conds_mutex_);
    const auto handle = allocate_cond_handle_locked();
    try {
        conds_.emplace(handle, record);
    } catch (const std::bad_alloc&) {
        return kPosixEnomem;
    }
    if (!write_guest_u64(slot_address, handle)) {
        conds_.erase(handle);
        return kPosixEinval;
    }
    return 0;
}

int KernelServices::cond_destroy(GuestAddress slot_address) {
    if (slot_address == 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    std::unique_lock table_lock(conds_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    if (handle == 0) {
        return 0;
    }
    if (handle == kCondDestroyed) {
        return kPosixEinval;
    }
    const auto it = conds_.find(handle);
    if (it == conds_.end()) {
        return kPosixEinval;
    }
    auto record = it->second;
    std::scoped_lock state_lock(record->mutex);
    if (!record->waiters.empty()) {
        return kPosixEbusy;
    }
    if (!write_guest_u64(slot_address, kCondDestroyed)) {
        return kPosixEinval;
    }
    conds_.erase(it);
    return 0;
}

int KernelServices::cond_wait(GuestAddress slot_address, GuestAddress mutex_slot_address) {
    return cond_wait_impl(slot_address, mutex_slot_address, CondWaitKind::infinite, GuestTimespec{}, 0);
}

int KernelServices::cond_timed_wait(GuestAddress slot_address, GuestAddress mutex_slot_address,
                                    GuestAddress absolute_timeout_address) {
    GuestTimespec timeout;
    if (!read_guest_timespec(absolute_timeout_address, timeout) || timeout.seconds < 0 ||
        timeout.nanoseconds < 0 || timeout.nanoseconds >= 1'000'000'000) {
        return kPosixEinval;
    }
    return cond_wait_impl(slot_address, mutex_slot_address, CondWaitKind::absolute, timeout, 0);
}

int KernelServices::cond_reltimed_wait(GuestAddress slot_address, GuestAddress mutex_slot_address,
                                       std::uint64_t microseconds) {
    return cond_wait_impl(slot_address, mutex_slot_address, CondWaitKind::relative, GuestTimespec{},
                          microseconds);
}

int KernelServices::cond_wait_impl(GuestAddress slot_address, GuestAddress mutex_slot_address,
                                   CondWaitKind kind, const GuestTimespec& absolute_timeout,
                                   std::uint64_t relative_microseconds) {
    if (slot_address == 0 || mutex_slot_address == 0) {
        return kPosixEinval;
    }

    std::shared_ptr<MutexRecord> mutex;
    {
        std::scoped_lock table_lock(mutexes_mutex_);
        int result = 0;
        mutex = mutex_for_slot_locked(mutex_slot_address, true, result);
        if (!mutex) {
            return result;
        }
    }

    std::shared_ptr<CondWaiter> waiter;
    try {
        waiter = std::make_shared<CondWaiter>();
    } catch (const std::bad_alloc&) {
        return kPosixEnomem;
    }

    std::shared_ptr<CondRecord> cond;
    std::unique_lock<std::mutex> cond_lock;
    {
        std::unique_lock table_lock(conds_mutex_);
        int result = 0;
        cond = cond_for_slot_locked(slot_address, true, result);
        if (!cond) {
            return result;
        }
        cond_lock = std::unique_lock(cond->mutex);
    }

    {
        std::unique_lock mutex_lock(mutex->mutex);
        if (!mutex->locked || mutex->owner != std::this_thread::get_id()) {
            return kPosixEperm;
        }
        try {
            cond->waiters.push_back(waiter);
        } catch (const std::bad_alloc&) {
            return kPosixEnomem;
        }
        mutex->locked = false;
        mutex->owner = {};
        mutex_lock.unlock();
        mutex->condition.notify_one();
    }
    cond_lock.unlock();

    std::chrono::nanoseconds timeout{};
    if (kind == CondWaitKind::relative) {
        timeout = relative_timeout(relative_microseconds);
    } else if (kind == CondWaitKind::absolute) {
        std::int64_t now_seconds = 0;
        std::int64_t now_nanoseconds = 0;
        if (!clock_now(cond->clock_id, now_seconds, now_nanoseconds)) {
            {
                std::unique_lock table_lock(conds_mutex_);
                std::unique_lock state_lock(cond->mutex);
                const auto it = std::find(cond->waiters.begin(), cond->waiters.end(), waiter);
                if (it != cond->waiters.end()) {
                    cond->waiters.erase(it);
                }
            }
            const auto lock_result = mutex_lock(mutex_slot_address);
            return lock_result == 0 ? kPosixEinval : lock_result;
        }
        timeout = remaining_until(absolute_timeout.seconds, absolute_timeout.nanoseconds,
                                  now_seconds, now_nanoseconds);
    }

    bool timed_out = false;
    std::unique_lock waiter_lock(waiter->mutex);
    try {
        if (kind == CondWaitKind::infinite) {
            waiter->condition.wait(waiter_lock, [&] { return waiter->signaled; });
        } else {
            timed_out = !waiter->condition.wait_for(waiter_lock, timeout,
                                                    [&] { return waiter->signaled; });
        }
    } catch (const std::system_error&) {
        waiter_lock.unlock();
        {
            std::unique_lock table_lock(conds_mutex_);
            std::unique_lock state_lock(cond->mutex);
            const auto it = std::find(cond->waiters.begin(), cond->waiters.end(), waiter);
            if (it != cond->waiters.end()) {
                cond->waiters.erase(it);
            }
        }
        const auto lock_result = mutex_lock(mutex_slot_address);
        return lock_result == 0 ? kPosixEinval : lock_result;
    }
    waiter_lock.unlock();

    if (timed_out) {
        std::unique_lock table_lock(conds_mutex_);
        std::unique_lock state_lock(cond->mutex);
        std::scoped_lock token_lock(waiter->mutex);
        if (waiter->signaled) {
            timed_out = false;
        } else {
            const auto it = std::find(cond->waiters.begin(), cond->waiters.end(), waiter);
            if (it != cond->waiters.end()) {
                cond->waiters.erase(it);
            }
        }
    }

    const auto lock_result = mutex_lock(mutex_slot_address);
    if (lock_result != 0) {
        return lock_result;
    }
    return timed_out ? kPosixEtimedout : 0;
}

int KernelServices::cond_signal(GuestAddress slot_address) {
    if (slot_address == 0) {
        return kPosixEinval;
    }

    std::shared_ptr<CondRecord> cond;
    std::shared_ptr<CondWaiter> waiter;
    {
        std::unique_lock table_lock(conds_mutex_);
        int result = 0;
        cond = cond_for_slot_locked(slot_address, true, result);
        if (!cond) {
            return result;
        }
        std::unique_lock cond_lock(cond->mutex);
        table_lock.unlock();
        if (cond->waiters.empty()) {
            return 0;
        }
        waiter = cond->waiters.front();
        cond->waiters.pop_front();
        std::scoped_lock waiter_lock(waiter->mutex);
        waiter->signaled = true;
    }
    waiter->condition.notify_one();
    return 0;
}

int KernelServices::cond_broadcast(GuestAddress slot_address) {
    if (slot_address == 0) {
        return kPosixEinval;
    }

    std::shared_ptr<CondRecord> cond;
    std::deque<std::shared_ptr<CondWaiter>> waiters;
    {
        std::unique_lock table_lock(conds_mutex_);
        int result = 0;
        cond = cond_for_slot_locked(slot_address, true, result);
        if (!cond) {
            return result;
        }
        std::unique_lock cond_lock(cond->mutex);
        table_lock.unlock();
        waiters.swap(cond->waiters);
        for (const auto& waiter : waiters) {
            std::scoped_lock waiter_lock(waiter->mutex);
            waiter->signaled = true;
        }
    }
    for (const auto& waiter : waiters) {
        waiter->condition.notify_one();
    }
    return 0;
}


int KernelServices::nanosleep(GuestAddress request_address, GuestAddress remaining_address) {
    GuestTimespec request;
    if (!read_guest_timespec(request_address, request) || request.seconds < 0 ||
        request.nanoseconds < 0 || request.nanoseconds >= 1'000'000'000) {
        return kPosixEinval;
    }

    constexpr auto max_ns = std::chrono::nanoseconds::max().count();
    constexpr auto max_seconds = max_ns / 1'000'000'000;
    std::chrono::nanoseconds duration;
    if (request.seconds > max_seconds) {
        duration = std::chrono::nanoseconds::max();
    } else {
        const auto base = request.seconds * 1'000'000'000;
        const auto total = request.nanoseconds > max_ns - base
                               ? max_ns
                               : base + request.nanoseconds;
        duration = std::chrono::nanoseconds(total);
    }
    std::this_thread::sleep_for(duration);

    if (remaining_address != 0) {
        const GuestTimespec remaining{};
        if (!write_guest_timespec(remaining_address, remaining)) {
            return kPosixEinval;
        }
    }
    return 0;
}

std::uint64_t KernelServices::allocate_semaphore_handle_locked() {
    while (next_semaphore_handle_ == 0 || semaphores_.contains(next_semaphore_handle_)) {
        next_semaphore_handle_ += 8;
        if (next_semaphore_handle_ < 0x50000) {
            next_semaphore_handle_ = 0x50000;
        }
    }
    const auto result = next_semaphore_handle_;
    next_semaphore_handle_ += 8;
    return result;
}

std::shared_ptr<KernelServices::SemaphoreRecord>
KernelServices::semaphore_for_slot_locked(GuestAddress slot_address, int& error_out) {
    std::uint64_t handle = 0;
    if (slot_address == 0 || !read_guest_u64(slot_address, handle) || handle == 0) {
        error_out = kPosixEinval;
        return {};
    }
    const auto it = semaphores_.find(handle);
    if (it == semaphores_.end()) {
        error_out = kPosixEinval;
        return {};
    }
    return it->second;
}

int KernelServices::sem_init(GuestAddress slot_address, int pshared, std::uint32_t value) {
    (void)pshared;
    if (value > kSemaphoreValueMax) {
        return kPosixEinval;
    }
    if (slot_address == 0) {
        return 0;
    }
    if (!guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    std::shared_ptr<SemaphoreRecord> record;
    try {
        record = std::make_shared<SemaphoreRecord>();
    } catch (const std::bad_alloc&) {
        return kPosixEnomem;
    }
    record->value = value;
    std::scoped_lock table_lock(semaphores_mutex_);
    const auto handle = allocate_semaphore_handle_locked();
    try {
        semaphores_.emplace(handle, record);
    } catch (const std::bad_alloc&) {
        return kPosixEnomem;
    }
    if (!write_guest_u64(slot_address, handle)) {
        semaphores_.erase(handle);
        return kPosixEinval;
    }
    return 0;
}

int KernelServices::sem_destroy(GuestAddress slot_address) {
    if (slot_address == 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    std::unique_lock table_lock(semaphores_mutex_);
    int error = 0;
    auto record = semaphore_for_slot_locked(slot_address, error);
    if (!record) {
        return error;
    }
    std::scoped_lock state_lock(record->mutex);
    if (record->waiters != 0) {
        return kPosixEbusy;
    }
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle) || !write_guest_u64(slot_address, 0)) {
        return kPosixEinval;
    }
    semaphores_.erase(handle);
    return 0;
}

int KernelServices::sem_wait(GuestAddress slot_address) {
    return sem_wait_impl(slot_address, SemaphoreWaitKind::infinite, GuestTimespec{}, 0);
}

int KernelServices::sem_try_wait(GuestAddress slot_address) {
    std::shared_ptr<SemaphoreRecord> record;
    {
        std::scoped_lock table_lock(semaphores_mutex_);
        int error = 0;
        record = semaphore_for_slot_locked(slot_address, error);
        if (!record) {
            return error;
        }
    }
    std::scoped_lock lock(record->mutex);
    if (record->value == 0) {
        return kPosixEagain;
    }
    --record->value;
    return 0;
}

int KernelServices::sem_timed_wait(GuestAddress slot_address,
                                   GuestAddress absolute_timeout_address) {
    // libkernel consumes an immediately available token before validating the timeout pointer.
    {
        std::shared_ptr<SemaphoreRecord> record;
        {
            std::scoped_lock table_lock(semaphores_mutex_);
            int error = 0;
            record = semaphore_for_slot_locked(slot_address, error);
            if (!record) {
                return error;
            }
        }
        std::scoped_lock lock(record->mutex);
        if (record->value != 0) {
            --record->value;
            return 0;
        }
    }

    GuestTimespec timeout;
    if (!read_guest_timespec(absolute_timeout_address, timeout) || timeout.nanoseconds < 0 ||
        timeout.nanoseconds >= 1'000'000'000) {
        return kPosixEinval;
    }
    return sem_wait_impl(slot_address, SemaphoreWaitKind::absolute, timeout, 0);
}

int KernelServices::sem_reltimed_wait(GuestAddress slot_address, std::uint64_t microseconds) {
    return sem_wait_impl(slot_address, SemaphoreWaitKind::relative, GuestTimespec{}, microseconds);
}

int KernelServices::sem_wait_impl(GuestAddress slot_address, SemaphoreWaitKind kind,
                                  const GuestTimespec& absolute_timeout,
                                  std::uint64_t relative_microseconds) {
    std::shared_ptr<SemaphoreRecord> record;
    std::unique_lock<std::mutex> state_lock;
    {
        std::unique_lock table_lock(semaphores_mutex_);
        int error = 0;
        record = semaphore_for_slot_locked(slot_address, error);
        if (!record) {
            return error;
        }
        state_lock = std::unique_lock(record->mutex);
        if (record->value != 0) {
            --record->value;
            return 0;
        }
        // Publish the waiter while the table entry is still protected. sem_destroy follows the
        // same table -> record lock order, so it cannot remove this guest-visible semaphore in
        // the lookup-to-wait gap.
        ++record->waiters;
    }

    std::chrono::nanoseconds timeout{};
    if (kind == SemaphoreWaitKind::relative) {
        timeout = relative_timeout(relative_microseconds);
    } else if (kind == SemaphoreWaitKind::absolute) {
        std::int64_t now_seconds = 0;
        std::int64_t now_nanoseconds = 0;
        if (!clock_now(kClockRealtime, now_seconds, now_nanoseconds)) {
            --record->waiters;
            return kPosixEinval;
        }
        timeout = remaining_until(absolute_timeout.seconds, absolute_timeout.nanoseconds,
                                  now_seconds, now_nanoseconds);
    }

    bool acquired = false;
    try {
        if (kind == SemaphoreWaitKind::infinite) {
            record->condition.wait(state_lock, [&] { return record->value != 0; });
            acquired = true;
        } else {
            acquired = record->condition.wait_for(state_lock, timeout,
                                                  [&] { return record->value != 0; });
        }
    } catch (const std::system_error&) {
        --record->waiters;
        return kPosixEinval;
    }
    --record->waiters;
    if (!acquired) {
        return kPosixEtimedout;
    }
    --record->value;
    return 0;
}

int KernelServices::sem_post(GuestAddress slot_address) {
    std::shared_ptr<SemaphoreRecord> record;
    {
        std::scoped_lock table_lock(semaphores_mutex_);
        int error = 0;
        record = semaphore_for_slot_locked(slot_address, error);
        if (!record) {
            return error;
        }
    }
    std::unique_lock lock(record->mutex);
    if (record->value == kSemaphoreValueMax) {
        return kPosixEoverflow;
    }
    ++record->value;
    lock.unlock();
    // libkernel wakes all published waiters after adding one token; only one can consume it.
    record->condition.notify_all();
    return 0;
}

int KernelServices::sem_get_value(GuestAddress slot_address, GuestAddress output_address) {
    std::shared_ptr<SemaphoreRecord> record;
    {
        std::scoped_lock table_lock(semaphores_mutex_);
        int error = 0;
        record = semaphore_for_slot_locked(slot_address, error);
        if (!record) {
            return error;
        }
    }
    std::scoped_lock lock(record->mutex);
    if (output_address == 0) {
        return 0;
    }
    return write_guest_u32(output_address, record->value) ? 0 : kPosixEinval;
}

} // namespace nyxora::runtime
