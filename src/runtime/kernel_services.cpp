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
constexpr GuestAddress kDefaultMappingBase = 0x2'0000'0000ULL;
constexpr std::uint32_t kMapShared = 0x0001;
constexpr std::uint32_t kMapPrivate = 0x0002;
constexpr std::uint32_t kMapFixed = 0x0010;
constexpr std::uint32_t kMapNoOverwrite = 0x0080;
constexpr std::uint32_t kMapVoid = 0x0100;
constexpr std::uint32_t kMapStack = 0x0400;
constexpr std::uint32_t kMapNoSync = 0x0800;
constexpr std::uint32_t kMapAnon = 0x1000;
constexpr std::uint32_t kMapSystem = 0x2000;
constexpr std::uint32_t kMapNoCore = 0x20000;
constexpr std::uint32_t kMapNoCoalesce = 0x400000;
constexpr std::uint32_t kKnownMapFlags =
    kMapShared | kMapPrivate | kMapFixed | kMapNoOverwrite | kMapVoid | kMapStack |
    kMapNoSync | kMapAnon | kMapSystem | kMapNoCore | kMapNoCoalesce;
constexpr std::uint32_t kKnownProtectionBits = 0x37U;
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

constexpr std::uint16_t kModeDirectory = 0040000;
constexpr std::uint16_t kModeRegular = 0100000;
constexpr std::uint16_t kModePermissions = 0000777;

struct GuestStatTimespec {
    std::int64_t seconds{};
    std::int64_t nanoseconds{};
};

struct GuestFileStat {
    std::uint32_t device{};
    std::uint32_t inode{};
    std::uint16_t mode{};
    std::uint16_t link_count{};
    std::uint32_t uid{};
    std::uint32_t gid{};
    std::uint32_t rdev{};
    GuestStatTimespec access_time{};
    GuestStatTimespec modification_time{};
    GuestStatTimespec change_time{};
    std::int64_t size{};
    std::int64_t blocks{};
    std::uint32_t block_size{};
    std::uint32_t flags{};
    std::uint32_t generation{};
    std::int32_t spare{};
    GuestStatTimespec birth_time{};
};

static_assert(sizeof(GuestStatTimespec) == 16);
static_assert(sizeof(GuestFileStat) == 120);

bool supported_mutex_type(std::uint32_t type) noexcept {
    return type >= KernelServices::kMutexTypeErrorCheck &&
           type <= KernelServices::kMutexTypeAdaptive;
}

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

bool file_modification_time(const std::filesystem::path& path, std::int64_t& seconds,
                            std::int64_t& nanoseconds) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return false;
    }
    ULARGE_INTEGER ticks{};
    ticks.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    constexpr std::uint64_t ticks_per_second = 10'000'000ULL;
    constexpr std::uint64_t unix_epoch_ticks = 116'444'736'000'000'000ULL;
    if (ticks.QuadPart >= unix_epoch_ticks) {
        const auto elapsed = ticks.QuadPart - unix_epoch_ticks;
        seconds = static_cast<std::int64_t>(elapsed / ticks_per_second);
        nanoseconds = static_cast<std::int64_t>((elapsed % ticks_per_second) * 100ULL);
        return true;
    }

    const auto before_epoch = unix_epoch_ticks - ticks.QuadPart;
    const auto whole_seconds = before_epoch / ticks_per_second;
    const auto remainder = before_epoch % ticks_per_second;
    if (remainder == 0) {
        seconds = -static_cast<std::int64_t>(whole_seconds);
        nanoseconds = 0;
    } else {
        seconds = -static_cast<std::int64_t>(whole_seconds) - 1;
        nanoseconds = static_cast<std::int64_t>((ticks_per_second - remainder) * 100ULL);
    }
    return true;
#else
    std::error_code error;
    const auto write_time = std::filesystem::last_write_time(path, error);
    if (error) {
        return false;
    }
    const auto system_time = std::filesystem::file_time_type::clock::to_sys(write_time);
    split_duration(system_time.time_since_epoch(), seconds, nanoseconds);
    return true;
#endif
}

