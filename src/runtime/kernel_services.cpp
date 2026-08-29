#include "nyxora/runtime/kernel_services.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <system_error>
#include <vector>

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

} // namespace nyxora::runtime
