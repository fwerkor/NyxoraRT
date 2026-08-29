#include "nyxora/memory/native_arena.hpp"

#include <cstring>
#include <limits>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace nyxora::memory {
namespace {

std::optional<GuestSize> round_up(GuestSize value, GuestSize alignment) {
    if (alignment == 0 || value == 0 || value > std::numeric_limits<GuestSize>::max() - (alignment - 1)) {
        return std::nullopt;
    }
    return (value + alignment - 1) / alignment * alignment;
}

#if defined(_WIN32)
DWORD windows_protection(Protection protection) {
    const bool read = has(protection, Protection::read);
    const bool write = has(protection, Protection::write);
    const bool execute = has(protection, Protection::execute);
    if (execute) {
        if (write) {
            return PAGE_EXECUTE_READWRITE;
        }
        return read ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    }
    if (write) {
        return PAGE_READWRITE;
    }
    return read ? PAGE_READONLY : PAGE_NOACCESS;
}
#else
int posix_protection(Protection protection) {
    int result = PROT_NONE;
    if (has(protection, Protection::read)) {
        result |= PROT_READ;
    }
    if (has(protection, Protection::write)) {
        result |= PROT_WRITE;
    }
    if (has(protection, Protection::execute)) {
        result |= PROT_EXEC;
    }
    return result;
}
#endif

} // namespace

NativeArena::~NativeArena() {
    release();
}

NativeArena::NativeArena(NativeArena&& other) noexcept
    : base_(std::exchange(other.base_, 0)), size_(std::exchange(other.size_, 0)) {}

NativeArena& NativeArena::operator=(NativeArena&& other) noexcept {
    if (this != &other) {
        release();
        base_ = std::exchange(other.base_, 0);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

std::size_t NativeArena::page_size() noexcept {
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    const auto value = sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<std::size_t>(value) : 4096U;
#endif
}

bool NativeArena::exact_reservation_supported() noexcept {
#if defined(_WIN32) || (defined(__linux__) && defined(MAP_FIXED_NOREPLACE))
    return true;
#else
    return false;
#endif
}

std::optional<NativeArena> NativeArena::reserve(GuestSize size, GuestAddress preferred_base) {
    const auto page = static_cast<GuestSize>(page_size());
    const auto rounded_size = round_up(size, page);
    if (!rounded_size || (preferred_base != 0 && preferred_base % page != 0)) {
        return std::nullopt;
    }

#if defined(_WIN32)
    auto* requested = preferred_base == 0 ? nullptr : reinterpret_cast<void*>(preferred_base);
    auto* pointer = VirtualAlloc(requested, static_cast<SIZE_T>(*rounded_size),
                                 MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (pointer == nullptr || (requested != nullptr && pointer != requested)) {
        if (pointer != nullptr) {
            VirtualFree(pointer, 0, MEM_RELEASE);
        }
        return std::nullopt;
    }
#else
    void* requested = preferred_base == 0 ? nullptr : reinterpret_cast<void*>(preferred_base);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (requested != nullptr) {
#if defined(__linux__) && defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#else
        return std::nullopt;
#endif
    }
    void* pointer = mmap(requested, static_cast<std::size_t>(*rounded_size), PROT_NONE, flags, -1, 0);
    if (pointer == MAP_FAILED || (requested != nullptr && pointer != requested)) {
        if (pointer != MAP_FAILED) {
            munmap(pointer, static_cast<std::size_t>(*rounded_size));
        }
        return std::nullopt;
    }
#endif

    return NativeArena(reinterpret_cast<GuestAddress>(pointer), *rounded_size);
}

bool NativeArena::protect(GuestSize offset, GuestSize size, Protection protection) {
    if (base_ == 0 || size == 0) {
        return false;
    }
    const auto page = static_cast<GuestSize>(page_size());
    if (offset % page != 0 || offset > size_) {
        return false;
    }
    const auto rounded_size = round_up(size, page);
    if (!rounded_size || *rounded_size > size_ - offset) {
        return false;
    }
    auto* pointer = reinterpret_cast<void*>(base_ + offset);
#if defined(_WIN32)
    DWORD old_protection{};
    return VirtualProtect(pointer, static_cast<SIZE_T>(*rounded_size), windows_protection(protection),
                          &old_protection) != 0;
#else
    return mprotect(pointer, static_cast<std::size_t>(*rounded_size), posix_protection(protection)) == 0;
#endif
}

bool NativeArena::copy(GuestSize offset, std::span<const std::byte> bytes) {
    if (base_ == 0 || offset > size_ || bytes.size() > size_ - offset) {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(base_ + offset), bytes.data(), bytes.size());
    return true;
}

void* NativeArena::host_pointer(GuestSize offset) noexcept {
    if (base_ == 0 || offset > size_) {
        return nullptr;
    }
    return reinterpret_cast<void*>(base_ + offset);
}

const void* NativeArena::host_pointer(GuestSize offset) const noexcept {
    if (base_ == 0 || offset > size_) {
        return nullptr;
    }
    return reinterpret_cast<const void*>(base_ + offset);
}

void NativeArena::release() noexcept {
    if (base_ == 0) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(reinterpret_cast<void*>(base_), 0, MEM_RELEASE);
#else
    munmap(reinterpret_cast<void*>(base_), static_cast<std::size_t>(size_));
#endif
    base_ = 0;
    size_ = 0;
}

} // namespace nyxora::memory