std::optional<memory::Protection> cpu_protection(std::uint32_t protection) {
    if ((protection & ~kKnownProtectionBits) != 0) {
        return std::nullopt;
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
    return host;
}

bool ranges_overlap(GuestAddress left_base, GuestSize left_size, const memory::RegionInfo& right) {
    if (left_size == 0 || right.size == 0 ||
        left_base > std::numeric_limits<GuestAddress>::max() - left_size ||
        right.base > std::numeric_limits<GuestAddress>::max() - right.size) {
        return false;
    }
    return left_base < right.base + right.size && right.base < left_base + left_size;
}

bool range_inside_native(const memory::GuestAddressSpace& memory, GuestAddress base, GuestSize size) {
    if (!memory.native_backed()) {
        return base <= std::numeric_limits<GuestAddress>::max() - size;
    }
    const auto native_base = memory.native_base();
    if (native_base > std::numeric_limits<GuestAddress>::max() - memory.native_size()) {
        return false;
    }
    const auto native_end = native_base + memory.native_size();
    return base >= native_base && base <= native_end && size <= native_end - base;
}

std::optional<GuestAddress> find_free_mapping(const memory::GuestAddressSpace& memory,
                                              GuestAddress requested, GuestSize size) {
    GuestAddress candidate = requested;
    if (memory.native_backed()) {
        const auto aligned_native = checked_align_up(memory.native_base(), kGuestPageSize);
        if (!aligned_native) {
            return std::nullopt;
        }
        candidate = std::max(candidate, *aligned_native);
    }
    if (candidate == 0) {
        candidate = kDefaultMappingBase;
    }
    if (candidate % kGuestPageSize != 0) {
        const auto aligned = checked_align_up(candidate, kGuestPageSize);
        if (!aligned) {
            return std::nullopt;
        }
        candidate = *aligned;
    }

    for (const auto& region : memory.regions()) {
        if (region.base > std::numeric_limits<GuestAddress>::max() - region.size) {
            return std::nullopt;
        }
        const auto region_end = region.base + region.size;
        if (region_end <= candidate) {
            continue;
        }
        if (candidate <= region.base && size <= region.base - candidate) {
            return range_inside_native(memory, candidate, size) ? std::optional{candidate}
                                                                 : std::nullopt;
        }
        const auto aligned = checked_align_up(region_end, kGuestPageSize);
        if (!aligned) {
            return std::nullopt;
        }
        candidate = *aligned;
    }
    return range_inside_native(memory, candidate, size) ? std::optional{candidate} : std::nullopt;
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
    const auto host_protection = cpu_protection(protection);
    if (!host_protection) {
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

    try {
        return memory_.protect_range(aligned_address, *aligned_size, *host_protection)
                   ? 0
                   : error(kErrorEnomem);
    } catch (const std::bad_alloc&) {
        return error(kErrorEnomem);
    }
}

std::int64_t KernelServices::map_memory(GuestAddress address, GuestSize size,
                                        std::uint32_t protection, std::uint32_t flags, int fd,
                                        std::int64_t offset) {
    if (size == 0 || (flags & ~kKnownMapFlags) != 0) {
        return error(kErrorEinval);
    }
    const auto host_protection = cpu_protection(protection);
    if (!host_protection) {
        return error(kErrorEinval);
    }
    if ((protection & 0x30U) != 0) {
        return error(kErrorEnotsup);
    }
    const auto aligned_size = checked_align_up(size, kGuestPageSize);
    if (!aligned_size) {
        return error(kErrorEinval);
    }

    GuestAddress aligned_address = address;
    if (address != 0) {
        const auto aligned = checked_align_up(address, kGuestPageSize);
        if (!aligned) {
            return error(kErrorEinval);
        }
        aligned_address = *aligned;
    }
    const bool fixed = (flags & kMapFixed) != 0;
    if (fixed && address != aligned_address) {
        return error(kErrorEinval);
    }

    const bool anonymous = (flags & kMapAnon) != 0;
    const bool stack = !anonymous && (flags & kMapStack) != 0;
    const bool reserved = !anonymous && !stack && (flags & kMapVoid) != 0;
    if (anonymous && (flags & kMapSystem) != 0) {
        return error(kErrorEnotsup);
    }
    const bool file_mapping = !anonymous && !stack && !reserved;
    if (file_mapping && ((flags & kMapShared) != 0 || (flags & kMapPrivate) == 0)) {
        return error(kErrorEnotsup);
    }
    if (file_mapping && (offset < 0 || static_cast<std::uint64_t>(offset) % kGuestPageSize != 0)) {
        return error(kErrorEinval);
    }

    memory::Protection final_protection = *host_protection;
    std::shared_ptr<FileRecord> record;
    std::vector<std::byte> file_snapshot;
    if (file_mapping) {
        {
            std::scoped_lock lock(files_mutex_);
            const auto it = files_.find(fd);
            if (it == files_.end()) {
                return error(kErrorEbadf);
            }
            record = it->second;
        }
        std::scoped_lock file_lock(record->mutex);
        const auto saved_state = record->file.rdstate();
        auto restore_file_position = [&] {
            record->file.clear();
            record->file.seekg(static_cast<std::streamoff>(record->position), std::ios::beg);
            const bool restored = static_cast<bool>(record->file);
            record->file.clear(saved_state);
            return restored;
        };

        record->file.clear();
        record->file.seekg(0, std::ios::end);
        const auto end_position = record->file.tellg();
        bool snapshot_ok = end_position != std::streampos{-1};
        std::uint64_t file_size = 0;
        if (snapshot_ok) {
            const auto stream_size = static_cast<std::streamoff>(end_position);
            snapshot_ok = stream_size >= 0;
            if (snapshot_ok) {
                file_size = static_cast<std::uint64_t>(stream_size);
            }
        }
        const auto file_offset = static_cast<std::uint64_t>(offset);
        snapshot_ok = snapshot_ok && file_offset <= file_size && size <= file_size - file_offset;
        if (!snapshot_ok) {
            return restore_file_position() ? error(kErrorEnotsup) : error(kErrorEbadf);
        }
        if (size > static_cast<GuestSize>(std::numeric_limits<std::size_t>::max())) {
            return restore_file_position() ? error(kErrorEnomem) : error(kErrorEbadf);
        }
        try {
            file_snapshot.resize(static_cast<std::size_t>(size));
        } catch (const std::bad_alloc&) {
            return restore_file_position() ? error(kErrorEnomem) : error(kErrorEbadf);
        }

        record->file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        snapshot_ok = static_cast<bool>(record->file);
        std::size_t copied = 0;
        constexpr std::size_t snapshot_chunk_size = 64 * 1024;
        while (snapshot_ok && copied < file_snapshot.size()) {
            const auto request = std::min(snapshot_chunk_size, file_snapshot.size() - copied);
            record->file.read(reinterpret_cast<char*>(file_snapshot.data() + copied),
                              static_cast<std::streamsize>(request));
            if (record->file.gcount() != static_cast<std::streamsize>(request)) {
                snapshot_ok = false;
                break;
            }
            copied += request;
        }
        const bool restored = restore_file_position();
        if (!restored) {
            return error(kErrorEbadf);
        }
        if (!snapshot_ok) {
            return error(kErrorEnotsup);
        }
        final_protection = (protection & 0x03U) != 0 ? memory::Protection::read
                                                     : memory::Protection::none;
    }

    std::string mapping_name;
    try {
        mapping_name = reserved ? "mmap-reserved"
                       : stack ? "mmap-stack"
                       : file_mapping ? "mmap-file"
                                      : "mmap-anon";
    } catch (const std::bad_alloc&) {
        return error(kErrorEnomem);
    }

    GuestAddress mapped_address{};
    if (fixed) {
        if (!range_inside_native(memory_, aligned_address, *aligned_size)) {
            return error(kErrorEnomem);
        }
        if ((flags & kMapNoOverwrite) != 0) {
            for (const auto& region : memory_.regions()) {
                if (ranges_overlap(aligned_address, *aligned_size, region)) {
                    return error(kErrorEnomem);
                }
            }
        } else if (!memory_.unmap_range(aligned_address, *aligned_size)) {
            return error(kErrorEnomem);
        }
        mapped_address = aligned_address;
    } else {
        const auto free = find_free_mapping(memory_, aligned_address, *aligned_size);
        if (!free) {
            return error(kErrorEnomem);
        }
        mapped_address = *free;
    }

    const auto staging = memory::Protection::read | memory::Protection::write;
    try {
        if (!memory_.map(mapped_address, *aligned_size, reserved ? memory::Protection::none : staging,
                         std::move(mapping_name))) {
            return error(kErrorEnomem);
        }
        if (reserved) {
            return static_cast<std::int64_t>(mapped_address);
        }
        if (!memory_.zero(mapped_address, *aligned_size)) {
            (void)memory_.unmap_range(mapped_address, *aligned_size);
            return error(kErrorEnomem);
        }

        if (file_mapping && !file_snapshot.empty() &&
            !memory_.write(mapped_address, file_snapshot)) {
            (void)memory_.unmap_range(mapped_address, *aligned_size);
            return error(kErrorEnomem);
        }

        if (final_protection != staging &&
            !memory_.protect(mapped_address, *aligned_size, final_protection)) {
            (void)memory_.unmap_range(mapped_address, *aligned_size);
            return error(kErrorEnomem);
        }
        return static_cast<std::int64_t>(mapped_address);
    } catch (const std::bad_alloc&) {
        if (memory_.find(mapped_address) != nullptr) {
            (void)memory_.unmap_range(mapped_address, *aligned_size);
        }
        return error(kErrorEnomem);
    }
}

std::int64_t KernelServices::map_memory_to(GuestAddress address, GuestSize size,
                                           std::uint32_t protection, std::uint32_t flags, int fd,
                                           std::int64_t offset, GuestAddress output_address) {
    if (!guest_writable(output_address, sizeof(std::uint64_t))) {
        return error(kErrorEfault);
    }
    const auto mapped = map_memory(address, size, protection, flags, fd, offset);
    if (mapped < 0) {
        return mapped;
    }
    if (!write_guest_u64(output_address, static_cast<std::uint64_t>(mapped))) {
        (void)unmap_memory(static_cast<GuestAddress>(mapped), size);
        return error(kErrorEfault);
    }
    return 0;
}

std::int64_t KernelServices::unmap_memory(GuestAddress address, GuestSize size) {
    if (size == 0) {
        return error(kErrorEinval);
    }
    const auto aligned_address = address / kGuestPageSize * kGuestPageSize;
    const auto aligned_size = checked_align_up(size, kGuestPageSize);
    if (!aligned_size || !range_inside_native(memory_, aligned_address, *aligned_size)) {
        return error(kErrorEinval);
    }
    try {
        return memory_.unmap_range(aligned_address, *aligned_size) ? 0 : error(kErrorEinval);
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

        auto record = std::make_shared<FileRecord>(std::move(stream), host_path);
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
        if (bytes_read > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max() - record->position)) {
            return total == 0 ? error(kErrorEbadf) : static_cast<std::int64_t>(total);
        }
        record->position += static_cast<std::int64_t>(bytes_read);
        if (record->file.bad()) {
            return total == 0 ? error(kErrorEbadf) : static_cast<std::int64_t>(total);
        }
        if (bytes_read < request) {
            break;
        }
    }
    return static_cast<std::int64_t>(total);
}

std::int64_t KernelServices::seek(int fd, std::int64_t offset, int whence) {
    std::shared_ptr<FileRecord> record;
    {
        std::scoped_lock lock(files_mutex_);
        const auto it = files_.find(fd);
        if (it == files_.end()) {
            return error(kErrorEbadf);
        }
        record = it->second;
    }

    std::ios_base::seekdir origin{};
    switch (whence) {
    case 0:
        origin = std::ios::beg;
        break;
    case 1:
        origin = std::ios::cur;
        break;
    case 2:
        origin = std::ios::end;
        break;
    case 3:
    case 4:
        return error(kErrorEnotty);
    default:
        return error(kErrorEinval);
    }
    if (offset < static_cast<std::int64_t>(std::numeric_limits<std::streamoff>::min()) ||
        offset > static_cast<std::int64_t>(std::numeric_limits<std::streamoff>::max())) {
        return error(kErrorEinval);
    }

    std::scoped_lock lock(record->mutex);
    record->file.clear();
    record->file.seekg(static_cast<std::streamoff>(offset), origin);
    if (!record->file) {
        record->file.clear();
        return error(kErrorEinval);
    }
    const auto position = record->file.tellg();
    if (position == std::streampos{-1}) {
        record->file.clear();
        return error(kErrorEinval);
    }
    const auto stream_offset = static_cast<std::streamoff>(position);
    if (stream_offset < 0) {
        return error(kErrorEinval);
    }
    record->position = static_cast<std::int64_t>(stream_offset);
    return record->position;
}

std::int64_t KernelServices::write_file_stat(const std::filesystem::path& path,
                                             GuestAddress stat_address) {
    if (!guest_writable(stat_address, sizeof(GuestFileStat))) {
        return error(kErrorEfault);
    }

    std::error_code fs_error;
    const auto status = std::filesystem::status(path, fs_error);
    if (fs_error || !std::filesystem::exists(status)) {
        return error(kErrorEnoent);
    }

    GuestFileStat stat{};
    stat.link_count = 1;
    stat.block_size = 512;
    if (std::filesystem::is_regular_file(status)) {
        stat.mode = static_cast<std::uint16_t>(kModeRegular | kModePermissions);
        const auto file_size = std::filesystem::file_size(path, fs_error);
        if (fs_error || file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max())) {
            return error(kErrorEinval);
        }
        stat.size = static_cast<std::int64_t>(file_size);
        stat.blocks = (stat.size + 511) / 512;
    } else if (std::filesystem::is_directory(status)) {
        stat.mode = static_cast<std::uint16_t>(kModeDirectory | kModePermissions);
        stat.size = 65'536;
        stat.block_size = 65'536;
        stat.blocks = 128;
    } else {
        return error(kErrorEnoent);
    }

    (void)file_modification_time(path, stat.modification_time.seconds,
                                 stat.modification_time.nanoseconds);

    const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(&stat), sizeof(stat));
    return memory_.write(stat_address, bytes) ? 0 : error(kErrorEfault);
}

std::int64_t KernelServices::stat_path(GuestAddress path_address, GuestAddress stat_address) {
    if (guest_root_.empty()) {
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
        return write_file_stat(host_path, stat_address);
    } catch (const std::bad_alloc&) {
        return error(kErrorEnomem);
    }
}

std::int64_t KernelServices::fstat(int fd, GuestAddress stat_address) {
    std::shared_ptr<FileRecord> record;
    {
        std::scoped_lock lock(files_mutex_);
        const auto it = files_.find(fd);
        if (it == files_.end()) {
            return error(kErrorEbadf);
        }
        record = it->second;
    }
    return write_file_stat(record->path, stat_address);
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

std::uint64_t KernelServices::allocate_mutex_attr_handle_locked() {
    while (next_mutex_attr_handle_ == 0 || mutex_attrs_.contains(next_mutex_attr_handle_)) {
        next_mutex_attr_handle_ += 8;
        if (next_mutex_attr_handle_ < 0x60000) {
            next_mutex_attr_handle_ = 0x60000;
        }
    }
    const auto result = next_mutex_attr_handle_;
    next_mutex_attr_handle_ += 8;
    return result;
}

int KernelServices::mutex_attr_init(GuestAddress slot_address) {
    if (slot_address == 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    std::scoped_lock lock(mutex_attrs_mutex_);
    const auto handle = allocate_mutex_attr_handle_locked();
    try {
        mutex_attrs_.emplace(handle, MutexAttributes{});
    } catch (const std::bad_alloc&) {
        return kPosixEnomem;
    }
    if (!write_guest_u64(slot_address, handle)) {
        mutex_attrs_.erase(handle);
        return kPosixEinval;
    }
    return 0;
}

int KernelServices::mutex_attr_destroy(GuestAddress slot_address) {
    if (slot_address == 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    std::scoped_lock lock(mutex_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = mutex_attrs_.find(handle);
    if (it == mutex_attrs_.end()) {
        return kPosixEinval;
    }
    if (!write_guest_u64(slot_address, 0)) {
        return kPosixEinval;
    }
    mutex_attrs_.erase(it);
    return 0;
}

int KernelServices::mutex_attr_snapshot(GuestAddress slot_address, MutexAttributes& attributes) const {
    if (slot_address == 0) {
        return kPosixEinval;
    }
    std::scoped_lock lock(mutex_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = mutex_attrs_.find(handle);
    if (it == mutex_attrs_.end()) {
        return kPosixEinval;
    }
    attributes = it->second;
    return 0;
}

int KernelServices::mutex_attr_get_type(GuestAddress slot_address, GuestAddress output_address) {
    if (!guest_writable(output_address, sizeof(std::uint32_t))) {
        return kPosixEinval;
    }
    MutexAttributes attributes;
    const auto result = mutex_attr_snapshot(slot_address, attributes);
    if (result != 0) {
        return result;
    }
    return write_guest_u32(output_address, attributes.type) ? 0 : kPosixEinval;
}

int KernelServices::mutex_attr_set_type(GuestAddress slot_address, std::uint32_t type) {
    if (!supported_mutex_type(type)) {
        return kPosixEinval;
    }
    std::scoped_lock lock(mutex_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = mutex_attrs_.find(handle);
    if (it == mutex_attrs_.end()) {
        return kPosixEinval;
    }
    it->second.type = type;
    return 0;
}

int KernelServices::mutex_attr_get_pshared(GuestAddress slot_address, GuestAddress output_address) {
    if (!guest_writable(output_address, sizeof(std::uint32_t))) {
        return kPosixEinval;
    }
    MutexAttributes attributes;
    const auto result = mutex_attr_snapshot(slot_address, attributes);
    if (result != 0) {
        return result;
    }
    return write_guest_u32(output_address, static_cast<std::uint32_t>(attributes.pshared)) ? 0
                                                                                            : kPosixEinval;
}

int KernelServices::mutex_attr_set_pshared(GuestAddress slot_address, int pshared) {
    if (pshared != 0) {
        return kPosixEinval;
    }
    std::scoped_lock lock(mutex_attrs_mutex_);
    std::uint64_t handle = 0;
    if (!read_guest_u64(slot_address, handle)) {
        return kPosixEinval;
    }
    const auto it = mutex_attrs_.find(handle);
    if (it == mutex_attrs_.end()) {
        return kPosixEinval;
    }
    it->second.pshared = 0;
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
        const auto type = handle == kMutexAdaptiveInitializer ? kMutexTypeAdaptive
                                                               : kMutexTypeErrorCheck;
        std::shared_ptr<MutexRecord> record;
        try {
            record = std::make_shared<MutexRecord>();
        } catch (const std::bad_alloc&) {
            error_out = kPosixEnomem;
            return {};
        }
        record->type = type;
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
    if (slot_address == 0 || !guest_writable(slot_address, sizeof(std::uint64_t))) {
        return kPosixEinval;
    }
    MutexAttributes resolved_attributes;
    if (attributes != 0) {
        const auto result = mutex_attr_snapshot(attributes, resolved_attributes);
        if (result != 0) {
            return result;
        }
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
    record->type = resolved_attributes.type;
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
        if (record->type == kMutexTypeRecursive) {
            if (record->recursion_depth == std::numeric_limits<std::size_t>::max()) {
                return kPosixEagain;
            }
            ++record->recursion_depth;
            return 0;
        }
        if (record->type != kMutexTypeNormal) {
            return kPosixEdeadlk;
        }
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
    record->recursion_depth = 1;
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
    if (record->type == kMutexTypeRecursive && record->recursion_depth > 1) {
        --record->recursion_depth;
        return 0;
    }
    record->locked = false;
    record->owner = {};
    record->recursion_depth = 0;
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

    std::size_t saved_mutex_depth = 1;
    {
        std::unique_lock mutex_lock(mutex->mutex);
        if (!mutex->locked || mutex->owner != std::this_thread::get_id()) {
            return kPosixEperm;
        }
        saved_mutex_depth = mutex->recursion_depth;
        try {
            cond->waiters.push_back(waiter);
        } catch (const std::bad_alloc&) {
            return kPosixEnomem;
        }
        mutex->locked = false;
        mutex->owner = {};
        mutex->recursion_depth = 0;
        mutex_lock.unlock();
        mutex->condition.notify_one();
    }
    cond_lock.unlock();

    const auto reacquire_mutex = [&] {
        const auto result = mutex_lock(mutex_slot_address);
        if (result == 0 && saved_mutex_depth > 1) {
            std::scoped_lock state_lock(mutex->mutex);
            if (mutex->locked && mutex->owner == std::this_thread::get_id() &&
                mutex->type == kMutexTypeRecursive) {
                mutex->recursion_depth = saved_mutex_depth;
            }
        }
        return result;
    };

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
            const auto lock_result = reacquire_mutex();
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
        const auto lock_result = reacquire_mutex();
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

    const auto lock_result = reacquire_mutex();
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


int KernelServices::realtime_deadline(
    GuestAddress timespec_address, std::chrono::system_clock::time_point& deadline) const {
    GuestTimespec value;
    if (!read_guest_timespec(timespec_address, value) || value.seconds < 0 ||
        value.nanoseconds < 0 || value.nanoseconds >= 1'000'000'000) {
        return kPosixEinval;
    }

    using Clock = std::chrono::system_clock;
    using Duration = Clock::duration;
    const auto max_duration = Duration::max();
    const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(max_duration);
    if (value.seconds > max_seconds.count()) {
        deadline = Clock::time_point::max();
        return 0;
    }

    const auto seconds_part = std::chrono::duration_cast<Duration>(
        std::chrono::seconds(value.seconds));
    const auto nanoseconds_part = std::chrono::duration_cast<Duration>(
        std::chrono::nanoseconds(value.nanoseconds));
    if (nanoseconds_part > max_duration - seconds_part) {
        deadline = Clock::time_point::max();
        return 0;
    }
    deadline = Clock::time_point(seconds_part + nanoseconds_part);
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
